#pragma once
#include "VelocitySampleContinuity.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace vfdual_android {
// Fit camera-compensated target positions over a short fixed window. Taking
// the derivative of each noisy box, then extrapolating it for a Bluetooth
// response interval, turns ordinary detector jitter into camera oscillation.
class MobileTargetVelocity final {
 public:
  struct Velocity { float x{}, y{}; };
  Velocity update(float dx, float dy, std::uint64_t previous,
                  std::uint64_t now, float maximum,
                  std::uint64_t adjacent_previous = 0U) noexcept {
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(maximum) || maximum <= 0) {
      reset(VelocityHistoryState::invalid_input); return {};
    }
    const auto interval=velocity_sample_interval(previous,now,adjacent_previous);
    if(interval!=VelocityHistoryState::insufficient) {
      reset(interval);
      if(now && interval!=VelocityHistoryState::invalid_input)samples_[size_++]={now,0,0};
      return {};
    }
    if(size_ && samples_[size_-1].at!=previous)reset(VelocityHistoryState::non_contiguous);
    if (!size_) samples_[size_++] = {previous, 0, 0};
    const auto last = samples_[size_-1];
    if (size_ == samples_.size()) {
      for (unsigned i=1; i<size_; ++i) samples_[i-1]=samples_[i];
      --size_;
    }
    samples_[size_++] = {now, last.x + dx, last.y + dy};
    bool sparse=false;
    for(unsigned i=1;i<size_;++i)sparse=sparse || samples_[i].at-samples_[i-1].at>30'000U;
    const auto minimum_span=now-samples_[size_>=3U?size_-3U:0U].at;
    const auto window=velocity_history_window(sparse,minimum_span);
    while(size_>1U && now-samples_[0].at>window) {
      for(unsigned i=1;i<size_;++i)samples_[i-1]=samples_[i];
      --size_;
    }
    if (size_ < 3) {state_=VelocityHistoryState::insufficient;return {};}
    double st{}, sx{}, sy{}, stt{}, stx{}, sty{}, sxx{}, syy{};
    for (unsigned i=0; i<size_; ++i) {
      const double t = double(samples_[i].at - samples_[0].at) / 1e6;
      st+=t; sx+=samples_[i].x; sy+=samples_[i].y; stt+=t*t;
      stx+=t*samples_[i].x; sty+=t*samples_[i].y;
      sxx+=samples_[i].x*samples_[i].x; syy+=samples_[i].y*samples_[i].y;
    }
    const double variance = stt-st*st/size_;
    if (variance < 1e-9) {state_=VelocityHistoryState::insufficient;return {};}
    const auto axis = [&](double cross, double position_variance) {
      const double explained = cross*cross/variance;
      const double quality = position_variance > 1e-6 ? explained/position_variance : 0;
      // Only a coherent trend may be extrapolated. Alternating body-box
      // edges should still receive position feedback, not velocity lead.
      const float confidence = std::clamp(float((quality-.30)/.50), 0.0F, 1.0F);
      return std::clamp(float(cross/variance)*confidence, -maximum, maximum);
    };
    Velocity result{axis(stx-st*sx/size_,sxx-sx*sx/size_),axis(sty-st*sy/size_,syy-sy*sy/size_)};
    // A newly admitted sparse sample must not continue an old slope through a
    // measured reversal. This retires an estimate; one sample never creates a
    // new opposite velocity. Keep the original <=30 ms contract unchanged.
    if(adjacent_previous && now-previous>30'000U) {
      const float dt=float(now-previous)/1e6F;
      if(result.x*dx<0.F && std::abs(dx)/dt>20.F)result.x=0.F;
      if(result.y*dy<0.F && std::abs(dy)/dt>20.F)result.y=0.F;
    }
    bool stationary=true;
    for(unsigned i=1;i<size_;++i)stationary=stationary &&
        std::abs(samples_[i].x-samples_[0].x)<.001 && std::abs(samples_[i].y-samples_[0].y)<.001;
    state_=stationary?VelocityHistoryState::stationary:
        (result.x!=0.F || result.y!=0.F)?VelocityHistoryState::moving:VelocityHistoryState::uncertain;
    return result;
  }
  void reset(VelocityHistoryState reason=VelocityHistoryState::insufficient) noexcept {size_=0;state_=reason;}
  [[nodiscard]] VelocityHistoryState state()const noexcept {return state_;}
  [[nodiscard]] unsigned sample_count()const noexcept {return size_;}
  [[nodiscard]] std::uint64_t span_us()const noexcept {return size_?samples_[size_-1U].at-samples_[0].at:0U;}
 private:
  struct Sample { std::uint64_t at{}; double x{}, y{}; };
  std::array<Sample,6> samples_{};
  unsigned size_{};
  VelocityHistoryState state_{VelocityHistoryState::insufficient};
};
}  // namespace vfdual_android
