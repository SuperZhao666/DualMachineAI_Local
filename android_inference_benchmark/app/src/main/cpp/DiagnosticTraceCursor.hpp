#pragma once
#include <algorithm>
#include <cstdint>

namespace vfdual_android {
// A single telemetry consumer drains a fixed ring without changing producers.
class DiagnosticTraceCursor {
 public:
  struct Range { std::uint64_t first{}, last{}, lost{}; bool reset{}; };
  Range advance(std::uint64_t total, std::uint64_t capacity) noexcept {
    const bool reset = total < last_;
    if (reset) last_ = 0;
    const auto oldest = total > capacity ? total - capacity + 1 : 1;
    const auto wanted = last_ + 1;
    const auto first = std::max(oldest, wanted);
    const auto lost = oldest > wanted ? oldest - wanted : 0;
    last_ = total;
    return {first, total, lost, reset};
  }
 private:
  std::uint64_t last_{};
};
}
