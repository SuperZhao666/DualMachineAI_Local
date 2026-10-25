#pragma once
#include <algorithm>
#include <cstdint>

namespace vfdual_android {
inline constexpr std::uint64_t kContinuousControlMaximumObservationAgeUs = 50'000U;
// A transport ticket lifetime cannot make a stale visual decision fresh.
// All values are on the Android monotonic clock; no wall-clock conversion.
[[nodiscard]] constexpr std::uint64_t visual_move_budget_us(
    std::uint64_t observed, std::uint64_t dispatch_now,
    std::uint64_t transport_remaining) noexcept {
  if (!observed || dispatch_now < observed ||
      dispatch_now-observed >= kContinuousControlMaximumObservationAgeUs) return 0;
  return std::min(transport_remaining,
      kContinuousControlMaximumObservationAgeUs-(dispatch_now-observed));
}
}
