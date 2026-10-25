#include "DecodedObservationAdmission.hpp"

#include <cstdint>
#include <iostream>
#include <limits>

int main() {
  using vfdual_android::decoded_observation_is_fresh;
  int failures{};
  const auto check = [&](bool expected, std::uint64_t receive, std::uint64_t observe,
                         std::uint64_t now, const char* name) {
    const bool actual = decoded_observation_is_fresh(receive, observe, now, 50'000U);
    if (actual != expected) { ++failures; std::cerr << "FAIL " << name << '\n'; }
  };
  check(false, 100'000, 40'000, 115'000, "fresh receive cannot revive expired Host observation");
  check(true, 100'000, 65'000, 115'000, "exact existing 50ms boundary remains admitted");
  check(false, 100'000, 64'999, 115'000, "one microsecond past observation boundary");
  check(true, 100'000, 95'000, 115'000, "normal fresh frame remains admitted");
  check(false, 100'000, 115'001, 115'000, "future observation rejected without unsigned wrap");
  check(false, 115'001, 100'000, 115'000, "future receive rejected");
  check(false, 64'999, 100'000, 115'000, "expired decoder receive still rejected");
  const auto last = std::numeric_limits<std::uint64_t>::max();
  check(true, last - 20'000, last - 50'000, last, "near UINT64_MAX subtraction stays valid");
  check(false, 0, 0, last, "large age cannot wrap into freshness");
  std::cout << "decoded_observation_admission cases=9 failures=" << failures
            << " scope=admission_only_not_device_latency\n";
  return failures ? 1 : 0;
}
