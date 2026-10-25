#pragma once

#include <cstdint>

namespace vfdual {

// UI-thread ownership of an asynchronous operation and its queued completion.
// Cancellation preserves ownership until that worker is reaped; invalidation
// relinquishes it before a new operation can acquire the shared UI/thread slot.
class HostOperationOwnership final {
public:
    [[nodiscard]] std::uint64_t begin() noexcept {
        ++generation_;
        active_ = true;
        cancelled_ = false;
        return generation_;
    }
    void cancel() noexcept { cancelled_ = true; }
    void invalidate() noexcept {
        ++generation_;
        active_ = false;
        cancelled_ = true;
    }
    [[nodiscard]] bool owns(std::uint64_t generation) const noexcept {
        return active_ && generation == generation_;
    }
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }
    void finish(std::uint64_t generation) noexcept {
        if (owns(generation)) active_ = false;
    }
private:
    std::uint64_t generation_{};
    bool active_{};
    bool cancelled_{};
};

}  // namespace vfdual
