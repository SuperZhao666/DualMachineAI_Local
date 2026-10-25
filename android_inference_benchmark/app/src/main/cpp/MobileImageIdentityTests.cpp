#include "ControlReferenceFixture.hpp"
#include "MobileControlCore.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
using namespace vfdual_android;
MobileControlConfig config(){
  auto c = reference_416_control_config();c.body_class_id=c.selected_class_id=0;c.head_class_id=vfdual::kNoModelClass;
  c.selected_class_uses_geometric_head=true;c.selected_class_uses_head_box=false;
  c.reject_border_touching=false;c.body_confidence_threshold=0;c.body_fallback_min_confidence=0;
  c.body_fallback_y_ratio=.18F;c.body_fallback_max_center_distance_pixels=416;
  c.body_min_width=c.body_min_height=2;c.body_max_width=c.body_max_height=416;
  c.body_min_aspect_ratio=.02F;c.body_max_aspect_ratio=50.F;
  c.tracker.high_confidence_threshold=c.tracker.new_track_confidence_threshold=.4F;
  c.tracker.low_confidence_threshold=.2F;c.motion.response_gain=.6F;c.motion.deadzone_pixels=.5F;
  c.switch_confirmation_duration_us=45'000;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
  c.observation_servo_enabled=true;c.lost_target_hold_duration_us=350'000;c.tracker.lost_track_duration_us=500'000;
  return c;
}
YoloDetection box(float x,float y=208){return {x-20,y-18,x+20,y+82,.9F,0};}
ControlFrameContext frame(unsigned n,std::uint64_t t=0){if(!t)t=100'000+(n-1)*12'000ULL;return {n,t,t+23'000};}
TargetImageMotion hint(std::uint64_t previous,float x,float y=0,unsigned index=0){
  return {previous,index,index,x,y,.96F,.4F,7U,9U,true};
}
void keeps_person_after_confirmed_pixel_correspondences(){
  for(bool invalid_prefix:{false,true})for(bool gap:{false,true}){
    MobileControlCore core(config());assert(core.restore_response_calibration(1,1,40'000,.95F,.95F));
    auto bad=box(40);bad.class_id=77;
    const std::array first{bad,box(300)};const auto offset=invalid_prefix?0U:1U;
    const auto original=core.process(std::span(first).subspan(offset),frame(1));assert(original.has_target);
    if(gap)(void)core.process({},frame(2,106'000));
    const unsigned index=invalid_prefix?1U:0U;
    const std::array second{bad,box(236)},third{bad,box(230)};
    const std::array one{hint(100'000,-64,0,index)};
    const auto pending=core.process(std::span(second).subspan(offset),frame(gap?3:2,112'000),one);
    assert(pending.held&&!pending.has_move());
    const std::array two{hint(112'000,-6,0,index)};
    const auto out=core.process(std::span(third).subspan(offset),frame(gap?4:3,124'000),two);
    assert(out.has_target&&!out.held&&!out.switched&&out.lock_id==original.lock_id&&out.has_move());
    assert(out.track_id!=original.track_id&&std::abs(out.target_x-230)<.01F&&std::abs(out.target_y-208)<.01F);
    assert(core.metrics().image_identity_rebinds==1&&core.response_estimator().x().response==1.F);
  }
}
void unsupported_correspondence_never_releases_a_lock(){
  for(unsigned mode=0;mode<8;++mode){
    MobileControlCore core(config());const std::array first{box(300)};
    const auto old=core.process(first,frame(1));const std::array second{box(236)},third{box(230)};
    auto a=hint(100'000,-64),b=hint(112'000,-6);
    if(mode==0)a.support=3;
    if(mode==1)a.quality=.85F;
    if(mode==2)a.forward_backward_error=4;
    if(mode==3)a.x=0;
    if(mode==4)a.previous_observed_at_us=90'000;
    if(mode==5)a.current_index=16;
    if(mode==6)b.previous_observed_at_us=100'000;
    const std::array one{a,a};const std::array two{b,b};
    const auto count=mode==7?2U:1U;
    auto out=core.process(second,frame(2),std::span(one).first(count));assert(out.held&&!out.has_move());
    out=core.process(third,frame(3),std::span(two).first(count));
    assert(out.held&&!out.has_move()&&out.lock_id==old.lock_id&&core.metrics().image_identity_rebinds==0);
  }
}
void known_neighbour_and_current_owner_are_not_overridden(){
  MobileControlCore core(config());const std::array first{box(240),box(350)};
  const auto old=core.process(first,frame(1));assert(old.target_x==240);
  for(unsigned n=2;n<=4;++n){
    const std::array neighbour{box(350)};auto m=hint(n==2?100'000:frame(n-1).observed_at_us,n==2?110:0);
    m.previous_index=n==2?0:0;const std::array evidence{m};
    const auto out=core.process(neighbour,frame(n),evidence);
    assert(out.held&&!out.has_move()&&out.lock_id==old.lock_id);
  }
  core.reset();const auto owner=core.process(first,frame(1));
  const std::array evidence{hint(100'000,0)};
  const auto out=core.process(first,frame(2),evidence);
  assert(!out.held&&out.lock_id==owner.lock_id&&out.target_x==240&&core.metrics().image_identity_rebinds==0);
}
void no_image_keeps_original_wait(){
  MobileControlCore core(config());const std::array first{box(300)},second{box(236)},third{box(230)};
  const auto old=core.process(first,frame(1));assert(core.process(second,frame(2)).held);
  const auto out=core.process(third,frame(3));assert(out.held&&!out.has_move()&&out.lock_id==old.lock_id);
}
void existing_owner_image_history_avoids_an_extra_confirmation_pause(){
  MobileControlCore core(config());const std::array first{box(300)},moved{box(236)};
  const auto original=core.process(first,frame(1));
  const std::array established{hint(100'000,0)},continuation{hint(112'000,-64)};
  assert(!core.process(first,frame(2),established).held);
  const auto out=core.process(moved,frame(3),continuation);
  assert(out.has_target&&!out.held&&!out.switched&&out.lock_id==original.lock_id);
  assert(core.metrics().image_identity_rebinds==1);
}
int main(){
  std::setvbuf(stdout,nullptr,_IONBF,0);
#ifdef _WIN32
  _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
  keeps_person_after_confirmed_pixel_correspondences();unsupported_correspondence_never_releases_a_lock();
  known_neighbour_and_current_owner_are_not_overridden();no_image_keeps_original_wait();
  existing_owner_image_history_avoids_an_extra_confirmation_pause();
  std::puts("PASS pixel-supported identity continuation, two observations, filter mapping, gaps, neighbours and 8 negative cases");
}
