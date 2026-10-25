#pragma once

#include "YoloPostprocessor.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace vfdual_android {

struct FreshPixelIdentity {
  std::uint64_t generation{}, sequence{}, observed_at_us{};
};

enum class FreshPixelRejection : std::uint8_t {
  none, not_configured, invalid_pixels, invalid_identity, generation_mismatch,
  not_newer, gap_over50ms, unsupported_box_count, invalid_source_box,
  low_texture_or_support, ambiguous_or_low_ncc, forward_backward,
  insufficient_matches, incoherent_or_narrow_support, low_quality,
  translation_out_of_range, projected_box_outside_frame,
  projected_people_merge, work_budget
};

[[nodiscard]] const char* fresh_pixel_rejection_name(FreshPixelRejection reason) noexcept;

struct FreshPixelTargetEvidence {
  std::uint32_t source_index{};
  std::uint64_t pixel_observed_at_us{};
  float dx_model_px{}, dy_model_px{}, quality{}, forward_backward_error_model_px{};
  std::uint32_t support{}, candidates{};
  FreshPixelRejection rejection{FreshPixelRejection::none};
  bool valid{};
};

struct FreshPixelProjectionResult {
  FreshPixelIdentity source_identity{}, target_identity{};
  // Empty unless every source body has a valid same-target-time projection.
  // Both views remain valid only until the next process/configure/reset call.
  std::span<const YoloDetection> projected_boxes;
  std::span<const FreshPixelTargetEvidence> targets;
  std::uint64_t elapsed_us{};
  std::uint32_t valid_count{};
  FreshPixelRejection rejection{FreshPixelRejection::none};
  bool complete{}, budget_exhausted{};
};

/** Real-pixel continuation of existing bodies; never a new detector result.
 * Single-worker ownership. configure() allocates reusable320/160 gray pyramids
 * for the640 input contract; process() performs no heap allocation. Its work
 * budget includes both RGB_U16 prefilters and all patch/FB/consensus work.
 * The source NPU identity is preserved separately from later pixel evidence.
 */
class MobileFreshPixelProjection final {
 public:
  static constexpr std::size_t kMaximumTargets = 16;
  [[nodiscard]] bool configure(std::uint32_t width, std::uint32_t height);
  void reset() noexcept;
  [[nodiscard]] FreshPixelProjectionResult process(
      std::span<const std::uint8_t> source_rgb_u16,
      std::span<const std::uint8_t> latest_rgb_u16,
      std::span<const YoloDetection> source_body_boxes,
      FreshPixelIdentity source_identity, FreshPixelIdentity latest_identity,
      std::uint64_t maximum_work_us = 2'500U) noexcept;

 private:
  struct Pyramid {
    std::vector<std::uint8_t> gray, coarse;
  };
  Pyramid source_, latest_;
  std::array<YoloDetection, kMaximumTargets> boxes_{};
  std::array<FreshPixelTargetEvidence, kMaximumTargets> evidence_{};
  std::uint32_t width_{}, height_{};
  bool configured_{};
};

} // namespace vfdual_android
