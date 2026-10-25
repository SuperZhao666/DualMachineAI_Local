#pragma once

#include "vfdual/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace vfdual {

struct CompletedAccessUnit {
    std::uint32_t frame_id{};
    std::vector<std::byte> bytes;
    std::uint64_t first_received_us{};
    std::uint32_t host_frame_age_us{kUnknownVideoFrameAgeUs};
    std::uint16_t capture_width{}, capture_height{};
};

/** Bounded, decode-order-preserving reassembly for UDP encoded access units. */
class AccessUnitReassembler final {
public:
    struct DiscardClassification {
        std::uint64_t ttl_complete{}, ttl_missing_fragments{};
        std::uint64_t capacity_complete{}, capacity_missing_fragments{};
        std::uint64_t maximum_ttl_age_us{};
    };
    explicit AccessUnitReassembler(std::size_t max_inflight_frames = 8) noexcept;
    /** Takes fragment ownership so the receiver performs no second payload allocation/copy. */
    [[nodiscard]] std::optional<CompletedAccessUnit> push(
        VideoFragment fragment, std::uint64_t now_us);
    /** Preserves decode order; a complete H264 IDR may bypass a missing reference. */
    [[nodiscard]] std::optional<CompletedAccessUnit> pop_completed();
    [[nodiscard]] std::size_t discard_expired(
        std::uint64_t now_us, std::uint64_t ttl_us = 50'000) noexcept;
    [[nodiscard]] std::size_t inflight_frame_count() const noexcept;
    /** Cumulative incomplete AUs lost to conflict, bounds, or expiry. */
    [[nodiscard]] std::uint64_t incomplete_access_unit_losses() const noexcept;
    /** Obsolete buffered AUs skipped at a complete, independent IDR boundary. */
    [[nodiscard]] std::uint64_t idr_resynchronization_discards() const noexcept;
    /** Classification at discard time, not a claim that UDP lost a fragment. */
    [[nodiscard]] const DiscardClassification& discard_classification() const noexcept;

private:
    struct PendingFrame {
        std::uint16_t expected_fragments{};
        std::uint16_t received_fragments{};
        std::size_t total_bytes{};
        std::uint64_t first_seen_us{};
        std::uint32_t host_frame_age_us{kUnknownVideoFrameAgeUs};
        std::uint16_t capture_width{}, capture_height{};
        std::vector<bool> received;
        std::vector<std::vector<std::byte>> parts;
        std::vector<std::byte> assembled_bytes;
        std::optional<bool> contains_idr;
    };

    static std::vector<std::byte>& materialize(PendingFrame& pending);
    void discard_oldest() noexcept;

    std::size_t max_inflight_frames_{};
    std::uint64_t incomplete_access_unit_losses_{};
    std::uint64_t idr_resynchronization_discards_{};
    DiscardClassification discard_classification_{};
    std::optional<std::uint32_t> last_delivered_sequence_;
    std::unordered_map<std::uint32_t, PendingFrame> pending_;
};

}  // namespace vfdual
