#include "ControlReferenceFixture.hpp"
#include "MobileControlCore.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

using namespace vfdual_android;
namespace {
MobileControlConfig config(bool servo) {
  auto c = reference_416_control_config();
  c.selected_class_id = c.body_class_id = 0;
  c.selected_class_uses_geometric_head = true;
  c.head_body_pairing_enabled = c.selected_class_uses_head_box = false;
  c.reject_border_touching = false;
  c.body_fallback_min_confidence = 0;
  c.body_fallback_max_center_distance_pixels = 416;
  c.tracker.high_confidence_threshold = .4F;
  c.tracker.new_track_confidence_threshold = .4F;
  c.motion.response_gain = .6F;
  c.motion.maximum_prediction_pixels = kMobileObservationPredictionLimitPixels;
  c.switch_confirmation_duration_us = 45'000;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2, c);
  c.observation_servo_enabled = servo;
  if (servo) {
    c.lost_target_hold_duration_us = 180'000U;
    c.tracker.lost_track_duration_us = 250'000U;
  }
  return c;
}
YoloDetection box(float x, float y, float confidence = .9F) {
  return {x - 20, y - 18, x + 20, y + 82, confidence, 0};
}
struct Event { std::uint64_t at; int x, y; };
struct Result { double rms{}, p95{}, final_error{}, move_hz{}, maximum_overshoot{}; std::uint64_t switches{}, held{}; };
Result simulate(bool servo, float response, std::uint64_t device_delay, std::uint64_t image_age, bool moving, bool measured = true, float detector_noise = 0.0F, unsigned complex_case = 0, std::uint64_t delay_jitter=24'000, float lead_limit=kMobileObservationPredictionLimitPixels, float steady_velocity=400.F, std::uint64_t interval=7'143, bool production_contract=false) {
  auto simulation_config=config(servo);
  if (production_contract) {
    simulation_config.model_width=simulation_config.model_height=1024.F;
    simulation_config.model_center_x=simulation_config.model_center_y=512.F;
    simulation_config.body_min_width=simulation_config.body_min_height=2.F;
    simulation_config.body_max_width=simulation_config.body_max_height=1024.F;
    simulation_config.body_min_aspect_ratio=.02F;simulation_config.body_max_aspect_ratio=50.F;
    simulation_config.body_fallback_max_center_distance_pixels=1024.F;
    simulation_config.lost_target_hold_duration_us=350'000U;
    simulation_config.tracker.lost_track_duration_us=500'000U;
    simulation_config.motion.response_gain=1.01F;
  }
  const float center=simulation_config.model_center_x;
  simulation_config.motion.maximum_prediction_pixels=lead_limit;
  simulation_config.model_based_motion_feedforward=(complex_case&64U)==0;
  MobileControlCore core(simulation_config);
  if (complex_case & 32U) assert(core.restore_response_calibration(response,response,device_delay,.95F,.95F));
  std::vector<Event> commands;
  std::vector<double> errors;
  constexpr std::uint64_t start = 100'000;
  const auto time = [](std::uint64_t at) { return double(at - start) / 1e6; };
  const auto target = [&](std::uint64_t at) {
    const double t = time(at);
    if (complex_case & 512U) {
      // Repeated travel, stop, return, stop, with continuous position. Include
      // stop/restart transients in the score rather than steady-motion only.
      const double phase=std::fmod(t,1.2);
      const double travel=phase<.35?phase:phase<.6?.35:phase<.95?.95-phase:0.;
      return std::pair{34.0+steady_velocity*travel,0.0};
    }
    if (complex_case & 128U) return std::pair{34.0+steady_velocity*t,0.0};
    if (complex_case & 4U) return std::pair{40.0 * (2.0 / 3.141592653589793) * std::asin(std::sin(t * 11.0)),
        22.0 * std::sin(t * 9.0)};
    return std::pair{moving ? 34.0 + 32.0 * std::sin(t * 6.0) : 60.0,
                     moving ? 15.0 * std::sin(t * 8.0) : 0.0};
  };
  const auto camera = [&](std::uint64_t at) {
    double x = moving && !(complex_case & 128U) ? 10.0 * std::sin(time(at) * 4.0) : 0.0, y{};
    if (complex_case & 1U) {
      x += 65.0 * std::sin(time(at) * 8.0);
      y += 24.0 * std::sin(time(at) * 12.0);
    }
    std::uint64_t last_due{};
    for (const auto& command : commands) {
      const auto jitter=(complex_case & 16U) ? std::int64_t(delay_jitter*std::sin(time(command.at)*23.0)) : 0;
      const auto due=std::max(last_due+1'000,command.at+std::uint64_t(std::max(std::int64_t(8'000),std::int64_t(device_delay)+jitter)));
      last_due=due;
      if (at>=due) { x+=response*command.x; y+=response*command.y; }
    }
    return std::pair{x, y};
  };
  bool serial_pending{};
  Event serial{};
  std::uint64_t previous_observed{}, lock_id{}, moves{};
  Result result;
  for (unsigned i = 0; i < 560; ++i) {
    const auto observed = start + i * interval, now = observed + image_age;
    const auto [tx, ty] = target(observed);
    const auto [cx, cy] = camera(observed);
    const float nx = detector_noise * (float((i * 13U) % 17U) / 8.0F - 1.0F);
    const float ny = detector_noise * (float((i * 7U) % 13U) / 6.0F - 1.0F);
    auto detection = box(float(center + tx - cx) + nx, float(center + ty - cy) + ny);
    if ((complex_case & 256U) && i % 80U == 65U) {
      // A transient extended edge when the person reappears must not keep
      // driving velocity after the box returns to its ordinary geometry.
      if ((i / 80U) % 2U) detection.x1 -= 60.0F;
      else detection.x2 += 60.0F;
    }
    const std::array detections{detection};
    // Cadence regressions use a wall-time visibility gap. The historical
    // adversarial shape fixture below intentionally keeps its five-frame gap.
    const auto phase=(observed-start)%600'000U;
    const bool timed_missing=(complex_case&1024U) && phase>=400'000U && phase<440'000U;
    const bool missing = timed_missing ||
        ((complex_case & 256U) && i % 80U >= 60U && i % 80U < 65U);
    const auto observations = missing ? std::span<const YoloDetection>{} : std::span<const YoloDetection>{detections};
    ControlFrameContext frame{std::uint64_t(i + 1), observed, now};
    if (measured && previous_observed && !((complex_case & 2U) && i > 150 && i % 3 == 0)) {
      const auto [px, py] = camera(previous_observed);
      frame.background = {previous_observed, float(px - cx), float(py - cy), 24, 32, true};
    }
    MobileControlOutput output;
    if (!servo && serial_pending && observed < serial.at) {
      static_cast<void>(core.observe_tracking_only(observations, frame));
    } else {
      if (!servo && serial_pending) {
        core.apply_visible_ego_motion(serial.x, serial.y);
        serial_pending = false;
      }
      output = core.process(observations, frame);
      if (output.has_target) {
        if (!lock_id) lock_id = output.lock_id;
        if (output.lock_id != lock_id) { ++result.switches; lock_id = output.lock_id; }
      }
      if (output.held) ++result.held;
      if (output.has_move()) {
        const Event command{now + 1'000, output.delta_x, output.delta_y};
        commands.push_back(command);
        if (servo) assert(core.record_accepted_move(command.x, command.y, command.at));
        else { serial_pending = true; serial = command; core.begin_move_delivery(); }
        ++moves;
      }
    }
    previous_observed = observed;
    const auto [actual_x, actual_y] = target(now);
    const auto [actual_camera_x, actual_camera_y] = camera(now);
    const double ex = actual_x - actual_camera_x, ey = actual_y - actual_camera_y;
    result.final_error = std::hypot(ex, ey);
    if (complex_case & 8U) {
      const auto& trace=core.control_trace(); const auto& sample=trace.samples[(trace.count-1)%trace.samples.size()];
      std::printf("trace,%u,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%llu,%llu,%llu,%u,%u,%u,%.3f,%.3f,%llu\n",i,ex,sample.target_x-center,sample.velocity_x,
          output.delta_x,sample.pending_x,sample.predicted_error_x,core.response_estimator().x().response,core.response_estimator().delay_us(),
          output.lock_id, sample.track_id, sample.flags, unsigned(output.suppression_reason), unsigned(missing),
          (detection.x1+detection.x2)*.5F-center, detection.x2-detection.x1, core.tracker_metrics().tracks_created);
    }
    if (!moving) result.maximum_overshoot = std::max(result.maximum_overshoot, -ex);
    if (i >= 100) errors.push_back(result.final_error);
  }
  for (double error : errors) result.rms += error * error;
  result.rms = std::sqrt(result.rms / errors.size());
  std::sort(errors.begin(), errors.end());
  result.p95 = errors[errors.size() * 95 / 100];
  result.move_hz = moves / (560.0 * interval / 1e6);
  if (servo && moving) std::printf("servo_response_estimate true=%.2f x=%.3f trusted_x=%u y=%.3f trusted_y=%u delay=%llu\n",
      response, core.response_estimator().x().response, core.response_estimator().x().trusted,
      core.response_estimator().y().response, core.response_estimator().y().trusted, core.response_estimator().delay_us());
  return result;
}
void test_delayed_closed_loop() {
  for (float response : {.35F, .5F, 1.0F, 2.0F}) {
    for (std::uint64_t delay : {8'000U, 16'000U, 24'000U, 40'000U, 56'000U, 72'000U}) {
      const auto stable = simulate(true, response, delay, 18'000, false);
      std::printf("servo_static response=%.2f delay=%llu final=%.2f rms=%.2f overshoot=%.2f switches=%llu\n",
          response, delay, stable.final_error, stable.rms, stable.maximum_overshoot, stable.switches);
      assert(stable.final_error <= 3.1 && stable.rms <= 3.1 && stable.switches == 0);
      assert(stable.maximum_overshoot < 35.0);
    }
  }
  for (float response : {.35F, .5F, 1.0F, 2.0F}) for (std::uint64_t delay : {16'000U, 40'000U, 56'000U, 72'000U}) {
    const auto old = simulate(false, response, delay, 23'000, true);
    const auto next = simulate(true, response, delay, 23'000, true);
    std::printf("servo_moving response=%.2f delay=%llu legacy_rms=%.2f new_rms=%.2f legacy_p95=%.2f new_p95=%.2f legacy_hz=%.2f new_hz=%.2f switches=%llu\n",
        response, delay, old.rms, next.rms, old.p95, next.p95, old.move_hz, next.move_hz, next.switches);
    assert(next.switches == 0 && next.rms < old.rms && next.p95 < old.p95);
  }
  const auto unavailable = simulate(true, .5F, 24'000, 30'000, false, false);
  assert(unavailable.final_error <= 3.1 && unavailable.switches == 0);
}
void test_camera_shift_crowd_and_dropouts() {
  MobileControlCore core(config(true));
  const std::array first{box(208, 208, .95F), box(250, 208)};
  const auto initial = core.process(first, {1, 100'000, 118'000});
  std::uint64_t previous = 100'000;
  float offset{};
  for (unsigned i = 1; i < 80; ++i) {
    const float step = i % 20 < 10 ? -12.0F : 12.0F;
    offset += step;
    const auto at = previous + 7'143U;
    const std::array crowd{box(208 + offset, 208, .85F), box(250 + offset, 208, .99F)};
    const bool missing = i >= 30 && i <= 49;
    const auto output = core.process(missing ? std::span<const YoloDetection>(crowd).subspan(1) : std::span<const YoloDetection>(crowd),
        {i + 1, at, at + 18'000, {previous, step, 0, 20, 30, true}});
    assert(output.lock_id == initial.lock_id && !output.switched);
    if (missing) assert(!output.has_move());
    else assert(std::abs(output.target_x - (208 + offset)) < 1.0F);
    previous = at;
  }
  // Missing or stale background metadata must not apply an unrelated offset.
  const std::array still{box(208 + offset, 208, .95F), box(250 + offset, 208)};
  const auto stale = core.process(still, {81, previous + 7'143, previous + 25'143, {100'000, 64, 0, 30, 30, true}});
  assert(stale.lock_id == initial.lock_id && std::abs(stale.target_x - (208 + offset)) < 1.0F);
}
void test_detector_jitter_does_not_become_camera_oscillation() {
  for (float noise : {1.0F, 2.0F, 3.0F}) {
    const auto still = simulate(true, .65F, 52'000, 23'000, false, true, noise);
    const auto moving = simulate(true, .65F, 52'000, 23'000, true, true, noise);
    std::printf("servo_jitter noise=%.1f static_rms=%.2f static_p95=%.2f moving_rms=%.2f moving_p95=%.2f\n",
        noise, still.rms, still.p95, moving.rms, moving.p95);
    assert(still.switches == 0 && moving.switches == 0);
    assert(still.rms < 1.2 && still.p95 < 2.0);
    assert(moving.rms < 11.0 && moving.p95 < 18.0);
  }
}

void test_provisional_geometry_does_not_seed_motion_feedforward() {
  MobileControlCore core(config(true));
  assert(core.restore_response_calibration(1, 1, 16'000, .95F, .95F));
  for (unsigned i = 0; i < 14; ++i) {
    const auto at = 100'000+i*8'000ULL;
    auto detection = box(240+2.0F*i, 208);
    if (i == 6) detection.x2 += 60;
    const std::array input{detection};
    ControlFrameContext frame{i+1, at, at+18'000};
    if (i) frame.background = {at-8'000, 0, 0, 24, 32, true};
    const auto output = core.process(input, frame);
    assert(output.has_target && !output.switched);
    const auto& trace = core.control_trace();
    const auto& sample = trace.samples[(trace.count-1)%trace.samples.size()];
    if (i == 6 || i == 7) {
      assert(sample.flags & 8192U);
      assert(sample.velocity_x == 0 && sample.velocity_y == 0);
    }
    if (i == 13) assert(sample.velocity_x > 100);
  }
  assert(core.metrics().body_shape_motion_resets == 2);
}
void test_complex_motion_diagnostics() {
  for (unsigned scenario : {1U, 2U, 3U, 4U, 7U}) {
    const auto result = simulate(true, .65F, 52'000, 23'000, true, true, 1.0F, scenario);
    std::printf("servo_complex scenario=%u rms=%.3f p95=%.3f switches=%llu held=%llu\n",
        scenario, result.rms, result.p95, result.switches, result.held);
    assert(result.switches == 0);
  }
}
void test_shape_change_during_closed_loop_tracking() {
  for (unsigned scenario : {0U, 1U, 2U, 3U, 4U, 7U, 16U, 17U}) {
    const auto r = simulate(true, .75F, 52'000, 23'000, true, true, 1.0F, 256U|32U|scenario);
    std::printf("servo_shape scenario=%u rms=%.3f p95=%.3f switches=%llu held=%llu\n",
        scenario, r.rms, r.p95, r.switches, r.held);
    // Identity failures already occur in the previous implementation in these
    // adversarial scenes. Compare all metrics against that immutable baseline;
    // do not label this diagnostic a zero-switch acceptance test.
    assert(std::isfinite(r.rms) && std::isfinite(r.p95));
  }
}
void test_time_aligned_steady_tracking() {
  // A 400 px/s target moves 30 px during a known 23+52 ms observation/actuator
  // horizon. Restricting the future point to 8 px from the OLD box creates a
  // permanent following error even with exact sensitivity and perfect boxes.
  for (float speed : {160.F,400.F,-400.F}) {
    const auto r=simulate(true,.75F,52'000,23'000,true,true,0.F,32U|128U,24'000,
        kMobileObservationPredictionLimitPixels,speed);
    std::printf("servo_steady speed=%.0f rms=%.3f p95=%.3f final=%.3f switches=%llu\n",
        speed,r.rms,r.p95,r.final_error,r.switches);
    // The fixture body is 40 px wide: a permanent error above 20 px is outside
    // the person even in this ideal case. This is a geometric regression floor,
    // not a claim of precise aiming or real-game acceptance.
    assert(r.switches==0 && r.rms<10 && r.p95<20);
  }
}
void test_pursuit_boost_requires_stable_timing_and_retreating_target() {
  auto settings = config(true).motion;
  settings.response_gain = .6F;
  settings.response_x_px_per_count = settings.response_y_px_per_count = 1.0F;
  MobileContinuousServo warmed;
  MobileMotionPlannerInput input{
      .target_id = 1,
      .observation_sequence = 1,
      .observed_at_us = 100'000,
      .now_us = 118'000,
      .error_x = 20.0F,
      .error_y = -20.0F,
      .target_velocity_x_pixels_per_second = 100.0F,
      .target_velocity_y_pixels_per_second = -100.0F,
  };
  const auto cold = warmed.plan(input, settings, 0, 0, true, true, 40'000);
  assert(!cold.pursuit_boosted_x && !cold.pursuit_boosted_y);
  for (unsigned i = 1; i <= 10; ++i) {
    input.observation_sequence = i + 1;
    input.observed_at_us = 100'000 + i * 8'000;
    input.now_us = input.observed_at_us + 18'000;
    (void)warmed.plan(input, settings, 0, 0, true, true, 40'000);
  }
  ++input.observation_sequence;
  input.observed_at_us += 8'000;
  input.now_us += 8'000;
  const auto evaluate = [&](MobileMotionPlannerInput observation, std::uint64_t delay, bool trusted = true) {
    auto copy = warmed;
    return copy.plan(observation, settings, 0, 0, trusted, trusted, delay);
  };
  const auto stable = evaluate(input, 40'000);
  const auto changed = evaluate(input, 44'000);
  assert(stable.pursuit_boosted_x && stable.pursuit_boosted_y);
  assert(!changed.pursuit_boosted_x && !changed.pursuit_boosted_y);
  assert(stable.delta_x > changed.delta_x && stable.delta_y < changed.delta_y);
  auto near = input; near.error_x = 2; near.error_y = -2;
  const auto settled = evaluate(near, 40'000);
  assert(!settled.pursuit_boosted_x && !settled.pursuit_boosted_y);
  auto returning = input;
  returning.target_velocity_x_pixels_per_second *= -1;
  returning.target_velocity_y_pixels_per_second *= -1;
  const auto reversed = evaluate(returning, 40'000);
  assert(!reversed.pursuit_boosted_x && !reversed.pursuit_boosted_y);
  const auto untrusted = evaluate(input, 40'000, false);
  assert(!untrusted.pursuit_boosted_x && !untrusted.pursuit_boosted_y);
  auto gap = input; gap.observed_at_us += 60'000; gap.now_us += 60'000;
  const auto resumed = evaluate(gap, 40'000);
  assert(!resumed.pursuit_boosted_x && !resumed.pursuit_boosted_y);
  auto new_target = input; new_target.target_id = 2;
  const auto switched = evaluate(new_target, 40'000);
  assert(!switched.pursuit_boosted_x && !switched.pursuit_boosted_y);
  std::printf("servo_pursuit stable_dx=%d changed_delay_dx=%d reversal_boost=0 gap_boost=0\n",
      stable.delta_x, changed.delta_x);
}
void test_new_lock_uses_observed_frame_cadence() {
  const auto settings=config(true).motion;
  MobileMotionPlannerInput input{
      .target_id=1,.observation_sequence=1,.observed_at_us=100'000,.now_us=123'000,
      .error_x=40.F,.error_y=-40.F,
  };
  for (const std::uint64_t interval:{5'000U,7'143U,8'333U,16'667U,25'000U,33'333U,40'000U,50'000U}) {
    MobileContinuousServo servo;
    const auto first=servo.plan(input,settings,0,0,true,true,32'000,interval);
    auto next_input=input;
    next_input.observed_at_us+=interval;next_input.now_us+=interval;
    next_input.observation_sequence++;
    const auto next=servo.plan(next_input,settings,0,0,true,true,32'000,interval);
    const float dt=std::clamp(float(interval)/1e6F,.002F,.050F);
    const float gain=1.F-std::pow(1.F-settings.response_gain,std::min(dt,.025F)*60.F);
    const int expected=int(std::lround(std::min(64.F,gain*input.error_x/settings.response_x_px_per_count)));
    std::printf("acquisition_cadence interval=%llu first=%d next=%d expected=%d\n",
        interval,first.delta_x,next.delta_x,expected);
    assert(first.delta_x==expected && first.delta_y==-expected);
    assert(std::abs(first.delta_x-next.delta_x)<=1);
  }
}
void test_continuous_feedforward_uses_elapsed_time_at_low_fps() {
  auto settings=config(true).motion;
  settings.response_x_px_per_count=settings.response_y_px_per_count=1.F;
  for(unsigned fps:{30U,40U,60U,200U}) {
    const auto interval=1'000'000U/fps;
    MobileContinuousServo servo;int total_x=0,total_y=0;
    for(unsigned n=0;n<fps;++n){
      const auto at=100'000+n*std::uint64_t(interval);
      MobileMotionPlannerInput in{.target_id=1,.observation_sequence=n+1,.observed_at_us=at,.now_us=at+18'000,
          .target_velocity_x_pixels_per_second=120.F,.target_velocity_y_pixels_per_second=-120.F};
      // The captured point is already centered. Pending motion cancels the
      // bounded temporal position lead, leaving the steady velocity command.
      const float pending=120.F*.042F;
      const auto out=servo.plan(in,settings,pending,-pending,true,true,24'000,interval);
      assert(out.delta_x>=0 && out.delta_y<=0);
      assert(std::abs(servo.diagnostic().dt_seconds-float(interval)/1e6F)<.000001F);
      total_x+=out.delta_x;total_y+=out.delta_y;
    }
    assert(std::abs(total_x-120)<=1 && std::abs(total_y+120)<=1);
    std::printf("servo_feedforward fps=%u integrated=%d,%d expected=120,-120\n",fps,total_x,total_y);
  }
}
void test_low_fps_delay_stability_and_stale_rejection() {
  auto settings=config(true).motion;settings.response_gain=.6F;
  for(const std::uint64_t interval:{5'000U,16'667U,25'000U,33'333U}){
    MobileContinuousServo servo;
    MobileMotionPlannerInput in{.target_id=1,.observation_sequence=1,.observed_at_us=100'000,.now_us=118'000,
        .error_x=20.F,.target_velocity_x_pixels_per_second=100.F};
    MobileMotionPlannerOutput out;
    for(unsigned n=0;n<15;++n){
      in.observation_sequence=n+1;in.observed_at_us=100'000+n*interval;in.now_us=in.observed_at_us+18'000;
      out=servo.plan(in,settings,0,0,true,true,24'000,interval);
    }
    assert(out.pursuit_boosted_x);
    auto stale=in;stale.observed_at_us+=interval;stale.now_us=stale.observed_at_us+50'000;
    const auto dropped=servo.plan(stale,settings,0,0,true,true,24'000,interval);
    assert(!dropped.has_move() && dropped.suppression_reason==MotionPlannerSuppressionReason::stale_observation);
    auto backwards=stale;backwards.now_us=backwards.observed_at_us-1;
    assert(!servo.plan(backwards,settings,0,0,true,true,24'000,interval).has_move());
    const auto duplicate=servo.plan(in,settings,0,0,true,true,24'000,interval);
    assert(!duplicate.has_move() && duplicate.suppression_reason==MotionPlannerSuppressionReason::non_monotonic_observation);
    // A fresh return after a genuine gap is actionable, but does not borrow
    // the missed time as movement debt or keep the prior pursuit boost.
    in.observed_at_us+=70'000;in.now_us=in.observed_at_us+18'000;++in.observation_sequence;
    const auto resumed=servo.plan(in,settings,0,0,true,true,24'000,70'000);
    assert(!resumed.pursuit_boosted_x);
    assert(std::abs(servo.diagnostic().dt_seconds-1.F/60.F)<.000001F);
  }
}
void test_missing_ticks_restart_delay_stability_without_rejecting_low_fps() {
  const auto settings=config(true).motion;
  for (const std::uint64_t cadence : {7'143U,16'667U,33'333U}) {
    MobileContinuousServo servo;
    MobileMotionPlannerInput in{.target_id=1,.observation_sequence=1,.observed_at_us=100'000,.now_us=123'000,
        .error_x=20.F,.target_velocity_x_pixels_per_second=100.F};
    MobileMotionPlannerOutput out;
    for(unsigned n=0;n<20;++n){
      in.observation_sequence=n+1;in.observed_at_us=100'000+n*cadence;in.now_us=in.observed_at_us+23'000;
      out=servo.plan(in,settings,0,0,true,true,24'000,cadence);
    }
    assert(out.pursuit_boosted_x); // including genuine uninterrupted30FPS
    const std::uint64_t missed_gap=cadence==7'143U ? 6*cadence : 2*cadence;
    in.observed_at_us+=missed_gap;in.now_us=in.observed_at_us+23'000;++in.observation_sequence;
    const auto returned=servo.plan(in,settings,0,0,true,true,24'000,cadence);
    assert(returned.has_move() && !returned.pursuit_boosted_x);
    assert(std::abs(servo.diagnostic().dt_seconds-float(cadence)/1e6F)<.000001F);
  }
}
void test_trusted_packet_cap_preserves_near_and_pending_limits() {
  auto settings=config(true).motion;settings.response_gain=.6F;
  settings.response_x_px_per_count=settings.response_y_px_per_count=.709F;
  settings.maximum_axis_delta=127;settings.uncalibrated_maximum_axis_delta=32;
  for(const std::uint64_t interval:{5'000U,16'667U,25'000U,33'333U}){
    MobileMotionPlannerInput in{.target_id=1,.observation_sequence=1,.observed_at_us=100'000,.now_us=123'000,
        .error_x=512.F,.error_y=-400.F};
    MobileContinuousServo calibrated;
    const auto far=calibrated.plan(in,settings,0,0,true,true,24'000,interval);
    assert(far.delta_x==64 && far.delta_y==-64);
    assert(MobilePendingMotion::bound_axis(far.delta_x,64.F,128)==64);
    MobileContinuousServo unknown;
    const auto limited=unknown.plan(in,settings,0,0,false,false,24'000,interval);
    assert(std::abs(limited.delta_x)<=32 && std::abs(limited.delta_y)<=32);
    // The next fresh opposite error replans in that direction immediately.
    in.error_x=-512;in.error_y=400;in.observed_at_us+=interval;in.now_us+=interval;++in.observation_sequence;
    const auto reversed=calibrated.plan(in,settings,0,0,true,true,24'000,interval);
    assert(reversed.delta_x==-64 && reversed.delta_y==64);
    MobileContinuousServo near;
    in.error_x=32;in.error_y=-32;
    in.target_velocity_x_pixels_per_second=1200;in.target_velocity_y_pixels_per_second=-1200;
    const auto close=near.plan(in,settings,0,0,true,true,24'000,interval);
    assert(std::abs(close.delta_x)<=64 && std::abs(close.delta_y)<=64);
  }
}
void test_low_fps_complex_motion_keeps_identity_and_bounded_error() {
  for(const std::uint64_t interval:{5'000U,16'667U,25'000U,33'333U}){
    for(unsigned scene:{32U|7U,32U|7U|1024U}){
      const auto result=simulate(true,.709F,24'000,23'000,true,true,1.F,scene,24'000,
          kMobileObservationPredictionLimitPixels,400.F,interval);
      std::printf("servo_cadence_complex interval=%llu scene=%u rms=%.3f p95=%.3f switches=%llu held=%llu\n",
          interval,scene,result.rms,result.p95,result.switches,result.held);
      std::fflush(stdout);
      assert(std::isfinite(result.rms) && result.rms<40 && result.p95<80 && result.switches==0);
    }
  }
}
void test_100ms_shape_occlusion_retains_the_measured_baseline() {
  // Preserve the adversarial60FPS diagnostic separately: five missing frames
  // plus the returning observation span100ms, and that first box grows2.5x.
  // Its prior cap64 implementation measured39.627RMS/89.149P95/3lock rebuilds.
  // This is a non-regression bound against that measured source, not a claim
  // that identity is solved under ambiguous geometry after occlusion.
  const auto r=simulate(true,.709F,24'000,23'000,true,true,1.F,32U|7U|256U,24'000,
      kMobileObservationPredictionLimitPixels,400.F,16'667);
  std::printf("servo_100ms_shape_baseline rms=%.3f p95=%.3f switches=%llu held=%llu\n",
      r.rms,r.p95,r.switches,r.held);
  assert(std::isfinite(r.rms) && r.rms<=39.7 && r.p95<=89.2 && r.switches<=3);
}
void test_production_severe_dropout_retains_shipping_baseline() {
  // Historical shipping-baseline trajectory in the physical1024 profile,
  // retained as an explicit synthetic regression rather than the current640
  // capture contract. This includes its
  // actual350/500ms retention and1.01 configured response gain. These long
  // gaps and2.5x return extents remain a known identity limitation; do not
  // trade a better ordinary-scene score for worse tail error after occlusion.
  for(const std::uint64_t cadence : {16'667U,33'333U}) {
    const auto r=simulate(true,.709F,24'000,23'000,true,true,1.F,32U|7U|256U,24'000,
        kMobileObservationPredictionLimitPixels,400.F,cadence,true);
    const double baseline_rms=cadence==16'667U ? 48.005438 : 47.752017;
    const double baseline_p95=cadence==16'667U ? 121.135536 : 98.551520;
    const unsigned baseline_switches=cadence==16'667U ? 3U : 4U;
    std::printf("servo_production_severe cadence=%llu rms=%.6f p95=%.6f switches=%llu\n",
        cadence,r.rms,r.p95,r.switches);
    assert(r.rms<=baseline_rms+.001 && r.p95<=baseline_p95+.001 && r.switches<=baseline_switches);
  }
}
void test_first_lock_after_empty_frames_uses_the_frame_clock() {
  for (const std::uint64_t interval : {5'000U,16'667U,25'000U,33'333U}) {
    MobileControlCore core(config(true));
    assert(core.restore_response_calibration(1,1,24'000,.99F,.99F));
    (void)core.process({}, {1,100'000,123'000});
    (void)core.process({}, {2,100'000+interval,123'000+interval});
    const std::array person{box(248.F,208.F)};
    const auto at=100'000+2*interval;
    const auto out=core.process(person,{3,at,at+23'000});
    assert(out.has_target && out.has_move());
    const auto& trace=core.control_trace();
    const auto& sample=trace.samples[(trace.count-1)%trace.samples.size()];
    assert(std::abs(sample.servo.dt_seconds-float(interval)/1e6F)<.000001F);
  }
}
void test_first_lock_final_output_respects_near_and_far_pacing() {
  for (const std::uint64_t interval : {5'000U,16'667U,25'000U,33'333U}) {
    for (const float error : {40.F,100.F}) {
      MobileControlCore core(config(true));
      assert(core.restore_response_calibration(1,1,24'000,.99F,.99F));
      (void)core.process({}, {1,100'000,123'000});
      (void)core.process({}, {2,100'000+interval,123'000+interval});
      const auto at=100'000+2*interval;
      const std::array target{box(208.F+error,208.F)};
      const auto out=core.process(target,{3,at,at+23'000});
      // The near target shares 128 trusted counts across at least 80 ms:
      // 16,5,4,3 reports respectively, hence 8,25,32,42 counts. Far initial
      // acquisition is exempt only while fewer than two reports are pending.
      const int near_cap=interval==5'000U?8:interval==16'667U?25:interval==25'000U?32:42;
      const float gain=1.F-std::pow(1.F-config(true).motion.response_gain,
          float(std::min(interval,std::uint64_t{25'000}))*60.F/1e6F);
      const int planner=std::min(64,
          int(std::lround(gain*error)));
      const int expected=error==40.F?std::min(planner,near_cap):planner;
      if(out.delta_x!=expected)std::fprintf(stderr,"pacing interval=%llu error=%.0f actual=%d expected=%d planner=%d\n",
          interval,error,out.delta_x,expected,planner);
      assert(out.delta_x==expected);
      assert(std::abs(out.delta_x)<=128);
    }
  }
}
void test_low_fps_pixel_ownership_reaches_the_tracker() {
  for(const std::uint64_t interval:{25'000U,33'333U,40'000U,50'000U,50'001U}){
    MobileControlCore core(config(true));
    const std::array first{box(208,208),box(268,208)};
    const auto original=core.process(first,{1,100'000,123'000});
    assert(original.has_target);
    const std::array next{box(248,208),box(228,208)};
    const std::array evidence{
        TargetImageMotion{100'000,0,1,-20,0,.99F,.2F,9,9,true},
        TargetImageMotion{100'000,1,0,20,0,.99F,.2F,9,9,true}};
    const auto at=100'000+interval;
    const auto continued=core.process(next,{2,at,at+23'000,{100'000,0,0,24,32,true}},evidence);
    if(interval<=50'000){
      assert(core.tracker_metrics().image_constraints_applied==2);
      assert(continued.has_target && continued.lock_id==original.lock_id);
    }else assert(core.tracker_metrics().image_constraints_applied==0);
  }
}
void test_variable_delay_cannot_generate_sustained_single_target_oscillation() {
  for (auto jitter : {16'000U,24'000U,32'000U,40'000U}) {
    for (unsigned scenario : {48U,50U}) {
      const auto still=simulate(true,.75F,60'000,23'000,false,true,1.F,scenario,jitter);
      assert(still.switches==0 && still.maximum_overshoot<35);
      assert(still.rms<(scenario==48U?1.2:5.0) && still.p95<(scenario==48U?2.0:10.0));
      const auto moving=simulate(true,.75F,60'000,23'000,true,true,1.F,scenario,jitter);
      assert(moving.switches==0 && moving.rms<30 && moving.p95<65);
    }
  }
}
void test_brief_occlusion_cannot_transfer_lock_to_reticle_neighbour() {
  auto c=config(true);c.lost_target_hold_duration_us=350'000;c.tracker.lost_track_duration_us=500'000;
  MobileControlCore core(c);
  const std::array both{box(208,208,.95F),box(250,208,.85F)};
  const auto initial=core.process(both,{1,100'000,118'000});
  const std::array neighbour{box(236,208,.99F)};
  for (unsigned i=1;i<=30;++i) {
    const auto at=100'000+i*8'000U;
    const auto held=core.process(neighbour,{i+1,at,at+18'000});
    assert(held.lock_id==initial.lock_id && held.held && !held.has_move());
  }
  const auto restored=core.process(both,{32,348'000,366'000});
  assert(restored.lock_id==initial.lock_id && restored.track_id==initial.track_id && !restored.switched);
}
void test_pending_contract() {
  // Reproduces real trace frame 68: requested zero, pending=-88, budget=64.
  // Clamping zero to [24,152] formerly manufactured a +24 reverse report.
  assert(MobilePendingMotion::bound_axis(0, -88, 64) == 0);
  assert(MobilePendingMotion::bound_axis(-12, -88, 64) == 0);
  assert(MobilePendingMotion::bound_axis(12, -88, 64) == 12);
  assert(MobilePendingMotion::bound_axis(32, 160, 128) == 0);
  for (int pending=-200; pending<=200; ++pending) for (int move=-127; move<=127; ++move) {
    const int limited=MobilePendingMotion::bound_axis(move,float(pending),128);
    assert(std::abs(limited)<=std::abs(move));
    assert(limited==0 || (limited>0)==(move>0));
  }
  MobilePendingMotion pending;
  assert(pending.accept(20, -5, 100'000));
  assert(pending.accept(10, 2, 107'000));
  const auto before = pending.predict(110'000);
  assert(before.reports == 2 && before.x == 30 && before.y == -3);
  assert(MobilePendingMotion::compensate(10, 30) == 0);
  assert(MobilePendingMotion::compensate(-10, -30) == 0);
  const auto after = pending.predict(132'000);
  assert(after.reports == 1 && after.x == 10);
  assert(pending.predict(139'000).reports == 0);
  // Visibility at the current estimate does not retire history until the
  // maximum supported delay, so a later estimate can still consult it.
  assert(pending.predict(207'000,100'000).reports == 0);
  for (unsigned i = 0; i < MobilePendingMotion::kCapacity; ++i) assert(pending.accept(1, 1, 208'000 + i));
  assert(!pending.accept(1, 1, 208'100));
  pending.reset();
  assert(pending.predict(300'000).reports == 0);
}
void test_restored_response_does_not_starve_large_error() {
  MobileControlCore cold(config(true)), restored(config(true));
  assert(restored.restore_response_calibration(.664926F,.753563F,60'000,.955341F,.891751F));
  assert(!restored.restore_response_calibration(.1F,.75F,60'000,.95F,.89F));
  const std::array detections{box(208,128)};
  for (auto* core : {&cold,&restored}) {
    (void)core->process(detections,{1,100'000,118'000});
    assert(core->record_accepted_move(0,-20,118'000));
    assert(core->record_accepted_move(0,-20,119'000));
    // Restoring a cache while reports are pending would corrupt their meaning.
    assert(!core->restore_response_calibration(.66F,.75F,60'000,.95F,.89F));
  }
  const auto uncalibrated=cold.process(detections,{2,107'143,125'143});
  const auto calibrated=restored.process(detections,{2,107'143,125'143});
  assert(uncalibrated.delta_y==0 && calibrated.delta_y<0);
  restored.reset();
  assert(restored.response_estimator().x().trusted);
  assert(restored.restore_response_calibration(.664926F,.753563F,60'000,.955341F,.891751F));
  assert(restored.response_estimator().updates()==0);
}
void test_combat_fit_cannot_replace_restored_sensitivity() {
  MobileResponseEstimator restored;
  assert(restored.restore(.665F,.754F,60'000,.95F,.89F));
  std::vector<Event> commands;
  const auto camera=[&](std::uint64_t at,bool y) {
    float v{};
    for (const auto& c:commands) if (c.at+16'000<=at) v+=(y?c.y*.271F:c.x*.665F);
    return v;
  };
  for (unsigned i=0;i<160;++i) {
    const auto at=100'000+i*7'143U, previous=at-7'143U;
    commands.push_back({at,int((i*7)%19)-9,int((i*11)%13)-6});
    const auto& c=commands.back(); restored.accept(c.x,c.y,at);
    restored.observe({previous,camera(previous,false)-camera(at,false),
        camera(previous,true)-camera(at,true),24,30,i>0},at);
  }
  assert(restored.x().trusted && restored.y().trusted);
  assert(restored.x().response==.665F && restored.y().response==.754F);
}
void test_distant_new_track_cannot_inherit_continuous_lock() {
  auto c=config(true);
  c.tracker.maximum_center_distance_pixels=10;
  c.tracker.first_stage_iou_threshold=.9F;
  c.tracker.second_stage_iou_threshold=.8F;
  MobileControlCore core(c);
  const std::array initial{box(250,208)};
  const auto first=core.process(initial,{1,100'000,118'000});
  const std::array neighbour{box(185,208)};
  const auto next=core.process(neighbour,{2,107'143,125'143});
  assert(next.held && next.lock_id==first.lock_id && !next.has_move());
}
void test_response_estimator_requires_excitation_and_background_agreement() {
  MobileResponseEstimator measured, unrelated, constant;
  std::vector<Event> commands;
  const auto camera = [&](std::uint64_t at, bool y) {
    float value{};
    for (const auto& c : commands) if (c.at + 16'000 <= at) value += (y ? c.y * 1.4F : c.x * .4F);
    return value;
  };
  for (unsigned i = 0; i < 160; ++i) {
    const std::uint64_t at = 100'000 + i * 7'143U, previous = at - 7'143U;
    commands.push_back({at, int((i * 7U) % 19U) - 9, int((i * 11U) % 13U) - 6});
    const auto& command = commands.back();
    measured.accept(command.x, command.y, at);
    unrelated.accept(command.x, command.y, at);
    constant.accept(3, 3, at);
    const float noise = (int(i % 3) - 1) * .08F;
    const BackgroundMotion motion{previous,
        camera(previous, false) - camera(at, false) + .45F + noise,
        camera(previous, true) - camera(at, true) - .25F - noise, 24, 30, i > 0};
    measured.observe(motion, at);
    unrelated.observe({previous, .45F, -.25F, 24, 30, i > 0}, at);
    constant.observe({previous, -1.2F, -1.2F, 24, 30, i > 0}, at);
  }
  assert(measured.x().trusted && measured.y().trusted);
  assert(std::abs(measured.x().response - .4F) < .05F);
  assert(std::abs(measured.y().response - 1.4F) < .1F);
  assert(!unrelated.x().trusted && !unrelated.y().trusted);
  assert(!constant.x().trusted && !constant.y().trusted);
}
}
void test_extent_change_cannot_move_a_stationary_requested_point() {
  // Real defect: an unchanged geometric head anchor, with a shrinking lower
  // body edge, used to send 34 moves from 48 observations. Keep the detector
  // input independent of the controller so this is a functional counterexample.
  for (const float y : {208.F, 236.F}) {
    MobileControlCore core(config(true));
    assert(core.restore_response_calibration(1,1,52'000,.95F,.95F));
    unsigned guarded{}, moves{};
    for (unsigned i=0;i<48;++i) {
      const auto at=100'000+i*10'000ULL;
      const float height=100.F+15.F*std::sin(float(i)*.3F);
      const std::array input{YoloDetection{188,y-.18F*height,228,y+.82F*height,.9F,0}};
      ControlFrameContext frame{i+1,at,at+23'000};
      if (i) frame.background={at-10'000,0,0,24,32,true};
      const auto out=core.process(input,frame);
      const auto& trace=core.control_trace();
      const auto& sample=trace.samples[(trace.count-1)%trace.samples.size()];
      assert(out.has_target && !out.switched);
      assert(std::abs(sample.velocity_y)<.01F && std::abs(sample.servo.y.lead)<.01F);
      if (y==208.F) assert(!out.has_move());
      else assert(out.delta_y>0); // Position feedback is still immediate.
      guarded+=(sample.velocity_flags&128U)!=0U;
      moves+=out.has_move();
    }
    assert(guarded>0 && (y==208.F || moves==48));
  }
}

void test_pixel_noise_cannot_disable_stationary_aim_evidence() {
  // Independent detector stream: head stays in a +/-0.6 px noise band while
  // the lower body edge changes with pose. Do not extrapolate centre motion.
  for (const float y : {208.F,236.F}) {
    MobileControlCore core(config(true));
    assert(core.restore_response_calibration(1,1,24'000,.99F,.99F));
    unsigned nontrivial{},guarded{};
    for(unsigned i=0;i<48;++i) {
      const auto at=100'000+i*10'000ULL;
      const float point=y+(i%2 ? .6F:-.6F);
      const float height=100.F+15.F*std::sin(float(i)*.3F);
      const std::array input{YoloDetection{188,point-.18F*height,228,point+.82F*height,.9F,0}};
      ControlFrameContext frame{i+1,at,at+18'000};
      if(i)frame.background={at-10'000,0,0,24,32,true};
      const auto out=core.process(input,frame);
      const auto& trace=core.control_trace();const auto& sample=trace.samples[(trace.count-1)%trace.samples.size()];
      assert(out.has_target && !out.switched);
      if(i>=6) {
        if(y==208.F) {
          assert(std::abs(sample.velocity_y)<.01F && std::abs(sample.servo.y.lead)<.01F);
          assert(std::abs(out.delta_y)<=1);
        }
        else assert(out.delta_y>0);
        nontrivial+=std::abs(out.delta_y)>1;
        guarded+=(sample.velocity_flags&128U)!=0U;
      }
    }
    assert(y==208.F ? guarded>0 && nontrivial==0 : nontrivial>0);
    std::printf("pixel_shape y=%.1f nontrivial=%u guarded=%u\n",y,nontrivial,guarded);
  }
}

void test_geometry_guard_preserves_translation_and_tracking_commands() {
  MobileResponseEstimator response;
  assert(response.restore(1,1,52'000,.95F,.95F));
  MobileDisturbanceVelocity changing, ordinary;
  for (unsigned i=0;i<40;++i) {
    const auto at=100'000+i*10'000ULL;
    const float anchor=208.F+3.F*i;
    const float extent=100.F+15.F*std::sin(float(i)*.3F);
    const auto actual=changing.update(anchor,anchor+.32F*extent,at,response,1200,0,0,0,-.32F*extent);
    const auto expected=ordinary.update(anchor,anchor+.32F*extent,at,response,1200);
    assert(actual.x==expected.x && actual.y==expected.y);
    assert(changing.geometry_bounded_axes()==0);
  }
  changing.reset(); ordinary.reset();
  // A stationary screen point can also mean successful active following.
  // Accepted motion explains why it stays still and must keep its velocity.
  for (unsigned i=0;i<60;++i) {
    const auto at=1'000'000+i*10'000ULL;
    response.accept(0,4,at);
    const float extent=100.F+15.F*std::sin(float(i)*.3F);
    const auto actual=changing.update(208,208+.32F*extent,at,response,1200,0,0,0,-.32F*extent);
    const auto expected=ordinary.update(208,208+.32F*extent,at,response,1200);
    assert(actual.y==expected.y && changing.geometry_bounded_axes()==0);
    if (i>20) assert(actual.y>100.F);
  }
}

int main(int argc, char** argv) {
  if(argc>1 && std::string_view(argv[1])=="--pixel-shape-only") {
    test_pixel_noise_cannot_disable_stationary_aim_evidence();return 0;
  }
  test_pixel_noise_cannot_disable_stationary_aim_evidence();
  test_continuous_feedforward_uses_elapsed_time_at_low_fps();
  test_low_fps_delay_stability_and_stale_rejection();
  test_missing_ticks_restart_delay_stability_without_rejecting_low_fps();
  test_trusted_packet_cap_preserves_near_and_pending_limits();
  test_first_lock_after_empty_frames_uses_the_frame_clock();
  test_first_lock_final_output_respects_near_and_far_pacing();
  test_low_fps_complex_motion_keeps_identity_and_bounded_error();
  test_100ms_shape_occlusion_retains_the_measured_baseline();
  test_production_severe_dropout_retains_shipping_baseline();
  test_low_fps_pixel_ownership_reaches_the_tracker();
  test_extent_change_cannot_move_a_stationary_requested_point();
  test_geometry_guard_preserves_translation_and_tracking_commands();
  test_provisional_geometry_does_not_seed_motion_feedforward();
  if (argc>1 && std::string_view(argv[1])=="--acquisition-cadence") {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    test_new_lock_uses_observed_frame_cadence();return 0;
  }
  if (argc == 3 && std::string_view(argv[1]) == "--trace-shape") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto scenario = static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10));
    (void)simulate(true, .75F, 52'000, 23'000, true, true, 1.0F, 256U|32U|scenario|8U);
    return 0;
  }
  if (argc>1 && std::string_view(argv[1])=="--trace-shape-complex") {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    (void)simulate(true,.75F,52'000,23'000,true,true,1.0F,256U|32U|7U|8U);
    return 0;
  }
  if (argc>1 && std::string_view(argv[1])=="--shape-only") {
    std::setvbuf(stdout,nullptr,_IONBF,0); test_shape_change_during_closed_loop_tracking(); return 0;
  }
  if (argc>1 && std::string_view(argv[1])=="--steady-only") {
    std::setvbuf(stdout,nullptr,_IONBF,0); test_time_aligned_steady_tracking(); return 0;
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc>1 && std::string_view(argv[1])=="--stop-sweep") {
    for (auto cadence : {7'143U,12'000U,16'667U})
    for (float speed : {160.F,400.F,600.F})
    for (auto delay : {32'000U,52'000U,72'000U})
    for (float noise : {0.F,1.F})
    for (unsigned scenario : {544U,560U,561U}) {
      const auto r=simulate(true,.75F,delay,23'000,true,true,noise,scenario,24'000,
          kMobileObservationPredictionLimitPixels,speed,cadence);
      std::printf("stop_sweep cadence=%u speed=%.0f delay=%u noise=%.0f scenario=%u rms=%.4f p95=%.4f switches=%llu\n",
          cadence,speed,delay,noise,scenario,r.rms,r.p95,r.switches);
      assert(std::isfinite(r.rms) && std::isfinite(r.p95));
    }
    return 0;
  }
  if (argc>1 && std::string_view(argv[1])=="--audit-dynamics") {
    for (float limit : {8.F,16.F,24.F,32.F,48.F}) for (auto scenario : {1U,3U,7U,33U,35U,39U,49U,51U}) {
      const auto r=simulate(true,.65F,52'000,23'000,true,true,1.F,scenario,24'000,limit);
      std::printf("audit_dynamics lead_limit=%.0f scenario=%u rms=%.2f p95=%.2f switches=%llu\n",limit,scenario,r.rms,r.p95,r.switches);
    }
    return 0;
  }
  if (argc>1 && std::string_view(argv[1])=="--variable-delay") {
    for (auto scenario : {16U,48U,50U,49U,51U,112U,114U,113U,115U}) for (bool moving : {false,true}) {
      const auto r=simulate(true,.75F,60'000,23'000,moving,true,1.F,scenario);
      std::printf("variable_delay scenario=%u moving=%d rms=%.2f p95=%.2f overshoot=%.2f switches=%llu\n",scenario,moving,r.rms,r.p95,r.maximum_overshoot,r.switches);
    }
    return 0;
  }
  if (argc>1 && std::string_view(argv[1])=="--delay-sweep") {
    for (auto jitter : {8'000U,16'000U,24'000U,32'000U,40'000U}) for (auto scenario : {48U,50U}) for (bool moving : {false,true}) {
      const auto r=simulate(true,.75F,60'000,23'000,moving,true,1.F,scenario,jitter);
      std::printf("delay_sweep jitter=%u scenario=%u moving=%d rms=%.2f p95=%.2f overshoot=%.2f\n",jitter,scenario,moving,r.rms,r.p95,r.maximum_overshoot);
    }
    return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "--complex-only") {
    test_complex_motion_diagnostics(); return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "--jitter-only") {
    test_detector_jitter_does_not_become_camera_oscillation(); return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "--trace-static") {
    (void)simulate(true, 2.F, 56'000, 18'000, false, true, 0.F, 8U); return 0;
  }
  test_pending_contract(); test_response_estimator_requires_excitation_and_background_agreement();
  test_restored_response_does_not_starve_large_error();
  test_combat_fit_cannot_replace_restored_sensitivity();
  test_distant_new_track_cannot_inherit_continuous_lock();
  test_delayed_closed_loop(); test_camera_shift_crowd_and_dropouts();
  test_detector_jitter_does_not_become_camera_oscillation();
  test_complex_motion_diagnostics();
  test_variable_delay_cannot_generate_sustained_single_target_oscillation();
  test_brief_occlusion_cannot_transfer_lock_to_reticle_neighbour();
  test_time_aligned_steady_tracking();
  test_pursuit_boost_requires_stable_timing_and_retreating_target();
  test_new_lock_uses_observed_frame_cadence();
  test_shape_change_during_closed_loop_tracking();
}
