#include "vfdual/host_runtime_service.hpp"

#include <cstdlib>
#include <iostream>
#include <latch>
#include <stop_token>
#include <string>
#include <thread>

int main() {
    // Force the reported ordering: Stop arrives after UI worker creation,
    // before the actual production start() entry. No capture/network is used.
    for (unsigned trial{}; trial < 128; ++trial) {
        vfdual::HostRuntimeService service;
        std::stop_source operation;
        std::latch at_entry{1}, release{1};
        bool started = true;
        std::string error;
        std::thread worker([&] {
            at_entry.count_down();
            release.wait();
            started = service.start({}, error, operation.get_token());
        });
        at_entry.wait();
        operation.request_stop();
        release.count_down();
        worker.join();
        if (started || service.is_running() || error != vfdual::kHostStartupCancelledError)
            std::abort();
        // Cancellation belongs to the old operation, not every future start.
        // Invalid settings reject before any application/bootstrap side effect.
        std::stop_source next;
        if (service.start({}, error, next.get_token()) || error == vfdual::kHostStartupCancelledError)
            std::abort();
    }
    std::cout << "PASS production HostRuntimeService early stop latch: 128 operations\n";
}
