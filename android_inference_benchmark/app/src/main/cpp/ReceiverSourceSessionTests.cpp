#include "ReceiverSourceSession.hpp"
#include "ReceiverReferenceSyncPolicy.hpp"
#include "vfdual/access_unit_reassembler.hpp"

#include <cassert>
#include <cstdio>

namespace {
using vfdual_android::ReceiverSourceDecision;
using vfdual_android::ReceiverSourceSession;
int failures{};
void check(bool passed, const char* name) {
  std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
  failures += !passed;
}
ReceiverSourceDecision observe(ReceiverSourceSession& source, std::uint32_t ip,
    std::uint16_t port, std::uint64_t now, std::uint32_t frame) {
#ifdef VF_TEST_LEGACY_SOURCE_SESSION
  return source.observe(ip, port, now);
#else
  return source.observe(ip, port, now, frame);
#endif
}
bool restarted(ReceiverSourceDecision decision) {
  return decision != ReceiverSourceDecision::accept &&
      decision != ReceiverSourceDecision::reject_foreign;
}
void same_port_restart_recovers_complete_keyframes() {
  for (std::uint64_t interval : {7'000U,16'667U,40'000U}) {
    ReceiverSourceSession source(1);
    vfdual::AccessUnitReassembler assembler(4);
    vfdual_android::ReceiverReferenceSyncPolicy reference;
    unsigned restarts{}, admitted{}, non_idr_before_sync{};
    bool synced=false;
    auto send=[&](std::uint32_t frame, bool idr, std::uint64_t now, bool recovery) {
      auto decision=observe(source,1,5002,now,frame);
      if (restarted(decision)) {
        ++restarts; assembler=vfdual::AccessUnitReassembler(4);
        reference.reset(); synced=false;
      }
      vfdual::VideoFragment part{frame,0,1,
        {std::byte{0},std::byte{0},std::byte{0},std::byte{1},idr?std::byte{0x65}:std::byte{0x41},std::byte{0x80}}};
      auto completed=assembler.push(std::move(part),now);
      if (!completed) return;
      auto seq=vfdual::video_logical_frame_sequence(frame);
      if (reference.evaluate(seq,idr)!=vfdual_android::ReceiverReferenceDecision::admit) return;
      if (recovery && !idr && !synced) ++non_idr_before_sync;
      if (idr) synced=true;
      reference.record_submit(seq,true);
      if (recovery) ++admitted;
    };
    send(270531,true,100'000,false);
    synced=false;
    for (unsigned i=0;i<80;++i) send(vfdual::make_video_wire_frame_id(i,i%2==0),
        i%10==0,1'000'000+i*interval,true);
    check(restarts==1,"same_port_restart_recognized_once");
    check(admitted>30,"new_numbering_reaches_decoder_after_restart");
    check(non_idr_before_sync==0,"restarted_reference_chain_waits_for_complete_idr");
    std::printf("recovery interval_us=%llu restarts=%u admitted=%u\n",interval,restarts,admitted);
  }
}
void reordered_and_repeated_packets_do_not_restart_a_live_stream() {
  ReceiverSourceSession source(1);
  (void)observe(source,1,5002,100'000,1000);
  bool stable=true;
  for(unsigned i=1;i<=200;++i) {
    const auto now=100'000+i*7'000U;
    stable &= observe(source,1,5002,now,1000+i)==ReceiverSourceDecision::accept;
    stable &= observe(source,1,5002,now+100,i)==ReceiverSourceDecision::accept;
    stable &= observe(source,1,5002,now+200,vfdual::make_video_wire_frame_id(i,true))==ReceiverSourceDecision::accept;
  }
  check(stable,"healthy_forward_progress_defeats_reordered_old_frames");
  ReceiverSourceSession duplicates(1);
  (void)observe(duplicates,1,5002,100'000,1000);
  for(unsigned i=0;i<100;++i) stable &= observe(duplicates,1,5002,1'000'000+i*10'000,
      vfdual::make_video_wire_frame_id(0,i%2))==ReceiverSourceDecision::accept;
  check(stable,"many_fragments_of_one_old_frame_are_not_restart_evidence");
  ReceiverSourceSession wrapping(1);
  (void)observe(wrapping,1,5002,100'000,vfdual::kVideoFrameSequenceMask-2);
  for(unsigned i=0;i<20;++i) stable &= observe(wrapping,1,5002,1'000'000+i*20'000,
      vfdual::make_video_wire_frame_id(i,i%2))==ReceiverSourceDecision::accept;
  check(stable,"natural_31_bit_wrap_and_repeat_flag_are_not_restart");
  ReceiverSourceSession foreign(1);
  (void)observe(foreign,1,5002,100'000,1000);
  for(unsigned i=0;i<100;++i) stable &= observe(foreign,2,5002,1'000'000+i*10'000,i)==ReceiverSourceDecision::reject_foreign;
  stable &= observe(foreign,1,5002,2'000'000,1001)==ReceiverSourceDecision::accept;
  check(stable,"foreign_sequence_cannot_reset_the_bound_sender");
}
void rapid_restart_requires_old_stream_to_stop_progressing() {
  ReceiverSourceSession source(1);
  (void)observe(source,1,5002,100'000,1000);
  unsigned resets{}; std::uint64_t first{};
  for(unsigned i=0;i<60;++i) {
    auto now=110'000+i*10'000;
    if (restarted(observe(source,1,5002,now,i))) {++resets;if(!first)first=now;}
  }
  check(resets==1 && first>=400'000 && first<=450'000,
      "rapid_same_port_restart_is_bounded_and_not_one_packet_triggered");
  source.reset(3);
  check(observe(source,3,5002,1,0)==ReceiverSourceDecision::accept,
      "network_reset_discards_old_sequence_evidence");
}
}

int main() {
  using vfdual_android::ReceiverSourceDecision;
  vfdual_android::ReceiverSourceSession session(1U);
  assert(session.observe(2U, 100U, 500U) == ReceiverSourceDecision::reject_foreign);
  assert(session.observe(1U, 100U, 1'000U) == ReceiverSourceDecision::accept);
  assert(session.observe(1U, 100U, 2'000U) == ReceiverSourceDecision::accept);
  assert(session.observe(1U, 200U, 301'999U) == ReceiverSourceDecision::reject_foreign);
  assert(session.observe(1U, 200U, 302'000U) == ReceiverSourceDecision::restart_source_port);
  assert(session.observe(1U, 100U, 400'000U) == ReceiverSourceDecision::reject_foreign);
  assert(session.observe(2U, 300U, 5'000'000U) == ReceiverSourceDecision::reject_foreign);
  assert(session.observe(1U, 300U, 5'000'001U) == ReceiverSourceDecision::restart_source_port);
  session.reset(2U);
  assert(session.observe(1U, 300U, 1U) == ReceiverSourceDecision::reject_foreign);
  assert(session.observe(2U, 300U, 2U) == ReceiverSourceDecision::accept);
  vfdual_android::ReceiverSourceSession stable_source(3U);
  assert(stable_source.observe(3U, 5002U, 1U) == ReceiverSourceDecision::accept);
  assert(stable_source.observe(3U, 5002U, 10'000'000U) == ReceiverSourceDecision::accept);
  vfdual_android::ReceiverSourceSession fail_closed_source(0U);
  assert(fail_closed_source.observe(3U, 5002U, 1U) ==
         ReceiverSourceDecision::reject_foreign);
  same_port_restart_recovers_complete_keyframes();
  reordered_and_repeated_packets_do_not_restart_a_live_stream();
  rapid_restart_requires_old_stream_to_stop_progressing();
  std::printf("source_restart_regressions failures=%d\n",failures);
  return failures ? 1 : 0;
}
