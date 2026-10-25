#include "ProjectionObservationGate.hpp"
#include <array>
#include <cassert>
#include <cstdio>

using Gate = vfdual_android::ProjectionObservationGate;
using Reason = Gate::Reason;
using Cache = Gate::CacheResult;

namespace {
Gate::Frame frame(std::uint64_t at, std::uint64_t sequence, std::uint64_t generation=1) {
  return {generation, sequence, sequence+1000, at};
}
Gate::Source source(std::uint64_t at=100'000, std::uint64_t sequence=10, std::size_t count=2,
                    std::uint64_t generation=1) {
  return {frame(at,sequence,generation),sequence+10'000,count,at+50'000};
}
Gate::Pixels pixels(std::uint64_t at=120'000, std::uint64_t sequence=20, std::uint64_t generation=1) {
  return {frame(at,sequence,generation),sequence+10'000,true,true};
}
struct Fixture {
  Gate gate;
  Gate::Source a{source()};
  Gate::Pixels b{pixels()};
  std::array<Gate::TargetEvidence,3> targets{};
  Fixture(){assert(gate.restart(1));gate.set_user_enabled(true);assert(gate.cache_source(a)==Cache::updated);}
  Gate::ProjectionEvidence evidence(std::size_t count=2) {
    for(std::size_t i=0;i<count;++i)targets[i]={i,true,b.frame.observed_us};
    return {a.frame,b.frame,a.detection_count,count,true,false,
            std::span<const Gate::TargetEvidence>(targets).first(count),{}};
  }
  void publish_b(std::uint64_t now=125'000){auto p=gate.prepare_projection(b,evidence(),now);assert(p.ready());assert(gate.publish(p.ticket,now).published());}
};

void source_pixel_control_clocks_stay_separate() {
  Fixture f;
  auto p=f.gate.prepare_projection(f.b,f.evidence(),125'000);
  assert(p.ready() && p.source.observed_us==100'000 && p.control.observed_us==120'000);
  assert(p.source_deadline_us==150'000 && p.previous_control_observed_us==0);
  assert(f.gate.last_published_control().observed_us==0); // prepare is not publication.
  auto published=f.gate.publish(p.ticket,126'000);
  assert(published.published() && f.gate.last_published_control().observed_us==120'000);
  assert(f.gate.cached_source()->frame.observed_us==100'000);
  assert(!f.gate.publish(p.ticket,126'001).published());
  std::puts("PASS independent A/B/L metadata; prepare does not publish; exact-once commit");
}

void old_nnp_results_and_empty_results_never_rewind() {
  Fixture f;f.publish_b();
  assert(f.gate.prepare_detector_fallback(126'000).reason==Reason::would_rewind_or_repeat);
  auto c=source(110'000,15,0);
  assert(f.gate.cache_source(c)==Cache::updated); // NNP semantic cache may be <= L.
  assert(f.gate.prepare_detector_fallback(127'000).reason==Reason::would_rewind_or_repeat);
  assert(f.gate.last_published_control().observed_us==120'000);
  auto at_b=source(120'000,20,0);
  assert(f.gate.cache_source(at_b)==Cache::updated);
  assert(f.gate.prepare_detector_fallback(128'000).reason==Reason::would_rewind_or_repeat);
  assert(f.gate.last_published_control().observed_us==120'000);
  std::puts("PASS next NNP A<L / A==L including zero detections only update semantic cache");
}

void out_of_order_sources_and_pixels() {
  Fixture f;f.publish_b();
  assert(f.gate.cache_source(source(90'000,9))==Cache::older_ignored);
  assert(f.gate.cached_source()->frame==f.a.frame);
  f.b=pixels(119'000,21);
  assert(f.gate.prepare_projection(f.b,f.evidence(),126'000).reason==Reason::would_rewind_or_repeat);
  f.b=pixels(125'000,19); // Timestamp forward, acquisition sequence backward from L.
  assert(f.gate.prepare_projection(f.b,f.evidence(),126'000).reason==Reason::non_monotonic_sequence);
  assert(f.gate.last_published_control().observed_us==120'000);
  std::puts("PASS out-of-order NNP / old pixels / inconsistent acquisition sequence");
}

void failures_are_atomic_and_fallback_is_explicit() {
  Fixture f;
  auto e=f.evidence();e.complete=false;
  assert(f.gate.prepare_projection(f.b,e,125'000).reason==Reason::incomplete_projection);
  assert(f.gate.last_published_control().observed_us==0);
  auto fallback=f.gate.prepare_detector_fallback(125'000);
  assert(fallback.ready() && fallback.kind==Gate::Kind::detector_targets && fallback.control==f.a.frame);
  assert(f.gate.publish(fallback.ticket,125'001).published());
  f.publish_b(126'000);
  e=f.evidence();e.valid_count=1;
  assert(f.gate.prepare_projection(pixels(130'000,21),e,131'000).reason!=Reason::ready);
  assert(f.gate.prepare_detector_fallback(131'000).reason==Reason::would_rewind_or_repeat);
  assert(f.gate.last_published_control().observed_us==120'000);
  std::puts("PASS partial-failure leaves L unchanged; fallback A only when A>L");
}

void every_target_must_be_real_valid_and_unique() {
  Fixture f;
  auto e=f.evidence();f.targets[1].source_index=0;
  assert(f.gate.prepare_projection(f.b,e,125'000).reason==Reason::duplicate_source_index);
  e=f.evidence();f.targets[1].valid=false;
  assert(f.gate.prepare_projection(f.b,e,125'000).reason==Reason::invalid_target_evidence);
  e=f.evidence();f.targets[1].pixel_observed_us=0;
  assert(f.gate.prepare_projection(f.b,e,125'000).reason==Reason::invalid_target_evidence);
  e=f.evidence();f.targets[1].pixel_observed_us=f.a.frame.observed_us;
  assert(f.gate.prepare_projection(f.b,e,125'000).reason==Reason::invalid_target_evidence);
  e=f.evidence();f.targets[1].source_index=2;
  assert(f.gate.prepare_projection(f.b,e,125'000).reason==Reason::invalid_target_evidence);
  e=f.evidence(1);
  assert(f.gate.prepare_projection(f.b,e,125'000).reason==Reason::incomplete_projection);
  e=f.evidence();e.work_budget_exhausted=true;
  assert(f.gate.prepare_projection(f.b,e,125'000).reason==Reason::incomplete_projection);
  assert(f.gate.last_published_control().observed_us==0);
  std::puts("PASS complete means every source target exactly once, no partial success or budget overrun");
}

void source_and_target_identity_cannot_be_substituted() {
  Fixture f;
  auto e=f.evidence();e.source.sequence++;
  assert(f.gate.prepare_projection(f.b,e,125'000).reason==Reason::source_mismatch);
  e=f.evidence();e.target.wire_frame_id++;
  assert(f.gate.prepare_projection(f.b,e,125'000).reason==Reason::target_mismatch);
  e=f.evidence();e.target.generation++;
  assert(f.gate.prepare_projection(f.b,e,125'000).reason==Reason::wrong_generation);
  f.b=pixels(100'000,20);
  assert(f.gate.prepare_projection(f.b,f.evidence(),125'000).reason==Reason::not_newer_than_source);
  f.b=pixels(120'000,10);
  assert(f.gate.prepare_projection(f.b,f.evidence(),125'000).reason==Reason::not_newer_than_source);
  std::puts("PASS generation/source/target tags and B>A acquisition ordering are checked");
}

void repeated_or_future_pixels_cannot_refresh_control() {
  Fixture f;
  f.b.content_updated=false;
  assert(f.gate.prepare_projection(f.b,f.evidence(),125'000).reason==Reason::repeated_pixels);
  f.b.content_updated=true;f.b.independent_acquisition=false;
  assert(f.gate.prepare_projection(f.b,f.evidence(),125'000).reason==Reason::repeated_pixels);
  f.b.independent_acquisition=true;f.b.independent_content_id=f.a.independent_content_id;
  assert(f.gate.prepare_projection(f.b,f.evidence(),125'000).reason==Reason::repeated_pixels);
  f.b=pixels();f.publish_b();
  const auto last_id=f.b.independent_content_id;
  f.b=pixels(130'000,21);f.b.independent_content_id=last_id;
  assert(f.gate.prepare_projection(f.b,f.evidence(),131'000).reason==Reason::repeated_pixels);
  f.b=pixels(140'000,22);
  assert(f.gate.prepare_projection(f.b,f.evidence(),139'999).reason==Reason::future_observation);
  std::puts("PASS repeated content / reused content identity / non-independent / future pixel buffers");
}

void original_deadline_never_moves_to_B_or_now() {
  Fixture f;
  auto p=f.gate.prepare_projection(f.b,f.evidence(),149'999);
  assert(p.ready() && p.source_deadline_us==150'000);
  assert(f.gate.publish(p.ticket,150'000).reason==Reason::source_expired);
  assert(f.gate.last_published_control().observed_us==0);
  f.b=pixels(149'999,30);
  assert(f.gate.prepare_projection(f.b,f.evidence(),150'000).reason==Reason::source_expired);
  auto refreshed=f.a;refreshed.valid_until_us=160'000;
  assert(f.gate.cache_source(refreshed)==Cache::invalid);
  Fixture short_lived;
  auto shorter=source(110'000,11);shorter.valid_until_us=130'000;
  assert(short_lived.gate.cache_source(shorter)==Cache::updated);
  short_lived.a=shorter;short_lived.b=pixels(125'000,21);
  assert(short_lived.gate.prepare_projection(short_lived.b,short_lived.evidence(),130'000).reason==Reason::source_expired);
  std::puts("PASS source age49999 accepted / age50000 rejected; original shorter deadline retained");
}

void disable_restart_and_replaced_proposals() {
  Fixture f;
  const auto p=f.gate.prepare_projection(f.b,f.evidence(),125'000);
  f.gate.set_user_enabled(false);
  assert(!f.gate.publish(p.ticket,125'001).published());
  assert(f.gate.prepare_detector_fallback(125'001).reason==Reason::user_disabled);
  f.gate.set_user_enabled(true);f.publish_b(126'000);
  f.gate.set_user_enabled(false);f.gate.set_user_enabled(true);
  assert(f.gate.prepare_projection(f.b,f.evidence(),127'000).reason==Reason::would_rewind_or_repeat);
  assert(!f.gate.restart(1));assert(f.gate.last_published_control().observed_us==120'000);
  assert(f.gate.restart(2));assert(f.gate.last_published_control().observed_us==0 && !f.gate.cached_source());
  f.gate.set_user_enabled(true);
  assert(f.gate.cache_source(f.a)==Cache::wrong_generation);
  f.a=source(200'000,1,2,2);f.b=pixels(210'000,2,2);
  assert(f.gate.cache_source(f.a)==Cache::updated);
  auto newer=f.gate.prepare_projection(f.b,f.evidence(),215'000);
  assert(newer.ready() && newer.ticket!=p.ticket);
  assert(!f.gate.publish(p.ticket,215'000).published());
  assert(f.gate.publish(newer.ticket,215'000).published());
  std::puts("PASS user-disable invalidates pending without resetting L; restart rejects old generation/tickets");
}

void empty_detections_are_negative_evidence_not_targets() {
  Fixture f;
  f.a=source(110'000,11,0);assert(f.gate.cache_source(f.a)==Cache::updated);
  auto e=f.evidence(0);
  assert(f.gate.prepare_projection(f.b,e,125'000).reason==Reason::empty_source);
  auto empty=f.gate.prepare_detector_fallback(125'000);
  assert(empty.ready() && empty.kind==Gate::Kind::detector_empty_observation && !empty.may_emit_target);
  auto publication=f.gate.publish(empty.ticket,125'000);
  assert(publication.published() && publication.observation.source_detection_count==0 && !publication.observation.may_emit_target);
  assert(f.gate.last_published_control().observed_us==110'000);
  std::puts("PASS fresh raw empty NNP publishes only negative observation; empty projection never invents target");
}

void new_targets_require_a_new_source_and_complete_new_batch() {
  Fixture f;f.publish_b();
  const auto old_evidence=f.evidence();
  f.a=source(115'000,16,3); // A newer detector result still before already-published B.
  assert(f.gate.cache_source(f.a)==Cache::updated);
  assert(f.gate.prepare_detector_fallback(127'000).reason==Reason::would_rewind_or_repeat);
  f.b=pixels(130'000,21);
  assert(f.gate.prepare_projection(f.b,old_evidence,135'000).reason==Reason::source_mismatch);
  auto partial=f.evidence(2);
  assert(f.gate.prepare_projection(f.b,partial,135'000).reason==Reason::incomplete_projection);
  auto p=f.gate.prepare_projection(f.b,f.evidence(3),135'000);
  assert(p.ready() && p.source_detection_count==3 && p.previous_control_observed_us==120'000);
  assert(f.gate.publish(p.ticket,135'000).published());
  std::puts("PASS new target comes from newer NNP cache and complete all-target evidence, never extrapolation of empty cache");
}

void background_and_proposal_lifetime() {
  Fixture f;
  auto e=f.evidence();e.background={true,1,99'000,120'000};
  assert(f.gate.prepare_projection(f.b,e,125'000).reason==Reason::misaligned_background);
  e.background={true,1,100'000,120'000};auto p=f.gate.prepare_projection(f.b,e,125'000);
  assert(p.ready());
  auto c=source(110'000,15);assert(f.gate.cache_source(c)==Cache::updated);
  assert(!f.gate.publish(p.ticket,126'000).published());
  f.a=c;auto next=f.gate.prepare_projection(f.b,f.evidence(),127'000);assert(next.ready());
  assert(f.gate.cache_source(source(100'000,10))==Cache::older_ignored);
  assert(f.gate.publish(next.ticket,127'000).published());
  auto conflict=c;conflict.detection_count=1;
  assert(f.gate.cache_source(conflict)==Cache::conflicting_same_frame);
  assert(!f.gate.cached_source());
  assert(f.gate.cache_source(source(100'000,10))==Cache::older_ignored);
  std::puts("PASS background A-to-B tags; new NNP invalidates proposal; old NNP cannot invalidate newer proposal or resurrect conflicted cache");
}

void deterministic_mixed_order_state_invariants() {
  Gate gate;
  std::uint64_t generation=1;assert(gate.restart(generation));gate.set_user_enabled(true);
  unsigned random=47;
  for(unsigned i=1;i<=20'000;++i){
    random=1664525U*random+1013904223U;
    if(i%997==0){assert(gate.restart(++generation));gate.set_user_enabled(true);}
    const auto previous=gate.last_published_control();
    const auto now=1'000'000ULL+i*1'000ULL;
    gate.set_user_enabled((random%23)!=0);
    auto a=source(now-11'000,4ULL*i,random%3,generation);
    if(random%13==0){a.frame.sequence=4ULL*i>8?4ULL*i-8:1;a.frame.observed_us-=15'000;a.valid_until_us=a.frame.observed_us+50'000;}
    (void)gate.cache_source(a);
    auto b=pixels(now-2'000,4ULL*i+2,generation);
    std::array<Gate::TargetEvidence,2> evidence_targets{};
    for(std::size_t n=0;n<a.detection_count;++n)evidence_targets[n]={n,true,b.frame.observed_us};
    Gate::ProjectionEvidence e{a.frame,b.frame,a.detection_count,a.detection_count,true,false,
        std::span<const Gate::TargetEvidence>(evidence_targets).first(a.detection_count),{}};
    if(random%7==0)e.complete=false;
    if(random%17==0)b.independent_acquisition=false;
    if(random%19==0 && previous.sequence)b.independent_content_id=previous.sequence+10'000;
    if(random%29==0 && a.detection_count==2)evidence_targets[1].source_index=0;
    const auto proposal=random%5==0?gate.prepare_detector_fallback(now):gate.prepare_projection(b,e,now);
    const auto late_now=now+(random%11==0?60'000:20);
    const auto publication=gate.publish(proposal.ticket,late_now);
    const auto after=gate.last_published_control();
    assert(after.observed_us>=previous.observed_us && after.sequence>=previous.sequence);
    if(publication.published()){
      const auto& p=publication.observation;
      assert(p.control.generation==generation && p.source.generation==generation);
      assert(p.control.observed_us>previous.observed_us && p.control.sequence>previous.sequence);
      assert(late_now>=p.control.observed_us && late_now-p.control.observed_us<50'000);
      assert(late_now>=p.source.observed_us && late_now-p.source.observed_us<50'000 && late_now<p.source_deadline_us);
      assert(p.source_deadline_us<=p.source.observed_us+50'000);
      if(p.kind==Gate::Kind::projected_targets)assert(p.control.observed_us>p.source.observed_us && p.source_detection_count>0);
      if(p.kind==Gate::Kind::detector_empty_observation)assert(!p.may_emit_target && p.source_detection_count==0 && p.control==p.source);
    }
  }
  std::puts("PASS deterministic20000mixed NNP/pixel/disable/restart/partial/late events: monotonic publication and immutable source deadline");
}
} // namespace

void test_consumed_observation_receipt_is_not_physical_authorization() {
  Fixture f;
  const auto p=f.gate.prepare_projection(f.b,f.evidence(),149'999);
  assert(p.ready());
  // Core entered while the proposal was valid; physical dispatch later expires.
  // A consumed-B fact still advances L; source deadline is not refreshed.
  assert(f.gate.record_consumed_observation(f.b.frame,f.b.independent_content_id,true));
  assert(f.gate.last_published_control()==f.b.frame);
  assert(f.gate.cached_source()->valid_until_us==150'000);
  assert(!f.gate.publish(p.ticket,150'001).published());
  assert(f.gate.cache_source(source(120'000,20,0))==Cache::updated);
  assert(f.gate.prepare_detector_fallback(150'002).reason==Reason::would_rewind_or_repeat);
  f.gate.set_user_enabled(false);
  auto legacy=frame(130'000,21);
  assert(!f.gate.record_legacy_observation(legacy,10021,false));
  assert(f.gate.last_published_control()==f.b.frame);
  assert(f.gate.record_legacy_observation(legacy,10021,true));
  assert(f.gate.prepare_detector_fallback(151'000).reason==Reason::user_disabled);
  assert(!f.gate.publish(p.ticket,151'000).published());
  assert(f.gate.restart(2));
  assert(!f.gate.record_consumed_observation(legacy,10021,true));
  assert(f.gate.last_published_control().observed_us==0);
  std::puts("PASS consumed observation receipt advances L independently of later physical expiry; disabled legacy tracking never authorizes output");
}

int main(){
  test_consumed_observation_receipt_is_not_physical_authorization();
  source_pixel_control_clocks_stay_separate();old_nnp_results_and_empty_results_never_rewind();
  out_of_order_sources_and_pixels();failures_are_atomic_and_fallback_is_explicit();
  every_target_must_be_real_valid_and_unique();source_and_target_identity_cannot_be_substituted();
  repeated_or_future_pixels_cannot_refresh_control();original_deadline_never_moves_to_B_or_now();
  disable_restart_and_replaced_proposals();empty_detections_are_negative_evidence_not_targets();
  new_targets_require_a_new_source_and_complete_new_batch();background_and_proposal_lifetime();
  deterministic_mixed_order_state_invariants();
  std::puts("PASS ProjectionObservationGate isolated state machine");
}
