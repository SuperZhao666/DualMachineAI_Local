#include "ControlReferenceFixture.hpp"
#include "MobileControlCore.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>
using namespace vfdual_android;
namespace {
MobileControlConfig setup() {
  auto c = reference_416_control_config();
  c.selected_class_id=c.body_class_id=0;
  c.selected_class_uses_geometric_head=true;
  c.head_body_pairing_enabled=c.selected_class_uses_head_box=false;
  c.reject_border_touching=false;
  c.body_fallback_min_confidence=.4F;
  c.body_fallback_max_center_distance_pixels=416;
  c.tracker.high_confidence_threshold=c.tracker.new_track_confidence_threshold=.4F;
  c.motion.response_gain=.6F;
  c.motion.maximum_prediction_pixels=kMobileObservationPredictionLimitPixels;
  c.switch_confirmation_duration_us=45'000;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
  c.observation_servo_enabled=true;
  c.lost_target_hold_duration_us=350'000;
  c.tracker.lost_track_duration_us=500'000;
  return c;
}
YoloDetection body(float x,float confidence=.95F) {return {x-16,190,x+16,290,confidence,0};}
void selection(bool check) {
  for (unsigned crowd:{1U,3U}) for (unsigned cadence:{8'000U,16'000U}) {
    MobileControlCore core(setup());
    unsigned sequence=1;std::uint64_t at=100'000;
    std::array initial{body(245)};
    assert(core.process(initial,{sequence++,at,at+20'000}).has_target);
    for(at+=cadence;at<1'000'000;at+=cadence)
      (void)core.process({}, {sequence++,at,at+20'000});
    const auto start=at;
    std::array people{body(290),body(120),body(370)};
    std::uint64_t acquired=0,identity=0;
    for(unsigned i=0;i<12;++i,at+=cadence) {
      // Equal-ranking neighbours fluctuate after commitment. They cannot
      // steal the new identity merely by becoming closer to the reticle.
      people[1]=body(i%2?130:120);
      const auto out=core.process(std::span(people).first(crowd),{sequence++,at,at+20'000});
      if(!i) assert(!out.has_move()); // two independent fresh observations
      if(out.has_target&&!out.held) {
        if(!identity) {identity=out.lock_id;acquired=at-start;}
        assert(identity==out.lock_id);
      }
    }
    assert(identity);
    std::printf("acquire,crowd=%u,cadence=%u,wait_us=%llu\n",crowd,cadence,acquired);
    if(check) assert(acquired<=2*cadence);
  }
}
void selection_guards() {
  for(unsigned mode=0;mode<4;++mode) {
    MobileControlCore core(setup());unsigned sequence=1;std::uint64_t at=100'000;
    const std::array old{body(245)};
    (void)core.process(old,{sequence++,at,at+20'000});
    if(mode==0) { // a long clock gap is not a continuously observed empty scene
      at=1'000'000;(void)core.process({}, {sequence++,at,at+20'000});at+=8'000;
    } else for(at+=8'000;at<1'000'000;at+=8'000)
      (void)core.process({}, {sequence++,at,at+20'000});
    const std::array target{body(300,mode==1?.65F:.95F)};
    assert(!core.process(target,{sequence++,at,at+20'000}).has_move());
    at+=8'000;
    if(mode==2) { // a one-frame false positive must never emit a movement
      assert(!core.process({}, {sequence++,at,at+20'000}).has_move());continue;
    }
    if(mode==3) at+=60'000; // disconnected target samples do not get fast confirmation
    const auto second=core.process(target,{sequence++,at,at+20'000});
    assert(!second.has_move());
  }
}
void visible_replacement() {
  MobileControlCore core(setup());
  unsigned sequence = 1;
  std::uint64_t at = 100'000;
  const std::array owner{body(208)};
  assert(core.process(owner, {sequence++, at, at + 20'000}).has_target);
  at += 8'000;
  const std::array stranger{body(360)};
  assert(!core.process(stranger, {sequence++, at, at + 20'000}).has_move());
  at += 8'000;
  const auto moved = core.process(stranger, {sequence++, at, at + 20'000});
  assert(moved.has_move() && moved.switched);
  MobileControlCore neighbor_core(setup());
  sequence = 1;
  at = 100'000;
  assert(neighbor_core.process(owner, {sequence++, at, at + 20'000}).has_target);
  at += 8'000;
  const std::array both{body(208), body(80)};
  (void)neighbor_core.process(both, {sequence++, at, at + 20'000});
  at += 8'000;
  const std::array neighbor{body(80)};
  const auto start = at;
  bool acquired = false;
  for (unsigned i = 0; i < 80 && !acquired; ++i, at += 8'000) {
    const auto out = neighbor_core.process(neighbor, {sequence++, at, at + 20'000});
    acquired = out.has_move();
  }
  assert(acquired && at - start >= 500'000);
}
void exited_owner(bool check) {
  for(unsigned support:{8U,24U}) for(float initial_x:{250.F,350.F}) {
    MobileControlCore core(setup());
    // The original owner is selected before its neighbour becomes visible.
    const std::array one{body(initial_x)};
    auto owner=core.process(one,{1,100'000,120'000});assert(owner.has_target);
    const std::array both{body(initial_x),body(100)};
    (void)core.process(both,{2,116'000,136'000,{100'000,0,0,24,32,true}});
    std::uint64_t first_switch=0;
    for(unsigned i=1;i<40;++i) {
      const std::uint64_t at=116'000+i*16'000;
      // Three independently measured camera steps; the whole owner leaves
      // the crop only for initial_x=350. The neighbour remains observable.
      const float pan=i<=3?40.F:0.F;
      const std::array neighbour{body(100+40.F*std::min(i,3U))};
      const auto out=core.process(neighbour,{i+2,at,at+20'000,{at-16'000,pan,0,support,32,true}});
      if(out.has_target&&!out.held&&out.lock_id!=owner.lock_id&&!first_switch)first_switch=at-116'000;
    }
    std::printf("exit,support=%u,initial=%.0f,wait_us=%llu\n",support,initial_x,first_switch);
    assert(first_switch);
    if(check&&support>=16&&initial_x==350.F)assert(first_switch<=160'000);
    else assert(first_switch>=500'000); // in-crop/weak motion still protects ownership
  }
}
struct Command {std::uint64_t due;int dx;};
void step_grid() {
  // The simulated camera is driven by delivered commands, not a replay of
  // unchanged detections. Include model error and both over/underestimated
  // delay; score the transient from frame zero (the old suite excludes it).
  unsigned scene=0;
  for(float distance:{40.F,100.F,150.F,185.F})
  for(float ratio:{.8F,1.F,1.2F})
  for(unsigned delay:{20'000U,40'000U,60'000U,80'000U})
  for(unsigned estimate:{40'000U,60'000U,80'000U})
  for(unsigned age:{20'000U,30'000U})
  for(unsigned cadence:{8'000U,16'000U}) {
    MobileControlCore core(setup());
    constexpr float response=.855F;
    assert(core.restore_response_calibration(response,response,estimate,.95F,.95F));
    std::vector<Command> commands;
    const auto camera=[&](std::uint64_t t) {float v=0;for(auto c:commands) if(t>=c.due)v+=c.dx*response*ratio;return v;};
    std::uint64_t previous=0,first=0,reach90=0,last_outside=0,last_change=0,gap=0;
    float previous_camera=0,overshoot=0;double integral=0;
    unsigned cap_holds=0;std::uint64_t lock=0,switches=0;
    for(unsigned i=0;i<160;++i) {
      const std::uint64_t at=100'000+i*cadence,now=at+age;
      std::array observations{body(208+distance-camera(at))};
      ControlFrameContext f{std::uint64_t(i+1),at,now};
      if(previous) f.background={previous,camera(previous)-camera(at),0,24,32,true};
      const auto out=core.process(observations,f);
      if(out.has_target&&!out.held) {if(lock&&lock!=out.lock_id)++switches;lock=out.lock_id;}
      if(out.has_move()) {
        assert(std::abs(out.delta_x)<=64);
        commands.push_back({now+1'000+delay,out.delta_x});
        assert(core.record_accepted_move(out.delta_x,out.delta_y,now+1'000));
      }
      if(out.suppression_reason==ControlSuppressionReason::response_guard)++cap_holds;
      float position=camera(now),error=distance-position;
      if(position!=previous_camera) {
        if(last_change&&error>std::max(8.F,.1F*distance))gap=std::max(gap,now-last_change);
        last_change=now;
      }
      previous_camera=position;
      if(!first&&position>=.1F*distance)first=now-100'000;
      if(!reach90&&position>=.9F*distance)reach90=now-100'000;
      if(std::abs(error)>5.F)last_outside=now-100'000;
      overshoot=std::max(overshoot,-error);
      if(i*cadence<=500'000)integral+=std::abs(error)*cadence/1e6;
      previous=at;
    }
    std::printf("step,%u,%.0f,%.1f,%u,%u,%u,%u,%llu,%llu,%llu,%llu,%.3f,%.3f,%u,%llu\n",
      scene++,distance,ratio,delay,estimate,age,cadence,first,reach90,last_outside,gap,overshoot,integral,cap_holds,switches);
    // Adversarial calibration/delay cases are compared with the identical
    // baseline by the audit gate; do not silently exclude a failing scene.
    assert(std::isfinite(overshoot)&&std::isfinite(integral));
  }
}
}
int main(int argc,char** argv) {
  std::setvbuf(stdout,nullptr,_IONBF,0);
#ifdef _WIN32
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
  const bool report=argc>1&&std::string_view(argv[1])=="--report";
  selection(!report);
  if(!report)selection_guards();
  if(!report)visible_replacement();
  exited_owner(!report);
  step_grid();
  std::puts("PASS acquisition latency and delayed step grid");
}
