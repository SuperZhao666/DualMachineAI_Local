#pragma once
#include "MobileControlDeadline.hpp"
#include <cstdint>

namespace vfdual_android {
// The original six-point history admitted at most five 30 ms intervals.
// Preserve that physical-time envelope when slower adjacent frames are valid;
// at 20 FPS it still contains the four points required by disturbance fitting.
inline constexpr std::uint64_t kVelocityHistoryMaximumWindowUs=5U*30'000U;
static_assert(kVelocityHistoryMaximumWindowUs>=3U*kContinuousControlMaximumObservationAgeUs);
inline constexpr std::uint64_t kVelocityHistoryNominalWindowUs=(5'000'000U+59U)/60U;
[[nodiscard]] inline std::uint64_t velocity_history_window(
    bool sparse,std::uint64_t minimum_independent_span) noexcept {
  if(!sparse)return kVelocityHistoryMaximumWindowUs;
  const auto needed=minimum_independent_span>kVelocityHistoryNominalWindowUs?
      minimum_independent_span:kVelocityHistoryNominalWindowUs;
  return needed<kVelocityHistoryMaximumWindowUs?needed:kVelocityHistoryMaximumWindowUs;
}
enum class VelocityHistoryState : std::uint8_t {
  insufficient, stationary, moving, uncertain, invalid_input,
  non_contiguous, unsupported_interval, invalid_background, stale_observation,
};
[[nodiscard]] inline const char* velocity_history_state_name(VelocityHistoryState state) noexcept {
  switch(state) {
    case VelocityHistoryState::insufficient:return "insufficient";
    case VelocityHistoryState::stationary:return "stationary";
    case VelocityHistoryState::moving:return "moving";
    case VelocityHistoryState::uncertain:return "uncertain";
    case VelocityHistoryState::invalid_input:return "invalid_input";
    case VelocityHistoryState::non_contiguous:return "non_contiguous";
    case VelocityHistoryState::unsupported_interval:return "unsupported_interval";
    case VelocityHistoryState::invalid_background:return "invalid_background";
    case VelocityHistoryState::stale_observation:return "stale_observation";
  }
  return "unknown";
}
// Only an explicit adjacent source timestamp permits the supported 50 ms
// frame cadence. It never turns two missed frames into one velocity sample.
[[nodiscard]] inline VelocityHistoryState velocity_sample_interval(
    std::uint64_t previous,std::uint64_t now,std::uint64_t adjacent_previous) noexcept {
  if(now<=previous)return VelocityHistoryState::invalid_input;
  if(adjacent_previous && adjacent_previous!=previous)return VelocityHistoryState::non_contiguous;
  const auto maximum=adjacent_previous?kContinuousControlMaximumObservationAgeUs:30'000U;
  if(now-previous>maximum)return VelocityHistoryState::unsupported_interval;
  return VelocityHistoryState::insufficient;
}
}
