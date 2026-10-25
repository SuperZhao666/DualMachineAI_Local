#include "YoloPostprocessor.hpp"

#include "vfdual/model_contract.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace {
std::atomic<bool> g_count_allocations{false};
std::atomic<std::size_t> g_allocation_count{0U};
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
  constexpr std::uint32_t anchors = 2100;
  std::array<std::uint8_t, 4 * anchors * sizeof(float)> coordinates{};
  std::array<std::uint8_t, 2 * anchors * sizeof(float)> confidences{};
  const auto set = [](auto& output, std::uint32_t channel, std::uint32_t anchor, float value) {
    std::memcpy(output.data() + (channel * anchors + anchor) * sizeof(float), &value, sizeof(value));
  };
  // Two overlapping class-0 candidates; NMS keeps only the stronger one.
  set(coordinates, 0, 0, 100); set(coordinates, 1, 0, 100); set(coordinates, 2, 0, 40); set(coordinates, 3, 0, 40); set(confidences, 0, 0, .90F);
  set(coordinates, 0, 1, 102); set(coordinates, 1, 1, 100); set(coordinates, 2, 1, 40); set(coordinates, 3, 1, 40); set(confidences, 0, 1, .80F);
  // A disjoint class-1 candidate remains.
  set(coordinates, 0, 2, 200); set(coordinates, 1, 2, 200); set(coordinates, 2, 2, 20); set(coordinates, 3, 2, 20); set(confidences, 1, 2, .95F);
  const vfdual_android::YoloPostprocessConfig config{.confidence_threshold = .10F, .iou_threshold = .45F};
  const auto detections = vfdual_android::decode_qnn_split_yolo_output(coordinates, confidences, config);
  assert(detections.size() == 2);
  assert(detections[0].class_id == 1U && detections[0].confidence == .95F);
  assert(detections[1].class_id == 0U && detections[1].confidence == .90F);

  // A 416 five-class model uses 3549 anchors. Verify that the selected class
  // comes from all confidence channels rather than the old two-class shortcut.
  constexpr std::uint32_t anchors_416 = 3549;
  constexpr std::uint32_t classes_416 = 5;
  std::vector<std::uint8_t> coordinates_416(
      4U * anchors_416 * sizeof(float));
  std::vector<std::uint8_t> confidences_416(
      classes_416 * anchors_416 * sizeof(float));
  const auto set_416 = [](std::vector<std::uint8_t>& output,
                          std::uint32_t channel, std::uint32_t anchor,
                          float value) {
    std::memcpy(
        output.data() +
            (static_cast<std::size_t>(channel) * anchors_416 + anchor) *
                sizeof(float),
        &value, sizeof(value));
  };
  set_416(coordinates_416, 0U, 7U, 208.0F);
  set_416(coordinates_416, 1U, 7U, 180.0F);
  set_416(coordinates_416, 2U, 7U, 24.0F);
  set_416(coordinates_416, 3U, 7U, 48.0F);
  set_416(confidences_416, 0U, 7U, 0.10F);
  set_416(confidences_416, 1U, 7U, 0.20F);
  set_416(confidences_416, 2U, 7U, 0.30F);
  set_416(confidences_416, 3U, 7U, 0.93F);
  set_416(confidences_416, 4U, 7U, 0.40F);
  const vfdual_android::YoloSplitTensorContract contract_416{
      .anchor_count = anchors_416,
      .class_count = classes_416,
  };
  const auto detections_416 =
      vfdual_android::decode_qnn_split_yolo_output(
          coordinates_416, confidences_416, config, contract_416);
  assert(detections_416.size() == 1U);
  assert(detections_416[0].class_id == 3U);
  assert(detections_416[0].confidence == .93F);

  // Every selectable OW size has one class. Reject former multi-class buffers.
  for (const auto* model : vfdual::kMobileModels) {
    assert(model->output.class_count == 1U);
    const auto anchors = model->output.anchor_count;
    std::vector<std::uint8_t> xywh(4U * anchors * sizeof(float));
    std::vector<std::uint8_t> confidence(anchors * sizeof(float));
    const auto put = [&](auto& bytes, std::size_t channel, float value) {
      std::memcpy(bytes.data() + channel * anchors * sizeof(float), &value, sizeof(float));
    };
    put(xywh, 0, 100.0F); put(xywh, 1, 120.0F);
    put(xywh, 2, 20.0F); put(xywh, 3, 40.0F); put(confidence, 0, 0.8F);
    const vfdual_android::YoloSplitTensorContract shape{anchors, 1U};
    auto decoded = vfdual_android::decode_qnn_split_yolo_output(xywh, confidence, config, shape);
    assert(decoded.size() == 1U && decoded[0].class_id == 0U);
    confidence.resize(2U * anchors * sizeof(float));
    assert(vfdual_android::decode_qnn_split_yolo_output(xywh, confidence, config, shape).empty());
  }

  const vfdual_android::YoloSplitTensorContract invalid_contract{
      .anchor_count = anchors_416,
      .class_count = 0U,
  };
  assert(vfdual_android::decode_qnn_split_yolo_output(
             coordinates_416, confidences_416, config, invalid_contract)
             .empty());

  for (std::uint32_t anchor = 0; anchor < anchors_416; ++anchor) {
    set_416(coordinates_416, 0U, anchor, 208.0F);
    set_416(coordinates_416, 1U, anchor, 180.0F);
    set_416(coordinates_416, 2U, anchor, 24.0F);
    set_416(coordinates_416, 3U, anchor, 48.0F);
    for (std::uint32_t class_id = 1U; class_id < classes_416; ++class_id) {
      set_416(confidences_416, class_id, anchor, 0.0F);
    }
    set_416(confidences_416, 0U, anchor, 0.50F);
  }
  std::vector<vfdual_android::YoloDetection> candidates;
  std::vector<vfdual_android::YoloDetection> reusable_detections;
  assert(vfdual_android::decode_qnn_split_yolo_output_into(
      coordinates_416, confidences_416, config, contract_416,
      candidates, reusable_detections));
  assert(reusable_detections.size() == 1U);
  g_allocation_count.store(0U, std::memory_order_relaxed);
  g_count_allocations.store(true, std::memory_order_relaxed);
  for (int index = 0; index < 200; ++index) {
    assert(vfdual_android::decode_qnn_split_yolo_output_into(
        coordinates_416, confidences_416, config, contract_416,
        candidates, reusable_detections));
    assert(reusable_detections.size() == 1U);
  }
  g_count_allocations.store(false, std::memory_order_relaxed);
  assert(g_allocation_count.load(std::memory_order_relaxed) == 0U);
  return 0;
}
