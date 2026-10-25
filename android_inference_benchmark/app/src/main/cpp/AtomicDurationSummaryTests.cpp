#include "AtomicDurationSummary.hpp"
#include <cassert>
#include <sstream>

int main() {
  vfdual_android::AtomicDurationSummary summary;
  for (const auto value : {0U, 2'000U, 2'001U, 50'000U, 50'001U}) summary.record(value);
  std::ostringstream out;
  summary.append(out, "wait");
  assert(out.str() == " wait_count=5 wait_total_us=104002 wait_maximum_us=50001 wait_over_2ms=3 wait_over_50ms=1");
  summary.reset();
  std::ostringstream reset;
  summary.append(reset, "wait");
  assert(reset.str() == " wait_count=0 wait_total_us=0 wait_maximum_us=0 wait_over_2ms=0 wait_over_50ms=0");
  { vfdual_android::AtomicDurationScope scope(summary); }
  std::ostringstream scoped;
  summary.append(scoped, "scope");
  assert(scoped.str().find(" scope_count=1 ") == 0);
}
