#include "ControlSpaceTransform.hpp"
#include "MobileBackgroundMotion.hpp"
#include "MobileControlCore.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <memory>
#include <limits>
#include <vector>

using namespace vfdual_android;
namespace {
int failures{};
void check(bool passed,const char* name,double actual,double expected) {
  std::printf("%s %s actual=%.9f expected=%.9f\n",passed?"PASS":"FAIL",name,actual,expected);
  if(!passed)++failures;
}
MobileControlConfig config() {
  auto c=screen_pixel_control_config();
  c.selected_class_id=c.body_class_id=0;c.selected_class_uses_geometric_head=true;
  c.head_body_pairing_enabled=c.selected_class_uses_head_box=false;c.reject_border_touching=false;
  c.body_fallback_min_confidence=.4F;c.body_fallback_max_center_distance_pixels=c.model_width;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
  c.observation_servo_enabled=true;c.lost_target_hold_duration_us=350'000;
  c.tracker.lost_track_duration_us=500'000;return c;
}
struct Result { bool accepted{},has_target{},held{};std::uint64_t track{},lock{},constraints{};float velocity{}; };
void production_native640_is_identity() {
  const auto c=screen_pixel_control_config();
  check(vfdual::kProductionCaptureWidth==640 && vfdual::kProductionCaptureHeight==640 &&
      c.model_width==640 && c.model_height==640 && c.model_center_x==320 && c.model_center_y==320,
      "production_capture_and_control_coordinates_are_native640",c.model_width,640);
  check(kModelToControlPixelScaleX==1.F && kModelToControlPixelScaleY==1.F,
      "native640_has_no_hidden_1_6_scale",kModelToControlPixelScaleX,1);
  const auto same=[](float a,float b){return std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b);};
  const std::array boxes{YoloDetection{10.25F,20.5F,70.75F,140.125F,.9375F,7}};
  const BackgroundMotion bg{98'765,-0.F,-2.5F,8,13,true,true};
  const std::array motions{TargetImageMotion{98'765,3,2,19.25F,-11.5F,.984375F,.75F,7,9,true}};
  ControlSpaceFrame frame;
  check(frame.transform(boxes,motions,bg) && frame.box_count==1 && frame.motion_count==1,
      "identity_transform_retains_counts",double(frame.box_count),1);
  const auto& b=frame.boxes[0];const auto& m=frame.motions[0];
  check(same(b.x1,boxes[0].x1)&&same(b.y1,boxes[0].y1)&&same(b.x2,boxes[0].x2)&&same(b.y2,boxes[0].y2)&&
      same(b.confidence,boxes[0].confidence)&&b.class_id==boxes[0].class_id,
      "identity_transform_keeps_every_detection_field_bitexact",b.x1,boxes[0].x1);
  check(same(frame.background.x,bg.x)&&same(frame.background.y,bg.y)&&
      frame.background.previous_observed_at_us==bg.previous_observed_at_us&&frame.background.support==bg.support&&
      frame.background.candidates==bg.candidates&&frame.background.valid==bg.valid&&
      frame.background.large_motion_recovered==bg.large_motion_recovered,
      "identity_transform_keeps_every_background_field_and_signed_zero",frame.background.y,bg.y);
  check(m.previous_observed_at_us==motions[0].previous_observed_at_us&&m.current_index==motions[0].current_index&&
      m.previous_index==motions[0].previous_index&&same(m.x,motions[0].x)&&same(m.y,motions[0].y)&&
      same(m.quality,motions[0].quality)&&same(m.forward_backward_error,motions[0].forward_backward_error)&&
      m.support==motions[0].support&&m.candidates==motions[0].candidates&&m.valid==motions[0].valid,
      "identity_transform_keeps_every_image_motion_field_bitexact",m.forward_backward_error,motions[0].forward_backward_error);
  const MobileControlConfig defaults;
  check(c.background_motion_maximum_shift_pixels==defaults.background_motion_maximum_shift_pixels&&
      c.background_motion_ordinary_shift_pixels==defaults.background_motion_ordinary_shift_pixels&&
      c.image_motion_maximum_displacement_pixels==defaults.image_motion_maximum_displacement_pixels&&
      c.image_constraint_maximum_forward_backward_error_pixels==defaults.image_constraint_maximum_forward_backward_error_pixels&&
      c.image_identity_maximum_forward_backward_error_pixels==defaults.image_identity_maximum_forward_backward_error_pixels,
      "native640_evidence_limits_remain_in_physical_pixel_units",c.background_motion_maximum_shift_pixels,128);
  auto core=std::make_unique<MobileControlCore>(config());
  const bool restored=core->restore_response_calibration(.709262F,.707058F,24'000,.99F,.99F);
  check(restored&&same(core->response_estimator().x().response,.709262F)&&same(core->response_estimator().y().response,.707058F),
      "physical_pixels_per_count_calibration_is_not_rescaled",core->response_estimator().x().response,.709262F);
}
Result run(float dx,float dy,unsigned support=24,unsigned candidates=32,
           std::uint64_t previous=100'000,std::uint64_t now_at=108'000,
           bool motion=false,float fb=0) {
  auto core=std::make_unique<MobileControlCore>(config());
  (void)core->restore_response_calibration(.71F,.71F,16'000,.95F,.95F);
  std::array first{YoloDetection{250,240,290,400,.9F,0}};
  ControlSpaceFrame initial;
  initial.transform(first,{},{});
  const auto old=core->process({initial.boxes.data(),initial.box_count},{1,100'000,120'000});
  auto second=first;for(auto& b:second){b.x1+=dx;b.x2+=dx;b.y1+=dy;b.y2+=dy;}
  BackgroundMotion bg{previous,dx,dy,support,candidates,true,true};
  std::array image{TargetImageMotion{100'000,0,0,dx,dy,.999F,fb,8,9,true}};
  ControlSpaceFrame next;
  next.transform(second,motion?std::span<const TargetImageMotion>(image):std::span<const TargetImageMotion>{},bg);
  const auto output=core->process({next.boxes.data(),next.box_count},{2,now_at,now_at+20'000,next.background},
      {next.motions.data(),next.motion_count});
  const auto& trace=core->control_trace();const auto& sample=trace.samples[(trace.count-1)%trace.samples.size()];
  std::printf("domain model_dx=%.3f model_dy=%.3f physical_dx=%.3f physical_dy=%.3f support=%u accepted=%d has=%d held=%d track_same=%d constraints=%llu velocity=%.6f\n",
      dx,dy,next.background.x,next.background.y,support,bool(sample.flags&8),output.has_target,output.held,
      output.track_id==old.track_id,static_cast<unsigned long long>(core->tracker_metrics().image_constraints_applied),sample.velocity_x);
  return {bool(sample.flags&8),output.has_target,output.held,output.track_id,output.lock_id,
      core->tracker_metrics().image_constraints_applied,sample.velocity_x};
}

bool identity_rebind_with_error(float error) {
  auto core=std::make_unique<MobileControlCore>(config());
  (void)core->restore_response_calibration(.71F,.71F,16'000,.95F,.95F);
  for(unsigned i=0;i<3;++i) {
    const float dx=i==0?0.F:i==1?8.F:88.F;
    const std::array boxes{YoloDetection{250+dx,240,290+dx,400,.9F,0}};
    const std::array motion{TargetImageMotion{100'000+(i-1)*8'000,0,0,i==1?8.F:80.F,0,.999F,error,8,9,true}};
    ControlSpaceFrame transformed;
    transformed.transform(boxes,i?std::span<const TargetImageMotion>(motion):std::span<const TargetImageMotion>{},{});
    const auto at=100'000+i*8'000;
    (void)core->process({transformed.boxes.data(),transformed.box_count},{i+1,at,at+20'000,{}},
        {transformed.motions.data(),transformed.motion_count});
  }
  return core->metrics().image_identity_rebinds==1;
}

void no_world_feedforward_for_manual_follow() {
  auto core=std::make_unique<MobileControlCore>(config());
  (void)core->restore_response_calibration(.71F,.71F,16'000,.95F,.95F);
  float maximum_velocity{},maximum_proposal{};int maximum_move{};
  for(unsigned i=0;i<12;++i) {
    const auto at=100'000+i*16'000;
    const std::array boxes{YoloDetection{300,291.2F,340,451.2F,.9F,0}};
    const BackgroundMotion bg{at-16'000,-4.F,0,24,32,i!=0,false};
    ControlSpaceFrame transformed;transformed.transform(boxes,{},bg);
    const auto out=core->process({transformed.boxes.data(),transformed.box_count},{i+1,at,at+20'000,transformed.background});
    const auto& trace=core->control_trace();const auto& sample=trace.samples[(trace.count-1)%trace.samples.size()];
    maximum_velocity=std::max(maximum_velocity,std::abs(sample.velocity_x));
    maximum_proposal=std::max(maximum_proposal,std::abs(sample.proposal_x));
    maximum_move=std::max(maximum_move,std::abs(out.delta_x));
  }
  const float expected_world_proposal=4.F*kModelToControlPixelScaleX/0.016F;
  check(std::abs(maximum_proposal-expected_world_proposal)<.1F,
      "manual_follow_contains_nonzero_world_proposal_in_current_physical_units",maximum_proposal,expected_world_proposal);
  check(maximum_velocity<.01F && maximum_move==0,
      "manual_follow_does_not_convert_world_proposal_into_screen_feedforward",maximum_velocity,0);
}

void small_motion_bit_pattern_trace() {
  // One deterministic fingerprint for individual numeric output fields;
  // no structure padding, process time, timing benchmark, or hidden allocation.
  auto core=std::make_unique<MobileControlCore>(config());
  (void)core->restore_response_calibration(.71F,.71F,16'000,.95F,.95F);
  std::uint64_t hash=1469598103934665603ULL,records{};
  const auto add=[&](std::uint64_t v){hash^=v;hash*=1099511628211ULL;};
  const auto fp=[&](float value){add(std::bit_cast<std::uint32_t>(value));};
  for(unsigned i=0;i<1024;++i) {
    const auto at=1'000'000+i*8'000;
    const float x=320+10*std::sin(float(i)*.13F),y=320+8*std::cos(float(i)*.11F);
    const std::array boxes{YoloDetection{x-20,y-28.8F,x+20,y+131.2F,.9F,0}};
    const float dx=float(int(i%5)-2),dy=float(int(i%3)-1);
    const BackgroundMotion bg{at-8'000,dx,dy,24,32,i!=0,false};
    const std::array motion{TargetImageMotion{at-8'000,0,0,0,0,.999F,.1F,8,9,true}};
    ControlSpaceFrame transformed;transformed.transform(boxes,i?std::span<const TargetImageMotion>(motion):std::span<const TargetImageMotion>{},bg);
    const auto out=core->process({transformed.boxes.data(),transformed.box_count},{i+1,at,at+20'000,transformed.background},
        {transformed.motions.data(),transformed.motion_count});
    if(out.has_move()) (void)core->record_accepted_move(out.delta_x,out.delta_y,at+21'000);
    const auto& trace=core->control_trace();const auto& s=trace.samples[(trace.count-1)%trace.samples.size()];
    add(s.sequence);add(s.lock_id);add(s.track_id);add(s.reason);add(s.flags);add(s.velocity_flags);
    add(s.delta_x);add(s.delta_y);add(s.pending_reports);add(s.selection_flags);add(s.motion_flags);
    fp(s.target_x);fp(s.target_y);fp(s.background_x);fp(s.background_y);fp(s.pending_x);fp(s.pending_y);
    fp(s.velocity_x);fp(s.velocity_y);fp(s.predicted_error_x);fp(s.predicted_error_y);
    fp(s.proposal_x);fp(s.proposal_y);fp(s.disturbance_x);fp(s.disturbance_y);
    fp(s.response_x);fp(s.response_y);add(s.response_delay_us);
    ++records;
  }
  std::printf("small_motion_bits records=%llu fields_per_record=28 fnv64=%016llx\n",
      static_cast<unsigned long long>(records),static_cast<unsigned long long>(hash));
}

void evidence_config_validation() {
#ifndef VF_DOMAIN_BASELINE
  MobileControlConfig legacy;
  check(legacy.background_motion_maximum_shift_pixels==128 && legacy.background_motion_ordinary_shift_pixels==64 &&
      legacy.image_motion_maximum_displacement_pixels==128 &&
      legacy.image_constraint_maximum_forward_backward_error_pixels==1.5F &&
      legacy.image_identity_maximum_forward_backward_error_pixels==2.5F,
      "legacy_416_evidence_threshold_defaults_remain_exact",legacy.background_motion_maximum_shift_pixels,128);
  constexpr std::array fields{
      &MobileControlConfig::background_motion_maximum_shift_pixels,
      &MobileControlConfig::background_motion_ordinary_shift_pixels,
      &MobileControlConfig::image_motion_maximum_displacement_pixels,
      &MobileControlConfig::image_constraint_maximum_forward_backward_error_pixels,
      &MobileControlConfig::image_identity_maximum_forward_backward_error_pixels};
  unsigned rejected{};
  for(auto field:fields)for(float value:{0.F,-1.F,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
    auto c=config();c.*field=value;
    auto core=std::make_unique<MobileControlCore>();
    rejected+=!core->configure(c);
  }
  check(rejected==20,"five_evidence_thresholds_reject_nonpositive_or_nonfinite",rejected,20);
#endif
}

constexpr unsigned W=640;
std::vector<std::uint8_t> image(unsigned shift) {
  constexpr unsigned S=160;
  std::array<std::uint8_t,S*S> reduced{};unsigned seed=19;
  for(auto& value:reduced){seed=1664525U*seed+1013904223U;value=std::uint8_t(seed>>24U);}
  std::vector<std::uint8_t> rgb(W*W*6);
  for(unsigned y=0;y<W;++y)for(unsigned x=0;x<W;++x) {
    const unsigned source_x=x>=shift?x-shift:0;
    const auto value=reduced[(y/4)*S+source_x/4];
    std::fill_n(rgb.data()+(std::size_t(y)*W+x)*6,6,value);
  }
  return rgb;
}
void actual_image_to_core() {
  MobileBackgroundMotion estimator;
  const auto first=image(0),second=image(96);
  (void)estimator.observe(first,W,W,{},100'000,1,0);
  const auto bg=estimator.observe(second,W,W,{},108'000,1,0);
  std::printf("actual_image model_bg_valid=%d dx=%.9f dy=%.9f support=%u candidates=%u recovered=%d\n",
      bg.valid,bg.x,bg.y,bg.support,bg.candidates,bg.large_motion_recovered);
  check(bg.valid && std::abs(bg.x-96)<1,"640_image_estimator_produces_legal_large_motion",bg.x,96);
  if(bg.valid) {
    const auto result=run(bg.x,bg.y,bg.support,bg.candidates);
    check(result.accepted,"legal_actual_image_background_survives_control_space_transform",result.accepted,1);
  }
  MobileBackgroundMotion gap_estimator;
  (void)gap_estimator.observe(first,W,W,{},100'000,1,0);
  const auto gap=gap_estimator.observe(second,W,W,{},160'001,1,0);
  check(!gap.valid,"actual_image_source_gap_over_60ms_remains_invalid",gap.valid,0);
  MobileBackgroundMotion beyond_estimator;
  (void)beyond_estimator.observe(first,W,W,{},100'000,1,0);
  const auto beyond=beyond_estimator.observe(image(136),W,W,{},108'000,1,0);
  check(!beyond.valid,"actual_image_shift_beyond_model_128_still_rejected_by_producer",beyond.valid,0);
}
}
int main() {
  production_native640_is_identity();
  for(float shift:{79.9F,80.F,80.1F,96.F,128.F,-96.F,-128.F}) {
    const auto result=run(shift,0);
    check(result.accepted,"legal_model_background_not_rejected_by_physical_128_guard",result.accepted,1);
  }
  const auto over=run(128.1F,0),weak=run(96.F,0,7,32),bad_support=run(96.F,0,33,32),
      bad_time=run(96.F,0,24,32,99'999),ordinary=run(60.F,0,6,32),
      flow=run(96.F,0,24,32,100'000,108'000,true),error=run(8.F,0,24,32,100'000,108'000,true,1.25F);
  check(!over.accepted,"model_over_128_remains_rejected",over.accepted,0);
  check(!weak.accepted,"large_motion_weak_support_remains_rejected",weak.accepted,0);
  check(!bad_support.accepted,"support_over_candidates_remains_rejected",bad_support.accepted,0);
  check(!bad_time.accepted,"mismatched_background_previous_time_remains_rejected",bad_time.accepted,0);
  check(ordinary.accepted,"ordinary_model_motion_keeps_ordinary_support_requirement",ordinary.accepted,1);
  check(flow.constraints==1,"legal_model_image_flow_survives_physical_displacement_guard",double(flow.constraints),1);
  check(error.constraints==1,"legal_model_image_flow_error_survives_physical_error_guard",double(error.constraints),1);
  const auto exact_fb=run(8.F,0,24,32,100'000,108'000,true,1.5F),
      excess_fb=run(8.F,0,24,32,100'000,108'000,true,1.5001F),
      excess_flow=run(128.1F,0,24,32,100'000,108'000,true),
      bad_ordinary=run(60.F,0,33,32),exact_ordinary=run(64.F,0,6,32),
      excess_ordinary=run(64.0001F,0,6,32),vertical=run(0,128.F),diagonal=run(96.F,96.F,24,32,100'000,108'000,true);
  check(exact_fb.constraints==1,"image_constraint_source_fb_exact_boundary_admitted",double(exact_fb.constraints),1);
  check(excess_fb.constraints==0,"image_constraint_source_fb_over_boundary_rejected",double(excess_fb.constraints),0);
  check(excess_flow.constraints==0,"image_flow_source_displacement_over_128_rejected",double(excess_flow.constraints),0);
  check(!bad_ordinary.accepted,"ordinary_motion_does_not_admit_impossible_support_counts",bad_ordinary.accepted,0);
  check(exact_ordinary.accepted && !excess_ordinary.accepted,
      "ordinary_model_64_boundary_preserves_support_rule",exact_ordinary.accepted,1);
  check(vertical.accepted,"vertical_model_128_uses_same_control_scale",vertical.accepted,1);
  check(diagonal.accepted && diagonal.constraints==0,
      "background_per_axis_bound_does_not_relax_image_flow_radial_bound",double(diagonal.constraints),0);
  const bool identity_exact=identity_rebind_with_error(2.5F),identity_excess=identity_rebind_with_error(2.5001F);
  check(identity_exact,"image_identity_source_fb_exact_boundary_admitted",identity_exact,1);
  check(!identity_excess,"image_identity_source_fb_over_boundary_rejected",identity_excess,0);
  no_world_feedforward_for_manual_follow();
  evidence_config_validation();
  small_motion_bit_pattern_trace();
  actual_image_to_core();
  std::printf("failures=%d scope=offline_coordinate_contract_not_device_latency\n",failures);
  return failures?1:0;
}
