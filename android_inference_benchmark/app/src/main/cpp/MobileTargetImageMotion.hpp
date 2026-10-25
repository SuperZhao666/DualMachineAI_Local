#pragma once
#include "MobileBackgroundMotion.hpp"
#include "YoloPostprocessor.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace vfdual_android {
// Independent pixel correspondence, not a detector score or a HID receipt.
struct TargetImageMotion {
  std::uint64_t previous_observed_at_us{};
  std::uint32_t current_index{}, previous_index{};
  float x{}, y{}, quality{}, forward_backward_error{};
  std::uint32_t support{}, candidates{};
  bool valid{};
};

// Bounded normalized patch matching around the existing 18%-of-box aim region.
// No extra neural model, GPU pass, image decode, or per-point heap allocation.
class MobileTargetImageMotion final {
 public:
  static constexpr std::size_t kMaximumTargets=16U;
  [[nodiscard]] std::span<const TargetImageMotion> observe(
      std::span<const std::uint8_t> rgb_u16,std::uint32_t width,std::uint32_t height,
      std::span<const YoloDetection> detections,const BackgroundMotion& background,
      std::uint64_t observed_at_us,std::uint64_t generation,float anchor_y_ratio=.18F,
      std::uint64_t maximum_work_us=0,bool selective=false);
  [[nodiscard]] bool work_budget_exhausted() const noexcept {return work_budget_exhausted_;}
  void reset() noexcept;
 private:
  std::vector<std::uint8_t> previous_,current_,previous_coarse_,current_coarse_;
  std::array<YoloDetection,kMaximumTargets> previous_boxes_{};
  std::array<TargetImageMotion,kMaximumTargets> motions_{};
  std::size_t previous_count_{};
  std::uint32_t width_{},height_{},scale_{};
  std::uint64_t observed_at_us_{},generation_{};
  bool work_budget_exhausted_{};
  unsigned continuation_frames_{};
};
}
