#pragma once
#include "MobileMotionPlanner.hpp"
#include "MobilePendingMotion.hpp"
#include "MobileControlDeadline.hpp"
#include <algorithm>
#include <cmath>

namespace vfdual_android {

// Matches the maximum temporal lead of the deployed continuous controller.
// Eight pixels was a legacy residual correction, not a sufficient delay
// compensation for a target travelling 30 px during the observation horizon.
inline constexpr float kMobileObservationPredictionLimitPixels = 48.0F;

// Position feedback plus measured target-velocity feedforward. A moving target
// needs a continuous velocity command even at zero position error; the legacy
// settle latch intentionally stopped there and therefore trailed every strafe.
class MobileContinuousServo final {
 public:
  struct AxisDiagnostic {
    float lead{}, feedback{}, feedforward{}, desired{}, residual{};
    float resolution_radius{};
    bool resolution_stopped{};
  };
  struct Diagnostic {
    float dt_seconds{}, horizon_seconds{};
    AxisDiagnostic x{}, y{};
  };
  [[nodiscard]] MobileMotionPlannerOutput plan(
      const MobileMotionPlannerInput& in, const MobileMotionPlannerConfig& config,
      float pending_x, float pending_y, bool trusted_x, bool trusted_y,
      std::uint64_t visibility_us, std::uint64_t observation_interval_us = 0) noexcept {
    diagnostic_ = {};
    if (!in.observed_at_us || in.now_us < in.observed_at_us) {
      return {.suppression_reason=MotionPlannerSuppressionReason::invalid_time};
    }
    if (in.now_us-in.observed_at_us >=
        std::min(config.maximum_observation_age_us,kContinuousControlMaximumObservationAgeUs)) {
      return {.suppression_reason=MotionPlannerSuppressionReason::stale_observation};
    }
    const bool new_target = target_ != in.target_id;
    if (new_target) reset();
    if (previous_us_ && in.observed_at_us <= previous_us_)
      return {.suppression_reason=MotionPlannerSuppressionReason::non_monotonic_observation};
    // An occluded target retains its identity, but missed output ticks are not
    // movement debt. Use the latest frame cadence on visible return instead
    // of integrating the entire time since the last actionable observation.
    // A new lock still arrives on the established frame clock. Assuming
    // 60 Hz for its first tick doubles the acquisition impulse at 120-140 Hz,
    // then pending compensation stalls later ticks until that impulse appears.
    const auto previous_interval = previous_us_ ? in.observed_at_us-previous_us_ : 0U;
    const bool measured_cadence = observation_interval_us &&
        observation_interval_us<=kContinuousControlMaximumObservationAgeUs;
    const bool continuous_previous = previous_interval &&
        previous_interval<=kContinuousControlMaximumObservationAgeUs;
    const float dt = measured_cadence
        ? std::clamp(float(observation_interval_us) / 1e6F, .002F, .050F)
        : continuous_previous
        ? std::clamp(float(previous_interval) / 1e6F, .002F, .050F)
        : 1.0F / 60.0F;
    // Preserve the shipping position-feedback law while integrating a known
    // velocity over the real elapsed interval below. Raising both together
    // regressed long-occlusion tail error in the30FPS closed-loop comparison.
    const float feedback_dt = std::min(dt, .025F);
    const float gain = 1.0F - std::pow(1.0F - std::clamp(config.response_gain, .01F, .95F), feedback_dt * 60.0F);
    const float horizon = std::min(.140F,
        float(in.now_us - in.observed_at_us + visibility_us) / 1e6F);
    diagnostic_ = {.dt_seconds = dt, .horizon_seconds = horizon};
    // Faster feedback is useful while catching a moving target, but can excite
    // queued reports when the delay fit changes. Observe two response periods
    // at the same estimate before enabling it; a visibility gap starts over.
    if (!delay_stable_since_us_ || previous_visibility_us_ != visibility_us ||
        (previous_us_ && (!continuous_previous ||
          (measured_cadence && previous_interval > 30'000U &&
           previous_interval > observation_interval_us)))) {
      delay_stable_since_us_ = in.observed_at_us;
    }
    previous_visibility_us_ = visibility_us;
    const bool stable_delay = in.observed_at_us >= delay_stable_since_us_ &&
        in.observed_at_us - delay_stable_since_us_ >= 2U * visibility_us;
    MobileMotionPlannerOutput output{};
    output.phase = std::hypot(in.error_x, in.error_y) < 3.0F ? MobileMotionPhase::settle : MobileMotionPhase::pursuit;
    const auto axis = [&](float raw_error, float velocity, float response,
                          float pending, bool trusted, float minimum_error, float maximum_error,
                          bool stationary_evidence,
                          Axis& state, float& predicted, bool& pursuit_boosted, AxisDiagnostic& diagnostic) {
      const float bounded_velocity = std::clamp(velocity, -config.maximum_target_velocity_pixels_per_second,
                                                         config.maximum_target_velocity_pixels_per_second);
      float lead = std::clamp(bounded_velocity * horizon,
          -config.maximum_prediction_pixels, config.maximum_prediction_pixels);
      if (in.prediction_bounds_enabled) {
        const float box_translation = in.prediction_bounds_follow_target_motion ? lead : 0.0F;
        lead = std::clamp(raw_error + lead, minimum_error + box_translation,
            maximum_error + box_translation) - raw_error;
      }
      output.prediction_limited |= std::abs(lead - bounded_velocity * horizon) > .001F;
      // Sensitivity/visibility fits remain uncertain in actual combat. A
      // pending estimate may brake pursuit but may not by itself reverse it.
      predicted = MobilePendingMotion::compensate(raw_error + lead, pending);
      const float feedback = std::abs(predicted) <= config.deadzone_pixels ? 0.0F : predicted;
      // Bounding how far a point can be forecast is not a velocity cap. A
      // steady target still needs its measured speed at every tick, otherwise
      // a longer image/actuator delay silently reduces following speed. When
      // that forecast was clipped, feedforward may brake the bounded position
      // correction but may not reverse it into an unsupported direction.
      const float feedforward = trusted ? bounded_velocity * dt : 0.0F;
      diagnostic.lead = lead;
      diagnostic.feedforward = feedforward;
      // A zero velocity can also mean unavailable evidence. Only an explicit
      // current observation may start this confirmation clock. Pending reports
      // may postpone entry, but do not erase evidence of a stationary target.
      const bool stationary = stationary_evidence && trusted && bounded_velocity == 0.0F;
      const bool interrupted = previous_us_ && (!continuous_previous ||
          (measured_cadence && previous_interval > observation_interval_us));
      if (!stationary || interrupted) {
        state.stationary_since_us = 0;
        state.resolution_stopped = false;
      } else if (!state.stationary_since_us) {
        state.stationary_since_us = in.observed_at_us;
      }
      const bool confirmed_stationary = stationary && state.stationary_since_us &&
          in.observed_at_us - state.stationary_since_us >= 2U * visibility_us;
      // An integer count cannot improve an error inside half a calibrated
      // count. This is a conditional physical resolution limit, not a global
      // deadzone or a latch that suppresses moving-target feedforward.
      const float radius = std::max(config.deadzone_pixels, .5F * response);
      diagnostic.resolution_radius = radius;
      const float numerical_hysteresis = std::max(.0001F, radius * .00001F);
      if (!confirmed_stationary || !in.no_pending_reports ||
          std::abs(raw_error) > radius + (state.resolution_stopped ? numerical_hysteresis : 0.F)) {
        state.resolution_stopped = false;
      } else {
        state.resolution_stopped = true;
        state.residual = state.previous_error = 0.F;
        diagnostic.resolution_stopped = true;
        output.settle_stopped = true;
        return 0;
      }
      if (feedback == 0.0F && std::abs(feedforward) < .01F) {
        state.residual = state.previous_error = 0.F;
        return 0;
      }
      if (state.previous_error * predicted < 0.0F) state.residual = 0.0F;
      // Increase position feedback only when the measured relative velocity
      // carries the target farther from the reticle. Retain the base response
      // near the target and on reversal; do not amplify velocity predictions.
      float feedback_gain = gain;
      if (trusted && stable_delay && raw_error * bounded_velocity > 0.0F && std::abs(raw_error) > 3.0F) {
        const float pursuit = std::clamp((std::abs(raw_error) - 3.0F) / 12.0F, 0.0F, 1.0F);
        const float base = std::clamp(config.response_gain, .01F, .95F);
        const float pursuit_response = std::min(.95F, base + (1.0F - base) * .5F * pursuit);
        feedback_gain = 1.0F - std::pow(1.0F - pursuit_response, feedback_dt * 60.0F);
      }
      pursuit_boosted = feedback_gain > gain && feedback != 0.0F;
      float correction = feedback_gain * feedback + feedforward;
      const bool clipped = std::abs(lead-bounded_velocity*horizon)>.001F;
      if (clipped && correction*predicted<0.0F) correction=0.0F;
      const float desired = correction / response + state.residual;
      diagnostic.feedback = feedback_gain * feedback;
      diagnostic.desired = desired;
      diagnostic.residual = state.residual;
      // Keep the per-packet ceiling compatible with the128-count in-flight
      // envelope. A127-count experiment alternated127/1 packets and doubled
      // visible large-move gaps without improving90% acquisition time.
      const float maximum = float(std::min(config.maximum_axis_delta,
          trusted ? 64 : config.uncalibrated_maximum_axis_delta));
      const float bounded = std::clamp(desired, -maximum, maximum);
      output.response_limited |= bounded != desired;
      const int delta = int(std::lround(bounded));
      state.residual = std::clamp(bounded - delta, -.5F, .5F);
      state.previous_error = predicted;
      return delta;
    };
    output.delta_x = axis(in.error_x, in.target_velocity_x_pixels_per_second,
        config.response_x_px_per_count, pending_x, trusted_x,
        in.prediction_min_error_x, in.prediction_max_error_x, in.stationary_x_evidence,
        x_, output.predicted_error_x,
        output.pursuit_boosted_x, diagnostic_.x);
    output.delta_y = axis(in.error_y, in.target_velocity_y_pixels_per_second,
        config.response_y_px_per_count, pending_y, trusted_y,
        in.prediction_min_error_y, in.prediction_max_error_y, in.stationary_y_evidence,
        y_, output.predicted_error_y,
        output.pursuit_boosted_y, diagnostic_.y);
    if (!output.has_move()) output.suppression_reason =
        std::hypot(output.predicted_error_x, output.predicted_error_y) <= config.deadzone_pixels
        ? MotionPlannerSuppressionReason::deadzone : MotionPlannerSuppressionReason::subcount_resolution;
    target_ = in.target_id; previous_us_ = in.observed_at_us;
    last_ = output;
    return output;
  }
  void reset() noexcept { x_ = {}; y_ = {}; target_ = previous_us_ = 0;
    previous_visibility_us_ = delay_stable_since_us_ = 0; last_ = {}; }
  [[nodiscard]] const MobileMotionPlannerOutput& last_output() const noexcept { return last_; }
  [[nodiscard]] const Diagnostic& diagnostic() const noexcept { return diagnostic_; }
 private:
  struct Axis {
    float residual{}, previous_error{};
    std::uint64_t stationary_since_us{};
    bool resolution_stopped{};
  };
  Axis x_{}, y_{};
  std::uint64_t target_{}, previous_us_{};
  std::uint64_t previous_visibility_us_{}, delay_stable_since_us_{};
  MobileMotionPlannerOutput last_{};
  Diagnostic diagnostic_{};
};
}  // namespace vfdual_android
