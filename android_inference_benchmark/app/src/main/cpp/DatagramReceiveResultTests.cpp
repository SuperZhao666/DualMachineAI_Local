#include "DatagramReceiveResult.hpp"

#include <cassert>
#include <initializer_list>

using namespace vfdual_android;
int main() {
  for (const int inherited : {0, EAGAIN, EINTR, EBADF}) {
    for (unsigned empty_packets = 0; empty_packets < 8; ++empty_packets) {
      assert(classify_datagram_receive(0, inherited) == DatagramReceiveResult::empty);
    }
    assert(classify_datagram_receive(128, inherited) == DatagramReceiveResult::packet);
  }
  // A restart completion callback can overwrite global errno after recvfrom.
  // Only the captured error determines the classification in either direction.
  errno = EAGAIN;
  const int timeout_error = errno;
  errno = EBADF;
  assert(classify_datagram_receive(-1, timeout_error) == DatagramReceiveResult::timeout);
  const int fatal_error = errno;
  errno = EAGAIN;
  assert(classify_datagram_receive(-1, fatal_error) == DatagramReceiveResult::fatal);
  assert(classify_datagram_receive(-1, EWOULDBLOCK) == DatagramReceiveResult::timeout);
  assert(classify_datagram_receive(-1, EINTR) == DatagramReceiveResult::interrupted);
  assert(classify_datagram_receive(-1, 0) == DatagramReceiveResult::fatal);
  return 0;
}
