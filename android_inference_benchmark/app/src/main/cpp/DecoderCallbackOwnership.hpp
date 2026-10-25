#pragma once
#include <cstdint>
namespace vfdual_android {
struct DecoderCallbackToken {
  std::uint64_t binding{},stream{},codec{};
};
/** All calls are serialized by the owning decoder mutex, including fatal writes. */
class DecoderCallbackOwnership {
 public:
  bool publish(DecoderCallbackToken token,std::uint64_t binding,std::uint64_t stream,bool active) noexcept {
    if(!active || !token.binding || !token.stream || !token.codec || token.binding!=binding || token.stream!=stream)return false;
    if(current_.binding==binding && current_.stream==stream && token.codec<current_.codec)return false;
    current_=token;return true;
  }
  bool accepts(DecoderCallbackToken token,std::uint64_t binding,std::uint64_t stream,bool active) const noexcept {
    return active && token.binding && token.stream && token.codec && token.binding==binding && token.stream==stream &&
        token.binding==current_.binding && token.stream==current_.stream && token.codec==current_.codec;
  }
  void reset() noexcept { current_={}; }
 private:
  DecoderCallbackToken current_{};
};
}
