#pragma once

#include <cerrno>
#include <cstddef>

namespace vfdual_android {
enum class DatagramReceiveResult { packet, empty, timeout, interrupted, fatal };

// Error codes belong only to negative syscall results. Callers must snapshot
// errno immediately after recvfrom, before clocks, restart callbacks or logs.
[[nodiscard]] inline DatagramReceiveResult classify_datagram_receive(
    std::ptrdiff_t received, int receive_errno) noexcept {
  if (received > 0) return DatagramReceiveResult::packet;
  if (received == 0) return DatagramReceiveResult::empty;
  if (receive_errno == EAGAIN || receive_errno == EWOULDBLOCK)
    return DatagramReceiveResult::timeout;
  if (receive_errno == EINTR) return DatagramReceiveResult::interrupted;
  return DatagramReceiveResult::fatal;
}
}  // namespace vfdual_android
