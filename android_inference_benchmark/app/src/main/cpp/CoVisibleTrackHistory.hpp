#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace vfdual_android {
// Only an actual co-observation establishes a neighbour. Tracker IDs are
// monotonic allocation numbers, not a set membership or physical identity.
template<std::size_t Capacity=64U>
class CoVisibleTrackHistory final {
  static_assert(Capacity>0U);
 public:
  void clear() noexcept { entries_={}; incomplete_=false; incomplete_until_=0U; }
  [[nodiscard]] bool contains(std::uint64_t id) const noexcept {
    if(!id)return false;
    for(const auto& entry:entries_)if(entry.id==id)return true;
    return false;
  }
  void advance(std::uint64_t now,std::uint64_t tracker_ttl) noexcept {
    if(tracker_ttl)for(auto& entry:entries_) {
      if(entry.id && now>entry.last_seen && now-entry.last_seen>tracker_ttl)entry={};
    }
    if(incomplete_ && now>incomplete_until_) {
      incomplete_=false;incomplete_until_=0U;
    }
  }
  // Refresh only exact previously co-visible identities. An unrecorded id
  // cannot silently erase the uncertainty caused by an earlier overflow.
  void observe(std::uint64_t id,std::uint64_t now,std::uint64_t tracker_ttl) noexcept {
    if(!id)return;
    for(auto& entry:entries_)if(entry.id==id) { entry.last_seen=now;return; }
    if(incomplete_)hold_incomplete(now,tracker_ttl);
  }
  void remember(std::uint64_t id,std::uint64_t now,std::uint64_t tracker_ttl) noexcept {
    if(!id)return;
    for(auto& entry:entries_)if(entry.id==id) {entry.last_seen=now;return;}
    for(auto& entry:entries_)if(!entry.id) {entry={id,now};return;}
    hold_incomplete(now,tracker_ttl);
  }
  [[nodiscard]] bool incomplete() const noexcept {return incomplete_;}
  [[nodiscard]] std::size_t size() const noexcept {
    std::size_t count{};for(const auto& entry:entries_)count+=entry.id!=0U;return count;
  }
 private:
  void hold_incomplete(std::uint64_t now,std::uint64_t tracker_ttl) noexcept {
    incomplete_=true;
    const auto maximum=std::numeric_limits<std::uint64_t>::max();
    const auto deadline=!tracker_ttl || now>maximum-tracker_ttl?maximum:now+tracker_ttl;
    if(deadline>incomplete_until_)incomplete_until_=deadline;
  }
  struct Entry {std::uint64_t id{},last_seen{};};
  std::array<Entry,Capacity> entries_{};
  bool incomplete_{};
  std::uint64_t incomplete_until_{};
};
} // namespace vfdual_android
