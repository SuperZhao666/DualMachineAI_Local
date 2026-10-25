#include "ControlReferenceFixture.hpp"
// Pixel-backed identity contracts; no device access or HID output.
#include "MobileTargetImageMotion.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace vfdual_android;
namespace {
constexpr int W=416;
std::vector<std::uint8_t> texture(unsigned seed) {
  std::vector<std::uint8_t> noise(W*W),out(W*W);
  for(auto& v:noise){seed=1664525U*seed+1013904223U;v=std::uint8_t(seed>>24U);}
  for(int y=2;y<W-2;++y)for(int x=2;x<W-2;++x){
    int sum{};for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx)sum+=noise[(y+dy)*W+x+dx];
    out[y*W+x]=std::uint8_t(std::clamp(128+(sum/25-128)*4,0,255));
  }
  return out;
}
float sample(const std::vector<std::uint8_t>& image,float x,float y) {
  x=std::clamp(x,0.F,float(W-2));y=std::clamp(y,0.F,float(W-2));
  const int ix=int(x),iy=int(y);const float fx=x-ix,fy=y-iy;
  return image[iy*W+ix]*(1-fx)*(1-fy)+image[iy*W+ix+1]*fx*(1-fy)+
      image[(iy+1)*W+ix]*(1-fx)*fy+image[(iy+1)*W+ix+1]*fx*fy;
}
struct Actor {YoloDetection box;const std::vector<std::uint8_t>* pattern;};
std::vector<std::uint8_t> scene(const std::vector<std::uint8_t>& background,
    float bgx,float bgy,std::span<const Actor> actors,float brightness=0) {
  std::vector<std::uint8_t> out(W*W*6);
  for(int y=0;y<W;++y)for(int x=0;x<W;++x){
    float value=sample(background,x-bgx,y-bgy);
    for(const auto& a:actors)if(x>=a.box.x1&&x<a.box.x2&&y>=a.box.y1&&y<a.box.y2)
      value=sample(*a.pattern,x-a.box.x1+80,y-a.box.y1+80)+brightness;
    std::fill_n(out.begin()+(y*W+x)*6,6,std::uint8_t(std::clamp(std::lround(value),0L,255L)));
  }
  return out;
}
YoloDetection shifted(YoloDetection b,float x,float y){b.x1+=x;b.x2+=x;b.y1+=y;b.y2+=y;return b;}
}
#include "MobileControlCore.hpp"
MobileControlConfig control_config(){
  auto c = reference_416_control_config();c.body_class_id=c.selected_class_id=0;c.head_class_id=vfdual::kNoModelClass;
  c.selected_class_uses_geometric_head=true;c.reject_border_touching=false;
  c.body_confidence_threshold=c.body_fallback_min_confidence=0;c.body_fallback_max_center_distance_pixels=416;
  c.body_min_width=c.body_min_height=2;c.body_max_width=c.body_max_height=416;
  c.body_min_aspect_ratio=.02F;c.body_max_aspect_ratio=50.F;
  c.tracker.high_confidence_threshold=c.tracker.new_track_confidence_threshold=.4F;c.tracker.low_confidence_threshold=.2F;
  c.motion.response_gain=.6F;c.motion.deadzone_pixels=.5F;c.motion.maximum_prediction_pixels=kMobileObservationPredictionLimitPixels;
  c.switch_confirmation_duration_us=45'000;apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
  c.observation_servo_enabled=true;c.lost_target_hold_duration_us=350'000;c.tracker.lost_track_duration_us=500'000;return c;
}
bool close_people_should_request_pixel_evidence(){
  const auto bg=texture(13),one=texture(57),two=texture(213);
  const YoloDetection a{160,130,200,270,.95F,0},b{190,130,230,270,.9F,0};
  const std::array old_actors{Actor{a,&one},Actor{b,&two}};const std::array old_boxes{a,b};
  const auto na=shifted(a,15,0),nb=shifted(b,15,0);
  const std::array new_actors{Actor{na,&one},Actor{nb,&two}};const std::array new_boxes{nb,na};
  const auto old_image=scene(bg,0,0,old_actors),new_image=scene(bg,0,0,new_actors);
  bool selective_good{};
  for(bool selective:{false,true}){
    MobileTargetImageMotion tracker;
    (void)tracker.observe(old_image,W,W,old_boxes,{},100'000,1,.18F,0,selective);
    const auto result=tracker.observe(new_image,W,W,new_boxes,{},112'000,1,.18F,0,selective);
    const bool good=result.size()==2&&result[0].valid&&result[0].previous_index==1&&result[1].valid&&result[1].previous_index==0;
    std::printf("close selective=%d good=%d first=%d/%u/%.3f second=%d/%u/%.3f\n",selective,good,result[0].valid,result[0].previous_index,result[0].quality,result[1].valid,result[1].previous_index,result[1].quality);
    if(!selective&&!good)return false;
    if(selective)selective_good=good;
  }
  return selective_good;
}
bool image_known_neighbour_must_not_inherit_occluded_owner(bool selective){
  const auto bg=texture(13),one=texture(57),two=texture(213);
  const YoloDetection a{188,182.8F,228,322.8F,.95F,0},b{218,182.8F,258,322.8F,.9F,0};
  const std::array old_actors{Actor{a,&one},Actor{b,&two}};const std::array old_boxes{a,b};
  MobileTargetImageMotion image;MobileControlCore core(control_config());
  assert(core.restore_response_calibration(.95F,1.06F,52'000,.95F,.95F));
  const auto first=image.observe(scene(bg,0,0,old_actors),W,W,old_boxes,{},100'000,1,.18F,0,selective);
  const auto locked=core.process(old_boxes,{1,100'000,123'000},first);
  assert(locked.has_target&&std::abs(locked.target_x-208)<.1F);
  bool good=true;
  for(unsigned frame=0;frame<2;++frame){
    const auto at=125'000+frame*25'000;const auto peer=shifted(b,-30.F-frame*10.F,0);
    const std::array actors{Actor{peer,&two}};const std::array boxes{peer};
    const BackgroundMotion camera{at-25'000,0,0,24,32,true};
    const auto motion=image.observe(scene(bg,0,0,actors),W,W,boxes,camera,at,1,.18F,0,selective);
    const auto out=core.process(boxes,{frame+2U,at,at+23'000,camera},motion);
    std::printf("peer frame=%u pixel_valid=%d previous_index=%u support=%u quality=%.3f lock=%llu track=%llu held=%d dx=%d target=%.3f\n",frame,motion[0].valid,motion[0].previous_index,motion[0].support,motion[0].quality,static_cast<unsigned long long>(out.lock_id),static_cast<unsigned long long>(out.track_id),out.held,out.delta_x,out.target_x);
    if(frame==0 && (!motion[0].valid||motion[0].previous_index!=1||motion[0].quality<.90F||motion[0].support<5))return false;
    good &= out.lock_id==locked.lock_id&&out.held&&!out.has_move();
  }
  const auto peer=shifted(b,-50,0);const std::array returned_actors{Actor{peer,&two},Actor{a,&one}};const std::array returned_boxes{peer,a};
  const BackgroundMotion camera{150'000,0,0,24,32,true};
  const auto motion=image.observe(scene(bg,0,0,returned_actors),W,W,returned_boxes,camera,175'000,1,.18F,0,selective);
  const auto recovered=core.process(returned_boxes,{4,175'000,198'000,camera},motion);
  std::printf("owner_return lock=%llu held=%d target=%.3f\n",static_cast<unsigned long long>(recovered.lock_id),recovered.held,recovered.target_x);
  good &= recovered.has_target&&!recovered.held&&recovered.lock_id==locked.lock_id&&std::abs(recovered.target_x-208)<1.F;
  return good;
}
#include "MobileTargetTracker.hpp"
#include "MobileControlCore.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
using namespace vfdual_android;
MobileTargetTrackerConfig cfg(){MobileTargetTrackerConfig c;c.high_confidence_threshold=c.new_track_confidence_threshold=.4F;c.low_confidence_threshold=.2F;c.control_smoothing_alpha=1;c.lost_track_duration_us=500'000;return c;}
YoloDetection box(float x,float score=.9F,unsigned cls=0){return {x-20,160,x+20,300,score,cls};}
void identified_peer_and_no_stale_reservation(){
  MobileTargetTracker t(cfg());const std::array old{box(208),box(238)};
  auto first=t.update(old,1,100'000);const auto a=first[0].track_id,b=first[1].track_id;
  std::vector<TrackedDetection> result;
  const std::array now{box(208)};const std::array hints{TrackImageConstraint{0,b,100'000}};
  t.update_into(now,2,120'000,result,hints);assert(result[0].track_id==b);
  assert(t.metrics().image_constraints_applied==1&&t.metrics().image_disallowed_pairs_skipped>0);
  const std::array next{box(208),box(198)};
  t.update_into(next,3,140'000,result);assert(result[0].track_id==a&&result[1].track_id==b);
}
void duplicate_and_invalid_claims_do_not_select_an_identity(){
  for(unsigned mode=0;mode<5;++mode){
    MobileTargetTracker t(cfg());const std::array old{box(208),box(238)};const auto ids=t.update(old,1,100'000);
    std::array<TrackImageConstraint,2> h{};unsigned count=1;
    if(mode==0){h={TrackImageConstraint{0,ids[0].track_id,100'000},TrackImageConstraint{1,ids[0].track_id,100'000}};count=2;}
    if(mode==1){h={TrackImageConstraint{0,ids[0].track_id,100'000},TrackImageConstraint{0,ids[1].track_id,100'000}};count=2;}
    if(mode==2)h[0]={0,ids[1].track_id,99'999};
    if(mode==3)h[0]={16,ids[1].track_id,100'000};
    if(mode==4)h[0]={0,999,100'000};
    std::vector<TrackedDetection> r;t.update_into(old,2,120'000,r,{h.data(),count});
    assert(r[0].track_id==ids[0].track_id&&r[1].track_id==ids[1].track_id);
    assert(t.metrics().image_constraints_applied==0&&t.metrics().image_constraints_rejected>0);
  }
}
void constraint_permutations_are_identical(){
  std::array<unsigned,3> order{0,1,2};std::array<std::uint64_t,2> reference{};bool first=true;
  do{
    MobileTargetTracker t(cfg());const std::array old{box(208),box(238)};const auto ids=t.update(old,1,100'000);
    const std::array all{TrackImageConstraint{0,ids[0].track_id,100'000},TrackImageConstraint{0,ids[1].track_id,100'000},TrackImageConstraint{1,ids[1].track_id,100'000}};
    const std::array h{all[order[0]],all[order[1]],all[order[2]]};std::vector<TrackedDetection> r;
    t.update_into(old,2,120'000,r,h);const std::array got{r[0].track_id,r[1].track_id};
    if(first){reference=got;first=false;}else assert(got==reference);
    assert(t.metrics().image_constraints_applied==1);
  }while(std::next_permutation(order.begin(),order.end()));
}
void weak_observation_reserves_owner_and_geometry_stays_required(){
  MobileTargetTracker t(cfg());const std::array old{box(208),box(238)};const auto ids=t.update(old,1,100'000);
  const std::array now{box(238),box(208,.23F)};
  const std::array h{TrackImageConstraint{1,ids[1].track_id,100'000}};
  std::vector<TrackedDetection> r;t.update_into(now,2,120'000,r,h);
  assert(r[1].track_id==ids[1].track_id&&r[0].track_id!=ids[1].track_id);
  MobileTargetTracker far(cfg());const auto f=far.update(old,1,100'000);const std::array distant{box(350)};
  const std::array far_hint{TrackImageConstraint{0,f[1].track_id,100'000}};
  far.update_into(distant,2,120'000,r,far_hint);assert(r[0].track_id!=f[0].track_id&&r[0].track_id!=f[1].track_id);
}
MobileControlConfig core_cfg(){
  auto c = reference_416_control_config();c.body_class_id=c.selected_class_id=0;c.head_class_id=vfdual::kNoModelClass;
  c.selected_class_uses_geometric_head=true;c.reject_border_touching=false;
  c.body_confidence_threshold=c.body_fallback_min_confidence=0;c.body_fallback_max_center_distance_pixels=416;
  c.body_min_width=c.body_min_height=2;c.body_max_width=c.body_max_height=416;c.body_min_aspect_ratio=.02F;c.body_max_aspect_ratio=50.F;
  c.tracker.high_confidence_threshold=c.tracker.new_track_confidence_threshold=.4F;c.tracker.low_confidence_threshold=.2F;
  c.switch_confirmation_duration_us=45'000;apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
  c.observation_servo_enabled=true;c.lost_target_hold_duration_us=350'000;c.tracker.lost_track_duration_us=500'000;return c;
}
void core_checks_evidence_and_filtered_raw_indices(){
  const std::array old{box(208),box(238)};const std::array now{box(208)};
  TargetImageMotion strong{.previous_observed_at_us=100'000,.current_index=0,.previous_index=1,
      .x=-30,.y=0,.quality=.99F,.forward_backward_error=0,.support=8,.candidates=8,.valid=true};
  for(unsigned mode=0;mode<12;++mode){
    MobileControlCore core(core_cfg());const auto initial=core.process(old,{1,100'000,123'000});auto m=strong;
    switch(mode){
      case 0:m.quality=.969F;break;
      case 1:m.support=5;break;
      case 2:m.candidates=10;break;
      case 3:m.forward_backward_error=1.51F;break;
      case 4:m.x=std::numeric_limits<float>::quiet_NaN();break;
      case 5:m.previous_observed_at_us=99'999;break;
      case 6:m.current_index=16;break;
      case 7:m.previous_index=16;break;
      case 8:m.x=0;break;
      case 9:m.valid=false;break;
      case 10:m.support=9;break;
      case 11:m.previous_observed_at_us=120'000;break;
    }
    const auto out=core.process(now,{2,120'000,143'000},{&m,1});
    assert(out.has_target&&!out.held&&out.lock_id==initial.lock_id&&out.track_id==initial.track_id);
    assert(core.tracker_metrics().image_constraints_applied==0);
  }
  MobileControlCore core(core_cfg());const auto initial=core.process(old,{1,100'000,123'000});
  const std::array raw{YoloDetection{0,0,0,0,.9F,0},box(208)};strong.current_index=1;
  const auto held=core.process(raw,{2,120'000,143'000},{&strong,1});
  assert(held.held&&!held.has_move()&&held.lock_id==initial.lock_id&&core.tracker_metrics().image_constraints_applied==1);
}
int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  assert(close_people_should_request_pixel_evidence());
  assert(image_known_neighbour_must_not_inherit_occluded_owner(false));
  assert(image_known_neighbour_must_not_inherit_occluded_owner(true));
  identified_peer_and_no_stale_reservation();
  duplicate_and_invalid_claims_do_not_select_an_identity();
  constraint_permutations_are_identical();
  weak_observation_reserves_owner_and_geometry_stays_required();
  core_checks_evidence_and_filtered_raw_indices();
  std::puts("PASS image association: ambiguity, known-neighbour occlusion, ownership, expiry, conflicts, weak evidence and raw indices");
  return 0;
}
