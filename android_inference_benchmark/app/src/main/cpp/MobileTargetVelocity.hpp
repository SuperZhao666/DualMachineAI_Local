#pragma once
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
                  std::uint64_t now, float maximum) noexcept {
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(maximum) || maximum <= 0) {
      reset(); return {};
    }
    if (now <= previous ||
        now - previous > 30'000U || (size_ && samples_[size_-1].at != previous)) reset();
    if (!size_) samples_[size_++] = {previous, 0, 0};
    if (now <= previous || now - previous > 30'000U) { reset(); return {}; }
    const auto last = samples_[size_-1];
    if (size_ == samples_.size()) {
      for (unsigned i=1; i<size_; ++i) samples_[i-1]=samples_[i];
      --size_;
    }
    samples_[size_++] = {now, last.x + dx, last.y + dy};
    if (size_ < 3) return {};
    double st{}, sx{}, sy{}, stt{}, stx{}, sty{}, sxx{}, syy{};
    for (unsigned i=0; i<size_; ++i) {
      const double t = double(samples_[i].at - samples_[0].at) / 1e6;
      st+=t; sx+=samples_[i].x; sy+=samples_[i].y; stt+=t*t;
      stx+=t*samples_[i].x; sty+=t*samples_[i].y;
      sxx+=samples_[i].x*samples_[i].x; syy+=samples_[i].y*samples_[i].y;
    }
    const double variance = stt-st*st/size_;
    if (variance < 1e-9) return {};
    const auto axis = [&](double cross, double position_variance) {
      const double explained = cross*cross/variance;
      const double quality = position_variance > 1e-6 ? explained/position_variance : 0;
      // Only a coherent trend may be extrapolated. Alternating body-box
      // edges should still receive position feedback, not velocity lead.
      const float confidence = std::clamp(float((quality-.30)/.50), 0.0F, 1.0F);
      return std::clamp(float(cross/variance)*confidence, -maximum, maximum);
    };
    return {axis(stx-st*sx/size_, sxx-sx*sx/size_), axis(sty-st*sy/size_, syy-sy*sy/size_)};
  }
  void reset() noexcept { size_=0; }
 private:
  struct Sample { std::uint64_t at{}; double x{}, y{}; };
  std::array<Sample,6> samples_{};
  unsigned size_{};
};
}  // namespace vfdual_android
