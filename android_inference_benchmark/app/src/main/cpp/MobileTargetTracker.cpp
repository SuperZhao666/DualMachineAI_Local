#include "MobileTargetTracker.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vfdual_android {
namespace {

float width(const YoloDetection& detection) noexcept {
  return detection.x2 - detection.x1;
}

float height(const YoloDetection& detection) noexcept {
  return detection.y2 - detection.y1;
}

float center_x(const YoloDetection& detection) noexcept {
  return (detection.x1 + detection.x2) * 0.5F;
}

float center_y(const YoloDetection& detection) noexcept {
  return (detection.y1 + detection.y2) * 0.5F;
}

float intersection_over_union(const YoloDetection& left, const YoloDetection& right) noexcept {
  const float x1 = std::max(left.x1, right.x1);
  const float y1 = std::max(left.y1, right.y1);
  const float x2 = std::min(left.x2, right.x2);
  const float y2 = std::min(left.y2, right.y2);
  const float intersection = std::max(0.0F, x2 - x1) * std::max(0.0F, y2 - y1);
  const float left_area = width(left) * height(left);
  const float right_area = width(right) * height(right);
  const float union_area = left_area + right_area - intersection;
  return union_area > 0.0F ? intersection / union_area : 0.0F;
}

float size_ratio(const YoloDetection& left, const YoloDetection& right) noexcept {
  const float width_ratio = std::max(width(left), width(right)) /
      std::max(std::min(width(left), width(right)), 1.0e-6F);
  const float height_ratio = std::max(height(left), height(right)) /
      std::max(std::min(height(left), height(right)), 1.0e-6F);
  return std::max(width_ratio, height_ratio);
}

bool size_compatible(const YoloDetection& left, const YoloDetection& right,
                     float limit) noexcept {
  // Detector coordinates are floats. Subtracting two edges and then dividing
  // can move an exact boundary (e.g. 96 / 38.4) a few ULPs above the limit.
  // Absorb only arithmetic round-off, not a meaningful change in body size.
  constexpr float rounding_slack = 16.0F * std::numeric_limits<float>::epsilon();
  return size_ratio(left, right) <= limit * (1.0F + rounding_slack);
}

}  // namespace

MobileTargetTracker::MobileTargetTracker(const MobileTargetTrackerConfig& config) {
  (void)configure(config);
}

bool MobileTargetTracker::valid_config(const MobileTargetTrackerConfig& config) noexcept {
  const float values[] = {
      config.high_confidence_threshold,
      config.low_confidence_threshold,
      config.new_track_confidence_threshold,
      config.first_stage_iou_threshold,
      config.second_stage_iou_threshold,
      config.maximum_center_distance_pixels,
      config.maximum_size_ratio,
      config.control_smoothing_alpha,
      config.control_smoothing_bypass_delta_pixels,
  };
  if (!std::all_of(std::begin(values), std::end(values), [](float value) {
        return std::isfinite(value);
      })) {
    return false;
  }
  return config.low_confidence_threshold >= 0.0F &&
      config.low_confidence_threshold <= config.high_confidence_threshold &&
      config.high_confidence_threshold <= 1.0F &&
      config.new_track_confidence_threshold >= config.high_confidence_threshold &&
      config.new_track_confidence_threshold <= 1.0F &&
      config.first_stage_iou_threshold >= 0.0F && config.first_stage_iou_threshold <= 1.0F &&
      config.second_stage_iou_threshold >= 0.0F &&
      config.second_stage_iou_threshold <= config.first_stage_iou_threshold &&
      config.maximum_center_distance_pixels > 0.0F &&
      config.maximum_size_ratio >= 1.0F &&
      config.control_smoothing_alpha > 0.0F &&
      config.control_smoothing_alpha <= 1.0F &&
      config.control_smoothing_bypass_delta_pixels > 0.0F &&
      config.lost_track_duration_us <=
          kMobileTargetTrackerMaximumLostDurationUs;
}

bool MobileTargetTracker::configure(const MobileTargetTrackerConfig& config) {
  if (!valid_config(config)) return false;
  config_ = config;
  reset();
  return true;
}

MobileVisibleFrameShift MobileTargetTracker::resolve_visible_frame_shift(
    std::span<const YoloDetection> detections, std::uint64_t sequence,
    float expected_shift_x, float expected_shift_y,
    std::uint64_t observed_at_us, std::uint32_t minimum_support,
    std::uint32_t witness_class_id) {
  MobileVisibleFrameShift result{
      .shift_x = expected_shift_x,
      .shift_y = expected_shift_y,
  };
  if (!std::isfinite(expected_shift_x) || !std::isfinite(expected_shift_y) ||
      sequence == 0U || minimum_support == 0U ||
      tracks_.size() < minimum_support ||
      detections.size() < minimum_support) {
    return result;
  }

  VisibleShiftFit best_fit{
      .residual = std::numeric_limits<float>::max(),
      .prior_error = std::numeric_limits<float>::max(),
  };
  visible_used_detections_.assign(detections.size(), 0U);

  for (const Track& seed_track : tracks_) {
    if (seed_track.lost_frames != 0U ||
        (witness_class_id != kMobileVisibleFrameShiftAnyClass &&
         seed_track.detection.class_id != witness_class_id)) {
      continue;
    }
    const YoloDetection seed_reference = predicted_box(
        seed_track, sequence, observed_at_us);
    for (const YoloDetection& seed_detection : detections) {
      if ((witness_class_id != kMobileVisibleFrameShiftAnyClass &&
           seed_detection.class_id != witness_class_id) ||
          seed_track.detection.class_id != seed_detection.class_id ||
          !size_compatible(seed_reference, seed_detection, config_.maximum_size_ratio)) {
        continue;
      }
      const float candidate_shift_x =
          center_x(seed_detection) - center_x(seed_reference);
      const float candidate_shift_y =
          center_y(seed_detection) - center_y(seed_reference);
      const VisibleShiftFit fit = fit_visible_frame_shift(
          detections, sequence, candidate_shift_x, candidate_shift_y,
          expected_shift_x, expected_shift_y, observed_at_us,
          visible_used_detections_, witness_class_id);
      if (fit.support >= minimum_support &&
          better_visible_shift_fit(fit, best_fit)) best_fit = fit;
    }
  }
  if (best_fit.support >= minimum_support) {
    result = {
        .shift_x = best_fit.shift_x,
        .shift_y = best_fit.shift_y,
        .support = best_fit.support,
        .measured = true,
        .raw_shift_x = best_fit.raw_shift_x,
        .raw_shift_y = best_fit.raw_shift_y,
    };
  }
  return result;
}

MobileTargetTracker::VisibleShiftFit MobileTargetTracker::fit_visible_frame_shift(
    std::span<const YoloDetection> detections, std::uint64_t sequence,
    float candidate_shift_x, float candidate_shift_y,
    float expected_shift_x, float expected_shift_y,
    std::uint64_t observed_at_us,
    std::span<std::uint8_t> used_detections,
    std::uint32_t witness_class_id, float minimum_confidence) const {
  const float tolerance_squared =
      config_.control_smoothing_bypass_delta_pixels *
      config_.control_smoothing_bypass_delta_pixels;
  VisibleShiftFit fit{};
  float minimum_x = std::numeric_limits<float>::max(), minimum_y = minimum_x;
  float maximum_x = -minimum_x, maximum_y = -minimum_x;
  std::fill(used_detections.begin(), used_detections.end(), 0U);
  for (const Track& track : tracks_) {
    if (track.lost_frames != 0U ||
        (witness_class_id != kMobileVisibleFrameShiftAnyClass &&
         track.detection.class_id != witness_class_id)) {
      continue;
    }
    const YoloDetection reference = predicted_box(
        track, sequence, observed_at_us);
    std::size_t best_index = detections.size();
    float best_residual = tolerance_squared;
    float best_shift_x{};
    float best_shift_y{};
    for (std::size_t index = 0; index < detections.size(); ++index) {
      const YoloDetection& detection = detections[index];
      if (used_detections[index] || detection.confidence < minimum_confidence ||
          (witness_class_id != kMobileVisibleFrameShiftAnyClass &&
           detection.class_id != witness_class_id) ||
          track.detection.class_id != detection.class_id ||
          !size_compatible(reference, detection, config_.maximum_size_ratio)) continue;
      const float shift_x = center_x(detection) - center_x(reference);
      const float shift_y = center_y(detection) - center_y(reference);
      const float residual_x = shift_x - candidate_shift_x;
      const float residual_y = shift_y - candidate_shift_y;
      const float residual = residual_x * residual_x + residual_y * residual_y;
      if (residual <= best_residual) {
        best_index = index;
        best_residual = residual;
        best_shift_x = shift_x;
        best_shift_y = shift_y;
      }
    }
    if (best_index == detections.size()) continue;
    used_detections[best_index] = 1U;
    ++fit.support;
    minimum_x = std::min(minimum_x, center_x(reference));
    maximum_x = std::max(maximum_x, center_x(reference));
    minimum_y = std::min(minimum_y, center_y(reference));
    maximum_y = std::max(maximum_y, center_y(reference));
    fit.residual += best_residual;
    fit.shift_x += best_shift_x;
    fit.shift_y += best_shift_y;
    fit.raw_shift_x += center_x(detections[best_index]) - center_x(track.detection);
    fit.raw_shift_y += center_y(detections[best_index]) - center_y(track.detection);
  }
  if (fit.support == 0U) return fit;
  fit.shift_x /= static_cast<float>(fit.support);
  fit.shift_y /= static_cast<float>(fit.support);
  fit.raw_shift_x /= static_cast<float>(fit.support);
  fit.raw_shift_y /= static_cast<float>(fit.support);
  fit.span_x = maximum_x - minimum_x;
  fit.span_y = maximum_y - minimum_y;
  const float prior_x = fit.shift_x - expected_shift_x;
  const float prior_y = fit.shift_y - expected_shift_y;
  fit.prior_error = prior_x * prior_x + prior_y * prior_y;
  return fit;
}

MobileVisibleFrameShift MobileTargetTracker::resolve_association_frame_shift(
    std::span<const YoloDetection> detections, std::uint64_t sequence,
    std::uint64_t observed_at_us) {
  // Independent nearest-box matching can swap identities when camera motion
  // exceeds half the separation between two people. Use the group's spatial
  // configuration only when it has a coherent and unambiguous correspondence.
  // It may include shared object motion, so callers must not treat it as
  // measured background motion or use it to train actuator calibration.
  if (sequence <= last_sequence_ || !observed_at_us ||
      detections.size() < 2 || detections.size() > 16 ||
      !last_observed_at_us_ || observed_at_us <= last_observed_at_us_ ||
      observed_at_us - last_observed_at_us_ > kMobileTargetTrackerMaximumEvidenceIntervalUs) return {};
  const auto active = std::count_if(tracks_.begin(), tracks_.end(), [](const Track& t) {
    return t.lost_frames == 0;
  });
  const auto eligible = std::count_if(detections.begin(), detections.end(), [&](const YoloDetection& d) {
    return d.confidence >= config_.low_confidence_threshold;
  });
  // Without background evidence, a disappearing person and a moving neighbour
  // can imitate a translation of a subset. Require the whole visible group;
  // partial coverage is not sufficient evidence to reassign retained geometry.
  if (active < 2 || active > 16) return {};
  if (eligible != active || std::size_t(active) != last_eligible_detection_count_) {
    ++metrics_.association_shift_rejections;
    return {};
  }
  visible_used_detections_.assign(detections.size(), 0U);
  VisibleShiftFit best{};
  for (const auto& track : tracks_) {
    if (track.lost_frames) continue;
    const auto reference = predicted_box(track, sequence, observed_at_us);
    for (const auto& detection : detections) {
      if (detection.confidence < config_.low_confidence_threshold ||
          detection.class_id != track.detection.class_id ||
          !size_compatible(reference, detection, config_.maximum_size_ratio)) continue;
      const float dx = center_x(detection) - center_x(reference);
      const float dy = center_y(detection) - center_y(reference);
      if (std::hypot(dx, dy) > 96.0F) continue;
      const auto fit = fit_visible_frame_shift(
          detections, sequence, dx, dy, 0, 0, observed_at_us,
          visible_used_detections_, kMobileVisibleFrameShiftAnyClass,
          config_.low_confidence_threshold);
      if (fit.support != std::size_t(active) ||
          fit.residual > static_cast<float>(fit.support) * 16.0F ||
          std::max(fit.span_x, fit.span_y) < 32.0F) continue;
      if (better_visible_shift_fit(fit, best)) best = fit;
    }
  }
  if (best.support < 2) {
    ++metrics_.association_shift_rejections;
    return {};
  }
  if (std::hypot(best.shift_x, best.shift_y) < 4.0F) return {};
  ++metrics_.association_shift_frames;
  return {.shift_x = best.shift_x, .shift_y = best.shift_y, .support = best.support,
          .measured = true, .raw_shift_x = best.raw_shift_x, .raw_shift_y = best.raw_shift_y};
}

bool MobileTargetTracker::better_visible_shift_fit(
    const VisibleShiftFit& candidate,
    const VisibleShiftFit& current) noexcept {
  return candidate.support > current.support ||
      (candidate.support == current.support &&
       candidate.residual < current.residual) ||
      (candidate.support == current.support &&
       candidate.residual == current.residual &&
       candidate.prior_error < current.prior_error);
}

void MobileTargetTracker::apply_visible_frame_shift(
    float shift_x, float shift_y) noexcept {
  if (!std::isfinite(shift_x) || !std::isfinite(shift_y) ||
      (shift_x == 0.0F && shift_y == 0.0F)) {
    return;
  }
  const auto shift_detection = [shift_x, shift_y](YoloDetection& detection) {
    detection.x1 += shift_x;
    detection.x2 += shift_x;
    detection.y1 += shift_y;
    detection.y2 += shift_y;
  };
  for (Track& track : tracks_) {
    shift_detection(track.detection);
    shift_detection(track.control_detection);
  }
}

void MobileTargetTracker::clear_motion_prediction() noexcept {
  for (auto& track : tracks_) track.velocity_x = track.velocity_y = 0.0F;
}

YoloDetection MobileTargetTracker::predicted_box(
    const Track& track, std::uint64_t sequence,
    std::uint64_t observed_at_us) const noexcept {
  float prediction_interval{};
  if (config_.lost_track_duration_us != 0U &&
      observed_at_us > track.last_observed_at_us) {
    const std::uint64_t elapsed_us = std::min(
        observed_at_us - track.last_observed_at_us,
        std::min(config_.lost_track_duration_us, kMobileTargetTrackerPredictionHorizonUs));
    prediction_interval = static_cast<float>(elapsed_us) / 1'000'000.0F;
  } else if (config_.lost_track_duration_us == 0U) {
    const std::uint64_t gap = sequence > track.last_sequence
        ? sequence - track.last_sequence : 0U;
    prediction_interval = static_cast<float>(
        std::min<std::uint64_t>(gap, 2U));
  }
  const float shift_x = track.velocity_x * prediction_interval;
  const float shift_y = track.velocity_y * prediction_interval;
  YoloDetection predicted = track.extent_provisional
      ? track.control_detection : track.detection;
  predicted.x1 += shift_x;
  predicted.x2 += shift_x;
  predicted.y1 += shift_y;
  predicted.y2 += shift_y;
  return predicted;
}

std::span<const MobileTargetTracker::Match> MobileTargetTracker::associate(
    std::span<const std::size_t> track_indices,
    std::span<const std::size_t> detection_indices,
    std::span<const YoloDetection> detections,
    float iou_threshold, std::uint64_t observed_at_us) {
  // Detector NMS can emit up to 300 candidates. Dense false-positive bursts
  // must not turn cubic assignment or its matrix into the frame-time bottleneck.
  // Beyond this bounded workspace, retain the original strict greedy policy.
  const bool use_global_assignment = detection_indices.size() <= 32U &&
      track_indices.size() <= 64U;
  association_visible_count_ = static_cast<std::size_t>(std::count_if(
      tracks_.begin(), tracks_.end(), [](const Track& track) {
        return track.lost_frames == 0U;
      }));
  possible_matches_.clear();
  const std::size_t maximum_possible_matches =
      track_indices.size() * detection_indices.size();
  if (possible_matches_.capacity() < maximum_possible_matches) {
    possible_matches_.reserve(maximum_possible_matches);
  }
  const float maximum_distance_squared =
      config_.maximum_center_distance_pixels * config_.maximum_center_distance_pixels;
  for (const std::size_t track_index : track_indices) {
    const Track& track = tracks_[track_index];
    const YoloDetection reference = predicted_box(
        track, last_sequence_, observed_at_us);
    for (const std::size_t detection_index : detection_indices) {
      const YoloDetection& detection = detections[detection_index];
      if (track.detection.class_id != detection.class_id) continue;
      if (!image_pair_allowed(detection_index,track)) {
        ++metrics_.image_disallowed_pairs_skipped;
        continue;
      }
      if (!size_compatible(reference, detection, config_.maximum_size_ratio)) continue;
      const float delta_x = center_x(reference) - center_x(detection);
      const float delta_y = center_y(reference) - center_y(detection);
      const float distance_squared = delta_x * delta_x + delta_y * delta_y;
      const float iou = intersection_over_union(reference, detection);
      const bool center_match = distance_squared <= maximum_distance_squared;
      if (iou < iou_threshold && !center_match) continue;
      // Overlap does not establish ownership. A high-confidence neighbour
      // must not steal an ID before its closer low-confidence observation gets
      // the second association pass, even when the two bodies overlap.
      if (has_closer_same_class_observation(
              track, reference, detection_index, detections,
              use_global_assignment ? detection_indices : std::span<const std::size_t>{},
              distance_squared)) {
        ++metrics_.ambiguous_center_matches_rejected;
        continue;
      }
      const float normalized_distance =
          std::sqrt(distance_squared / std::max(maximum_distance_squared, 1.0F));
      const float score = iou * 2.0F + std::max(0.0F, 1.0F - normalized_distance) * 0.5F;
      possible_matches_.push_back({
          track_index, detection_index, score, iou < iou_threshold});
    }
  }
  matches_.clear();
  association_used_tracks_.assign(tracks_.size(), 0U);
  association_used_detections_.assign(detections.size(), 0U);
  // Solve the whole visible set before recovering missing tracks. A missing
  // identity may never outbid an accepted visible continuation.
  if (use_global_assignment) {
    append_global_matches(true, track_indices, detection_indices, detections.size());
  }
  bool relaxed = false;
  bool all_strong = true;
  for (const auto& match : matches_) {
    const auto& track = tracks_[match.track_index];
    const auto reference = predicted_box(track, last_sequence_, observed_at_us);
    const auto& detection = detections[match.detection_index];
    const float dx = center_x(reference) - center_x(detection);
    const float dy = center_y(reference) - center_y(detection);
    relaxed |= has_closer_same_class_observation(
        track, reference, match.detection_index, detections, {}, dx*dx + dy*dy);
    all_strong &= intersection_over_union(reference, detection) >= .4F &&
        size_compatible(reference, detection, 1.5F);
  }
  const auto visible = std::count_if(track_indices.begin(), track_indices.end(),
      [&](auto index) { return tracks_[index].lost_frames == 0U; });
  if (!use_global_assignment ||
      (relaxed && (!all_strong || matches_.size() != std::size_t(visible)))) {
    // A complete strong solution is required before overriding local ambiguity.
    // Otherwise preserve the original partial ownership evidence; a globally
    // cheaper weak pair is not evidence that a missing person has returned.
    if (use_global_assignment) ++metrics_.joint_assignment_rejections;
    else ++metrics_.global_assignment_size_fallbacks;
    matches_.clear();
    std::fill(association_used_tracks_.begin(), association_used_tracks_.end(), 0U);
    std::fill(association_used_detections_.begin(), association_used_detections_.end(), 0U);
    std::sort(possible_matches_.begin(), possible_matches_.end(), [&](const Match& a, const Match& b) {
      const bool a_visible = tracks_[a.track_index].lost_frames == 0U;
      const bool b_visible = tracks_[b.track_index].lost_frames == 0U;
      if (a_visible != b_visible) return a_visible;
      if (a.score != b.score) return a.score > b.score;
      if (a.track_index != b.track_index) return a.track_index < b.track_index;
      return a.detection_index < b.detection_index;
    });
    for (const auto& match : possible_matches_) {
      if (association_used_tracks_[match.track_index] ||
          association_used_detections_[match.detection_index]) continue;
      const auto& track = tracks_[match.track_index];
      const auto reference = predicted_box(track, last_sequence_, observed_at_us);
      const auto& detection = detections[match.detection_index];
      const float dx = center_x(reference) - center_x(detection);
      const float dy = center_y(reference) - center_y(detection);
      if (has_closer_same_class_observation(track, reference, match.detection_index,
              detections, {}, dx*dx + dy*dy)) continue;
      association_used_tracks_[match.track_index] = 1U;
      association_used_detections_[match.detection_index] = track.lost_frames == 0U ? 2U : 1U;
      matches_.push_back(match);
    }
    return matches_;
  }
  if (relaxed) ++metrics_.joint_assignment_acceptances;
  for (const Match& match : possible_matches_) {
    if (tracks_[match.track_index].lost_frames != 0U &&
        association_used_detections_[match.detection_index] == 2U) {
      ++metrics_.lost_track_claims_blocked;
    }
  }
  append_global_matches(false, track_indices, detection_indices, detections.size());
  metrics_.global_assignment_matches += matches_.size();
  return matches_;
}

void MobileTargetTracker::append_global_matches(
    bool visible, std::span<const std::size_t> track_indices,
    std::span<const std::size_t> detection_indices, std::size_t detection_count) {
  constexpr auto missing = std::numeric_limits<std::size_t>::max();
  assignment_tracks_.clear(); assignment_detections_.clear();
  assignment_track_columns_.assign(tracks_.size(), missing);
  assignment_detection_rows_.assign(detection_count, missing);
  for (auto index : track_indices) {
    if ((tracks_[index].lost_frames == 0U) != visible) continue;
    assignment_track_columns_[index] = assignment_tracks_.size();
    assignment_tracks_.push_back(index);
  }
  for (auto index : detection_indices) {
    if (association_used_detections_[index]) continue;
    assignment_detection_rows_[index] = assignment_detections_.size();
    assignment_detections_.push_back(index);
  }
  const auto rows = assignment_detections_.size();
  const auto track_columns = assignment_tracks_.size();
  if (!rows || !track_columns) return;
  const auto columns = track_columns + rows;
  constexpr double forbidden = 1e6;
  assignment_costs_.assign(rows * columns, forbidden);
  for (std::size_t row = 0; row < rows; ++row) {
    std::fill(assignment_costs_.begin() + row * columns + track_columns,
              assignment_costs_.begin() + (row + 1) * columns, 0.0);
  }
  for (const auto& match : possible_matches_) {
    const auto row = assignment_detection_rows_[match.detection_index];
    const auto col = assignment_track_columns_[match.track_index];
    if (row != missing && col != missing)
      assignment_costs_[row * columns + col] = -double(match.score) - 1e-6;
  }
  // Minimum-cost rectangular assignment with explicit unmatched slots.
  // Forbidden class/geometry/cross-confidence pairs cannot be selected merely
  // to maximize cardinality. Lower total cost, not one strongest edge, wins.
  assignment_u_.assign(rows + 1, 0.0);
  assignment_v_.assign(columns + 1, 0.0);
  assignment_p_.assign(columns + 1, 0U);
  assignment_way_.resize(columns + 1);
  assignment_min_.resize(columns + 1);
  assignment_used_.resize(columns + 1);
  for (std::size_t row = 1; row <= rows; ++row) {
    assignment_p_[0] = row;
    std::size_t col = 0;
    std::fill(assignment_min_.begin(), assignment_min_.end(), forbidden);
    std::fill(assignment_used_.begin(), assignment_used_.end(), 0U);
    do {
      assignment_used_[col] = 1U;
      const auto current_row = assignment_p_[col];
      double delta = forbidden;
      std::size_t next = 0;
      for (std::size_t candidate = 1; candidate <= columns; ++candidate) {
        if (assignment_used_[candidate]) continue;
        const double cost = assignment_costs_[(current_row-1)*columns + candidate-1] -
            assignment_u_[current_row] - assignment_v_[candidate];
        if (cost < assignment_min_[candidate]) {
          assignment_min_[candidate] = cost;
          assignment_way_[candidate] = col;
        }
        if (assignment_min_[candidate] < delta) {
          delta = assignment_min_[candidate]; next = candidate;
        }
      }
      for (std::size_t candidate = 0; candidate <= columns; ++candidate) {
        if (assignment_used_[candidate]) {
          assignment_u_[assignment_p_[candidate]] += delta;
          assignment_v_[candidate] -= delta;
        } else assignment_min_[candidate] -= delta;
      }
      col = next;
    } while (assignment_p_[col] != 0U);
    do {
      const auto previous = assignment_way_[col];
      assignment_p_[col] = assignment_p_[previous];
      col = previous;
    } while (col != 0U);
  }
  ++metrics_.global_assignment_calls;
  for (std::size_t col = 1; col <= track_columns; ++col) {
    const auto row = assignment_p_[col];
    if (!row || assignment_costs_[(row-1)*columns+col-1] >= 0.0) continue;
    const auto ti = assignment_tracks_[col-1], di = assignment_detections_[row-1];
    const auto selected = std::find_if(possible_matches_.begin(), possible_matches_.end(),
        [&](const Match& match) { return match.track_index == ti && match.detection_index == di; });
    if (selected == possible_matches_.end()) continue;
    association_used_tracks_[ti] = 1U;
    association_used_detections_[di] = visible ? 2U : 1U;
    matches_.push_back(*selected);
  }
}

bool MobileTargetTracker::has_closer_same_class_observation(
    const Track& track, const YoloDetection& reference,
    std::size_t candidate_index,
    std::span<const YoloDetection> detections,
    std::span<const std::size_t> stage_detection_indices,
    float candidate_distance_squared) noexcept {
  for (std::size_t index = 0; index < detections.size(); ++index) {
    if (index == candidate_index) continue;
    if (!image_pair_allowed(index,track)) continue;
    const YoloDetection& observation = detections[index];
    if (observation.class_id != track.detection.class_id ||
        observation.confidence < config_.low_confidence_threshold) {
      continue;
    }
    const float delta_x = center_x(reference) - center_x(observation);
    const float delta_y = center_y(reference) - center_y(observation);
    const float distance_squared = delta_x * delta_x + delta_y * delta_y;
    if (distance_squared >= candidate_distance_squared) continue;
    if (size_compatible(reference, observation, config_.maximum_size_ratio)) {
      // Competing plausible observations in THIS pass belong in the joint
      // assignment. A nearer low-score observation outside this high-score
      // pass still reserves its owner for the second pass.
      const auto& candidate = detections[candidate_index];
      const bool fresh = config_.lost_track_duration_us != 0U
          ? last_observed_at_us_ >= track.last_observed_at_us &&
              last_observed_at_us_ - track.last_observed_at_us <= kMobileTargetTrackerMaximumEvidenceIntervalUs
          : last_sequence_ - track.last_sequence <= 1U;
      const bool complete_visible_set = track.lost_frames == 0U && fresh &&
          association_visible_count_ >= 2 &&
          stage_detection_indices.size() == association_visible_count_ &&
          detections.size() == association_visible_count_;
      if (complete_visible_set && size_compatible(reference, candidate, 1.5F) &&
          intersection_over_union(reference, candidate) >= .4F &&
          std::find(stage_detection_indices.begin(), stage_detection_indices.end(), index) !=
              stage_detection_indices.end()) continue;
    } else {
      // A scale-incompatible box may still be this person's abrupt pose change,
      // so it normally blocks a neighbour from stealing the ID. The exception
      // is an observation that strongly continues a different visible identity.
      // That known owner must not make the original, still-visible person vanish.
      const bool has_distinct_owner = std::any_of(
          tracks_.begin(), tracks_.end(), [&](const Track& other) {
            const bool fresh = config_.lost_track_duration_us != 0U
                ? other.last_observed_at_us != 0U &&
                    last_observed_at_us_ >= other.last_observed_at_us &&
                    last_observed_at_us_ - other.last_observed_at_us <= kMobileTargetTrackerMaximumEvidenceIntervalUs
                : last_sequence_ >= other.last_sequence &&
                    last_sequence_ - other.last_sequence <= 1U;
            return other.id != track.id && other.lost_frames == 0 &&
                fresh &&
                image_pair_allowed(index,other) &&
                other.detection.class_id == observation.class_id &&
                size_compatible(other.detection, observation,
                    std::min(1.5F, config_.maximum_size_ratio)) &&
                intersection_over_union(other.detection, observation) >=
                    std::max(0.5F, config_.first_stage_iou_threshold);
          });
      if (has_distinct_owner) {
        ++metrics_.known_owner_conflicts_resolved;
        continue;
      }
    }
    return true;
  }
  return false;
}

void MobileTargetTracker::update_control_detection(
    Track& track, const YoloDetection& detection) noexcept {
  const YoloDetection& previous = track.control_detection;
  track.extent_provisional = false;
  if (config_.confirm_abrupt_extent) {
    // A one-frame extra edge (weapon/effect/partial occlusion) must not move
    // the control anchor and then poison both velocity histories. Preserve
    // the last extent for ONE observation, while following translation that
    // both edges support. A repeated new extent is accepted on the next frame.
    const auto provisional = [&](float before, float after, float previous_raw) {
      return std::abs(after - before) > std::max(8.0F, .4F * std::min(before, after)) &&
          (track.lost_frames != 0 ||
           std::abs(after - previous_raw) > std::max(4.0F, .15F * std::min(after, previous_raw)));
    };
    const bool x = provisional(previous.x2-previous.x1, detection.x2-detection.x1,
        track.detection.x2-track.detection.x1);
    const bool y = provisional(previous.y2-previous.y1, detection.y2-detection.y1,
        track.detection.y2-track.detection.y1);
    if (x || y) {
      track.extent_provisional = true;
      const auto translation = [](float low, float high) {
        return low*high <= 0.0F ? 0.0F : std::copysign(std::min(std::abs(low),std::abs(high)),low);
      };
      auto corrected = detection;
      if (x) {
        const auto shift = translation(detection.x1-previous.x1,detection.x2-previous.x2);
        corrected.x1=previous.x1+shift; corrected.x2=previous.x2+shift;
      }
      if (y) {
        const auto shift = translation(detection.y1-previous.y1,detection.y2-previous.y2);
        corrected.y1=previous.y1+shift; corrected.y2=previous.y2+shift;
      }
      track.control_detection=corrected;
      ++metrics_.control_extent_confirmations;
      return;
    }
  }
  // Only rapid aim-point motion may bypass smoothing.  Detector scale noise
  // can move opposite box edges by many pixels while leaving the target
  // center stationary; treating that as motion forwarded raw size jitter into
  // the head aim point on nearly every other frame in real OW2 sessions.
  const float innovation_x = center_x(detection) - center_x(previous);
  const float innovation_y = center_y(detection) - center_y(previous);
  const float innovation = std::hypot(innovation_x, innovation_y);
  metrics_.maximum_control_innovation_pixels = std::max(
      metrics_.maximum_control_innovation_pixels, innovation);
  if (innovation >= config_.control_smoothing_bypass_delta_pixels) {
    track.control_detection = detection;
    ++metrics_.control_filter_bypasses;
    return;
  }

  const float progress = std::clamp(
      innovation / config_.control_smoothing_bypass_delta_pixels,
      0.0F, 1.0F);
  const float alpha = config_.control_smoothing_alpha +
      (1.0F - config_.control_smoothing_alpha) * progress;
  const float retained = 1.0F - alpha;
  YoloDetection filtered = detection;
  filtered.x1 = previous.x1 * retained + detection.x1 * alpha;
  filtered.y1 = previous.y1 * retained + detection.y1 * alpha;
  filtered.x2 = previous.x2 * retained + detection.x2 * alpha;
  filtered.y2 = previous.y2 * retained + detection.y2 * alpha;
  track.control_detection = filtered;
  ++metrics_.control_filter_blends;
}

void MobileTargetTracker::update_track(
    Track& track, const YoloDetection& detection,
    std::uint64_t sequence, std::uint64_t observed_at_us) noexcept {
  const auto previous_control = track.control_detection;
  const bool previous_provisional = track.extent_provisional;
  update_control_detection(track, detection);
  track.motion_discontinuity = previous_provisional || track.extent_provisional;
  float inverse_interval{};
  if (config_.lost_track_duration_us != 0U &&
      observed_at_us > track.last_observed_at_us) {
    const std::uint64_t interval_us =
        observed_at_us - track.last_observed_at_us;
    inverse_interval = 1'000'000.0F / static_cast<float>(interval_us);
    ++metrics_.time_based_velocity_updates;
    metrics_.maximum_observation_interval_us = std::max(
        metrics_.maximum_observation_interval_us, interval_us);
  } else {
    const std::uint64_t gap = sequence > track.last_sequence
        ? sequence - track.last_sequence : 1U;
    inverse_interval = 1.0F / static_cast<float>(gap);
  }
  const auto& velocity_before = config_.confirm_abrupt_extent ? previous_control : track.detection;
  const auto& velocity_after = config_.confirm_abrupt_extent ? track.control_detection : detection;
  const float measured_velocity_x =
      (center_x(velocity_after) - center_x(velocity_before)) * inverse_interval;
  const float measured_velocity_y =
      (center_y(velocity_after) - center_y(velocity_before)) * inverse_interval;
  track.velocity_x = track.motion_discontinuity ? 0.0F :
      track.velocity_x * 0.45F + measured_velocity_x * 0.55F;
  track.velocity_y = track.motion_discontinuity ? 0.0F :
      track.velocity_y * 0.45F + measured_velocity_y * 0.55F;
  track.detection = detection;
  track.last_sequence = sequence;
  if (observed_at_us != 0U) track.last_observed_at_us = observed_at_us;
  track.lost_frames = 0;
}

std::vector<TrackedDetection> MobileTargetTracker::update(
    std::span<const YoloDetection> detections, std::uint64_t sequence,
    std::uint64_t observed_at_us) {
  std::vector<TrackedDetection> result;
  update_into(detections, sequence, observed_at_us, result);
  return result;
}

bool MobileTargetTracker::match_complete_mixed_confidence_group(
    std::span<const YoloDetection> detections, std::uint64_t sequence,
    std::uint64_t observed_at_us, std::vector<TrackedDetection>& result) {
  if (high_detection_indices_.empty() || low_detection_indices_.empty() ||
      detections.size() > 32U) return false;
  visible_track_indices_.clear();
  for (std::size_t index = 0; index < tracks_.size(); ++index) {
    const auto& track = tracks_[index];
    if (track.lost_frames != 0U) continue;
    const bool fresh = config_.lost_track_duration_us != 0U
        ? track.last_observed_at_us != 0U && observed_at_us >= track.last_observed_at_us &&
            observed_at_us - track.last_observed_at_us <= kMobileTargetTrackerMaximumEvidenceIntervalUs
        : sequence > track.last_sequence && sequence - track.last_sequence == 1U;
    if (!fresh) return false;
    visible_track_indices_.push_back(index);
  }
  if (visible_track_indices_.size() < 2U ||
      visible_track_indices_.size() != detections.size() ||
      high_detection_indices_.size() + low_detection_indices_.size() != detections.size()) {
    return false;
  }
  joint_detection_indices_.clear();
  for (std::size_t index = 0; index < detections.size(); ++index) {
    joint_detection_indices_.push_back(index);
  }
  // Confidence is detection quality, not identity. In a complete, fresh group,
  // reserving a weak box for pass two can make BOTH neighbours claim it and
  // orphan the strong box. Solve the existing observations jointly before
  // splitting them by confidence. Missing tracks and new weak tracks never
  // participate, and partial/weak geometry keeps the original two-pass policy.
  const auto joint = associate(visible_track_indices_, joint_detection_indices_,
      detections, config_.first_stage_iou_threshold, observed_at_us);
  const bool complete = joint.size() == visible_track_indices_.size() &&
      std::all_of(joint.begin(), joint.end(), [&](const Match& match) {
        const auto reference = predicted_box(tracks_[match.track_index], sequence, observed_at_us);
        return intersection_over_union(reference, detections[match.detection_index]) >= .4F &&
            size_compatible(reference, detections[match.detection_index], 1.5F);
      });
  // Highly overlapping people do not supply distinct ownership evidence.
  // Keep the ordinary confidence/ambiguity policy at crossings; a complete
  // assignment alone must not be mistaken for a reliable identity match.
  bool distinct = true;
  for (std::size_t left = 0; left < joint.size() && distinct; ++left) {
    for (std::size_t right = left + 1; right < joint.size(); ++right) {
      if (intersection_over_union(tracks_[joint[left].track_index].control_detection,
                                 tracks_[joint[right].track_index].control_detection) > .5F ||
          intersection_over_union(detections[joint[left].detection_index],
                                 detections[joint[right].detection_index]) > .5F) {
        distinct = false;
        break;
      }
    }
  }
  if (!complete || !distinct) {
    ++metrics_.mixed_confidence_joint_rejections;
    return false;
  }
  for (const auto& match : joint) {
    auto& track = tracks_[match.track_index];
    update_track(track, detections[match.detection_index], sequence, observed_at_us);
    result[match.detection_index] = {track.control_detection, track.id, track.motion_discontinuity};
    matched_tracks_[match.track_index] = 1U;
    if (detections[match.detection_index].confidence >= config_.high_confidence_threshold) {
      matched_high_detections_[match.detection_index] = 1U;
      ++metrics_.first_stage_matches;
    } else {
      ++metrics_.second_stage_matches;
    }
  }
  ++metrics_.mixed_confidence_joint_frames;
  return true;
}

bool MobileTargetTracker::image_pair_allowed(std::size_t index,const Track& track) const noexcept {
  const auto owner=index<image_owners_.size()?image_owners_[index]:0U;
  return (!owner || owner==track.id) &&
      (track.image_detection_index==std::numeric_limits<std::size_t>::max() || track.image_detection_index==index);
}

void MobileTargetTracker::prepare_image_constraints(
    std::span<const TrackImageConstraint> constraints,std::span<const YoloDetection> detections,
    std::uint64_t at) noexcept {
  if (constraints.empty()) return;
  if (constraints.size()>image_owners_.size()) {metrics_.image_constraints_rejected+=constraints.size();return;}
  std::array<bool,16> ambiguous{};
  for(const auto& c:constraints){
    const auto track=std::find_if(tracks_.begin(),tracks_.end(),[&](const Track& t){return t.id==c.previous_track_id;});
    if(!c.previous_track_id || c.detection_index>=image_owners_.size() || c.detection_index>=detections.size() ||
        !c.previous_observed_at_us || at<=c.previous_observed_at_us ||
        at-c.previous_observed_at_us>kMobileTargetTrackerMaximumEvidenceIntervalUs ||
        track==tracks_.end() || track->lost_frames!=0U || track->last_observed_at_us!=c.previous_observed_at_us ||
        track->detection.class_id!=detections[c.detection_index].class_id){
      ++metrics_.image_constraints_rejected;continue;
    }
    auto& owner=image_owners_[c.detection_index];
    if(ambiguous[c.detection_index])continue;
    if(owner && owner!=c.previous_track_id){ambiguous[c.detection_index]=true;owner=0;}
    else owner=c.previous_track_id;
  }
  // Reject duplicate ownership in either direction; input order cannot decide
  // which conflicting pixel correspondence wins.
  for(std::size_t i=0;i<image_owners_.size();++i)for(std::size_t j=i+1;j<image_owners_.size();++j)
    if(image_owners_[i] && image_owners_[i]==image_owners_[j])ambiguous[i]=ambiguous[j]=true;
  for(std::size_t i=0;i<image_owners_.size();++i){
    if(ambiguous[i]){image_owners_[i]=0;++metrics_.image_constraints_rejected;}
    if(!image_owners_[i])continue;
    for(auto& track:tracks_)if(track.id==image_owners_[i])track.image_detection_index=i;
    ++metrics_.image_constraints_applied;
  }
}

void MobileTargetTracker::update_into(
    std::span<const YoloDetection> detections, std::uint64_t sequence,
    std::uint64_t observed_at_us,
    std::vector<TrackedDetection>& result,
    std::span<const TrackImageConstraint> image_constraints) {
  image_owners_.fill(0);
  for(auto& track:tracks_)track.image_detection_index=std::numeric_limits<std::size_t>::max();
  result.clear();
  if (result.capacity() < detections.size()) {
    result.reserve(detections.size());
  }
  for (const YoloDetection& detection : detections) result.push_back({detection, 0});
  if (sequence == 0 || (last_sequence_ != 0 && sequence <= last_sequence_) ||
      (config_.lost_track_duration_us != 0U &&
       (observed_at_us == 0U ||
        (last_observed_at_us_ != 0U &&
         observed_at_us <= last_observed_at_us_)))) {
    return;
  }
  last_sequence_ = sequence;
  if (observed_at_us != 0U) last_observed_at_us_ = observed_at_us;

  if (config_.lost_track_duration_us != 0U) {
    const auto old_size = tracks_.size();
    std::erase_if(tracks_, [this, observed_at_us](const Track& track) {
      return observed_at_us - track.last_observed_at_us >
          config_.lost_track_duration_us;
    });
    metrics_.tracks_expired += old_size - tracks_.size();
  }

  prepare_image_constraints(image_constraints,detections,observed_at_us);
  high_detection_indices_.clear();
  low_detection_indices_.clear();
  if (high_detection_indices_.capacity() < detections.size()) {
    high_detection_indices_.reserve(detections.size());
    low_detection_indices_.reserve(detections.size());
  }
  for (std::size_t index = 0; index < detections.size(); ++index) {
    const float confidence = detections[index].confidence;
    if (confidence >= config_.high_confidence_threshold) {
      high_detection_indices_.push_back(index);
    } else if (confidence >= config_.low_confidence_threshold) {
      low_detection_indices_.push_back(index);
    }
  }
  last_eligible_detection_count_ = high_detection_indices_.size() + low_detection_indices_.size();
  track_indices_.resize(tracks_.size());
  for (std::size_t index = 0; index < tracks_.size(); ++index) {
    track_indices_[index] = index;
  }

  matched_tracks_.assign(tracks_.size(), 0U);
  matched_high_detections_.assign(detections.size(), 0U);
  const bool complete_mixed_group = match_complete_mixed_confidence_group(
      detections, sequence, observed_at_us, result);
  if (!complete_mixed_group) {
    const std::span<const Match> first_matches = associate(
        track_indices_, high_detection_indices_, detections,
        config_.first_stage_iou_threshold, observed_at_us);
    for (const Match& match : first_matches) {
      update_track(
          tracks_[match.track_index], detections[match.detection_index],
          sequence, observed_at_us);
      result[match.detection_index].detection =
          tracks_[match.track_index].control_detection;
      result[match.detection_index].track_id = tracks_[match.track_index].id;
      result[match.detection_index].motion_discontinuity =
          tracks_[match.track_index].motion_discontinuity;
      matched_tracks_[match.track_index] = 1U;
      matched_high_detections_[match.detection_index] = 1U;
      ++metrics_.first_stage_matches;
      if (match.center_distance_only) ++metrics_.center_distance_matches;
    }

    unmatched_track_indices_.clear();
    if (unmatched_track_indices_.capacity() < tracks_.size()) {
      unmatched_track_indices_.reserve(tracks_.size());
    }
    for (std::size_t index = 0; index < tracks_.size(); ++index) {
      if (!matched_tracks_[index]) unmatched_track_indices_.push_back(index);
    }
    const std::span<const Match> second_matches = associate(
        unmatched_track_indices_, low_detection_indices_, detections,
        config_.second_stage_iou_threshold, observed_at_us);
    for (const Match& match : second_matches) {
      update_track(
          tracks_[match.track_index], detections[match.detection_index],
          sequence, observed_at_us);
      result[match.detection_index].detection =
          tracks_[match.track_index].control_detection;
      result[match.detection_index].track_id = tracks_[match.track_index].id;
      result[match.detection_index].motion_discontinuity =
          tracks_[match.track_index].motion_discontinuity;
      matched_tracks_[match.track_index] = 1U;
      ++metrics_.second_stage_matches;
      if (match.center_distance_only) ++metrics_.center_distance_matches;
    }
  }

  for (std::size_t index = 0; index < tracks_.size(); ++index) {
    if (!matched_tracks_[index]) ++tracks_[index].lost_frames;
  }
  for (const std::size_t detection_index : high_detection_indices_) {
    if (matched_high_detections_[detection_index] ||
        detections[detection_index].confidence < config_.new_track_confidence_threshold) {
      continue;
    }
    Track track{
        .id = next_track_id_++,
        .detection = detections[detection_index],
        .control_detection = detections[detection_index],
        .last_sequence = sequence,
        .last_observed_at_us = observed_at_us,
    };
    result[detection_index].track_id = track.id;
    tracks_.push_back(track);
    ++metrics_.tracks_created;
  }

  if (config_.lost_track_duration_us == 0U) {
    const auto old_size = tracks_.size();
    std::erase_if(tracks_, [this](const Track& track) {
      return track.lost_frames > config_.lost_frame_buffer;
    });
    metrics_.tracks_expired += old_size - tracks_.size();
  }
}

void MobileTargetTracker::reset() noexcept {
  tracks_.clear();
  image_owners_.fill(0);
  next_track_id_ = 1;
  last_sequence_ = 0;
  last_observed_at_us_ = 0;
  last_eligible_detection_count_ = 0;
}

}  // namespace vfdual_android
