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
  struct Prediction {
    // Position compensation stays at the nominal delay. Risk bounds are the
    // extrema over all plausible delays, including mixed-sign report history.
    float x{}, y{}; std::uint32_t reports{};
    float minimum_x{}, maximum_x{}, minimum_y{}, maximum_y{};
    // Counts of nonzero reports that may still be unseen on each axis.
    // Opposite reports remain risk even when their signed sum cancels to zero.
    std::uint32_t reports_x{},reports_y{};
  };
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
    return predict(observed_at_us,visibility_us,visibility_us,visibility_us);
  }
  [[nodiscard]] Prediction predict(std::uint64_t observed_at_us,
                                   std::uint64_t visibility_us,
                                   std::uint64_t minimum_visibility_us,
                                   std::uint64_t maximum_visibility_us) noexcept {
    std::size_t retired{};
    while (retired < size_ && observed_at_us >= entries_[retired].at_us &&
           observed_at_us - entries_[retired].at_us >= kMaximumVisibilityUs) ++retired;
    for (std::size_t i = retired; i < size_; ++i) entries_[i - retired] = entries_[i];
    size_ -= retired;
    Prediction result{};
    const auto delay = std::min(visibility_us,kMaximumVisibilityUs);
    auto low=std::min(minimum_visibility_us,kMaximumVisibilityUs);
    auto high=std::min(maximum_visibility_us,kMaximumVisibilityUs);
    if(low>high) std::swap(low,high);
    low=std::min(low,delay); high=std::max(high,delay);
    for (std::size_t i = 0; i < size_; ++i) {
      if (observed_at_us >= entries_[i].at_us && observed_at_us-entries_[i].at_us >= delay) continue;
      result.x += entries_[i].x; result.y += entries_[i].y; ++result.reports;
      result.reports_x+=entries_[i].x!=0;result.reports_y+=entries_[i].y!=0;
    }
    result.minimum_x=result.maximum_x=result.x;
    result.minimum_y=result.maximum_y=result.y;
    if(low==high) return result;
    const auto include=[&](std::uint64_t hypothesis) {
      float x{},y{}; std::uint32_t reports{},reports_x{},reports_y{};
      for(std::size_t i{};i<size_;++i) {
        if(observed_at_us>=entries_[i].at_us && observed_at_us-entries_[i].at_us>=hypothesis) continue;
        x+=entries_[i].x; y+=entries_[i].y; ++reports;
        reports_x+=entries_[i].x!=0;reports_y+=entries_[i].y!=0;
      }
      result.minimum_x=std::min(result.minimum_x,x); result.maximum_x=std::max(result.maximum_x,x);
      result.minimum_y=std::min(result.minimum_y,y); result.maximum_y=std::max(result.maximum_y,y);
      result.reports=std::max(result.reports,reports);
      result.reports_x=std::max(result.reports_x,reports_x);
      result.reports_y=std::max(result.reports_y,reports_y);
    };
    include(low); include(high);
    // Pending sums change only just after an entry's visibility boundary.
    // Enumerating those boundaries is exact for this bounded step-response
    // ledger and does not assume same-sign commands or an interpolation model.
    for(std::size_t i{};i<size_;++i) if(observed_at_us>=entries_[i].at_us) {
      const auto age=observed_at_us-entries_[i].at_us;
      if(age>=low && age<high) include(age+1U);
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
  [[nodiscard]] static int bound_axis(int requested, float minimum_pending,
                                      float maximum_pending, int budget) noexcept {
    if(minimum_pending>maximum_pending) std::swap(minimum_pending,maximum_pending);
    return bound_axis(requested,requested>0?maximum_pending:minimum_pending,budget);
  }
  void reset() noexcept { size_ = 0U; }
 private:
  struct Entry { int x{}, y{}; std::uint64_t at_us{}; };
  std::array<Entry, kCapacity> entries_{};
  std::size_t size_{};
};
}  // namespace vfdual_android
