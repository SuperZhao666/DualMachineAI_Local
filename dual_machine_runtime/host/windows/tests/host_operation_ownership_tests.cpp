#include "vfdual/host_operation_ownership.hpp"

#include <cstdlib>
#include <iostream>

void check(bool value) { if (!value) std::abort(); }

int main() {
    vfdual::HostOperationOwnership owner;
    const auto old_refresh = owner.begin();
    // Reaper reclaims a finished worker before its queued message arrives.
    owner.invalidate();
    const auto new_refresh = owner.begin();
    unsigned joined_new_worker{};
    unsigned reset_new_ui{};
    if (owner.owns(old_refresh)) { ++joined_new_worker; ++reset_new_ui; }
    check(joined_new_worker == 0 && reset_new_ui == 0 && owner.owns(new_refresh));
    // Starting transmission takes ownership of UI before the old message.
    owner.invalidate();
    check(!owner.owns(new_refresh));
    // Cancellation retains ownership to complete a deferred start exactly once.
    const auto cancelled_refresh = owner.begin();
    owner.cancel();
    unsigned starts{};
    for (unsigned duplicate{}; duplicate < 2; ++duplicate) {
        if (owner.owns(cancelled_refresh)) {
            check(owner.cancelled());
            owner.finish(cancelled_refresh);
            ++starts;
        }
    }
    check(starts == 1 && !owner.owns(cancelled_refresh));
    const auto next = owner.begin();
    check(!owner.cancelled() && owner.owns(next));
    owner.finish(old_refresh);
    check(owner.owns(next));
    std::cout << "PASS reaped/queued ownership, stale worker isolation, cancelled continuation\n";
}
