#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ostream>
#include <string_view>

namespace vfdual_android {

// Diagnostic only: no allocation, lock or formatting on the measured path.
// Snapshots can straddle a concurrent record; they are not transactional.
class AtomicDurationSummary final {
 public:
  static std::uint64_t now_us() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  }
  void record(std::uint64_t elapsed_us) noexcept {
    count_.fetch_add(1, std::memory_order_relaxed);
    total_us_.fetch_add(elapsed_us, std::memory_order_relaxed);
    auto previous = maximum_us_.load(std::memory_order_relaxed);
    while (elapsed_us > previous && !maximum_us_.compare_exchange_weak(
        previous, elapsed_us, std::memory_order_relaxed)) {}
    if (elapsed_us > 2'000U) over_2ms_.fetch_add(1, std::memory_order_relaxed);
    if (elapsed_us > 50'000U) over_50ms_.fetch_add(1, std::memory_order_relaxed);
  }
  void reset() noexcept {
    count_.store(0, std::memory_order_relaxed);
    total_us_.store(0, std::memory_order_relaxed);
    maximum_us_.store(0, std::memory_order_relaxed);
    over_2ms_.store(0, std::memory_order_relaxed);
    over_50ms_.store(0, std::memory_order_relaxed);
  }
  void append(std::ostream& out, std::string_view name) const {
    out << ' ' << name << "_count=" << count_.load(std::memory_order_relaxed)
        << ' ' << name << "_total_us=" << total_us_.load(std::memory_order_relaxed)
        << ' ' << name << "_maximum_us=" << maximum_us_.load(std::memory_order_relaxed)
        << ' ' << name << "_over_2ms=" << over_2ms_.load(std::memory_order_relaxed)
        << ' ' << name << "_over_50ms=" << over_50ms_.load(std::memory_order_relaxed);
  }
 private:
  std::atomic_uint64_t count_{}, total_us_{}, maximum_us_{}, over_2ms_{}, over_50ms_{};
};

class AtomicDurationScope final {
 public:
  explicit AtomicDurationScope(AtomicDurationSummary& summary) noexcept
      : summary_(summary), started_(AtomicDurationSummary::now_us()) {}
  ~AtomicDurationScope() { summary_.record(AtomicDurationSummary::now_us() - started_); }
  AtomicDurationScope(const AtomicDurationScope&) = delete;
  AtomicDurationScope& operator=(const AtomicDurationScope&) = delete;
 private:
  AtomicDurationSummary& summary_;
  std::uint64_t started_;
};

}  // namespace vfdual_android
