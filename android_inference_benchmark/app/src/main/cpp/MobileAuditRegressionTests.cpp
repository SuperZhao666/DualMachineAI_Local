#include "MobileControlCore.hpp"

#include <array>
#include <cmath>
#include <cstdio>

using namespace vfdual_android;
namespace {
int failures{};
void check(bool ok, const char* message) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
  if (!ok) ++failures;
}
MobileControlConfig configuration(std::uint64_t confirmation = 100'000U) {
  MobileControlConfig c;
  c.body_class_id = c.selected_class_id = 0;
  c.head_class_id = vfdual::kNoModelClass;
  c.selected_class_uses_geometric_head = true;
  c.reject_border_touching = false;
  c.body_fallback_min_confidence = 0;
  c.body_fallback_max_center_distance_pixels = 640;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2, c);
  c.observation_servo_enabled = true;
  c.switch_confirmation_duration_us = confirmation;
  c.lost_target_hold_duration_us = 350'000U;
  c.tracker.lost_track_duration_us = 500'000U;
  return c;
}
YoloDetection person(float x, float confidence = .95F) {
  return {x - 20, 302, x + 20, 402, confidence, 0};
}
ControlFrameContext frame(unsigned n, std::uint64_t at) {
  return {n, at, at + 23'000U};
}
void rejected_position_is_not_a_fresh_output() {
  for (bool trusted_background : {false, true}) {
    MobileControlCore core(configuration());
    const std::array before{person(335)}, after{person(305)};
    const auto original = core.process(before, frame(1, 100'000));
    (void)core.process(before, frame(2, 130'000));
    auto changed = frame(3, 160'000);
    if (trusted_background) changed.background = {130'000, 0, 0, 24, 32, true};
    const auto rejected = core.process(after, changed);
    std::printf("spike target=%.3f delta=%d held=%d lock=%llu\n",
        rejected.target_x, rejected.delta_x, rejected.held,
        static_cast<unsigned long long>(rejected.lock_id));
    check(rejected.held && !rejected.has_move() && rejected.lock_id == original.lock_id,
        "new rejected geometry never drives an old-direction command");
    const auto& trace = core.control_trace();
    const auto& sample = trace.samples[(trace.count - 1U) % trace.samples.size()];
    check(sample.velocity_x == 0 && sample.velocity_y == 0,
        "rejected geometry does not feed position derivatives");
    const auto repeated = core.process(after, frame(4, 190'000));
    check(!repeated.held && repeated.delta_x < 0 && repeated.lock_id == original.lock_id &&
        std::abs(repeated.target_x - 305) < .01F,
        "consistent fresh geometry resumes toward the accepted position");
  }
}
void single_spike_recovery_and_time_guards() {
  MobileControlCore core(configuration());
  const std::array before{person(335)}, noise{person(305)};
  const auto original = core.process(before, frame(1, 100'000));
  (void)core.process(before, frame(2, 130'000));
  const auto spike = core.process(noise, frame(3, 160'000));
  check(spike.held && !spike.has_move(), "single-frame noise is held");
  check(core.process(noise, frame(4, 160'000)).suppression_reason ==
      ControlSuppressionReason::non_monotonic_frame, "repeated observation cannot confirm a spike");
  check(core.process(noise, frame(5, 155'000)).suppression_reason ==
      ControlSuppressionReason::invalid_time, "backward processing time cannot confirm a spike");
  const auto restored = core.process(before, frame(6, 190'000));
  check(!restored.held && restored.delta_x > 0 && restored.lock_id == original.lock_id &&
      std::abs(restored.target_x - 335) < .01F,
      "single-frame noise recovers the original owner without switching");
}
void ordinary_crowd_respects_user_confirmation() {
  for (const auto duration : {10'000U, 25'000U, 45'000U, 100'000U}) {
    for (bool crowd : {true}) {
      MobileControlCore core(configuration(duration));
      const std::array owner{person(412)};
      const auto original = core.process(owner, frame(1, 100'000));
      const std::array replacements{person(348), person(550)};
      const auto visible = std::span<const YoloDetection>(replacements).first(crowd ? 2 : 1);
      bool switched = false;
      for (unsigned i = 0; i < 13; ++i) {
        const auto at = 112'000U + i * 12'000U;
        const auto out = core.process(visible, frame(i + 2, at));
        const bool confirmed = i >= 2U && at - 112'000U >= duration;
        if (!confirmed) {
          check(out.held && !out.has_move() && out.lock_id == original.lock_id,
              "retained owner cannot be replaced before configured frames and duration");
        } else {
          check(out.switched && out.lock_id != original.lock_id,
              "retained owner can switch once the unified confirmation completes");
          switched = true;
          break;
        }
      }
      check(switched, "ordinary confirmed replacement remains reachable");
    }
  }
}
void unique_stranger_fast_switch_is_an_explicit_exception() {
  for(unsigned mode=0;mode<5;++mode) {
    MobileControlCore core(configuration(100'000U));
    const std::array owner{person(412)};
    const auto old=core.process(owner,frame(1,100'000));
    const std::array replacement{person(348,mode==1?.74F:.95F),person(550)};
    const auto visible=std::span<const YoloDetection>(replacement).first(mode==2?2U:1U);
    const auto first=core.process(visible,frame(2,112'000));
    check(first.held && !first.has_move(),"a one-frame stranger cannot acquire the lock");
    const auto at=mode==3?119'999U:mode==4?180'000U:120'000U;
    const auto second=core.process(visible,frame(3,at));
    if(mode==0) {
      check(second.switched && second.lock_id!=old.lock_id,
          "one high-confidence unknown person can switch after two fresh frames and 8 ms");
    } else {
      check(second.held && !second.has_move() && second.lock_id==old.lock_id,
          "weak ambiguous sub-8ms or disconnected observations cannot use fast switching");
    }
  }
}
void expired_identity_cannot_rebind_without_confirmation() {
  for(const auto duration:{45'000U,100'000U})for(bool tracking_only:{false,true}) {
    MobileControlCore core(configuration(duration));
    check(core.restore_response_calibration(.709F,.707F,24'000U,.99F,.99F),
        "expiry test restores calibration");
    const std::array before{person(335)},after{person(345)};
    const auto original=core.process(before,frame(1,100'000));
    (void)core.process(before,frame(2,130'000));
    if(tracking_only) {
      (void)core.observe_tracking_only(after,frame(3,730'000));
      check(core.metrics().track_id_rebinds==0 && core.metrics().tracking_only_lock_updates==0,
          "tracking-only shared matching cannot revive an expired identity");
    } else {
      const auto first=core.process(after,frame(3,730'000));
      check(!first.has_move() && core.metrics().track_id_rebinds==0,
          "first new track after a 600 ms input gap cannot inherit the old lock");
    }
    const auto start=tracking_only?750'000U:730'000U;
    bool acquired=false;
    for(unsigned i=tracking_only?0U:1U;i<8;++i) {
      const auto at=start+i*20'000U;
      const auto out=core.process(after,frame(4+i,at));
      if(at-start<duration || i<2U) {
        check(!out.has_move(),"expiry does not refresh the old timestamp or bypass ordinary confirmation");
      } else {
        check(out.switched && out.lock_id!=original.lock_id && core.metrics().track_id_rebinds==0,
            "fresh stable observations can establish a new lock after normal confirmation");
        acquired=true;break;
      }
    }
    check(acquired,"expired identity can recover without resetting the entire Core");
    check(std::abs(core.response_estimator().x().response-.709F)<.00001F &&
        std::abs(core.response_estimator().y().response-.707F)<.00001F,
        "identity expiry preserves saved response calibration");
  }
  MobileControlCore continuous(configuration());
  const std::array owner{person(335)};
  const auto first=continuous.process(owner,frame(1,100'000));
  const auto next=continuous.process(owner,frame(2,140'000));
  check(!next.held && next.lock_id==first.lock_id && next.track_id==first.track_id,
      "ordinary 40 ms same-track continuity is preserved");
}
void retained_extent_cannot_count_as_fresh_fast_switch_evidence() {
  MobileControlCore core(configuration(100'000U));
  const std::array original{person(412)};
  const auto owner=core.process(original,frame(1,100'000));
  auto expanded=person(348);expanded.x1-=30;expanded.x2+=30;
  const std::array wide{expanded},normal{person(348)};
  for(unsigned i=0;i<5;++i) {
    const auto at=112'000U+i*8'000U;
    const auto out=core.process(i==0?std::span<const YoloDetection>(wide):std::span<const YoloDetection>(normal),
        frame(i+2,at));
    if(i<4)check(!out.has_move() && out.lock_id==owner.lock_id,
        "2.5x transient extent and its retained geometry do not accumulate fast confirmation");
    else check(out.switched && out.lock_id!=owner.lock_id && out.has_move(),
        "two stable observations retain the authorized 8 ms fast-switch contract after shape confirmation");
  }
}
void only_actual_owner_covisibility_creates_a_known_neighbour() {
  for(bool covisible:{false,true}) {
    MobileControlCore core(configuration(100'000U));
    const std::array a{person(100)},b{person(400)};
    const auto first=core.process(a,frame(1,100'000));
    (void)core.process(b,frame(2,108'000));
    const auto second=core.process(b,frame(3,116'000));
    check(second.switched && second.track_id>first.track_id,
        "history fixture creates a later owner without co-observing the earlier track");
    if(covisible) {
      const std::array both{person(100),person(400)};
      const auto visible=core.process(both,frame(4,124'000));
      check(visible.lock_id==second.lock_id && visible.track_id==second.track_id,
          "observed owner and older track establish actual neighbour evidence");
    } else {
      const auto pending=core.process(a,frame(4,124'000));
      check(pending.held && !pending.has_move(),"older unknown ID still needs two observations");
    }
    const auto returned=core.process(a,frame(5,132'000));
    check(returned.held && !returned.has_move() && returned.lock_id==second.lock_id,
        "a previously seen target does not use the stranger-only shortcut");
    const auto& trace=core.control_trace();const auto& sample=trace.samples[(trace.count-1U)%trace.samples.size()];
    check(bool(sample.selection_flags&32U)==covisible,
        "only actual co-observation marks the identical older ID as a known neighbour");
    {
      bool acquired=false;
      for(unsigned i=0;i<70;++i) {
        const auto at=140'000U+i*8'000U;
        const auto out=core.process(a,frame(6+i,at));
        const auto minimum=covisible?500'000U:100'000U;
        if(at-124'000U<minimum)
          check(out.lock_id==second.lock_id && !out.has_move(),
              "observations preserve either normal confirmation or true neighbour protection");
        if(out.switched) {check(at-124'000U>=minimum && (covisible || at-124'000U<350'000U),
            "old unseen-as-neighbour track uses normal confirmation; true neighbour keeps 500 ms");acquired=true;break;}
      }
      check(acquired,"a previously seen target remains selectable after its proper protection window");
    }
  }
}
void bounded_history_overflow_keeps_unrecorded_neighbours_protected() {
  MobileControlCore core(configuration(45'000U));
  const auto owner_box=person(320);
  const std::array first{owner_box};
  const auto owner=core.process(first,frame(1,100'000));
  std::uint64_t at=100'000U;unsigned sequence=2,count=0;YoloDetection omitted{};
  for(unsigned row=0;row<8 && count<65;++row)for(unsigned col=0;col<9 && count<65;++col) {
    const float x=40+col*70.F,y=50+row*70.F;
    if(std::hypot(x-320,y-320)<60.F)continue;
    omitted={x-20,y-18,x+20,y+82,.95F,0};at+=5'000U;
    const std::array both{owner_box,omitted};auto context=frame(sequence++,at);
    context.background={at-5'000U,0,0,24,32,true};(void)core.process(both,context);++count;
  }
  const auto& trace=core.control_trace();const auto& sample=trace.samples[(trace.count-1U)%trace.samples.size()];
  check(count==65 && core.tracker_metrics().tracks_created>=66 && (sample.selection_flags&8192U),
      "65 actual co-visible tracks trigger the fixed 64-entry explicit incomplete state");
  const auto lost_at=at;bool acquired=false;
  for(unsigned i=0;i<80;++i) {
    at+=8'000U;const std::array visible{omitted};auto context=frame(sequence++,at);
    context.background={at-8'000U,0,0,24,32,true};const auto out=core.process(visible,context);
    if(at-lost_at<500'000U)check(out.lock_id==owner.lock_id && !out.has_move(),
        "unrecorded co-visible neighbour cannot bypass full tracker retention after overflow");
    if(out.switched) {check(at-lost_at>=500'000U,"overflow replacement waits for safe retention");acquired=true;break;}
  }
  check(acquired,"overflow does not prevent later normally confirmed reacquisition");
}
void camera_and_tracking_only_do_not_forge_observations() {
  for (bool supported : {false, true}) {
    MobileControlCore core(configuration());
    const std::array before{person(335)}, after{person(305)};
    const auto owner = core.process(before, frame(1,100'000));
    (void)core.process(before, frame(2,130'000));
    auto current = frame(3,160'000);
    // Exact adjacent-frame background evidence may move the retained box;
    // the same displacement with an unaligned timestamp is not evidence.
    current.background = {supported ? 130'000U : 120'000U,-30,0,24,32,true};
    const auto out = core.process(after,current);
    check(out.lock_id == owner.lock_id, "camera evidence preserves the existing owner");
    check(supported ? !out.held && out.delta_x < 0 : out.held && !out.has_move(),
        "only aligned supported camera evidence can accept a displaced position immediately");
  }
  MobileControlCore core(configuration());
  const std::array before{person(335)}, after{person(305)};
  const auto owner=core.process(before,frame(1,100'000));
  (void)core.process(before,frame(2,130'000));
  check(core.observe_tracking_only(after,frame(3,160'000)) == ControlSuppressionReason::none,
      "tracking-only processing remains available without issuing a move");
  const auto& trace=core.control_trace();
  const auto sample=trace.samples[(trace.count-1U)%trace.samples.size()];
  check((sample.flags & 16U) && sample.missing_us == 30'000U &&
      sample.delta_x == 0 && sample.delta_y == 0 && sample.target_x == 335,
      "tracking-only rejected position does not advance accepted geometry or timestamp");
  check(core.metrics().tracking_only_lock_updates == 0,
      "rejected tracking-only sample is not counted as a lock update");
  const auto confirmed=core.process(after,frame(4,190'000));
  check(!confirmed.held && confirmed.delta_x < 0 && confirmed.lock_id==owner.lock_id,
      "fresh consistent geometry can resume after a tracking-only rejection");
}
void provisional_positions_move_only_with_shared_direction_evidence() {
  for(unsigned mode=0;mode<6;++mode) {
    MobileControlCore core(configuration());
    check(core.restore_response_calibration(1,1,24'000,.99F,.99F),"provisional test calibration");
    const std::array initial{person(350),person(550)};
    const auto count=mode==3?2U:1U;
    const auto owner=core.process(std::span(initial).first(count),frame(1,100'000));
    auto stable=frame(2,130'000);stable.background={100'000,0,0,24,32,true};
    (void)core.process(std::span(initial).first(count),stable);
    const std::array changed{person(mode==1 || mode==5?305.F:380.F,mode==2?.74F:.95F),person(550)};
    auto current=frame(3,160'000);current.background={130'000,0,0,24,32,mode!=4 && mode!=5};
    const auto out=core.process(std::span(changed).first(count),current);
    check(out.lock_id==owner.lock_id && out.position_provisional,
        "unconfirmed geometry is explicitly marked and retains the owner");
    const auto& trace=core.control_trace();const auto sample=trace.samples[(trace.count-1)%trace.samples.size()];
    check((sample.selection_flags&4096U) && sample.missing_us==30'000U &&
        sample.velocity_x==0 && sample.velocity_y==0,
        "provisional control never refreshes accepted position time or derivatives");
    if(mode==0 || mode==4) {
      check(!out.held && out.delta_x>0 && out.delta_x<=30 && out.target_x==350,
          "same-side uncertainty allows only a bounded move toward the nearer endpoint");
      check(sample.servo.x.feedforward==0 && sample.servo.x.lead==0,
          "provisional control has no velocity feedforward or positional lead");
    } else {
      check(out.held && !out.has_move(),
          "cross-reticle weak or crowd hypotheses cannot move");
    }
  }
}

}
int main() {
  rejected_position_is_not_a_fresh_output();
  single_spike_recovery_and_time_guards();
  ordinary_crowd_respects_user_confirmation();
  unique_stranger_fast_switch_is_an_explicit_exception();
  expired_identity_cannot_rebind_without_confirmation();
  retained_extent_cannot_count_as_fresh_fast_switch_evidence();
  only_actual_owner_covisibility_creates_a_known_neighbour();
  bounded_history_overflow_keeps_unrecorded_neighbours_protected();
  camera_and_tracking_only_do_not_forge_observations();
  provisional_positions_move_only_with_shared_direction_evidence();
  return failures ? 1 : 0;
}
