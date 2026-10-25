#pragma once
#include <cstdint>
#include <mutex>

namespace vfdual_android {
/** Atomic handoff versus the first validated video packet, not a cached zero. */
class NeverWorkingRouteGuard final {
 public:
  struct Scope {
    std::uint64_t network;
    std::uint32_t local,host;
    bool operator==(const Scope&) const = default;
  };
  std::uint64_t begin(Scope scope) {
    std::scoped_lock lock(mutex_);scope_=scope;seen_=claimed_=false;active_=true;return ++generation_;
  }
  void end(std::uint64_t generation) {
    std::scoped_lock lock(mutex_);
    if(generation!=generation_)return;
    active_=false;seen_=claimed_=false;++generation_;
  }
  [[nodiscard]] bool admit(std::uint64_t generation) {
    std::scoped_lock lock(mutex_);
    if(!active_ || generation!=generation_ || claimed_)return false;
    seen_=true;return true;
  }
  [[nodiscard]] bool claim(Scope expected) {
    std::scoped_lock lock(mutex_);
    if(!active_ || scope_!=expected || seen_ || claimed_)return false;
    claimed_=true;return true;
  }
 private:
  mutable std::mutex mutex_;
  Scope scope_{};
  std::uint64_t generation_{};
  bool seen_{},claimed_{},active_{};
};
} // namespace vfdual_android
