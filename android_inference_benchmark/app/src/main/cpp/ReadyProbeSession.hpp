#pragma once
#include "DatagramReceiveResult.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>

namespace vfdual_android {
inline constexpr std::size_t kReadyProbeBytes = 1200;
inline constexpr std::uint64_t kReadyRequesterMaximumAgeUs = 2'000'000;

// One production receive transaction: classify this syscall, validate the
// complete protocol/source, then publish a session-owned discovered peer.
class ReadyProbeSession final {
 public:
  enum class Outcome { ignored, valid_probe, fatal };
  struct Result { Outcome outcome{Outcome::ignored}; int error{}; };
  std::uint64_t begin(std::uint32_t local_host_order) {
    std::scoped_lock lock(mutex_);
    ++generation_; active_=true; local_=local_host_order; peer_=0; at_=0;
    return generation_;
  }
  void end(std::uint64_t generation) {
    std::scoped_lock lock(mutex_);
    if (generation!=generation_) return;
    active_=false; peer_=0; at_=0; ++generation_;
  }
  [[nodiscard]] Result receive(std::uint64_t generation, std::ptrdiff_t received,
      int receive_errno, std::span<const char> datagram, std::uint32_t peer_host_order,
      std::uint64_t now_us, std::array<char,kReadyProbeBytes>& response) {
    const auto state=classify_datagram_receive(received,receive_errno);
    if(state==DatagramReceiveResult::empty || state==DatagramReceiveResult::timeout ||
        state==DatagramReceiveResult::interrupted) return {};
    if(state==DatagramReceiveResult::fatal) {
      std::scoped_lock lock(mutex_);
      return active_ && generation==generation_ ? Result{Outcome::fatal,receive_errno} : Result{};
    }
    if(received!=static_cast<std::ptrdiff_t>(datagram.size()) ||
        !make_ack(datagram,response)) return {};
    std::scoped_lock lock(mutex_);
    if(!active_ || generation!=generation_ || !now_us ||
        !usable_peer(peer_host_order) || peer_host_order==local_) return {};
    peer_=peer_host_order; at_=now_us;
    return {Outcome::valid_probe,0};
  }
  [[nodiscard]] std::uint32_t requester(std::uint64_t now_us) const {
    std::scoped_lock lock(mutex_);
    if(!active_ || !at_ || now_us<at_ || now_us-at_>kReadyRequesterMaximumAgeUs) return 0;
    return peer_;
  }
  [[nodiscard]] static bool usable_peer(std::uint32_t address) noexcept {
    const auto first=address>>24U,second=(address>>16U)&255U;
    return first!=0U && first!=127U && first<224U && !(first==169U && second==254U);
  }
  [[nodiscard]] static bool make_ack(std::span<const char> datagram,
      std::array<char,kReadyProbeBytes>& response) {
    constexpr std::string_view prefix="VF_CAT6_PROBE_V1|",ack="VF_CAT6_PROBE_ACK_V1|";
    if(datagram.size()!=kReadyProbeBytes) return false;
    const auto zero=std::find(datagram.begin(),datagram.end(),'\0');
    if(zero==datagram.end() || std::any_of(zero,datagram.end(),[](char c){return c!=0;})) return false;
    const std::string_view header(datagram.data(),static_cast<std::size_t>(zero-datagram.begin()));
    if(!header.starts_with(prefix) || !header.ends_with('|')) return false;
    const auto token=header.substr(prefix.size(),header.size()-prefix.size()-1U);
    if(token.empty() || token.size()>64U || !std::all_of(token.begin(),token.end(),[](char c){
        return (c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_'||c=='-';})) return false;
    response.fill(0);
    std::copy(ack.begin(),ack.end(),response.begin());
    std::copy(token.begin(),token.end(),response.begin()+ack.size());
    response[ack.size()+token.size()]='|';
    return true;
  }
 private:
  mutable std::mutex mutex_;
  std::uint64_t generation_{},at_{};
  std::uint32_t local_{},peer_{};
  bool active_{};
};
} // namespace vfdual_android
