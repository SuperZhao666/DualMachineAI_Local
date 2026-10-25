#include "MobileTargetTracker.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace {

using vfdual_android::MobileTargetTracker;
using vfdual_android::MobileTargetTrackerConfig;
using vfdual_android::TrackedDetection;
using vfdual_android::YoloDetection;

YoloDetection box(float center_x, float center_y, float width, float height,
                  float confidence, std::uint32_t class_id) {
  return {center_x - width * 0.5F, center_y - height * 0.5F,
          center_x + width * 0.5F, center_y + height * 0.5F,
          confidence, class_id};
}

float tracked_center_x(const TrackedDetection& tracked) {
  return (tracked.detection.x1 + tracked.detection.x2) * 0.5F;
}

float tracked_width(const TrackedDetection& tracked) {
  return tracked.detection.x2 - tracked.detection.x1;
}

void test_ids_follow_targets_when_input_order_changes() {
  MobileTargetTracker tracker;
  const std::array first{
      box(80.0F, 100.0F, 20.0F, 20.0F, 0.90F, 1U),
      box(240.0F, 100.0F, 20.0F, 20.0F, 0.90F, 1U),
  };
  const auto initial = tracker.update(first, 1);
  assert(initial[0].track_id != 0 && initial[1].track_id != 0);
  assert(initial[0].track_id != initial[1].track_id);

  const std::array reordered{
      box(238.0F, 100.0F, 20.0F, 20.0F, 0.90F, 1U),
      box(82.0F, 100.0F, 20.0F, 20.0F, 0.90F, 1U),
  };
  const auto next = tracker.update(reordered, 2);
  assert(next[0].track_id == initial[1].track_id);
  assert(next[1].track_id == initial[0].track_id);
}

void test_joint_assignment_preserves_two_visible_people() {
  for (bool reverse : {false,true}) {
    MobileTargetTrackerConfig config;
    config.lost_track_duration_us=500'000;
    config.control_smoothing_alpha=1;
    MobileTargetTracker tracker(config);
    const std::array before{box(100,200,40,100,.9F,0),box(120,200,40,100,.9F,0)};
    const auto initial=tracker.update(before,1,100'000);
    std::array after{box(112,200,40,100,.9F,0),box(135,200,40,100,.9F,0)};
    if (reverse) std::swap(after[0],after[1]);
    const auto next=tracker.update(after,2,108'000);
    // Both old tracks locally prefer the first observation. The complete
    // assignment has a higher total geometric score and preserves both IDs.
    assert(next[reverse ? 1 : 0].track_id==initial[0].track_id);
    assert(next[reverse ? 0 : 1].track_id==initial[1].track_id);
    assert(tracker.metrics().tracks_created==2);
    assert(tracker.metrics().joint_assignment_acceptances == 1);
  }
}

void test_size_boundary_is_invariant_to_coordinate_rounding() {
  for (float center : {100.1F,200.F,240.3F,320.7F}) {
    MobileTargetTrackerConfig config;
    config.control_smoothing_alpha=1;
    MobileTargetTracker tracker(config);
    const std::array before{box(center,200,38.4F,100,.9F,0)};
    const auto initial=tracker.update(before,1);
    // 96 / 38.4 is exactly the configured 2.5 limit; subtracting float
    // coordinates must not make the same geometry alternate identities.
    const std::array boundary{box(center,200,96.F,100,.9F,0)};
    const auto next=tracker.update(boundary,2);
    assert(next[0].track_id==initial[0].track_id);
    MobileTargetTracker outside(config);
    const auto start=outside.update(before,1);
    const std::array too_large{box(center,200,97.F,100,.9F,0)};
    assert(outside.update(too_large,2)[0].track_id!=start[0].track_id);
  }
}

void test_unconfirmed_extent_cannot_redirect_next_identity() {
  MobileTargetTrackerConfig config;
  config.control_smoothing_alpha = 1.0F;
  config.confirm_abrupt_extent = true;
  MobileTargetTracker tracker(config);
  const std::array before{
      box(100, 200, 40, 100, .9F, 0), box(180, 200, 40, 100, .9F, 0)};
  const auto initial = tracker.update(before, 1);
  auto extended = before;
  extended[0].x2 += 60.0F; // One-frame effect changes only the right edge.
  const auto transient = tracker.update(extended, 2);
  assert(transient[0].track_id == initial[0].track_id);
  assert(transient[0].motion_discontinuity);
  assert(std::abs(tracked_center_x(transient[0]) - 100.0F) < .01F);
  const std::array returning{
      box(90, 200, 40, 100, .9F, 0), box(160, 200, 40, 100, .9F, 0)};
  const auto next = tracker.update(returning, 3);
  assert(next[0].track_id == initial[0].track_id);
  assert(next[1].track_id == initial[1].track_id);
  assert(next[0].motion_discontinuity);
  assert(!next[1].motion_discontinuity);
  const auto confirmed = tracker.update(returning, 4);
  assert(!confirmed[0].motion_discontinuity);
}

void test_retention_does_not_extend_motion_prediction_for_half_a_second() {
  MobileTargetTrackerConfig config;
  config.lost_track_duration_us = 500'000;
  config.control_smoothing_alpha = 1;
  MobileTargetTracker tracker(config);
  std::uint64_t first_id{};
  for (unsigned frame = 0; frame < 4; ++frame) {
    const std::array detection{box(180-10.0F*frame, 200, 40, 100, .9F, 0)};
    const auto tracked = tracker.update(detection, frame+1, 100'000+8'000*frame);
    if (!frame) first_id = tracked[0].track_id;
    assert(tracked[0].track_id == first_id);
  }
  (void)tracker.update({}, 5, 140'000);
  // It stopped while occluded. Keeping its identity for 500 ms must not move
  // the association gate hundreds of pixels using a four-frame-old velocity.
  const std::array returned{box(150, 200, 40, 100, .9F, 0)};
  assert(tracker.update(returned, 6, 300'000)[0].track_id == first_id);
}

void test_dense_observation_burst_has_bounded_assignment_workspace() {
  MobileTargetTracker tracker;
  std::array<YoloDetection, 40> detections{};
  for (unsigned i = 0; i < detections.size(); ++i) {
    detections[i] = box(40+float(i%8)*45, 70+float(i/8)*75, 20, 40, .9F, 0);
  }
  const auto before = tracker.update(detections, 1);
  const auto after = tracker.update(detections, 2);
  for (unsigned i = 0; i < detections.size(); ++i) {
    assert(after[i].track_id == before[i].track_id);
  }
  assert(tracker.metrics().global_assignment_calls == 0);
  assert(tracker.metrics().global_assignment_size_fallbacks > 0);
}

void test_three_crossing_targets_keep_identity_when_order_changes() {
  MobileTargetTracker tracker;
  const std::array initial{
      box(80.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U),
      box(160.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U),
      box(240.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U),
  };
  const auto created = tracker.update(initial, 1);
  const std::uint64_t left_id = created[0].track_id;
  const std::uint64_t middle_id = created[1].track_id;
  const std::uint64_t right_id = created[2].track_id;

  const std::array approaching{
      box(235.0F, 100.0F, 20.0F, 40.0F, 0.92F, 1U),
      box(98.0F, 100.0F, 20.0F, 40.0F, 0.91F, 1U),
      box(148.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U),
  };
  const auto second = tracker.update(approaching, 2);
  assert(second[0].track_id == right_id);
  assert(second[1].track_id == left_id);
  assert(second[2].track_id == middle_id);

  const std::array near_crossing{
      box(136.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U),
      box(230.0F, 100.0F, 20.0F, 40.0F, 0.92F, 1U),
      box(116.0F, 100.0F, 20.0F, 40.0F, 0.91F, 1U),
  };
  const auto third = tracker.update(near_crossing, 3);
  assert(third[0].track_id == middle_id);
  assert(third[1].track_id == right_id);
  assert(third[2].track_id == left_id);

  const std::array crossed_and_reordered{
      box(134.0F, 100.0F, 20.0F, 40.0F, 0.91F, 1U),
      box(124.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U),
      box(225.0F, 100.0F, 20.0F, 40.0F, 0.92F, 1U),
  };
  const auto crossed = tracker.update(crossed_and_reordered, 4);
  assert(crossed[0].track_id == left_id);
  assert(crossed[1].track_id == middle_id);
  assert(crossed[2].track_id == right_id);
}

void test_low_confidence_owner_blocks_high_confidence_neighbour_id_theft() {
  MobileTargetTracker tracker;
  const std::array initial{
      box(100.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U)};
  const auto created = tracker.update(initial, 1);

  // OW2 effects can briefly lower the locked person's confidence while a
  // nearby enemy remains clear. The low-confidence near observation must get
  // ByteTrack's continuation pass before the farther enemy can inherit its ID.
  const std::array obscured_with_neighbour{
      box(130.0F, 100.0F, 20.0F, 40.0F, 0.99F, 1U),
      box(102.0F, 100.0F, 20.0F, 40.0F, 0.30F, 1U),
  };
  const auto tracked = tracker.update(obscured_with_neighbour, 2);

  assert(tracked[1].track_id == created[0].track_id);
  assert(tracked[0].track_id != created[0].track_id);
  assert(tracker.metrics().ambiguous_center_matches_rejected >= 1U);
}

void test_scale_jump_cannot_transfer_identity_to_neighbour() {
  MobileTargetTracker tracker;
  const std::array initial{
      box(100.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U)};
  const auto created = tracker.update(initial, 1);

  // A skill/pose transition can make the original box temporarily
  // size-incompatible. Fail closed instead of handing its ID to the nearby
  // normal-sized enemy solely because that enemy is within the distance gate.
  const std::array scale_jump_with_neighbour{
      box(130.0F, 100.0F, 20.0F, 40.0F, 0.99F, 1U),
      box(102.0F, 100.0F, 60.0F, 120.0F, 0.90F, 1U),
  };
  const auto tracked = tracker.update(scale_jump_with_neighbour, 2);

  assert(tracked[0].track_id != created[0].track_id);
  assert(tracked[1].track_id != created[0].track_id);
  assert(tracker.metrics().ambiguous_center_matches_rejected >= 1U);
}

void test_low_confidence_second_stage_continues_but_does_not_create() {
  MobileTargetTracker tracker;
  const std::array initial{box(100.0F, 100.0F, 20.0F, 20.0F, 0.90F, 1U)};
  const auto created = tracker.update(initial, 1);
  const std::array low{
      box(103.0F, 100.0F, 20.0F, 20.0F, 0.30F, 1U),
      box(220.0F, 100.0F, 20.0F, 20.0F, 0.30F, 1U),
  };
  const auto continued = tracker.update(low, 2);
  assert(continued[0].track_id == created[0].track_id);
  assert(continued[1].track_id == 0);
  assert(tracker.metrics().second_stage_matches == 1);
}

void test_association_is_class_isolated() {
  MobileTargetTracker tracker;
  const std::array detections{
      box(160.0F, 120.0F, 20.0F, 20.0F, 0.90F, 1U),
      box(160.0F, 120.0F, 20.0F, 20.0F, 0.90F, 0U),
  };
  const auto tracked = tracker.update(detections, 1);
  assert(tracked[0].track_id != 0 && tracked[1].track_id != 0);
  assert(tracked[0].track_id != tracked[1].track_id);
}

void test_lost_buffer_expiry_assigns_a_new_identity() {
  MobileTargetTrackerConfig config{};
  config.lost_frame_buffer = 1;
  MobileTargetTracker tracker(config);
  const std::array target{box(160.0F, 120.0F, 20.0F, 20.0F, 0.90F, 1U)};
  const auto initial = tracker.update(target, 1);
  (void)tracker.update({}, 2);
  (void)tracker.update({}, 3);
  const auto reacquired = tracker.update(target, 4);
  assert(reacquired[0].track_id != initial[0].track_id);
  assert(tracker.metrics().tracks_expired == 1);
}

void test_lost_duration_is_stable_across_fresh_frame_rates() {
  MobileTargetTrackerConfig config{};
  config.lost_frame_buffer = 0;
  config.lost_track_duration_us = 100'000U;
  const std::array target{box(160.0F, 120.0F, 20.0F, 20.0F, 0.90F, 1U)};
  const auto verify_reacquisition = [&](const auto& missing_times) {
    MobileTargetTracker tracker(config);
    const auto initial = tracker.update(target, 1U, 10'000U);
    std::uint64_t sequence = 2U;
    for (const std::uint64_t observed_at_us : missing_times) {
      (void)tracker.update({}, sequence++, observed_at_us);
    }
    const auto reacquired = tracker.update(target, sequence, 105'000U);
    assert(reacquired[0].track_id == initial[0].track_id);
    assert(tracker.metrics().tracks_expired == 0U);
  };

  constexpr std::array<std::uint64_t, 18> high_fps_missing_times{
      15'000U, 20'000U, 25'000U, 30'000U, 35'000U, 40'000U,
      45'000U, 50'000U, 55'000U, 60'000U, 65'000U, 70'000U,
      75'000U, 80'000U, 85'000U, 90'000U, 95'000U, 100'000U,
  };
  constexpr std::array<std::uint64_t, 3> low_fps_missing_times{
      40'000U, 70'000U, 100'000U};
  verify_reacquisition(high_fps_missing_times);
  verify_reacquisition(low_fps_missing_times);

  MobileTargetTracker expired(config);
  const auto initial = expired.update(target, 1U, 10'000U);
  (void)expired.update({}, 2U, 110'001U);
  const auto reacquired = expired.update(target, 3U, 120'000U);
  assert(reacquired[0].track_id != initial[0].track_id);
  assert(expired.metrics().tracks_expired == 1U);
}

void test_invalid_control_filter_config_is_rejected() {
  MobileTargetTrackerConfig invalid_alpha{};
  invalid_alpha.control_smoothing_alpha = 0.0F;
  MobileTargetTracker tracker;
  assert(!tracker.configure(invalid_alpha));

  MobileTargetTrackerConfig invalid_bypass{};
  invalid_bypass.control_smoothing_bypass_delta_pixels = 0.0F;
  assert(!tracker.configure(invalid_bypass));

  MobileTargetTrackerConfig invalid_lost_duration{};
  invalid_lost_duration.lost_track_duration_us = 500'001U;
  assert(!tracker.configure(invalid_lost_duration));
}

void test_non_monotonic_sequence_has_no_identity_output() {
  MobileTargetTracker tracker;
  const std::array target{box(160.0F, 120.0F, 20.0F, 20.0F, 0.90F, 1U)};
  const auto initial = tracker.update(target, 2);
  const auto rejected = tracker.update(target, 2);
  const auto next = tracker.update(target, 3);
  assert(initial[0].track_id != 0);
  assert(rejected[0].track_id == 0);
  assert(next[0].track_id == initial[0].track_id);
}

void test_stationary_alternating_jitter_is_damped_for_control_output() {
  MobileTargetTracker tracker;
  const std::array initial{box(100.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U)};
  const auto created = tracker.update(initial, 1);
  const std::uint64_t track_id = created[0].track_id;
  float previous_center_x = tracked_center_x(created[0]);
  float maximum_output_step = 0.0F;

  for (std::uint64_t sequence = 2; sequence <= 18; ++sequence) {
    const float noisy_center_x = sequence % 2 == 0 ? 102.0F : 98.0F;
    const std::array detection{
        box(noisy_center_x, 100.0F, 20.0F, 40.0F, 0.90F, 1U)};
    const auto tracked = tracker.update(detection, sequence);
    assert(tracked[0].track_id == track_id);
    const float output_center_x = tracked_center_x(tracked[0]);
    maximum_output_step = std::max(
        maximum_output_step, std::fabs(output_center_x - previous_center_x));
    previous_center_x = output_center_x;
  }

  // The detector jumps by four model pixels every frame.  That raw oscillation
  // must not be forwarded unchanged into the motion planner.
  assert(maximum_output_step <= 2.5F);
}

void test_stationary_box_size_jitter_does_not_bypass_control_filter() {
  MobileTargetTracker tracker;
  const std::array initial{box(100.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U)};
  const auto created = tracker.update(initial, 1);

  // Detector scale noise can move every edge by more than the bypass
  // threshold even though the aim point is stationary.  That must remain a
  // filtered observation instead of forwarding a raw size jump to control.
  const std::array scale_jitter{
      box(100.0F, 100.0F, 40.0F, 60.0F, 0.90F, 1U)};
  const auto tracked = tracker.update(scale_jitter, 2);

  assert(tracked[0].track_id == created[0].track_id);
  assert(std::fabs(tracked_center_x(tracked[0]) - 100.0F) <= 0.001F);
  assert(tracked_width(tracked[0]) < 40.0F);
  assert(tracker.metrics().control_filter_blends == 1);
  assert(tracker.metrics().control_filter_bypasses == 0);
}

void test_large_motion_and_reversal_are_not_delayed_by_jitter_filtering() {
  MobileTargetTracker tracker;
  const std::array initial{box(100.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U)};
  const auto created = tracker.update(initial, 1);

  const std::array moved{box(120.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U)};
  const auto forward = tracker.update(moved, 2);
  assert(forward[0].track_id == created[0].track_id);
  assert(tracked_center_x(forward[0]) >= 119.5F);

  const std::array reversed{box(80.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U)};
  const auto backward = tracker.update(reversed, 3);
  assert(backward[0].track_id == created[0].track_id);
  assert(tracked_center_x(backward[0]) <= 80.5F);
}

void test_visible_frame_shift_preserves_nearby_target_identities() {
  MobileTargetTracker tracker;
  const std::array initial{
      box(100.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U),
      box(160.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U),
  };
  const auto created = tracker.update(initial, 1);

  // A rightward physical move shifts both targets left. Without shifting the
  // retained tracker state, the greedy association prefers the second target
  // for the first ID and creates the exact ID swap seen in crowded OW2 fights.
  const std::array visible{
      box(68.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U),
      box(128.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U),
  };
  const auto resolved = tracker.resolve_visible_frame_shift(
      visible, 2, -16.0F, 0.0F);
  assert(resolved.measured && resolved.support == 2U);
  assert(std::fabs(resolved.shift_x - (-32.0F)) < 0.001F);
  assert(std::fabs(resolved.shift_y) < 0.001F);
  tracker.apply_visible_frame_shift(resolved.shift_x, resolved.shift_y);
  const auto tracked = tracker.update(visible, 2);

  assert(tracked[0].track_id == created[0].track_id);
  assert(tracked[1].track_id == created[1].track_id);
  assert(std::fabs(tracked_center_x(tracked[0]) - 68.0F) < 0.001F);
  assert(std::fabs(tracked_center_x(tracked[1]) - 128.0F) < 0.001F);
}

void test_single_target_motion_cannot_override_expected_camera_shift() {
  MobileTargetTracker tracker;
  const std::array initial{
      box(100.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U)};
  (void)tracker.update(initial, 1);
  const std::array moved{
      box(130.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U)};

  const auto resolved = tracker.resolve_visible_frame_shift(
      moved, 2, -12.0F, 0.0F);

  assert(!resolved.measured && resolved.support == 0U);
  assert(std::fabs(resolved.shift_x - (-12.0F)) < 0.001F);
  assert(std::fabs(resolved.shift_y) < 0.001F);
}

void test_independent_target_motion_cannot_form_common_camera_shift() {
  MobileTargetTracker tracker;
  const std::array initial{
      box(100.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U),
      box(160.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U),
  };
  (void)tracker.update(initial, 1);
  const std::array diverged{
      box(90.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U),
      box(170.0F, 100.0F, 20.0F, 40.0F, 0.90F, 1U),
  };

  const auto resolved = tracker.resolve_visible_frame_shift(
      diverged, 2, -4.0F, 0.0F);

  assert(!resolved.measured && resolved.support == 0U);
  assert(std::fabs(resolved.shift_x - (-4.0F)) < 0.001F);
  assert(std::fabs(resolved.shift_y) < 0.001F);
}

void test_consistent_motion_prediction_avoids_filter_lag() {
  MobileTargetTracker tracker;
  float observed_center_x = 100.0F;
  const std::array initial{
      box(observed_center_x, 100.0F, 20.0F, 40.0F, 0.90F, 1U)};
  const auto created = tracker.update(initial, 1);
  const std::uint64_t track_id = created[0].track_id;

  for (std::uint64_t sequence = 2; sequence <= 12; ++sequence) {
    observed_center_x += 3.0F;
    const std::array detection{
        box(observed_center_x, 100.0F, 20.0F, 40.0F, 0.90F, 1U)};
    const auto tracked = tracker.update(detection, sequence);
    assert(tracked[0].track_id == track_id);
    if (sequence >= 5) {
      assert(std::fabs(
          tracked_center_x(tracked[0]) - observed_center_x) <= 1.5F);
    }
  }
}

void test_time_based_prediction_survives_variable_wire_sequence_gap() {
  MobileTargetTrackerConfig config{};
  config.maximum_center_distance_pixels = 6.0F;
  config.lost_track_duration_us = 100'000U;
  MobileTargetTracker tracker(config);

  const std::array initial{
      box(100.0F, 100.0F, 4.0F, 20.0F, 0.90F, 1U)};
  const auto created = tracker.update(initial, 1U, 100'000U);
  const std::array warmup{
      box(104.0F, 100.0F, 4.0F, 20.0F, 0.90F, 1U)};
  const auto warmed = tracker.update(warmup, 2U, 110'000U);
  assert(warmed[0].track_id == created[0].track_id);

  // The next real picture arrives 30 ms later, but two repeated wire frames
  // occupied sequence values in between. Prediction must follow elapsed time;
  // a fixed two-sequence cap falls behind and creates a replacement identity.
  const std::array variable_gap{
      box(116.0F, 100.0F, 4.0F, 20.0F, 0.90F, 1U)};
  const auto tracked = tracker.update(variable_gap, 5U, 140'000U);

  assert(tracked[0].track_id == created[0].track_id);
  assert(tracker.metrics().tracks_created == 1U);
}

void test_association_consensus_is_joint_and_bounded() {
  MobileTargetTrackerConfig config; config.lost_track_duration_us=500'000;
  const std::array initial{
      box(208,240,40,100,.9F,0),box(268,240,40,100,.9F,0),box(348,240,40,100,.9F,0)};
  for(unsigned count:{2U,3U}) {
    MobileTargetTracker tracker(config);
    const auto ids=tracker.update(std::span(initial).first(count),1,100'000);
    const std::array shifted{
        box(176,240,40,100,.9F,0),box(236,240,40,100,.9F,0),box(316,240,40,100,.9F,0)};
    const auto flow=tracker.resolve_association_frame_shift(std::span(shifted).first(count),2,108'000);
    assert(flow.measured && flow.support==count && std::abs(flow.shift_x+32)<.01F);
    tracker.apply_visible_frame_shift(flow.shift_x,flow.shift_y);
    const auto next=tracker.update(std::span(shifted).first(count),2,108'000);
    for(unsigned i=0;i<count;++i) assert(next[i].track_id==ids[i].track_id);
  }
  {
    MobileTargetTracker tracker(config); (void)tracker.update(initial,1,100'000);
    const std::array independent{
        box(197,240,40,100,.9F,0),box(280,240,40,100,.9F,0),box(348,260,40,100,.9F,0)};
    assert(!tracker.resolve_association_frame_shift(independent,2,108'000).measured);
    const std::array too_far{
        box(78,240,40,100,.9F,0),box(138,240,40,100,.9F,0),box(218,240,40,100,.9F,0)};
    assert(!tracker.resolve_association_frame_shift(too_far,2,108'000).measured);
  }
  {
    MobileTargetTracker tracker(config); (void)tracker.update(std::span(initial).first(2),1,100'000);
    const std::array weak{box(176,240,40,100,.9F,0),box(236,240,40,100,.01F,0)};
    assert(!tracker.resolve_association_frame_shift(weak,2,108'000).measured);
    assert(!tracker.resolve_association_frame_shift(std::span(weak).first(1),2,108'000).measured);
    const std::array shifted{box(176,240,40,100,.9F,0),box(236,240,40,100,.9F,0)};
    assert(!tracker.resolve_association_frame_shift(shifted,2,150'001).measured);
  }
}
void test_low_fps_consensus_and_image_ownership_keep_their_validity_window() {
  const std::array initial{box(100,240,40,100,.9F,0),box(160,240,40,100,.9F,0)};
  const std::array shifted{box(68,240,40,100,.9F,0),box(128,240,40,100,.9F,0)};
  for(const std::uint64_t interval:{5'000U,16'667U,25'000U,33'333U,40'000U,50'000U}){
    MobileTargetTrackerConfig c;c.lost_track_duration_us=500'000;
    MobileTargetTracker tracker(c);const auto ids=tracker.update(initial,1,100'000);
    const auto common=tracker.resolve_association_frame_shift(shifted,2,100'000+interval);
    assert(common.measured && common.support==2 && std::abs(common.shift_x+32.F)<.01F);
    // Pixel ownership is independently validated by the caller. It remains
    // usable at30FPS, while geometry/class/confidence checks still apply.
    const std::array next{box(140,240,40,100,.9F,0),box(120,240,40,100,.9F,0)};
    const std::array evidence{
        vfdual_android::TrackImageConstraint{0,ids[1].track_id,100'000},
        vfdual_android::TrackImageConstraint{1,ids[0].track_id,100'000}};
    std::vector<TrackedDetection> tracked;
    tracker.update_into(next,2,100'000+interval,tracked,evidence);
    assert(tracker.metrics().image_constraints_applied==2);
    assert(tracked[0].track_id==ids[1].track_id && tracked[1].track_id==ids[0].track_id);
  }
  MobileTargetTrackerConfig c;c.lost_track_duration_us=500'000;
  MobileTargetTracker stale(c);const auto ids=stale.update(initial,1,100'000);
  const std::array evidence{vfdual_android::TrackImageConstraint{0,ids[0].track_id,100'000}};
  std::vector<TrackedDetection> tracked;stale.update_into(initial,2,150'001,tracked,evidence);
  assert(stale.metrics().image_constraints_applied==0 && stale.metrics().image_constraints_rejected==1);
}
void test_repeated_crowd_geometry_cannot_authorize_ambiguous_translation() {
  MobileTargetTrackerConfig config; config.lost_track_duration_us=500'000;
  MobileTargetTracker tracker(config);
  const std::array initial{box(100,240,40,100,.9F,0),box(160,240,40,100,.9F,0),box(220,240,40,100,.9F,0)};
  (void)tracker.update(initial,1,100'000);
  // With one member gone, both -32 and +28 exactly match two people.
  const std::array ambiguous{box(128,240,40,100,.9F,0),box(188,240,40,100,.9F,0)};
  assert(!tracker.resolve_association_frame_shift(ambiguous,2,108'000).measured);
  assert(tracker.metrics().association_shift_frames==0);
}
void test_disappearance_plus_independent_motion_cannot_shift_entire_group() {
  MobileTargetTrackerConfig config;
  config.lost_track_duration_us = 500'000;
  MobileTargetTracker tracker(config);
  const std::array initial{
      box(217,225,32,48,.83F,0), box(253,225,35,47,.54F,0), box(319,223,35,53,.48F,0)};
  (void)tracker.update(initial,1,100'000);
  // The left person disappeared; the other two move toward each other. Their
  // new spacing resembles the old left pair, but a +51 px group pan is false.
  const std::array remaining{
      box(269,241,35,47,.54F,0), box(303,239,35,53,.48F,0)};
  assert(!tracker.resolve_association_frame_shift(remaining,2,108'000).measured);
}
void test_incompatible_neighbour_cannot_block_visible_owner() {
  MobileTargetTrackerConfig config;
  config.lost_track_duration_us = 500'000;
  MobileTargetTracker tracker(config);
  const std::array before{
      box(100,200,20,40,.9F,0), box(110,200,80,160,.9F,0)};
  const auto initial = tracker.update(before,1,100'000);
  // The larger, lower-confidence neighbour is closest to the old small box,
  // but cannot be its owner under the tracker's own size-ratio contract.
  const std::array after{
      box(105,200,20,40,.9F,0), box(100,200,80,160,.3F,0)};
  const auto next = tracker.update(after,2,108'000);
  assert(next[0].track_id == initial[0].track_id);
  assert(next[1].track_id == initial[1].track_id);
}
void test_stale_or_unrelated_owner_cannot_bypass_scale_jump_guard() {
  for (unsigned scenario = 0; scenario < 4; ++scenario) {
    MobileTargetTrackerConfig config;
    config.lost_track_duration_us = 500'000;
    MobileTargetTracker tracker(config);
    const std::array before{
        box(100,200,20,40,.9F,0),
        box(scenario==2 ? 300 : 110,200,80,160,.9F,scenario==3 ? 1 : 0)};
    const auto initial = tracker.update(before,1,100'000);
    if (scenario==0) {
      const std::array only_original{before[0]};
      (void)tracker.update(only_original,2,108'000);
    }
    const std::array after{
        box(130,200,20,40,.99F,0),box(102,200,60,120,.9F,0)};
    const auto next = tracker.update(after,3,scenario==1 ? 250'000 : 116'000);
    assert(next[0].track_id != initial[0].track_id);
    assert(next[1].track_id != initial[0].track_id);
  }
}
void test_missing_identity_cannot_take_over_a_continuously_visible_neighbour() {
  for (const float confidence : {.9F, .3F}) {
    MobileTargetTrackerConfig config;
    config.lost_track_duration_us = 500'000;
    MobileTargetTracker tracker(config);
    const std::array before{
        box(208,240,40,100,.9F,0),box(260,240,40,100,.9F,0)};
    const auto initial = tracker.update(before,1,100'000);
    // The first person disappears. The second remains continuously observed
    // while moving through the old position. An unavailable scenery estimate
    // clears prediction in the production core; it must not clear ownership.
    for (unsigned i=1; i<=3; ++i) {
      tracker.clear_motion_prediction();
      const std::array remaining{box(260-16*float(i),240,40,100,confidence,0)};
      const auto next = tracker.update(remaining,i+1,100'000+i*8'000);
      assert(next[0].track_id == initial[1].track_id);
      assert(next[0].track_id != initial[0].track_id);
    }
    assert(tracker.metrics().lost_track_claims_blocked > 0);
    // A separate observation away from the visible person must not disturb
    // that person's ongoing identity. Lost-identity recovery is tested below.
    const std::array separated{
        box(208,150,40,100,.9F,0),box(196,240,40,100,confidence,0)};
    const auto next = tracker.update(separated,5,132'000);
    assert(next[1].track_id == initial[1].track_id);
  }
}
void test_missing_identity_can_return_without_a_visible_claimant() {
  MobileTargetTrackerConfig config;
  config.lost_track_duration_us = 500'000;
  MobileTargetTracker tracker(config);
  const std::array before{
      box(100,200,40,100,.9F,0),box(300,200,40,100,.9F,0)};
  const auto initial = tracker.update(before,1,100'000);
  const std::array remaining{before[1]};
  (void)tracker.update(remaining,2,108'000);
  const auto returned = tracker.update(before,3,116'000);
  assert(returned[0].track_id == initial[0].track_id);
  assert(returned[1].track_id == initial[1].track_id);
}
}  // namespace

int main() {
  test_dense_observation_burst_has_bounded_assignment_workspace();
  test_retention_does_not_extend_motion_prediction_for_half_a_second();
  test_unconfirmed_extent_cannot_redirect_next_identity();
  test_joint_assignment_preserves_two_visible_people();
  test_size_boundary_is_invariant_to_coordinate_rounding();
  test_ids_follow_targets_when_input_order_changes();
  test_three_crossing_targets_keep_identity_when_order_changes();
  test_low_confidence_owner_blocks_high_confidence_neighbour_id_theft();
  test_scale_jump_cannot_transfer_identity_to_neighbour();
  test_low_confidence_second_stage_continues_but_does_not_create();
  test_association_is_class_isolated();
  test_lost_buffer_expiry_assigns_a_new_identity();
  test_lost_duration_is_stable_across_fresh_frame_rates();
  test_invalid_control_filter_config_is_rejected();
  test_non_monotonic_sequence_has_no_identity_output();
  test_stationary_alternating_jitter_is_damped_for_control_output();
  test_stationary_box_size_jitter_does_not_bypass_control_filter();
  test_large_motion_and_reversal_are_not_delayed_by_jitter_filtering();
  test_visible_frame_shift_preserves_nearby_target_identities();
  test_single_target_motion_cannot_override_expected_camera_shift();
  test_independent_target_motion_cannot_form_common_camera_shift();
  test_consistent_motion_prediction_avoids_filter_lag();
  test_time_based_prediction_survives_variable_wire_sequence_gap();
  test_association_consensus_is_joint_and_bounded();
  test_low_fps_consensus_and_image_ownership_keep_their_validity_window();
  test_repeated_crowd_geometry_cannot_authorize_ambiguous_translation();
  test_disappearance_plus_independent_motion_cannot_shift_entire_group();
  test_incompatible_neighbour_cannot_block_visible_owner();
  test_stale_or_unrelated_owner_cannot_bypass_scale_jump_guard();
  test_missing_identity_cannot_take_over_a_continuously_visible_neighbour();
  test_missing_identity_can_return_without_a_visible_claimant();
  return 0;
}
