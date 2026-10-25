#pragma once

#include "YoloPostprocessor.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace vfdual_android {

inline constexpr float kMobileBackgroundMaximumShiftPixels = 128.F;

// Image-space displacement of the background, never a HID delivery receipt.
struct BackgroundMotion {
  std::uint64_t previous_observed_at_us{};
  float x{};
  float y{};
  std::uint32_t support{};
  std::uint32_t candidates{};
  bool valid{};
  bool large_motion_recovered{};
};

// Bounded sparse block matching on the existing RGB U16 inference input.
// Person boxes and the central reticle are excluded. No GPU or extra image
// decode is involved. All scratch storage is reused after the first frame.
class MobileBackgroundMotion final {
 public:
  [[nodiscard]] BackgroundMotion observe(
      std::span<const std::uint8_t> rgb_u16, std::uint32_t width,
      std::uint32_t height, std::span<const YoloDetection> detections,
      std::uint64_t observed_at_us, std::uint64_t generation,
      std::uint64_t recovery_budget_us = 2'500U);
  void reset() noexcept;

 private:
  std::vector<std::uint8_t> previous_;
  std::vector<std::uint8_t> current_;
  std::vector<std::uint8_t> previous_mask_;
  std::vector<std::uint8_t> current_mask_;
  std::uint32_t width_{};
  std::uint32_t height_{};
  std::uint32_t scale_{};
  std::uint64_t observed_at_us_{};
  std::uint64_t generation_{};
};

}  // namespace vfdual_android
