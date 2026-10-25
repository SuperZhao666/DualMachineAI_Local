#include "DecoderCallbackOwnership.hpp"
#include <cassert>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <cstdio>
using namespace vfdual_android;
int main() {
  for(bool compressed:{false,true}) {
    DecoderCallbackOwnership ownership;
    std::uint64_t binding=1,stream=10;bool active=true,fatal=false;
    std::mutex mutex;std::condition_variable cv;bool captured=false,resume=false;
    const DecoderCallbackToken old{1,10,1},replacement{1,11,2};
    assert(ownership.publish(old,binding,stream,active));
    std::thread stale([&] {
      std::unique_lock lock(mutex);captured=true;cv.notify_one();cv.wait(lock,[&]{return resume;});
      if(ownership.accepts(old,binding,stream,active))fatal=true;
    });
    {
      std::unique_lock lock(mutex);cv.wait(lock,[&]{return captured;});
      stream=11;assert(ownership.publish(replacement,binding,stream,active));fatal=false;resume=true;
    }
    cv.notify_one();stale.join();assert(!fatal);
    // Current fatal does not require any pending frame/PTS match.
    {std::scoped_lock lock(mutex);if(ownership.accepts(replacement,binding,stream,active))fatal=true;}
    assert(fatal);active=false;fatal=false;assert(!ownership.accepts(replacement,binding,stream,active));
    active=true;assert(ownership.publish({1,11,3},binding,stream,active));
    assert(!ownership.publish(replacement,binding,stream,active)); // Late publication cannot replace newer codec.
    assert(!ownership.accepts(replacement,binding,stream,active));
    binding=2;assert(!ownership.accepts({1,11,3},binding,stream,active));
    assert(ownership.publish({2,11,1},binding,stream,active));
    assert(!ownership.publish({0,11,1},binding,stream,active));
    std::printf("PASS %s stale_fatal_rejected current_fatal_without_pts_allowed stop_rejected late_codec_publish_rejected\n",compressed?"compressed":"decoded");
  }
}
