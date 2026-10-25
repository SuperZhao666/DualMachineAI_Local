#include "vfdual/access_unit_reassembler.hpp"
#include "vfdual/h264_access_unit.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace vfdual {
AccessUnitReassembler::AccessUnitReassembler(std::size_t max_inflight_frames) noexcept
    : max_inflight_frames_(std::max<std::size_t>(1, max_inflight_frames)) {}

std::optional<CompletedAccessUnit> AccessUnitReassembler::push(
    VideoFragment fragment, std::uint64_t now_us) {
    if (fragment.fragment_count == 0 ||
        fragment.fragment_count > kMaxVideoFragmentsPerAccessUnit ||
        fragment.fragment_index >= fragment.fragment_count ||
        fragment.access_unit_part.empty() ||
        fragment.access_unit_part.size() > kVideoPacketPayloadBytes) {
        return std::nullopt;
    }
    const std::uint32_t logical_sequence =
        video_logical_frame_sequence(fragment.frame_id);
    if (last_delivered_sequence_.has_value() &&
        !video_logical_frame_sequence_is_newer(
            logical_sequence, *last_delivered_sequence_)) {
        // Late UDP duplicates must not create a second incomplete AU that
        // expires later and falsely breaks a healthy reference chain.
        return pop_completed();
    }
    auto [iterator, inserted] = pending_.try_emplace(fragment.frame_id);
    auto& pending = iterator->second;
    if (inserted) {
        pending.expected_fragments = fragment.fragment_count;
        pending.first_seen_us = now_us;
        pending.host_frame_age_us = fragment.host_frame_age_us;
        pending.capture_width = fragment.capture_width;
        pending.capture_height = fragment.capture_height;
        pending.received.assign(fragment.fragment_count, false);
        pending.parts.resize(fragment.fragment_count);
    }
    if (pending.expected_fragments != fragment.fragment_count ||
        pending.host_frame_age_us != fragment.host_frame_age_us ||
        pending.capture_width != fragment.capture_width || pending.capture_height != fragment.capture_height) {
        pending_.erase(iterator);
        ++incomplete_access_unit_losses_;
        return std::nullopt;
    }
    if (!pending.received[fragment.fragment_index]) {
        const std::size_t part_size = fragment.access_unit_part.size();
        if (pending.total_bytes > kMaxAccessUnitBytes - part_size) {
            pending_.erase(iterator);
            ++incomplete_access_unit_losses_;
            return std::nullopt;
        }
        pending.received[fragment.fragment_index] = true;
        pending.parts[fragment.fragment_index] =
            std::move(fragment.access_unit_part);
        pending.total_bytes += part_size;
        ++pending.received_fragments;
    }
    while (pending_.size() > max_inflight_frames_) discard_oldest();
    return pop_completed();
}

std::optional<CompletedAccessUnit> AccessUnitReassembler::pop_completed() {
    if (pending_.empty()) return std::nullopt;
    auto oldest = pending_.end();
    if (last_delivered_sequence_.has_value()) {
        const std::uint32_t expected = next_video_logical_frame_sequence(
            *last_delivered_sequence_);
        oldest = std::find_if(
            pending_.begin(), pending_.end(), [expected](const auto& candidate) {
                return video_logical_frame_sequence(candidate.first) == expected;
            });
    } else {
        oldest = std::min_element(
            pending_.begin(), pending_.end(), [](const auto& left, const auto& right) {
                return video_logical_frame_sequence_is_newer(
                    video_logical_frame_sequence(right.first),
                    video_logical_frame_sequence(left.first));
            });
    }
    if (oldest == pending_.end() ||
        oldest->second.received_fragments != oldest->second.expected_fragments) {
        // A wholly missing sequence has no PendingFrame to expire. Waiting for
        // it forever also blocks the very IDR requested to recover the stream.
        // Only an independent, fully assembled IDR may jump this gap. Prefer
        // the newest one and retain any newer dependent frames already in flight.
        oldest = pending_.end();
        for (auto candidate = pending_.begin(); candidate != pending_.end(); ++candidate) {
            auto& frame = candidate->second;
            if (frame.received_fragments != frame.expected_fragments) continue;
            if (!frame.contains_idr.has_value()) {
                frame.contains_idr = h264_access_unit_contains_idr(materialize(frame));
            }
            if (!*frame.contains_idr) continue;
            if (oldest == pending_.end() || video_logical_frame_sequence_is_newer(
                    video_logical_frame_sequence(candidate->first),
                    video_logical_frame_sequence(oldest->first))) {
                oldest = candidate;
            }
        }
        if (oldest == pending_.end()) return std::nullopt;
    }
    CompletedAccessUnit completed{oldest->first, {}};
    completed.first_received_us = oldest->second.first_seen_us;
    completed.host_frame_age_us = oldest->second.host_frame_age_us;
    completed.capture_width = oldest->second.capture_width;
    completed.capture_height = oldest->second.capture_height;
    completed.bytes = std::move(materialize(oldest->second));
    last_delivered_sequence_ = video_logical_frame_sequence(oldest->first);
    pending_.erase(oldest);
    for (auto iterator = pending_.begin(); iterator != pending_.end();) {
        if (!video_logical_frame_sequence_is_newer(
                video_logical_frame_sequence(iterator->first), *last_delivered_sequence_)) {
            iterator = pending_.erase(iterator);
            ++idr_resynchronization_discards_;
        } else {
            ++iterator;
        }
    }
    return completed;
}

std::vector<std::byte>& AccessUnitReassembler::materialize(PendingFrame& pending) {
    if (pending.assembled_bytes.empty()) {
        pending.assembled_bytes.reserve(pending.total_bytes);
        for (const auto& part : pending.parts) {
            pending.assembled_bytes.insert(pending.assembled_bytes.end(), part.begin(), part.end());
        }
        pending.parts.clear();
    }
    return pending.assembled_bytes;
}

std::size_t AccessUnitReassembler::discard_expired(
    std::uint64_t now_us, std::uint64_t ttl_us) noexcept {
    std::size_t discarded{};
    for (auto iterator = pending_.begin(); iterator != pending_.end();) {
        const bool expired = now_us >= iterator->second.first_seen_us && now_us - iterator->second.first_seen_us > ttl_us;
        if (!expired) {
            ++iterator;
            continue;
        }
        if (iterator->second.received_fragments == iterator->second.expected_fragments) {
            ++discard_classification_.ttl_complete;
        } else {
            ++discard_classification_.ttl_missing_fragments;
        }
        discard_classification_.maximum_ttl_age_us = std::max(
            discard_classification_.maximum_ttl_age_us, now_us - iterator->second.first_seen_us);
        iterator = pending_.erase(iterator);
        ++discarded;
    }
    incomplete_access_unit_losses_ += discarded;
    return discarded;
}

std::size_t AccessUnitReassembler::inflight_frame_count() const noexcept { return pending_.size(); }

std::uint64_t AccessUnitReassembler::incomplete_access_unit_losses() const noexcept {
    return incomplete_access_unit_losses_;
}

std::uint64_t AccessUnitReassembler::idr_resynchronization_discards() const noexcept {
    return idr_resynchronization_discards_;
}

const AccessUnitReassembler::DiscardClassification&
AccessUnitReassembler::discard_classification() const noexcept {
    return discard_classification_;
}

void AccessUnitReassembler::discard_oldest() noexcept {
    if (pending_.empty()) return;
    const auto oldest = std::min_element(pending_.begin(), pending_.end(), [](const auto& left, const auto& right) {
        return video_logical_frame_sequence_is_newer(
            video_logical_frame_sequence(right.first),
            video_logical_frame_sequence(left.first));
    });
    if (oldest->second.received_fragments == oldest->second.expected_fragments) {
        ++discard_classification_.capacity_complete;
    } else {
        ++discard_classification_.capacity_missing_fragments;
    }
    pending_.erase(oldest);
    ++incomplete_access_unit_losses_;
}
}  // namespace vfdual
