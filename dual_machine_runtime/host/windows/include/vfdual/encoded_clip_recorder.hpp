#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace vfdual {
// Explicit diagnostic only: copy the already encoded game crop into bounded
// memory. Disk output occurs on a separate thread after capture has stopped.
// No additional screen capture, GPU context, encoder, or network listener.
class EncodedClipRecorder final {
public:
    EncodedClipRecorder() noexcept;
    ~EncodedClipRecorder();
    void append(std::uint32_t frame_id, std::span<const std::byte> access_unit,
                std::uint64_t published_us, std::uint64_t captured_us,
                bool repeated) noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
