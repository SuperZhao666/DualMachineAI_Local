#pragma once

#include <cstdint>

namespace vfdual_android {

// Receive time alone can be fresh even after a long known Host capture/encode
// delay. Both clocks have already been mapped to phone monotonic time by the
// receiver. Check before RGB conversion, without touching the H.264 chain.
[[nodiscard]] constexpr bool decoded_observation_is_fresh(
    std::uint64_t received_us, std::uint64_t observed_us, std::uint64_t now_us,
    std::uint64_t maximum_age_us) noexcept {
  return now_us >= received_us && now_us >= observed_us &&
      now_us - received_us <= maximum_age_us && now_us - observed_us <= maximum_age_us;
}

}  // namespace vfdual_android
