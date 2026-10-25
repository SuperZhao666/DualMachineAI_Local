#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace vfdual_android {

// A bounded model of reports accepted by the transport but not yet expected
// in a captured frame. The timestamp is API acceptance, not physical ACK.
class MobilePendingMotion final {
 public:
  struct Prediction { float x{}, y{}; std::uint32_t reports{}; };
  static constexpr std::uint64_t kExpectedVisibilityUs = 32'000U;
  static constexpr std::uint64_t kMaximumVisibilityUs = 100'000U;
  // Keep recent history beyond the current delay estimate. A later delay
  // revision may need those reports again; acceptance is not execution proof.
  static constexpr std::size_t kCapacity = 64U;
  [[nodiscard]] bool accept(int x, int y, std::uint64_t accepted_at_us) noexcept {
    if (size_ == entries_.size() || accepted_at_us == 0U ||
        (size_ && accepted_at_us < entries_[size_ - 1U].at_us)) return false;
    entries_[size_++] = {x, y, accepted_at_us};
    return true;
  }
  [[nodiscard]] Prediction predict(std::uint64_t observed_at_us,
                                   std::uint64_t visibility_us = kExpectedVisibilityUs) noexcept {
    std::size_t retired{};
    while (retired < size_ && observed_at_us >= entries_[retired].at_us &&
           observed_at_us - entries_[retired].at_us >= kMaximumVisibilityUs) ++retired;
    for (std::size_t i = retired; i < size_; ++i) entries_[i - retired] = entries_[i];
    size_ -= retired;
    Prediction result{};
    const auto delay = std::min(visibility_us,kMaximumVisibilityUs);
    for (std::size_t i = 0; i < size_; ++i) {
      if (observed_at_us >= entries_[i].at_us && observed_at_us-entries_[i].at_us >= delay) continue;
      result.x += entries_[i].x; result.y += entries_[i].y; ++result.reports;
    }
    return result;
  }
  // Never reverse solely because the uncertain pending-response estimate
  // crossed zero. A reversal needs an actually observed opposite error.
  [[nodiscard]] static float compensate(float error, float pending_pixels) noexcept {
    // A visible sign reversal can also mean a report is already present sooner
    // than the estimated delay. Never amplify the return move with that report.
    if (error * pending_pixels <= 0.0F) return error;
    const float compensated = error - pending_pixels;
    return error * compensated < 0.0F ? 0.0F : compensated;
  }
  [[nodiscard]] static int bound_axis(int requested, float pending, int budget) noexcept {
    // Retiring an older opposite report can leave the remaining signed sum
    // outside the budget. A limiter may suppress or reduce a requested move;
    // it must never manufacture a correction, reverse it, or amplify it.
    if (requested > 0) return std::min(requested, std::max(0, int(std::floor(budget-pending))));
    if (requested < 0) return std::max(requested, std::min(0, int(std::ceil(-budget-pending))));
    return 0;
  }
  void reset() noexcept { size_ = 0U; }
 private:
  struct Entry { int x{}, y{}; std::uint64_t at_us{}; };
  std::array<Entry, kCapacity> entries_{};
  std::size_t size_{};
};
}  // namespace vfdual_android
