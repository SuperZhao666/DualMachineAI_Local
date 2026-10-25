#include "CoVisibleTrackHistory.hpp"
#include <cassert>

int main() {
  vfdual_android::CoVisibleTrackHistory<2> history;
  history.remember(50,100,500);
  assert(history.contains(50) && !history.contains(1) && !history.contains(49));
  history.observe(50,400,500);
  history.advance(601,500);
  assert(history.contains(50));
  history.advance(900,500);
  assert(history.contains(50)); // the tracker also retains the exact boundary
  history.advance(901,500);
  assert(!history.contains(50));
  history.remember(60,1000,500);
  history.remember(70,1000,500);
  history.remember(80,1000,500);
  assert(history.size()==2 && history.incomplete() && !history.contains(80));
  assert(!history.contains(1) && !history.contains(69));
  history.observe(80,1400,500);
  history.advance(1501,500);
  assert(history.incomplete()); // omitted but still visible identity extends uncertainty
  history.advance(1900,500);
  assert(history.incomplete());
  history.advance(1901,500);
  assert(!history.incomplete() && history.size()==0);
  history.remember(5,2000,0);
  history.remember(6,2000,0);
  history.remember(7,2000,0);
  history.advance(1'000'000,0);
  assert(history.incomplete()); // no invented wall-clock TTL for count-only policy
  history.clear();
  assert(!history.incomplete() && history.size()==0);
  return 0;
}
