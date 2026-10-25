#pragma once
#include "MobileControlCore.hpp"
#include <algorithm>
#include <array>
#include <span>

namespace vfdual_android {
// Numeric replay inputs, never images. Explicitly report clipping above 16 boxes.
struct DetectionTraceSample {
  std::uint64_t id{};
  ControlFrameContext frame{};
  std::uint32_t total{}, kept{};
  std::array<YoloDetection, 16> boxes{};
  std::uint32_t image_kept{};
  std::array<TargetImageMotion,16> image_motion{};
};
struct DetectionTrace {
  std::array<DetectionTraceSample, 1024> samples{};
  std::uint64_t count{};
  void record(std::span<const YoloDetection> detections, const ControlFrameContext& frame,
      std::span<const TargetImageMotion> image_motion={}) noexcept {
    const auto id = ++count;
    auto& sample = samples[(id - 1) % samples.size()];
    sample.id = id; sample.frame = frame;
    sample.total = static_cast<std::uint32_t>(detections.size());
    sample.kept = static_cast<std::uint32_t>(std::min(detections.size(), sample.boxes.size()));
    std::copy_n(detections.begin(), sample.kept, sample.boxes.begin());
    sample.image_kept=0;
    for(const auto& motion:image_motion) {
      if(sample.image_kept==sample.image_motion.size())break;
      if(motion.valid)sample.image_motion[sample.image_kept++]=motion;
    }
  }
};
}
