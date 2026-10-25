#include "ControlReferenceFixture.hpp"
#include "MobileControlCore.hpp"
#include "DetectionTrace.hpp"
#include <memory>

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<std::size_t> g_allocation_count{0U};

vfdual_android::YoloDetection box(
    float center_x, float center_y, float width, float height,
    float confidence, std::uint32_t class_id) {
  return {
      center_x - width * 0.5F,
      center_y - height * 0.5F,
      center_x + width * 0.5F,
      center_y + height * 0.5F,
      confidence,
      class_id,
  };
}

}  // namespace

void* operator new(std::size_t size) {
  if (g_count_allocations.load(std::memory_order_relaxed)) {
    g_allocation_count.fetch_add(1U, std::memory_order_relaxed);
  }
  if (void* allocation = std::malloc(size == 0U ? 1U : size)) {
    return allocation;
  }
  throw std::bad_alloc{};
}

void* operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void* allocation) noexcept {
  std::free(allocation);
}

void operator delete[](void* allocation) noexcept {
  std::free(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept {
  std::free(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept {
  std::free(allocation);
}

int main() {
  auto diagnostic_trace = std::make_unique<vfdual_android::DetectionTrace>();
  auto config = reference_416_control_config();
  config.motion.prediction_horizon_ms = 0.0F;
  config.observation_servo_enabled = true;
  // Force a fresh monotonically increasing track id on every observation.
  // Identity history must remain allocation-free and bounded even when a
  // detector repeatedly loses and recreates the same physical target.
  config.tracker.lost_track_duration_us = 1U;
  vfdual_android::MobileControlCore core(config);
  const std::array detections{
      box(260.0F, 140.0F, 24.0F, 24.0F, 0.95F,
          config.head_class_id),
      box(260.0F, 220.0F, 80.0F, 180.0F, 0.95F,
          config.body_class_id),
  };

  std::uint64_t sequence = 1U;
  std::uint64_t observed_at_us = 100'000U;
  for (int index = 0; index < 16; ++index) {
    const auto output = core.process(
        detections, {sequence++, observed_at_us, observed_at_us});
    assert(output.has_target);
    observed_at_us += 6'667U;
  }

  g_allocation_count.store(0U, std::memory_order_relaxed);
  g_count_allocations.store(true, std::memory_order_relaxed);
  constexpr int kMeasuredFrames = 200;
  for (int index = 0; index < kMeasuredFrames; ++index) {
    diagnostic_trace->record(detections, {sequence, observed_at_us, observed_at_us});
    const auto output = core.process(
        detections, {sequence++, observed_at_us, observed_at_us});
    assert(output.has_target);
    observed_at_us += 6'667U;
  }
  g_count_allocations.store(false, std::memory_order_relaxed);

  assert(g_allocation_count.load(std::memory_order_relaxed) == 0U);
  assert(core.tracker_metrics().tracks_created >=
         static_cast<std::uint64_t>(kMeasuredFrames));

  // Exercise the real assignment workspaces: the expiry test above never
  // has an old track to match and therefore cannot detect matrix allocations.
  vfdual_android::MobileTargetTrackerConfig tracker_config;
  tracker_config.lost_track_duration_us = 500'000U;
  tracker_config.control_smoothing_alpha = 1;
  vfdual_android::MobileTargetTracker tracker(tracker_config);
  std::array<vfdual_android::YoloDetection, 32> crowd{};
  std::vector<vfdual_android::TrackedDetection> tracked;
  for (unsigned frame_index = 0; frame_index < 256; ++frame_index) {
    for (unsigned j = 0; j < crowd.size(); ++j) {
      const float offset = frame_index % 2 ? 2.0F : 0.0F;
      crowd[j] = box(40+float(j%8)*45+offset, 70+float(j/8)*75, 40, 100,
          frame_index && frame_index%3 == 0 && j%2 == 0 ? .3F : .9F, 0);
    }
    tracker.clear_motion_prediction();
    if (frame_index == 32) {
      g_allocation_count.store(0U, std::memory_order_relaxed);
      g_count_allocations.store(true, std::memory_order_relaxed);
    }
    tracker.update_into(crowd, frame_index+1, 100'000+frame_index*8'000ULL, tracked);
    diagnostic_trace->record(crowd, {frame_index+1,100'000+frame_index*8'000ULL,100'000+frame_index*8'000ULL});
    assert(tracked.size() == crowd.size());
    for (unsigned j = 0; j < tracked.size(); ++j) {
      assert(tracked[j].track_id == j + 1U);
    }
  }
  g_count_allocations.store(false, std::memory_order_relaxed);
  assert(g_allocation_count.load(std::memory_order_relaxed) == 0U);
  // A complete mixed-confidence group now needs one joint solve, instead of
  // one high-score solve plus one low-score solve. Require real assignment on
  // every frame after creation, while verifying ownership and zero allocation.
  assert(tracker.metrics().global_assignment_calls >= 255);
  return 0;
}
