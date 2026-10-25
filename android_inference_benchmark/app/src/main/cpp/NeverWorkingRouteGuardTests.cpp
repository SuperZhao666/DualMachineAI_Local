#include "NeverWorkingRouteGuard.hpp"
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <thread>
using vfdual_android::NeverWorkingRouteGuard;
void require(bool ok){if(!ok)std::abort();}
int main(){
  const NeverWorkingRouteGuard::Scope a{41,0xc0a8012aU,0xc0a80116U},b{42,0xc0a8012aU,0xc0a80116U};
  NeverWorkingRouteGuard guard;
  auto generation=guard.begin(a);require(guard.admit(generation));require(!guard.claim(a));
  generation=guard.begin(a);require(guard.claim(a));require(!guard.admit(generation));require(!guard.claim(a));
  generation=guard.begin(a);require(!guard.claim(b));require(guard.admit(generation));
  const auto old=generation;generation=guard.begin(a);guard.end(old);
  require(!guard.admit(old));require(guard.admit(generation));
  guard.end(generation);require(!guard.claim(a));
  for(unsigned trial=0;trial<300;++trial){
    const auto current=guard.begin(a);std::latch go{1};bool admitted{},claimed{};
    std::thread receiver([&]{go.wait();admitted=guard.admit(current);});
    std::thread service([&]{go.wait();claimed=guard.claim(a);});
    go.count_down();receiver.join();service.join();
    require(admitted!=claimed);
    if(claimed)require(!guard.admit(current));else require(!guard.claim(a));
  }
  std::puts("PASS production first-packet/rediscovery atomic handoff, scope and stale-generation guards");
}
