#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vfdual {

/**
 * v5.1-compatible wire contract:
 * [magic:u32][frame_id:u32][frag_idx:u16][frag_cnt:u16].
 *
 * Normal frames retain the v5.0 magic. Repeated-content frames use a distinct
 * magic and the high frame-id bit, so an older receiver safely rejects only
 * synthetic repeats instead of misclassifying them as control observations.
 * The high bit is not part of the logical sequence.
 */
constexpr std::uint32_t kVideoPacketMagic = 0x56465247U;  // "VFRG"
constexpr std::uint32_t kVideoRepeatedPacketMagic = 0x56465252U;  // "VFRR"
// v5.2 adds the host capture-to-send duration, not an unsynchronized clock.
constexpr std::uint32_t kVideoTimedPacketMagic = 0x56465447U;  // "VFTG"
constexpr std::uint32_t kVideoTimedRepeatedPacketMagic = 0x56465452U;  // "VFTR"
// Explicit physical capture dimensions prevent a new model from silently
// applying its mouse calibration to an older, differently scaled Host stream.
constexpr std::uint32_t kVideoViewPacketMagic = 0x56465347U;  // "VFSG"
constexpr std::uint32_t kVideoViewRepeatedPacketMagic = 0x56465352U;  // "VFSR"
// Keep desktop crop, encoded frame, detector input and control coordinates at
// the same pixel density. Older 1024-crop streams are rejected by this contract.
constexpr std::uint16_t kProductionCaptureWidth = 640U;
constexpr std::uint16_t kProductionCaptureHeight = 640U;
constexpr std::uint32_t kUnknownVideoFrameAgeUs = 0xffff'ffffU;
constexpr std::uint32_t kVideoRepeatedContentFlag = 0x8000'0000U;
constexpr std::uint32_t kVideoFrameSequenceMask = 0x7fff'ffffU;
constexpr std::uint32_t kVideoFrameSequenceHalfRange = 0x4000'0000U;
constexpr std::size_t kVideoPacketHeaderBytes = 12;
constexpr std::size_t kVideoTimedPacketHeaderBytes = 16;
constexpr std::size_t kVideoViewPacketHeaderBytes = 20;
constexpr std::size_t kVideoPacketPayloadBytes = 1400;
constexpr std::size_t kMaxDatagramBytes = kVideoViewPacketHeaderBytes + kVideoPacketPayloadBytes;
constexpr std::size_t kMaxAccessUnitBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaxVideoFragmentsPerAccessUnit =
    (kMaxAccessUnitBytes + kVideoPacketPayloadBytes - 1U) /
    kVideoPacketPayloadBytes;

struct VideoFragment {
    std::uint32_t frame_id{};
    std::uint16_t fragment_index{};
    std::uint16_t fragment_count{};
    std::vector<std::byte> access_unit_part;
    std::uint32_t host_frame_age_us{kUnknownVideoFrameAgeUs};
    std::uint16_t capture_width{}, capture_height{};
};

[[nodiscard]] constexpr bool production_capture_geometry_matches(const VideoFragment& frame) noexcept {
    return frame.capture_width == kProductionCaptureWidth && frame.capture_height == kProductionCaptureHeight;
}

[[nodiscard]] constexpr std::uint32_t video_host_frame_age_us(
    std::uint64_t published_us, std::uint64_t captured_us) noexcept {
    if (captured_us == 0 || published_us < captured_us) return kUnknownVideoFrameAgeUs;
    const auto age = published_us - captured_us;
    return age >= kUnknownVideoFrameAgeUs ? kUnknownVideoFrameAgeUs - 1U : static_cast<std::uint32_t>(age);
}

// Upper estimate of observation time on the receiver's clock. Network transit
// is not measured here; this corrects host backlog without claiming clock sync.
[[nodiscard]] constexpr std::uint64_t estimate_video_observation_us(
    std::uint64_t first_received_us, std::uint32_t host_frame_age_us) noexcept {
    if (host_frame_age_us == kUnknownVideoFrameAgeUs) return first_received_us;
    return first_received_us > host_frame_age_us ? first_received_us - host_frame_age_us : 0U;
}

[[nodiscard]] constexpr std::uint32_t make_video_wire_frame_id(
    std::uint32_t logical_sequence, bool repeated_content) noexcept {
  return (logical_sequence & kVideoFrameSequenceMask) |
      (repeated_content ? kVideoRepeatedContentFlag : 0U);
}

[[nodiscard]] constexpr std::uint32_t video_logical_frame_sequence(
    std::uint32_t wire_frame_id) noexcept {
  return wire_frame_id & kVideoFrameSequenceMask;
}

[[nodiscard]] constexpr bool video_repeats_content(
    std::uint32_t wire_frame_id) noexcept {
  return (wire_frame_id & kVideoRepeatedContentFlag) != 0U;
}

/**
 * Advances the unsigned logical sequence inside the 31-bit wire domain.
 *
 * Keeping the producer counter in that domain makes the host metric and the
 * receiver-visible sequence identical even after a very long-running stream.
 */
[[nodiscard]] constexpr std::uint32_t next_video_logical_frame_sequence(
    std::uint32_t logical_sequence) noexcept {
  return (logical_sequence + 1U) & kVideoFrameSequenceMask;
}

/** RFC-1982-style ordering for the 31-bit video sequence number. */
[[nodiscard]] constexpr bool video_logical_frame_sequence_is_newer(
    std::uint32_t candidate, std::uint32_t reference) noexcept {
  const std::uint32_t distance =
      (candidate - reference) & kVideoFrameSequenceMask;
  return distance != 0U && distance < kVideoFrameSequenceHalfRange;
}

/**
 * Serializes one v5.0 UDP datagram into caller-owned storage.
 *
 * This allocation-free overload is the production hot-path contract. It
 * returns the encoded byte count, or zero when any field/storage is invalid.
 */
[[nodiscard]] std::size_t encode_video_packet_into(
    std::uint32_t frame_id,
    std::uint16_t fragment_index,
    std::uint16_t fragment_count,
    std::span<const std::byte> access_unit_part,
    std::span<std::byte> destination,
    std::uint32_t host_frame_age_us = kUnknownVideoFrameAgeUs,
    std::uint16_t capture_width = 0, std::uint16_t capture_height = 0) noexcept;

/** Serializes exactly one UDP datagram according to the fixed v5.0 header. */
[[nodiscard]] std::vector<std::byte> encode_video_packet(const VideoFragment& fragment);
[[nodiscard]] bool decode_video_packet(std::span<const std::byte> datagram, VideoFragment& destination) noexcept;

}  // namespace vfdual
