#include "vfdual/access_unit_fragmenter.hpp"
#include "vfdual/access_unit_reassembler.hpp"
#include "vfdual/h264_access_unit.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
int failures{};
void check(bool ok, const char* name) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    failures += !ok;
}
std::vector<vfdual::VideoFragment> frame(std::uint32_t id, bool idr,
                                       std::size_t parts = 3) {
    std::vector<std::byte> bytes((parts - 1) * 1400 + 6, std::byte{0x80});
    bytes[0] = bytes[1] = bytes[2] = std::byte{0};
    bytes[3] = std::byte{1};
    bytes[4] = idr ? std::byte{0x65} : std::byte{0x41};
    auto result = vfdual::fragment_access_unit(id, bytes);
    for (auto& part : result) part.host_frame_age_us = 5'000;
    return result;
}
std::optional<vfdual::CompletedAccessUnit> send(
    vfdual::AccessUnitReassembler& receiver, std::uint32_t id, bool idr,
    std::uint64_t now) {
    std::optional<vfdual::CompletedAccessUnit> result;
    for (auto& fragment : frame(id, idr)) {
        auto completed = receiver.push(std::move(fragment), now++);
        if (completed) result = std::move(completed);
    }
    return result;
}

void recovery_bypasses_incomplete_prediction() {
    vfdual::AccessUnitReassembler receiver(4);
    (void)send(receiver, 99, true, 1'000);
    (void)receiver.push(frame(100, false)[0], 10'000);
    auto idr = send(receiver, 101, true, 20'000);
    check(idr && idr->frame_id == 101, "complete IDR bypasses one lost P fragment");
    check(idr && idr->first_received_us == 20'000 && idr->host_frame_age_us == 5'000,
          "recovered IDR preserves capture-age metadata");
    auto prediction = send(receiver, 102, false, 30'000);
    check(prediction && prediction->frame_id == 102, "consecutive P resumes after IDR");
    auto before = receiver.incomplete_access_unit_losses();
    (void)receiver.push(frame(100, false)[1], 31'000);
    (void)receiver.discard_expired(100'000);
    check(receiver.inflight_frame_count() == 0 &&
          receiver.incomplete_access_unit_losses() == before,
          "late fragment cannot resurrect pre-IDR frame");
}

void wholly_missing_frame_and_wrap() {
    for (std::uint32_t seed : {40U, vfdual::kVideoFrameSequenceMask - 1U}) {
        vfdual::AccessUnitReassembler receiver(4);
        (void)send(receiver, seed, true, 1'000);
        auto gap = vfdual::next_video_logical_frame_sequence(seed);
        auto predicted = vfdual::next_video_logical_frame_sequence(gap);
        auto sync = vfdual::next_video_logical_frame_sequence(predicted);
        check(!send(receiver, predicted, false, 20'000),
              "P frame never jumps a wholly missing sequence");
        auto idr = send(receiver, vfdual::make_video_wire_frame_id(sync, true), true, 30'000);
        check(idr && vfdual::video_logical_frame_sequence(idr->frame_id) == sync &&
              vfdual::video_repeats_content(idr->frame_id),
              "repeat-marked IDR bypasses whole-frame gap including sequence wrap");
    }
}

void preserve_partly_arrived_idr_under_bounds_pressure() {
    vfdual::AccessUnitReassembler receiver(4);
    (void)send(receiver, 99, true, 1'000);
    // Out-of-order old fragments arrive after the new IDR has started. Bounds
    // eviction must retire the oldest logical frame, not the first arrival.
    auto idr = frame(102, true);
    (void)receiver.push(idr[0], 20'000);
    (void)receiver.push(frame(100, false)[0], 21'000);
    (void)receiver.push(frame(101, false)[0], 22'000);
    (void)receiver.push(frame(103, false)[0], 23'000);
    (void)receiver.push(frame(104, false)[0], 24'000);
    (void)receiver.push(idr[2], 25'000);
    auto completed = receiver.push(idr[1], 26'000);
    check(completed && completed->frame_id == 102,
          "bounds loss does not evict an already arriving newer IDR");
    check(receiver.inflight_frame_count() <= 4, "recovery memory stays bounded");
}

void reordered_prediction_still_waits_for_its_reference() {
    vfdual::AccessUnitReassembler receiver(4);
    (void)send(receiver, 49, true, 1'000);
    auto old = frame(50, false);
    (void)receiver.push(old[0], 10'000);
    check(!send(receiver, 51, false, 11'000), "completed P waits for earlier fragments");
    (void)receiver.push(old[2], 12'000);
    auto first = receiver.push(old[1], 13'000);
    auto next = receiver.pop_completed();
    check(first && first->frame_id == 50 && next && next->frame_id == 51,
          "ordinary reordering retains exact H264 decode order");
}

void expired_predecessor_does_not_block_recovery() {
    vfdual::AccessUnitReassembler receiver(4);
    (void)send(receiver, 69, true, 1'000);
    (void)receiver.push(frame(70, false)[0], 2'000);
    auto idr = frame(71, true);
    (void)receiver.push(idr[0], 45'000);
    check(receiver.discard_expired(53'000, 50'000) == 1,
          "only expired predecessor is discarded");
    (void)receiver.push(idr[1], 54'000);
    auto complete = receiver.push(idr[2], 55'000);
    check(complete && complete->frame_id == 71,
          "IDR can finish after predecessor expiry without global reset");
}

void discard_diagnostics_distinguish_blocked_complete_frames() {
    vfdual::AccessUnitReassembler ttl(4);
    (void)send(ttl, 1, true, 100);
    (void)ttl.push(frame(2, false)[0], 1'000);
    check(!send(ttl, 3, false, 2'000), "complete frame remains blocked behind partial predecessor");
    check(ttl.discard_expired(52'003, 50'000) == 2, "TTL is unchanged for complete blocked and partial frames");
    const auto& expired = ttl.discard_classification();
    check(expired.ttl_complete == 1 && expired.ttl_missing_fragments == 1 &&
          expired.capacity_complete == 0 && expired.capacity_missing_fragments == 0,
          "TTL diagnostics distinguish1complete blocked AU from1partial AU");
    check(expired.maximum_ttl_age_us == 51'003 && ttl.incomplete_access_unit_losses() == 2,
          "TTL diagnostics preserve aggregate loss semantics and record actual age");

    vfdual::AccessUnitReassembler bounded(2);
    (void)send(bounded, 1, true, 100);
    (void)bounded.push(frame(2, false)[0], 1'000);
    (void)send(bounded, 3, false, 2'000);
    (void)bounded.push(frame(4, false)[0], 3'000);
    (void)bounded.push(frame(5, false)[0], 4'000);
    const auto& capacity = bounded.discard_classification();
    check(capacity.capacity_complete == 1 && capacity.capacity_missing_fragments == 1 &&
          capacity.ttl_complete == 0 && capacity.ttl_missing_fragments == 0,
          "capacity diagnostics distinguish1complete blocked AU from1partial AU");
    check(bounded.inflight_frame_count() == 2 && bounded.incomplete_access_unit_losses() == 2,
          "diagnostics never enlarge the inflight capacity or release dependent frames");
}
}  // namespace

int main() {
    recovery_bypasses_incomplete_prediction();
    wholly_missing_frame_and_wrap();
    preserve_partly_arrived_idr_under_bounds_pressure();
    reordered_prediction_still_waits_for_its_reference();
    expired_predecessor_does_not_block_recovery();
    discard_diagnostics_distinguish_blocked_complete_frames();
    std::cout << "failures=" << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
