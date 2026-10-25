#pragma once

#include "DiagnosticTraceCursor.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

namespace vfdual_android {
// One successful, current-generation frame, in the phone's monotonic clock.
// observed_us is receive time minus Host-reported frame age. The unknown
// one-way network delay is absent; qnn_us is NESTED inside inference time.
struct PipelineTimingSample final {
  std::uint64_t id{}, sequence{}, wire_frame_id{}, generation{};
  std::uint64_t observed_us{}, received_us{}, decoded_planes_us{};
  std::uint64_t preprocess_started_us{}, preprocess_finished_us{}, enqueued_us{};
  std::uint64_t inference_started_us{}, inference_finished_us{}, result_ready_us{};
  std::uint64_t control_started_us{}, control_finished_us{};
  std::uint64_t qnn_us{}, background_us{}, image_motion_us{}, detections{}, content_updated{};

  [[nodiscard]] std::array<std::uint64_t, 20> fields() const noexcept {
    return {id, sequence, wire_frame_id, generation, observed_us, received_us,
        decoded_planes_us, preprocess_started_us, preprocess_finished_us, enqueued_us,
        inference_started_us, inference_finished_us, result_ready_us, control_started_us,
        control_finished_us, qnn_us, background_us, image_motion_us, detections, content_updated};
  }
};

// Caller owns synchronization. Producers copy fixed numeric values only; the
// single diagnostics consumer snapshots under the lock and formats after it.
// IDs remain monotonic across stream restarts; generation is recorded per row.
class PipelineTimingTrace final {
 public:
  static constexpr std::size_t capacity = 1024;
  struct Snapshot final {
    std::array<PipelineTimingSample, capacity> samples{};
    std::size_t count{};
    std::uint64_t lost{};
    [[nodiscard]] std::string report() const {
      std::ostringstream out;
      out << "pipeline_clock=monotonic_us pipeline_observation_excludes_network=1"
          << " pipeline_trace_lost=" << lost << " pipeline_trace_v1=[";
      for (std::size_t i = 0; i < count; ++i) {
        if (i) out << ';';
        const auto fields = samples[i].fields();
        for (std::size_t j = 0; j < fields.size(); ++j) {
          if (j) out << ',';
          out << fields[j];
        }
      }
      return out.str() + ']';
    }
  };

  void record(PipelineTimingSample sample) noexcept {
    sample.id = ++total_;
    samples_[(sample.id - 1U) % capacity] = sample;
  }
  void drain(Snapshot& snapshot) noexcept {
    const auto range = cursor_.advance(total_, capacity);
    snapshot.count = 0;
    snapshot.lost = range.lost;
    for (auto id = range.first; id <= range.last; ++id)
      snapshot.samples[snapshot.count++] = samples_[(id - 1U) % capacity];
  }

 private:
  std::array<PipelineTimingSample, capacity> samples_{};
  std::uint64_t total_{};
  DiagnosticTraceCursor cursor_{};
};
}  // namespace vfdual_android
