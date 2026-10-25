#pragma once
#include "MobileControlDeadline.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace vfdual_android {
// Prefer real newer ready input after warmup. A bounded wait for an already
// running conversion needs either deadline pressure or a measured near finish.
// Never drop the only input; the worker exchanges at most once, then executes.
class InferenceDeadlineAdmission final {
 public:
  struct Observation { std::uint64_t observed_us{},generation{}; };
  struct Preparing { Observation observation{};std::uint64_t started_us{},expected_duration_us{}; };
  static constexpr std::uint64_t kMaximumReplacementWaitUs=2'000U;
  static constexpr std::uint64_t kMaximumOpportunisticWaitUs=1'000U;
  static constexpr std::uint64_t kMinimumNetFreshnessGainUs=3'000U;
  static constexpr std::uint64_t kMaximumPreparingAgeUs=8'000U;
  static constexpr std::uint64_t kDownstreamMarginUs=3'000U;
  static constexpr std::size_t kMinimumSamples=8U;
  static constexpr std::size_t kHistory=16U;

  void record_backend_wall(std::uint64_t duration_us) noexcept {
    if(!duration_us || duration_us>1'000'000U)return;
    samples_[count_%samples_.size()]=duration_us;++count_;
    const auto n=std::min<std::uint64_t>(count_,samples_.size());
    auto ordered=samples_;std::sort(ordered.begin(),ordered.begin()+n);
    estimate_us_=ordered[(n-1U)*3U/4U];
  }
  [[nodiscard]] std::uint64_t backend_estimate_us() const noexcept {return estimate_us_;}
  [[nodiscard]] bool ready() const noexcept {return count_>=kMinimumSamples;}
  [[nodiscard]] bool prefer_ready(Observation selected,Observation replacement,std::uint64_t now) const noexcept {
    return ready() && valid_pair(selected,replacement,now);
  }
  [[nodiscard]] std::uint64_t replacement_wait_budget_us(Observation selected,Preparing replacement,std::uint64_t now) const noexcept {
    if(!can_predict() || !valid_pair(selected,replacement.observation,now) || !replacement.started_us ||
       replacement.started_us<replacement.observation.observed_us || now<replacement.started_us ||
       now-replacement.started_us>kMaximumPreparingAgeUs)return 0;
    const auto gain=replacement.observation.observed_us-selected.observed_us;
    const auto age=now-selected.observed_us;
    const auto new_age=now-replacement.observation.observed_us;
    // Preserve the existing deadline-pressure path, including unknown hints.
    if(age>=kContinuousControlMaximumObservationAgeUs-(estimate_us_+kDownstreamMarginUs) &&
       gain>kMaximumReplacementWaitUs+1'000U &&
       new_age<kContinuousControlMaximumObservationAgeUs-(estimate_us_+kDownstreamMarginUs+kMaximumReplacementWaitUs))
      return kMaximumReplacementWaitUs;
    const auto elapsed=now-replacement.started_us;
    // An overdue conversion has no reliable remaining-time prediction.
    if(!replacement.expected_duration_us || elapsed>=replacement.expected_duration_us)return 0;
    const auto remaining=replacement.expected_duration_us-elapsed;
    if(remaining>kMaximumOpportunisticWaitUs || gain<remaining+kMinimumNetFreshnessGainUs)return 0;
    return new_age<kContinuousControlMaximumObservationAgeUs-(estimate_us_+kDownstreamMarginUs+remaining)?remaining:0;
  }
  [[nodiscard]] bool wait_for_preparing(Observation selected,Preparing replacement,std::uint64_t now) const noexcept {
    return replacement_wait_budget_us(selected,replacement,now)!=0;
  }
 private:
  [[nodiscard]] bool can_predict() const noexcept {
    // Startup has no measured estimate. On a device whose backend alone cannot
    // meet the horizon, continuing inference is more useful than waiting forever.
    return ready() && estimate_us_+kDownstreamMarginUs+kMaximumReplacementWaitUs<kContinuousControlMaximumObservationAgeUs;
  }
  static bool valid_pair(Observation a,Observation b,std::uint64_t now) noexcept {
    return a.observed_us && a.observed_us<=now && b.observed_us<=now &&
      b.observed_us>a.observed_us && a.generation==b.generation;
  }
  std::array<std::uint64_t,kHistory> samples_{};
  std::uint64_t count_{},estimate_us_{};
};
}
