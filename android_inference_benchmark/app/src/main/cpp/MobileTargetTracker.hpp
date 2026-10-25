#pragma once

#include "YoloPostprocessor.hpp"
#include "MobileControlDeadline.hpp"

#include <cstdint>
#include <array>
#include <limits>
#include <span>
#include <vector>

namespace vfdual_android {

inline constexpr std::uint64_t kMobileTargetTrackerMaximumLostDurationUs =
    500'000U;
// Adjacent, independently measured evidence may span a normal30FPS frame.
// This does not authorize a longer extrapolation of an unseen retained track.
inline constexpr std::uint64_t kMobileTargetTrackerMaximumEvidenceIntervalUs =
    kContinuousControlMaximumObservationAgeUs;
inline constexpr std::uint64_t kMobileTargetTrackerPredictionHorizonUs = 30'000U;
inline constexpr std::uint32_t kMobileVisibleFrameShiftAnyClass =
    std::numeric_limits<std::uint32_t>::max();

struct MobileTargetTrackerConfig {
  float high_confidence_threshold{0.50F};
  float low_confidence_threshold{0.25F};
  float new_track_confidence_threshold{0.50F};
  float first_stage_iou_threshold{0.20F};
  float second_stage_iou_threshold{0.10F};
  float maximum_center_distance_pixels{52.0F};
  float maximum_size_ratio{2.50F};
  // Smooth only the control-facing geometry. Association continues to use
  // the raw observation so filtering cannot make the tracker lose fast motion.
  float control_smoothing_alpha{0.45F};
  float control_smoothing_bypass_delta_pixels{8.0F};
  bool confirm_abrupt_extent{};
  std::uint32_t lost_frame_buffer{6};
  // Zero preserves count-based standalone use. Production profiles pass
  // monotonic observation time so retention is independent of fresh FPS.
  std::uint64_t lost_track_duration_us{};
};

struct TrackedDetection {
  YoloDetection detection{};
  std::uint64_t track_id{};
  // Position remains usable; the step to/from provisional geometry is not
  // a measured target velocity and must not enter derivative histories.
  bool motion_discontinuity{};
};

struct MobileTargetTrackerMetrics {
  std::uint64_t tracks_created{};
  std::uint64_t first_stage_matches{};
  std::uint64_t second_stage_matches{};
  std::uint64_t center_distance_matches{};
  std::uint64_t ambiguous_center_matches_rejected{};
  std::uint64_t tracks_expired{};
  std::uint64_t control_filter_blends{};
  std::uint64_t control_extent_confirmations{};
  std::uint64_t control_filter_bypasses{};
  std::uint64_t time_based_velocity_updates{};
  std::uint64_t maximum_observation_interval_us{};
  std::uint64_t association_shift_frames{};
  std::uint64_t association_shift_rejections{};
  std::uint64_t known_owner_conflicts_resolved{};
  std::uint64_t lost_track_claims_blocked{};
  std::uint64_t global_assignment_calls{};
  std::uint64_t global_assignment_matches{};
  std::uint64_t global_assignment_size_fallbacks{};
  std::uint64_t joint_assignment_acceptances{};
  std::uint64_t joint_assignment_rejections{};
  std::uint64_t mixed_confidence_joint_frames{};
  std::uint64_t mixed_confidence_joint_rejections{};
  std::uint64_t image_constraints_applied{};
  std::uint64_t image_constraints_rejected{};
  std::uint64_t image_disallowed_pairs_skipped{};
  float maximum_control_innovation_pixels{};
};

struct MobileVisibleFrameShift {
  float shift_x{};
  float shift_y{};
  std::uint32_t support{};
  bool measured{};
  float raw_shift_x{};
  float raw_shift_y{};
};

// Caller-validated adjacent-frame pixel evidence only excludes contradictory
// identity pairs. Ordinary geometry/confidence gates still apply.
struct TrackImageConstraint {
  std::size_t detection_index{};
  std::uint64_t previous_track_id{};
  std::uint64_t previous_observed_at_us{};
};

/**
 * Small class-isolated two-stage tracker for the Android control hot path.
 * It has no JNI, USB, network, clock or allocation ownership outside this object.
 */
class MobileTargetTracker final {
 public:
  MobileTargetTracker() = default;
  explicit MobileTargetTracker(const MobileTargetTrackerConfig& config);

  [[nodiscard]] bool configure(const MobileTargetTrackerConfig& config);
  [[nodiscard]] std::vector<TrackedDetection> update(
      std::span<const YoloDetection> detections, std::uint64_t sequence,
      std::uint64_t observed_at_us = 0U);
  /** Reuses caller-owned result storage for the per-frame control hot path. */
  void update_into(
      std::span<const YoloDetection> detections, std::uint64_t sequence,
      std::uint64_t observed_at_us,
      std::vector<TrackedDetection>& result,
      std::span<const TrackImageConstraint> image_constraints = {});
  [[nodiscard]] const MobileTargetTrackerMetrics& metrics() const noexcept { return metrics_; }
  /** Resolves a common camera translation from independent same-class witnesses. */
  [[nodiscard]] MobileVisibleFrameShift resolve_visible_frame_shift(
      std::span<const YoloDetection> detections, std::uint64_t sequence,
      float expected_shift_x, float expected_shift_y,
      std::uint64_t observed_at_us = 0U,
      std::uint32_t minimum_support = 2U,
      std::uint32_t witness_class_id = kMobileVisibleFrameShiftAnyClass);
  /** Joint correspondence fallback, never evidence of camera/HID response. */
  [[nodiscard]] MobileVisibleFrameShift resolve_association_frame_shift(
      std::span<const YoloDetection> detections, std::uint64_t sequence,
      std::uint64_t observed_at_us);
  /** Aligns every retained track with a completed camera move visible on screen. */
  void apply_visible_frame_shift(float shift_x, float shift_y) noexcept;
  void clear_motion_prediction() noexcept;
  void reset() noexcept;

 private:
  struct Track {
    std::uint64_t id{};
    // Raw detector geometry is the association and velocity authority.
    YoloDetection detection{};
    // Filtered geometry is exposed to target selection and motion planning.
    YoloDetection control_detection{};
    // Do not use a one-observation extra edge as the next identity anchor.
    bool extent_provisional{};
    bool motion_discontinuity{};
    // Pixels/second when time-based retention is enabled; otherwise the
    // standalone compatibility path keeps pixels/sequence semantics.
    float velocity_x{};
    float velocity_y{};
    std::uint64_t last_sequence{};
    std::uint64_t last_observed_at_us{};
    std::uint32_t lost_frames{};
    std::size_t image_detection_index{std::numeric_limits<std::size_t>::max()};
  };

  struct Match {
    std::size_t track_index{};
    std::size_t detection_index{};
    float score{};
    bool center_distance_only{};
  };

  struct VisibleShiftFit {
    float shift_x{};
    float shift_y{};
    float residual{};
    float prior_error{};
    std::uint32_t support{};
    float span_x{}, span_y{};
    float raw_shift_x{};
    float raw_shift_y{};
  };

  [[nodiscard]] static bool valid_config(const MobileTargetTrackerConfig& config) noexcept;
  void prepare_image_constraints(std::span<const TrackImageConstraint> constraints,
      std::span<const YoloDetection> detections,std::uint64_t observed_at_us) noexcept;
  [[nodiscard]] bool image_pair_allowed(std::size_t detection_index,const Track& track) const noexcept;
  [[nodiscard]] YoloDetection predicted_box(
      const Track& track, std::uint64_t sequence,
      std::uint64_t observed_at_us) const noexcept;
  [[nodiscard]] std::span<const Match> associate(
      std::span<const std::size_t> track_indices,
      std::span<const std::size_t> detection_indices,
      std::span<const YoloDetection> detections,
      float iou_threshold, std::uint64_t observed_at_us);
  [[nodiscard]] bool has_closer_same_class_observation(
      const Track& track, const YoloDetection& reference,
      std::size_t candidate_index,
      std::span<const YoloDetection> detections,
      std::span<const std::size_t> stage_detection_indices,
      float candidate_distance_squared) noexcept;
  void append_global_matches(
      bool visible, std::span<const std::size_t> track_indices,
      std::span<const std::size_t> detection_indices,
      std::size_t detection_count);
  [[nodiscard]] VisibleShiftFit fit_visible_frame_shift(
      std::span<const YoloDetection> detections, std::uint64_t sequence,
      float candidate_shift_x, float candidate_shift_y,
      float expected_shift_x, float expected_shift_y,
      std::uint64_t observed_at_us,
      std::span<std::uint8_t> used_detections,
      std::uint32_t witness_class_id, float minimum_confidence = 0.0F) const;
  [[nodiscard]] static bool better_visible_shift_fit(
      const VisibleShiftFit& candidate,
      const VisibleShiftFit& current) noexcept;
  void update_control_detection(
      Track& track, const YoloDetection& detection) noexcept;
  void update_track(
      Track& track, const YoloDetection& detection,
      std::uint64_t sequence, std::uint64_t observed_at_us) noexcept;
  [[nodiscard]] bool match_complete_mixed_confidence_group(
      std::span<const YoloDetection> detections, std::uint64_t sequence,
      std::uint64_t observed_at_us, std::vector<TrackedDetection>& result);

  MobileTargetTrackerConfig config_{};
  MobileTargetTrackerMetrics metrics_{};
  std::vector<Track> tracks_{};
  std::vector<std::size_t> high_detection_indices_{};
  std::vector<std::size_t> low_detection_indices_{};
  std::vector<std::size_t> track_indices_{};
  std::vector<std::size_t> unmatched_track_indices_{};
  std::vector<std::size_t> visible_track_indices_{}, joint_detection_indices_{};
  std::vector<std::uint8_t> matched_tracks_{};
  std::vector<std::uint8_t> matched_high_detections_{};
  std::vector<std::uint8_t> association_used_tracks_{};
  std::vector<std::uint8_t> association_used_detections_{};
  std::vector<std::uint8_t> visible_used_detections_{};
  std::vector<Match> possible_matches_{};
  std::vector<Match> matches_{};
  // Rectangular assignment: detections are rows, tracks plus unmatched slots
  // are columns. Reuse all workspace after warm-up, including dummy columns.
  std::vector<std::size_t> assignment_tracks_{}, assignment_detections_{};
  std::vector<std::size_t> assignment_track_columns_{}, assignment_detection_rows_{};
  std::vector<double> assignment_costs_{}, assignment_u_{}, assignment_v_{}, assignment_min_{};
  std::vector<std::size_t> assignment_p_{}, assignment_way_{};
  std::vector<std::uint8_t> assignment_used_{};
  std::uint64_t next_track_id_{1};
  std::uint64_t last_sequence_{};
  std::uint64_t last_observed_at_us_{};
  std::size_t last_eligible_detection_count_{};
  std::size_t association_visible_count_{};
  std::array<std::uint64_t,16> image_owners_{};
};

}  // namespace vfdual_android
