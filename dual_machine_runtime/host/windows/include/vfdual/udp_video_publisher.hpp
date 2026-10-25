#pragma once

#include "vfdual/udp_socket.hpp"
#include "vfdual/encoded_clip_recorder.hpp"

#include <cstdint>
#include <span>

namespace vfdual {


struct VideoPublishResult {
    std::uint16_t fragments_sent{};
    std::uint32_t bytes_sent{};
    bool success{};
    enum class Stage : std::uint8_t {
        none,
        validate_input,
        encode_datagram,
        socket_send,
        complete,
    } stage{Stage::none};
};

/** Windows transport adapter. It accepts encoded access units only; it never captures or re-encodes frames. */
class UdpVideoPublisher final {
public:
    UdpVideoPublisher() noexcept = default;
    [[nodiscard]] bool connect_to(
        std::string_view host, std::uint16_t port, std::uint16_t local_port = 0,
        std::string_view local_host = {}) noexcept;
    void reset() noexcept;
    void set_capture_geometry(std::uint16_t width, std::uint16_t height) noexcept {
        capture_width_ = width; capture_height_ = height;
    }
    [[nodiscard]] VideoPublishResult publish(
        std::uint32_t frame_id, std::span<const std::byte> access_unit,
        std::uint64_t monotonic_us, bool repeated_content = false,
        std::uint64_t capture_present_us = 0U) noexcept;
    [[nodiscard]] std::uint32_t last_socket_error() const noexcept;
    [[nodiscard]] std::uint16_t local_port() const noexcept;

private:

    UdpSocket socket_;
    std::uint16_t capture_width_{}, capture_height_{};
    EncodedClipRecorder diagnostic_clip_;
};

}  // namespace vfdual
