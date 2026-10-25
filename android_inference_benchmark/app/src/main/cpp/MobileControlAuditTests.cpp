#include "ControlReferenceFixture.hpp"
#include "MobileControlCore.hpp"
#include "MobileControlDeadline.hpp"
#include "DiagnosticTraceCursor.hpp"
#include "DetectionTrace.hpp"
#include <memory>
#include <array>
#include <cmath>
#include <cstdio>

using namespace vfdual_android;
namespace {
int failures{};
void check(bool passed, const char* name, double actual, double expected) {
  std::printf("%s %s actual=%.6f expected=%.6f\n", passed ? "PASS" : "FAIL", name, actual, expected);
  if (!passed) ++failures;
}
YoloDetection person(float x, float y, float height=100, float confidence=.9F) {
  return {x-20,y-height*.5F,x+20,y+height*.5F,confidence,0};
}
void prediction_contract() {
  MobileContinuousServo servo;
  MobileMotionPlannerConfig c;
  c.response_gain=.6F; c.response_x_px_per_count=c.response_y_px_per_count=.75F;
  c.maximum_prediction_pixels=8;
  MobileMotionPlannerInput in{.target_id=1,.observation_sequence=1,.observed_at_us=100'000,.now_us=123'000,
      .error_x=12,.error_y=0,.target_velocity_x_pixels_per_second=-1200,
      .prediction_bounds_enabled=true,.prediction_min_error_x=-8,.prediction_max_error_x=32,
      .prediction_min_error_y=-18,.prediction_max_error_y=82};
  const auto out=servo.plan(in,c,0,0,true,true,60'000);
  check(std::abs(out.predicted_error_x-in.error_x)<=8.001F,"configured_prediction_bound",out.predicted_error_x,4);
  check(out.delta_x>=0,"limited_prediction_also_limits_velocity_feedforward",out.delta_x,0);
  servo.reset(); in.error_x=5; in.target_velocity_x_pixels_per_second=600;
  in.prediction_min_error_x=3; in.prediction_max_error_x=7;
  const auto small=servo.plan(in,c,0,0,true,true,60'000);
  check(small.predicted_error_x<=7.001F,"current_target_geometry_bound",small.predicted_error_x,7);
  servo.reset();
  in.prediction_bounds_follow_target_motion=true;
  const auto future=servo.plan(in,c,0,0,true,true,60'000);
  check(std::abs(future.predicted_error_x-13)<.001F,
      "moving_target_bounds_use_same_future_time",future.predicted_error_x,13);
}
void confidence_continuity() {
  MobileTargetTrackerConfig c; c.control_smoothing_alpha=1;
  MobileTargetTracker tracker(c);
  const std::array first{person(100,200),person(130,200)};
  const auto ids=tracker.update(first,1);
  // Both boxes overlap. The old non-overlap ambiguity check does not apply.
  const std::array next{person(100,200,100,.30F),person(110,200,100,.99F)};
  const auto tracked=tracker.update(next,2);
  check(tracked[0].track_id==ids[0].track_id,"overlapping_low_confidence_owner",double(tracked[0].track_id),double(ids[0].track_id));
  check(tracked[1].track_id==ids[1].track_id,"overlapping_neighbour_identity",double(tracked[1].track_id),double(ids[1].track_id));
}
void revised_delay_ledger() {
  MobilePendingMotion ledger;
  check(ledger.accept(20,0,100'000),"ledger_accept",1,1);
  (void)ledger.predict(140'000,32'000);
  const auto revised=ledger.predict(145'000,64'000);
  check(revised.x==20,"delay_revision_preserves_recent_reports",revised.x,20);
}
void changing_body_shape() {
  auto c = reference_416_control_config();
  c.selected_class_id=c.body_class_id=0; c.selected_class_uses_geometric_head=true;
  c.reject_border_touching=false; c.body_fallback_min_confidence=0; c.body_fallback_max_center_distance_pixels=416;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2,c); c.observation_servo_enabled=true;
  // Isolate the independent visual path from the separately tested HID model.
  c.model_based_motion_feedforward=false;
  MobileControlCore core(c);
  (void)core.restore_response_calibration(.75F,.75F,60'000,.95F,.95F);
  double maximum_velocity{};
  for (unsigned i=0;i<8;++i) {
    const auto at=100'000+i*7'143U;
    const std::array boxes{person(208,240,100+float(i)*4)};
    (void)core.process(boxes,{i+1,at,at+23'000,{i?at-7'143U:0,0,0,24,32,i!=0}});
    const auto& trace=core.control_trace();
    maximum_velocity=std::max(maximum_velocity,double(std::abs(trace.samples[(trace.count-1)%trace.samples.size()].velocity_y)));
  }
  check(maximum_velocity<1,"body_shape_change_is_not_translation",maximum_velocity,0);
}
void asymmetric_box_resize_must_not_become_sustained_motion() {
  for (bool vertical : {false, true}) for (int direction : {-1, 1}) {
    auto c = reference_416_control_config();
    c.selected_class_id = c.body_class_id = 0;
    c.selected_class_uses_geometric_head = false;
    c.selected_class_uses_head_box = c.head_body_pairing_enabled = false;
    c.reject_border_touching = false; c.body_fallback_min_confidence = 0;
    c.body_fallback_max_center_distance_pixels = 416;
    apply_mobile_model_control_tuning(vfdual::kOverwatch2, c);
    c.observation_servo_enabled = true;
    c.lost_target_hold_duration_us = 350'000;
    c.tracker.lost_track_duration_us = 500'000;
    c.motion.maximum_prediction_pixels = kMobileObservationPredictionLimitPixels;
    MobileControlCore core(c);
    (void)core.restore_response_calibration(1, 1, 44'000, .95F, .95F);
    int wrong_way{}; float residual_velocity{};
    for (unsigned i = 0; i < 18; ++i) {
      const auto at = 100'000 + i * 8'000U;
      // The same person's initially extended box retracts at one edge. After
      // that single geometry correction the observed anchor is stationary.
      // No output was delivered, so the velocity cannot be our HID response.
      const float offset = direction * (i == 10 ? 62.0F : 32.0F);
      const float x = 208 + (vertical ? 0 : offset);
      const float y = 208 + (vertical ? offset : 0);
      const float w = !vertical && i == 10 ? 100.0F : 40.0F;
      const float h = vertical && i == 10 ? 160.0F : 100.0F;
      const std::array detections{YoloDetection{x-w*.5F,y-h*.5F,x+w*.5F,y+h*.5F,.95F,0}};
      const auto visible = i < 4 || i >= 10;
      const auto out = core.process(visible ? std::span<const YoloDetection>{detections} : std::span<const YoloDetection>{},
          {i+1,at,at+23'000,{i?at-8'000:0,0,0,24,32,i!=0}});
      const auto& t = core.control_trace(); const auto& sample = t.samples[(t.count-1)%t.samples.size()];
      if (i >= 12) {
        wrong_way = std::max(wrong_way, -direction * (vertical ? out.delta_y : out.delta_x));
        residual_velocity = std::max(residual_velocity, std::abs(vertical ? sample.velocity_y : sample.velocity_x));
      }
    }
    check(wrong_way == 0, "asymmetric_resize_does_not_reverse_stationary_target", wrong_way, 0);
    check(residual_velocity < 1, "settled_geometry_does_not_retain_resize_velocity", residual_velocity, 0);
  }
}
void extent_confirmation_preserves_translation_and_accepts_persistent_geometry() {
  MobileTargetTrackerConfig c;
  c.control_smoothing_alpha = 1;
  c.confirm_abrupt_extent = true;
  MobileTargetTracker tracker(c);
  const std::array initial{YoloDetection{188,158,228,258,.95F,0}};
  const auto first = tracker.update(initial,1,100'000);
  const std::array extended{YoloDetection{198,158,298,258,.95F,0}};
  const auto provisional = tracker.update(extended,2,108'000);
  const float x = (provisional[0].detection.x1+provisional[0].detection.x2)*.5F;
  check(provisional[0].track_id==first[0].track_id && std::abs(x-218)<.01F,
      "extent_confirmation_keeps_immediate_shared_edge_translation",x,218);
  const std::array persistent{YoloDetection{210,158,310,258,.95F,0}};
  const auto confirmed = tracker.update(persistent,3,116'000);
  check(confirmed[0].track_id==first[0].track_id && confirmed[0].detection.x1==210 && confirmed[0].detection.x2==310,
      "persistent_extent_is_accepted_on_next_observation",confirmed[0].detection.x2,310);
  check(tracker.metrics().control_extent_confirmations==1,
      "extent_confirmation_does_not_accumulate_position_lag",double(tracker.metrics().control_extent_confirmations),1);
}
void partially_visible_body_keeps_identity() {
  auto c = reference_416_control_config();
  c.selected_class_id=c.body_class_id=0;
  c.selected_class_uses_geometric_head=true;
  c.reject_border_touching=false;
  c.body_fallback_min_confidence=0;
  c.body_fallback_max_center_distance_pixels=416;
  c.body_min_width=c.body_min_height=2;
  c.body_max_width=c.body_max_height=416;
  c.body_min_aspect_ratio=.02F; c.body_max_aspect_ratio=50;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
  c.observation_servo_enabled=true;
  c.lost_target_hold_duration_us=350'000;
  c.tracker.lost_track_duration_us=500'000;
  MobileControlCore core(c);
  const std::array first{person(396,220)};
  const auto initial=core.process(first,{1,100'000,123'000});
  const std::array crossing{person(398,220)}; // x2=418, almost all of the body is visible.
  const auto continued=core.process(crossing,{2,108'000,131'000});
  check(continued.has_target && !continued.held,"visible_body_not_discarded_at_roi_edge",continued.held,0);
  check(continued.track_id==initial.track_id && continued.lock_id==initial.lock_id,
      "partial_body_preserves_identity",double(continued.track_id),double(initial.track_id));
  check(std::abs(continued.target_x-398)<.01F,"partial_body_keeps_unclipped_anchor",continued.target_x,398);
  // More than a numerical epsilon can legitimately cross the crop. Keep the
  // raw geometry for association; only the requested point is bounded visible.
  const std::array near{person(420,220)};
  const auto edge=core.process(near,{3,116'000,139'000});
  check(edge.has_target && !edge.held && edge.track_id==initial.track_id,
      "partly_occluded_body_remains_same_track",edge.held,0);
  check(edge.target_x>=400 && edge.target_x<=416,"partial_body_aim_stays_visible",edge.target_x,416);
  const std::array outside{person(450,220)};
  const auto absent=core.process(outside,{4,124'000,147'000});
  check(absent.held && !absent.has_move(),"fully_outside_body_cannot_move",absent.delta_x,0);
  MobileControlCore invalid(c);
  const std::array enormous{YoloDetection{-10000,-10000,10000,10000,.99F,0}};
  check(!invalid.process(enormous,{1,100'000,123'000}).has_target,
      "unbounded_box_is_not_rescued_by_clipping",0,0);
}
void manual_camera_tracking_must_not_become_extra_target_motion() {
  auto c = reference_416_control_config();
  c.selected_class_id=c.body_class_id=0; c.selected_class_uses_geometric_head=true;
  c.reject_border_touching=false; c.body_fallback_min_confidence=0;
  c.body_fallback_max_center_distance_pixels=416;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
  c.observation_servo_enabled=true;
  c.motion.maximum_prediction_pixels=kMobileObservationPredictionLimitPixels;
  MobileControlCore core(c);
  (void)core.restore_response_calibration(1,1,40'000,.95F,.95F);
  int maximum_move{};
  double maximum_velocity{};
  for (unsigned i=0;i<10;++i) {
    const auto at=100'000+i*8'000U;
    // The user already follows a moving person perfectly: the selected point
    // stays on the reticle, while the scenery moves left by four pixels.
    const std::array boxes{person(208,240)};
    const auto out=core.process(boxes,{i+1,at,at+23'000,{i?at-8'000:0,-4,0,24,32,i!=0}});
    maximum_move=std::max(maximum_move,std::abs(out.delta_x));
    const auto& trace=core.control_trace();
    maximum_velocity=std::max(maximum_velocity,double(std::abs(trace.samples[(trace.count-1)%trace.samples.size()].velocity_x)));
  }
  check(maximum_move==0,"already_aligned_manual_tracking_has_no_extra_hid",maximum_move,0);
  check(maximum_velocity<.01,"world_motion_is_not_relative_screen_motion",maximum_velocity,0);
  MobileResponseEstimator response;
  (void)response.restore(1,1,40'000,.95F,.95F);
  const std::array observations{
      MobileResponseEstimator::TargetObservation{208,208,100'000},
      MobileResponseEstimator::TargetObservation{208,208,108'000},
      MobileResponseEstimator::TargetObservation{208,208,116'000},
      MobileResponseEstimator::TargetObservation{208,208,124'000}};
  const auto stationary=response.disturbance_velocity(observations);
  check(stationary.x_valid && stationary.y_valid && stationary.x==0 && stationary.y==0,
      "zero_motion_has_explicit_validity",stationary.x_valid,1);
  response.accept(20,0,70'000);
  const auto uncertain=response.disturbance_velocity(observations);
  check(!uncertain.x_valid && uncertain.y_valid,
      "uncertain_delivery_is_not_a_measured_stop",uncertain.x_valid,0);
}
void brief_visibility_gap_must_not_accumulate_control_gain() {
  auto c = reference_416_control_config();
  c.selected_class_id=c.body_class_id=0; c.selected_class_uses_geometric_head=true;
  c.reject_border_touching=false; c.body_fallback_min_confidence=0;
  c.body_fallback_max_center_distance_pixels=416;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
  c.observation_servo_enabled=true; c.lost_target_hold_duration_us=350'000;
  c.tracker.lost_track_duration_us=500'000;
  MobileControlCore reference(c), interrupted(c);
  (void)reference.restore_response_calibration(1,1,32'000,.95F,.95F);
  (void)interrupted.restore_response_calibration(1,1,32'000,.95F,.95F);
  const std::array target{person(238,240)};
  MobileControlOutput normal{}, resumed{};
  std::uint64_t initial_lock{};
  for(unsigned i=0;i<8;++i) {
    const auto at=100'000+i*8'000U;
    const ControlFrameContext frame{i+1,at,at+23'000,{i?at-8'000:0,0,0,24,32,i!=0}};
    normal=reference.process(target,frame);
    resumed=interrupted.process(i>=2 && i<7 ? std::span<const YoloDetection>{} : std::span<const YoloDetection>{target},frame);
    if(!i) initial_lock=resumed.lock_id;
    if(i>=2 && i<7) check(resumed.held && !resumed.has_move(),"occluded_identity_has_no_output",resumed.delta_x,0);
  }
  check(resumed.has_target && !resumed.held && resumed.lock_id==initial_lock,
      "visible_return_retains_original_lock",double(resumed.lock_id),double(initial_lock));
  check(std::abs(resumed.delta_x-normal.delta_x)<=1,
      "occlusion_does_not_multiply_first_return_command",resumed.delta_x,normal.delta_x);
}
void uncertain_hid_timing_must_not_reverse_manual_tracking() {
  for (bool vertical : {false,true}) for (int direction : {-1,1}) {
    auto c = reference_416_control_config();
    c.selected_class_id=c.body_class_id=0; c.selected_class_uses_geometric_head=true;
    c.reject_border_touching=false; c.body_fallback_min_confidence=0;
    c.body_fallback_max_center_distance_pixels=416;
    apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
    c.observation_servo_enabled=true;
    c.motion.maximum_prediction_pixels=kMobileObservationPredictionLimitPixels;
    MobileControlCore core(c);
    (void)core.restore_response_calibration(1,1,40'000,.95F,.95F);
    // An earlier positive move may or may not already be visible. The player
    // keeps a person ten pixels to the same side while scenery moves four
    // pixels per observation. Screen-relative motion can be zero or positive;
    // subtracting all camera motion invents a -500 px/s target velocity.
    (void)core.record_accepted_move(vertical?0:20*direction,vertical?20*direction:0,70'000);
    int largest_wrong_way{};
    for (unsigned i=0;i<10;++i) {
      const auto at=100'000+i*8'000U;
      const std::array boxes{person(208+(vertical?0:10*direction),240+(vertical?10*direction:0))};
      const auto out=core.process(boxes,{i+1,at,at+23'000,
          {i?at-8'000:0,float(vertical?0:4*direction),float(vertical?4*direction:0),24,32,i!=0}});
      largest_wrong_way=std::max(largest_wrong_way,-direction*(vertical?out.delta_y:out.delta_x));
    }
    check(largest_wrong_way==0,"uncertain_hid_cannot_replace_retinal_motion_with_world_motion",largest_wrong_way,0);
  }
}
void geometric_rebind_must_not_restart_acquisition_gain() {
  auto c = reference_416_control_config();
  c.selected_class_id=c.body_class_id=0; c.selected_class_uses_geometric_head=true;
  c.reject_border_touching=false; c.body_fallback_min_confidence=0;
  c.body_fallback_max_center_distance_pixels=416;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
  c.observation_servo_enabled=true;
  // Force an ID rebuild for a small, overlapping box movement. The control
  // layer can rebind its established identity using its existing strict gate.
  c.tracker.maximum_center_distance_pixels=1;
  c.tracker.first_stage_iou_threshold=.95F;
  c.tracker.second_stage_iou_threshold=.90F;
  MobileControlCore core(c);
  (void)core.restore_response_calibration(1,1,32'000,.95F,.95F);
  const std::array before{person(238,240)}, after{person(246,240)};
  MobileControlOutput first{}, resumed{};
  for(unsigned i=0;i<6;++i) {
    const auto at=100'000+i*8'000U;
    resumed=core.process(i<5 ? std::span<const YoloDetection>{before} : std::span<const YoloDetection>{after},
        {i+1,at,at+23'000,{i?at-8'000:0,0,0,24,32,i!=0}});
    if(!i) first=resumed;
  }
  check(resumed.has_target && !resumed.held && resumed.lock_id==first.lock_id && resumed.track_id!=first.track_id,
      "strict_geometric_rebind_preserves_control_identity",double(resumed.lock_id),double(first.lock_id));
  MobileContinuousServo reference;
  MobileMotionPlannerInput in{.target_id=first.lock_id,.observed_at_us=132'000,.now_us=155'000,.error_x=30};
  (void)reference.plan(in,c.motion,0,0,true,true,32'000,8'000);
  in.observed_at_us=140'000; in.now_us=163'000; in.error_x=38;
  const auto expected=reference.plan(in,c.motion,0,0,true,true,32'000,8'000);
  check(resumed.delta_x==expected.delta_x,"rebound_identity_does_not_repeat_acquisition_impulse",resumed.delta_x,expected.delta_x);
}
void missing_background_must_not_transfer_lock_to_shifted_neighbour() {
  auto c = reference_416_control_config();
  c.selected_class_id=c.body_class_id=0; c.selected_class_uses_geometric_head=true;
  c.reject_border_touching=false; c.body_fallback_min_confidence=0;
  c.body_fallback_max_center_distance_pixels=416;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
  c.observation_servo_enabled=true;
  MobileControlCore core(c);
  (void)core.restore_response_calibration(1,1,32'000,.95F,.95F);
  const std::array before{person(208,240),person(268,240),person(348,240)};
  const auto first=core.process(before,{1,100'000,123'000});
  // A quick pan moves all three people left; effects leave no trustworthy
  // scenery flow for this frame. The second person now lies nearer the old
  // first-person location, so independent nearest-box matching steals its ID.
  const std::array after{person(176,240),person(236,240),person(316,240)};
  const auto second=core.process(after,{2,108'000,131'000});
  check(second.has_target && !second.held && second.track_id==first.track_id && second.lock_id==first.lock_id,
      "crowd_pan_keeps_control_identity",double(second.track_id),double(first.track_id));
  check(std::abs(second.target_x-176)<.01F,
      "crowd_pan_keeps_physical_person_not_only_track_number",second.target_x,176);
  check(second.delta_x<0,"crowd_pan_does_not_chase_opposite_neighbour",second.delta_x,-1);
  const auto& trace = core.control_trace();
  const auto& sample = trace.samples[(trace.count - 1) % trace.samples.size()];
  check((sample.flags & 128U) && !(sample.flags & 8U),
      "identity_correspondence_is_not_camera_measurement",sample.flags,128);
  check(core.metrics().measured_ego_motion_adjustments==0 &&
        core.response_estimator().x().response==1 && core.response_estimator().y().response==1,
      "crowd_correspondence_cannot_recalibrate_mouse",core.metrics().measured_ego_motion_adjustments,0);
}
void known_large_neighbour_must_not_interrupt_smaller_visible_target() {
  auto c = reference_416_control_config();
  c.selected_class_id = c.body_class_id = 0;
  c.selected_class_uses_geometric_head = true;
  c.reject_border_touching = false;
  c.body_fallback_min_confidence = 0;
  c.body_fallback_max_center_distance_pixels = 416;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
  c.observation_servo_enabled = true;
  MobileControlCore core(c);
  (void)core.restore_response_calibration(1,1,32'000,.95F,.95F);
  const std::array<YoloDetection,2> before{{
      {198,205,218,245,.9F,0},{178,145,258,305,.9F,0}}};
  const auto first = core.process(before,{1,100'000,123'000});
  const std::array<YoloDetection,2> after{{
      {214,205,234,245,.9F,0},{168,145,248,305,.3F,0}}};
  const auto second = core.process(after,{2,108'000,131'000});
  check(first.has_target && std::abs(first.target_x-208)<.01F,
      "different_size_initial_owner",first.target_x,208);
  check(second.has_target && !second.held && second.lock_id==first.lock_id && second.track_id==first.track_id,
      "known_neighbour_cannot_force_visible_target_into_hold",double(second.held),0);
  check(std::abs(second.target_x-224)<.01F && second.delta_x>0,
      "small_target_keeps_moving_toward_its_current_position",second.target_x,224);
  const auto& trace = core.control_trace();
  check((trace.samples[(trace.count-1)%trace.samples.size()].flags & 256U)!=0,
      "known_owner_resolution_is_traceable",core.tracker_metrics().known_owner_conflicts_resolved,1);
}
void missing_lock_must_not_reappear_on_continuously_visible_neighbour() {
  auto c = reference_416_control_config();
  c.selected_class_id = c.body_class_id = 0;
  c.selected_class_uses_geometric_head = true;
  c.reject_border_touching = false;
  c.body_fallback_min_confidence = 0;
  c.body_fallback_max_center_distance_pixels = 416;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
  c.observation_servo_enabled = true;
  c.lost_target_hold_duration_us = 350'000;
  c.tracker.lost_track_duration_us = 500'000;
  MobileControlCore core(c);
  (void)core.restore_response_calibration(1,1,32'000,.95F,.95F);
  const std::array before{person(208,240),person(260,240)};
  const auto first = core.process(before,{1,100'000,123'000});
  check(first.has_target && std::abs(first.target_x-208)<.01F,
      "missing_lock_initial_person",first.target_x,208);
  for (unsigned i=1; i<=3; ++i) {
    const auto at = 100'000+i*8'000;
    const std::array remaining{person(260-16*float(i),240)};
    const auto next = core.process(remaining,{i+1,at,at+23'000});
    check(next.held && !next.has_move() && next.lock_id==first.lock_id,
        "missing_lock_cannot_silently_transfer_to_known_neighbour",next.held,1);
  }
}
MobileControlConfig rebind_recovery_config() {
  auto c = reference_416_control_config();
  c.selected_class_id=c.body_class_id=0;
  c.selected_class_uses_geometric_head=true;
  c.reject_border_touching=false; c.body_fallback_min_confidence=0;
  c.body_fallback_max_center_distance_pixels=416;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
  c.observation_servo_enabled=true;
  c.lost_target_hold_duration_us=350'000;
  c.tracker.lost_track_duration_us=500'000;
  return c;
}
void rejected_first_observation_must_not_poison_replacement_identity() {
  // Use production association thresholds. A 60px discontinuity creates a
  // new ID, then its visible track returns within the strict 24px/IoU gate.
  // A co-visible neighbour follows the identical path but must stay blocked.
  for (int direction : {-1,1}) for (float pan : {-6.0F,0.0F,6.0F})
      for (unsigned rejected_frames : {1U,3U,6U}) for (bool known : {false,true}) {
    MobileControlCore core(rebind_recovery_config());
    const float x=208+direction*42.0F;
    const std::array initial{person(x,240),person(x+direction*60.0F,240)};
    const auto first=core.process(known ? std::span<const YoloDetection>{initial}
                                     : std::span<const YoloDetection>{initial}.first(1),
                                  {1,100'000,123'000});
    bool withheld=true;
    for (unsigned i=1;i<=rejected_frames;++i) {
      const auto at=100'000+i*8'000U;
      const std::array displaced{person(x+direction*60.0F+pan*i,240)};
      const auto out=core.process(displaced,{i+1,at,at+23'000,{at-8'000,pan,0,24,32,true}});
      withheld &= out.held && !out.has_move() && out.lock_id==first.lock_id;
    }
    const auto i=rejected_frames+1;
    const auto at=100'000+i*8'000U;
    const std::array aligned{person(x+direction*18.0F+pan*i,240)};
    const auto recovered=core.process(aligned,{i+1,at,at+23'000,{at-8'000,pan,0,24,32,true}});
    check(withheld,"weak_replacement_geometry_never_dispatches",withheld,1);
    check(recovered.lock_id==first.lock_id && !recovered.switched,
        "recovery_retains_lock_without_switch",double(recovered.lock_id),double(first.lock_id));
    check(known ? recovered.held && !recovered.has_move()
                : !recovered.held && recovered.has_move() && recovered.track_id!=first.track_id,
        known ? "co_visible_neighbour_stays_blocked_after_rejections"
              : "replacement_recovers_when_later_observation_aligns",recovered.held,known?1:0);
    check(known ? core.metrics().track_id_rebinds==0 : core.metrics().track_id_rebinds==1,
        "recovery_rebind_count",double(core.metrics().track_id_rebinds),known?0:1);
  }
}
void deferred_rebind_keeps_ambiguity_confidence_and_distance_gates() {
  const auto c=rebind_recovery_config();
  for (int direction : {-1,1}) {
    const float x=208+direction*42.0F;
    MobileControlCore core(c);
    const std::array initial{person(x,240)}, displaced{person(x+direction*60.0F,240)};
    const auto first=core.process(initial,{1,100'000,123'000});
    (void)core.process(displaced,{2,108'000,131'000,{100'000,0,0,24,32,true}});
    const std::array weak{person(x+direction*18.0F,240,100,.40F)};
    const auto low=core.process(weak,{3,116'000,139'000,{108'000,0,0,24,32,true}});
    check(low.held && !low.has_move(),"deferred_rebind_rejects_low_confidence",low.held,1);
    const std::array distant{person(x+direction*30.0F,240)};
    const auto far=core.process(distant,{4,124'000,147'000,{116'000,0,0,24,32,true}});
    check(far.held && !far.has_move(),"deferred_rebind_keeps_strict_distance",far.held,1);
    const std::array aligned{person(x+direction*18.0F,240)};
    const auto good=core.process(aligned,{5,132'000,155'000,{124'000,0,0,24,32,true}});
    check(!good.held && good.has_move() && good.lock_id==first.lock_id,
        "recovery_requires_all_existing_gates",good.held,0);
  }
  MobileControlCore ambiguous(c);
  const std::array initial{person(250,240)};
  const auto first=ambiguous.process(initial,{1,100'000,123'000});
  const std::array separated{person(190,240),person(310,240)};
  (void)ambiguous.process(separated,{2,108'000,131'000,{100'000,0,0,24,32,true}});
  const std::array overlap{person(232,240),person(268,240)};
  for(unsigned i=2;i<6;++i) {
    const auto at=100'000+i*8'000U;
    const auto out=ambiguous.process(overlap,{i+1,at,at+23'000,{at-8'000,0,0,24,32,true}});
    check(out.held && !out.has_move() && out.lock_id==first.lock_id,
        "two_plausible_replacements_never_choose_arbitrarily",out.held,1);
  }
  check(ambiguous.metrics().track_id_rebinds==0,"ambiguous_recovery_has_no_rebind",double(ambiguous.metrics().track_id_rebinds),0);
  MobileControlCore co_visible_later(c);
  const auto owner=co_visible_later.process(initial,{1,100'000,123'000});
  const std::array newcomer{person(310,240)};
  (void)co_visible_later.process(newcomer,{2,108'000,131'000,{100'000,0,0,24,32,true}});
  const std::array together{person(250,240),person(310,240)};
  const auto restored=co_visible_later.process(together,{3,116'000,139'000,{108'000,0,0,24,32,true}});
  check(!restored.held && restored.track_id==owner.track_id,
      "returning_owner_precedes_fresh_replacement",double(restored.track_id),double(owner.track_id));
  // Preserve the newcomer's visible association while approaching the owner.
  // An unobserved one-frame jump to the owner's former box is not enough
  // visual evidence to distinguish two otherwise identical people.
  const std::array approaching{person(290,240)}, other{person(268,240)};
  const auto approach=co_visible_later.process(approaching,{4,124'000,147'000,{116'000,0,0,24,32,true}});
  check(approach.held && !approach.has_move(),"approaching_known_neighbour_stays_blocked",approach.held,1);
  const auto blocked=co_visible_later.process(other,{5,132'000,155'000,{124'000,0,0,24,32,true}});
  check(blocked.held && !blocked.has_move() && blocked.lock_id==owner.lock_id,
      "later_co_visibility_establishes_neighbour_identity",blocked.held,1);
}
MobileControlConfig crowd_control_config() {
  auto c = reference_416_control_config();
  c.selected_class_id=c.body_class_id=0;
  c.selected_class_uses_head_box=c.head_body_pairing_enabled=false;
  c.body_fallback_y_ratio=.5F;
  c.reject_border_touching=false; c.body_fallback_min_confidence=0;
  c.body_fallback_max_center_distance_pixels=416;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
  c.observation_servo_enabled=true;
  c.lost_target_hold_duration_us=350'000;
  c.tracker.lost_track_duration_us=500'000;
  c.switch_confirmation_duration_us=45'000;
  c.tracker.high_confidence_threshold=c.tracker.new_track_confidence_threshold=.4F;
  c.tracker.low_confidence_threshold=.2F;
  return c;
}
void weak_neighbour_cannot_bypass_confidence_after_lock_expiry() {
  for(float confidence : {.27F,.40F,.46F}) {
    MobileControlCore core(crowd_control_config());
    const std::array first{person(208,208),person(320,208)};
    const auto original=core.process(first,{1,100'000,123'000});
    bool held_or_absent=true;
    for(unsigned i=1;i<=70;++i) {
      const auto at=100'000+i*16'000U;
      const std::array weak{person(320,208,100,confidence)};
      const auto out=core.process(weak,{i+1,at,at+23'000,{at-16'000,0,0,24,32,true}});
      held_or_absent &= !out.has_move() && (out.held || !out.has_target);
    }
    check(held_or_absent,"expired_lock_does_not_admit_weak_neighbour",held_or_absent,1);
    MobileControlOutput recovered;
    for(unsigned i=71;i<=76;++i) {
      const auto at=100'000+i*16'000U;
      const std::array credible{person(320,208,100,.8F)};
      recovered=core.process(credible,{i+1,at,at+23'000,{at-16'000,0,0,24,32,true}});
    }
    check(recovered.has_target && !recovered.held && recovered.lock_id!=original.lock_id,
        "credible_replacement_can_still_be_confirmed",recovered.has_target,1);
  }
  MobileControlCore original(crowd_control_config());
  const std::array first{person(208,208),person(320,208)};
  const auto initial=original.process(first,{1,100'000,123'000});
  const std::array low_owner{person(210,208,100,.27F),person(320,208)};
  const auto continued=original.process(low_owner,{2,108'000,131'000,{100'000,0,0,24,32,true}});
  check(continued.has_target && !continued.held && continued.track_id==initial.track_id,
      "weak_original_owner_is_still_retained",double(continued.track_id),double(initial.track_id));
}
void lost_neighbour_cannot_veto_fresh_visible_owner() {
  MobileControlCore core(crowd_control_config());
  const std::array first{person(208,208),person(220,208)};
  const auto initial=core.process(first,{1,100'000,123'000});
  const std::array unambiguous{person(208,208)};
  (void)core.process(unambiguous,{2,108'000,131'000,{100'000,0,0,24,32,true}});
  const std::array moving{person(214,208)};
  const auto continued=core.process(moving,{3,116'000,139'000,{108'000,0,0,24,32,true}});
  check(continued.has_target && !continued.held && continued.track_id==initial.track_id,
      "lost_neighbour_does_not_block_recent_visible_owner",double(continued.track_id),double(initial.track_id));
}
void known_neighbour_waits_for_retained_owner_before_switching() {
  for(unsigned interval : {8'000U,16'000U,25'000U})
  for(unsigned return_after : {416'000U,448'000U,480'000U})
  for(float pan : {-.5F,0.F,.5F}) for(bool reversed : {false,true}) {
    MobileControlCore core(crowd_control_config());
    std::array first{person(208,208),person(248,208)};
    if(reversed)std::swap(first[0],first[1]);
    const auto owner=core.process(first,{1,100'000,123'000});
    const auto steps=(return_after+interval-1)/interval;
    bool retained=true;
    for(unsigned i=1;i<steps;++i) {
      const auto at=100'000+i*interval;
      const std::array neighbour{person(248+pan*i,208)};
      const auto out=core.process(neighbour,{i+1,at,at+23'000,{at-interval,pan,0,24,32,true}});
      retained &= out.held && !out.has_move() && out.lock_id==owner.lock_id;
    }
    std::array returning{person(208+pan*steps,208),person(248+pan*steps,208)};
    if(!reversed)std::swap(returning[0],returning[1]);
    const auto at=100'000+steps*interval;
    const auto restored=core.process(returning,{steps+1,at,at+23'000,{at-interval,pan,0,24,32,true}});
    check(retained,"known_neighbour_does_not_take_live_retained_identity",retained,1);
    check(restored.has_target && !restored.held && restored.track_id==owner.track_id &&
        restored.lock_id==owner.lock_id && !restored.switched,
        "late_occlusion_return_keeps_original_lock",double(restored.track_id),double(owner.track_id));
  }
  for(bool known : {false,true}) {
    MobileControlCore core(crowd_control_config());
    const std::array first{person(208,208),person(248,208)};
    const auto initial=core.process(std::span<const YoloDetection>(first.data(),known?2:1),{1,100'000,123'000});
    std::uint64_t first_switch{};
    for(unsigned i=1;i<=90;++i) {
      const auto at=100'000+i*8'000U;
      const std::array replacement{person(known?248.F:320.F,208)};
      const auto out=core.process(replacement,{i+1,at,at+23'000,{at-8'000,0,0,24,32,true}});
      if(out.has_target && !out.held && out.lock_id!=initial.lock_id && !first_switch)first_switch=at-100'000;
    }
    check(first_switch && first_switch<=(known?576'000U:432'000U) &&
        (!known || first_switch>=500'000U),known?"known_neighbour_switches_after_identity_expiry":
        "new_replacement_keeps_ordinary_confirmation_delay",double(first_switch),known?500000:400000);
  }
  MobileControlCore intermittent(crowd_control_config());
  const std::array original{person(208,208),person(248,208)};
  const auto owner=intermittent.process(original,{1,100'000,123'000});
  bool retained=true;
  for(unsigned i=1;i<=59;++i) {
    const auto at=100'000+i*8'000U;
    const std::array neighbour{person(248,208)};
    const auto present=i<40 || i%4==0;
    const auto out=intermittent.process(present?std::span<const YoloDetection>(neighbour):
        std::span<const YoloDetection>{},{i+1,at,at+23'000,{at-8'000,0,0,24,32,true}});
    retained &= out.held && !out.has_move() && out.lock_id==owner.lock_id;
  }
  const auto returned=intermittent.process(original,{61,580'000,603'000,{572'000,0,0,24,32,true}});
  check(retained && returned.track_id==owner.track_id && returned.lock_id==owner.lock_id && !returned.held,
      "intermittent_neighbour_does_not_shorten_committed_retention",retained,1);
}
void expire_acquisition_owner(MobileControlCore& core) {
  const std::array initial{person(208,208)};
  (void)core.process(initial,{1,100'000,123'000});
  for(unsigned i=1;i<=50;++i) {
    const auto at=100'000+i*16'000U;
    (void)core.process({}, {i+1,at,at+23'000,{at-16'000,0,0,24,32,true}});
  }
}
void crowd_reacquisition_must_not_restart_on_ranking_fluctuation() {
  for(unsigned interval : {8'000U,12'000U,16'000U,25'000U,33'000U})
  for(unsigned count : {2U,3U}) for(bool reverse : {false,true})
  for(int direction : {-1,1}) {
    auto c=crowd_control_config();
    MobileControlCore core(c);
    expire_acquisition_owner(core);
    std::uint64_t first_acquired{}, acquired_track{};
    bool stable=true, correct_first_choice=false;
    for(unsigned i=0;i<48;++i) {
      const auto at=1'000'000+i*interval;
      // Two continuously visible people alternately rank first. A third
      // lower-ranked person and reversed detector order must not affect the
      // confirmation already in progress for the first credible choice.
      std::array detections{person(208+direction*64.F,208,100,i%2?.70F:.86F),
                            person(208-direction*64.F,208,100,i%2?.86F:.70F),
                            person(208,340,100,.65F)};
      if(reverse)std::swap(detections[0],detections[1]);
      const auto out=core.process(std::span<const YoloDetection>(detections.data(),count),
          {i+52,at,at+23'000,{i?at-interval:900'000,0,0,24,32,true}});
      if(out.has_target && !out.held) {
        if(!first_acquired) {
          first_acquired=at;
          acquired_track=out.track_id;
          correct_first_choice=std::abs(out.target_x-(208+direction*64.F))<.01F;
        }
        stable &= out.track_id==acquired_track;
      }
    }
    const auto earliest=std::max(2U*interval,
        ((45'000U+interval-1U)/interval)*interval);
    const auto delay=first_acquired?first_acquired-1'000'000:0U;
    check(first_acquired && delay==earliest,
        "crowd_rank_fluctuation_has_bounded_reacquisition",double(delay),double(earliest));
    check(correct_first_choice,"reacquisition_keeps_first_credible_choice",correct_first_choice,1);
    check(first_acquired && stable,"confirmed_crowd_choice_remains_stable",first_acquired && stable,1);
  }
}
void invalid_pending_choice_requires_fresh_confirmation() {
  for(unsigned mode : {0U,1U,2U}) {
    MobileControlCore core(crowd_control_config());
    expire_acquisition_owner(core);
    std::uint64_t first_acquired{};
    float acquired_x{};
    for(unsigned i=0;i<20;++i) {
      const auto at=1'000'000+i*8'000U;
      std::array detections{person(144,208,100,.95F),person(272,208,100,.8F)};
      std::span<const YoloDetection> visible=detections;
      if(i==3) {
        if(mode==0)visible=std::span<const YoloDetection>(detections.data()+1,1);
        if(mode==1)detections[0]=person(144,208,100,.4F);
        if(mode==2)detections[0]=person(400,208,100,.95F);
      }
      const auto out=core.process(visible,
          {i+52,at,at+23'000,{i?at-8'000:900'000,0,0,24,32,true}});
      if(out.has_target && !out.held && !first_acquired) {
        first_acquired=at;
        acquired_x=out.target_x;
      }
    }
    check(first_acquired==1'072'000,
        "invalid_pending_identity_does_not_lend_its_confirmation_time",double(first_acquired),1'072'000);
    check(std::abs(acquired_x-272)<.01F,
        "valid_replacement_is_not_interrupted_by_previous_candidate_return",acquired_x,272);
  }
  MobileControlCore startup(crowd_control_config());
  const std::array crowd{person(144,208,100,.9F),person(272,208,100,.8F)};
  const auto acquired=startup.process(crowd,{1,100'000,123'000});
  check(acquired.has_target && !acquired.held && acquired.target_x<208,
      "fresh_start_keeps_immediate_initial_choice",acquired.has_target,1);
}
void retained_identity_requires_confirmation_after_hold() {
  for(unsigned interval : {8'000U,16'000U,25'000U,33'000U}) {
    MobileControlCore core(crowd_control_config());
    const std::array initial{person(208,208),person(248,208)};
    const auto original=core.process(initial,{1,100'000,123'000});
    std::uint64_t switched_at{};
    bool protected_identity=true;
    for(unsigned i=1;i<80;++i) {
      const auto at=100'000+i*interval;
      const std::array neighbour{person(248,208)};
      const auto out=core.process(neighbour,{i+1,at,at+23'000,{at-interval,0,0,24,32,true}});
      if(i*interval<500'000)
        protected_identity &= out.held && !out.has_move() && out.lock_id==original.lock_id;
      if(out.has_target && !out.held && out.lock_id!=original.lock_id && !switched_at)switched_at=at;
    }
    const auto confirmation=std::max(2U*interval,((45'000+interval-1)/interval)*interval);
    const auto earliest=100'000+((500'000+interval-1)/interval)*interval+confirmation;
    check(protected_identity,"full_original_identity_retention_is_preserved",protected_identity,1);
    check(switched_at==earliest,"known_neighbour_keeps_post_retention_confirmation",
        double(switched_at),double(earliest));
  }
  for(unsigned mode : {0U,1U,2U}) {
    MobileControlCore core(crowd_control_config());
    const std::array initial{person(208,208),person(248,208),person(168,208)};
    const auto original=core.process(initial,{1,100'000,123'000});
    std::uint64_t switched_at{};
    float acquired_x{};
    for(unsigned i=1;i<50;++i) {
      const auto at=100'000+i*16'000U;
      std::array visible{person(mode==0 && i>=29?168.F:248.F,208)};
      std::span<const YoloDetection> observations=visible;
      if(mode==1 && i==31) observations={};
      if(mode==2 && i<32) visible[0].confidence=.4F;
      const auto out=core.process(observations,{i+1,at,at+23'000,{at-16'000,0,0,24,32,true}});
      if(out.has_target && !out.held && out.lock_id!=original.lock_id && !switched_at) {
        switched_at=at;acquired_x=out.target_x;
      }
    }
    const auto expected=660'000U;
    check(switched_at==expected,"retention_observation_does_not_preconfirm_replacement",
        double(switched_at),double(expected));
    check(std::abs(acquired_x-(mode==0?168.F:248.F))<.01F,
        "post_retention_confirmation_uses_current_valid_identity",acquired_x,mode==0?168:248);
  }
}
void mixed_confidence_group_preserves_physical_owners() {
  for (unsigned interval : {8'000U, 16'000U, 25'000U})
  for (bool reverse : {false, true}) for (bool mirror : {false, true})
  for (float threshold : {.4F, .5F}) for (float weak : {.30F, threshold-.01F}) {
    MobileTargetTrackerConfig c;
    c.lost_track_duration_us=500'000; c.control_smoothing_alpha=1;
    c.high_confidence_threshold=c.new_track_confidence_threshold=threshold;
    c.low_confidence_threshold=.2F;
    MobileTargetTracker tracker(c);
    const auto x = [&](float value) { return mirror ? 416.F-value : value; };
    const std::array first{person(x(100),208),person(x(120),208)};
    const auto original=tracker.update(first,1,100'000);
    bool identities=true;
    for (unsigned frame=1;frame<=6;++frame) {
      // Both old centres prefer the weak observation. Confidence-first
      // matching used to give it the neighbour's ID and create a third ID.
      std::array next{person(x(112+12.F*(frame-1)),208,100,weak),
                      person(x(135+12.F*(frame-1)),208,100,.9F)};
      if (reverse) std::swap(next[0],next[1]);
      const auto tracked=tracker.update(next,frame+1,100'000+frame*interval);
      identities &= tracked[reverse?1:0].track_id==original[0].track_id &&
          tracked[reverse?0:1].track_id==original[1].track_id;
    }
    check(identities,"mixed_confidence_complete_group_preserves_both_owners",identities,1);
    check(tracker.metrics().tracks_created==2,"mixed_confidence_does_not_fragment_visible_group",
        double(tracker.metrics().tracks_created),2);
  }
  for (unsigned interval : {8'000U,16'000U,25'000U})
  for (bool reverse : {false,true}) for (unsigned locked_owner : {0U,1U}) {
    MobileControlCore core(crowd_control_config());
    const float left=locked_owner==0 ? 208.F : 188.F;
    std::array initial{person(left,208),person(left+20,208)};
    const auto old=core.process(initial,{1,100'000,123'000});
    std::array next{person(left+12,208,100,.3F),person(left+35,208,100,.9F)};
    if(reverse)std::swap(next[0],next[1]);
    const auto at=100'000+interval;
    const auto out=core.process(next,{2,at,at+23'000,{100'000,0,0,24,32,true}});
    const float expected=left+(locked_owner==0?12.F:35.F);
    check(out.has_target && !out.held && !out.switched && out.lock_id==old.lock_id,
        "confidence_boundary_does_not_hold_or_switch_visible_owner",out.held,0);
    check(std::abs(out.target_x-expected)<.01F,
        "confidence_boundary_control_stays_on_physical_owner",out.target_x,expected);
  }
}
}
void confirmed_stop_and_reversal_retire_old_velocity() {
  for (unsigned interval : {8'000U,12'000U,16'000U,25'000U})
  for (float direction : {-1.F,1.F}) for (bool vertical : {false,true})
  for (bool reverse : {false,true}) {
    MobileResponseEstimator response;
    (void)response.restore(1,1,32'000,.95F,.95F);
    MobileDisturbanceVelocity velocity;
    float position=100;
    MobileResponseEstimator::VisibleMotion result;
    for(unsigned frame=0;frame<12;++frame) {
      if(frame)position+=direction*600.F*interval/1e6F;
      result=velocity.update(vertical?100:position,vertical?position:100,
          100'000+frame*interval,response,1200);
    }
    const float initial=vertical?result.y:result.x;
    check(initial*direction>550.F,"steady_motion_keeps_full_tracking_speed",initial*direction,580);
    for(unsigned frame=12;frame<15;++frame) {
      if(reverse)position-=direction*600.F*interval/1e6F;
      result=velocity.update(vertical?100:position,vertical?position:100,
          100'000+frame*interval,response,1200);
    }
    const float after=vertical?result.y:result.x;
    check(reverse ? after*direction <= .01F : std::abs(after)<.01F,
        reverse?"reversal_does_not_keep_forward_velocity":"confirmed_stop_discards_old_slope",
        after*direction,0);
  }
  for(unsigned interval : {8'000U,16'000U,25'000U}) {
    MobileResponseEstimator response;
    (void)response.restore(1,1,32'000,.95F,.95F);
    MobileDisturbanceVelocity velocity;
    float maximum{};
    for(unsigned frame=0;frame<80;++frame) {
      const float jitter=frame%2?1.F:-1.F;
      const auto result=velocity.update(200+jitter,200-jitter,100'000+frame*interval,response,1200);
      if(frame>=8)maximum=std::max({maximum,std::abs(result.x),std::abs(result.y)});
    }
    check(maximum<.01F,"alternating_box_noise_does_not_enable_recent_trend",maximum,0);
  }
}
int main() {
  {
    auto trace = std::make_unique<DetectionTrace>();
    std::array<YoloDetection,20> boxes{};
    for (unsigned n=0;n<boxes.size();++n) boxes[n]=person(float(n),208);
    trace->record(boxes, {1,100'000,130'000});
    check(trace->samples[0].total==20 && trace->samples[0].kept==16,
        "detection_trace_clipping_is_explicit", trace->samples[0].kept,16);
    check(trace->samples[0].boxes[15].x1==boxes[15].x1, "detection_trace_values_preserved",trace->samples[0].boxes[15].x1,boxes[15].x1);
    for (unsigned n=2;n<=1025;++n) trace->record({}, {n,100'000+n,130'000+n});
    check(trace->samples[0].id==1025 && trace->samples[0].kept==0,
        "detection_trace_wrap_has_no_stale_boxes",trace->samples[0].id,1025);
  }
  {
    DiagnosticTraceCursor cursor;
    auto r = cursor.advance(5, 4);
    check(r.first == 2 && r.last == 5 && r.lost == 1, "trace_overrun_is_explicit", r.lost, 1);
    r = cursor.advance(5, 4);
    check(r.first > r.last, "trace_incremental_no_duplicates", r.first, 6);
    r = cursor.advance(6, 4);
    check(r.first == 6 && r.last == 6 && r.lost == 0, "trace_incremental_next", r.first, 6);
    r = cursor.advance(2, 4);
    check(r.reset && r.first == 1 && r.last == 2, "trace_counter_reset_visible", r.first, 1);
    MobileContinuousServo servo;
    MobileMotionPlannerConfig config;
    config.response_gain = .6F; config.response_x_px_per_count = config.response_y_px_per_count = 1.F;
    MobileMotionPlannerInput input{.target_id=1, .observed_at_us=100'000, .now_us=125'000,
        .error_x=30.F, .error_y=0.F, .target_velocity_x_pixels_per_second=100.F};
    const auto output = servo.plan(input, config, 0, 0, true, true, 40'000, 10'000);
    const auto d = servo.diagnostic();
    check(std::abs(d.dt_seconds-.01F)<.000001F && std::abs(d.horizon_seconds-.065F)<.000001F,
        "trace_actual_prediction_horizon_and_cadence", d.horizon_seconds, .065);
    check(std::abs(d.x.desired-(d.x.feedback+d.x.feedforward+d.x.residual))<.0001F,
        "trace_reconstructs_command_components", d.x.desired, d.x.feedback+d.x.feedforward+d.x.residual);
    check(output.delta_x == int(std::lround(d.x.desired)), "trace_matches_unclipped_output", output.delta_x, std::lround(d.x.desired));
  }
  prediction_contract(); confidence_continuity(); revised_delay_ledger(); changing_body_shape();
  partially_visible_body_keeps_identity();
  manual_camera_tracking_must_not_become_extra_target_motion();
  uncertain_hid_timing_must_not_reverse_manual_tracking();
  asymmetric_box_resize_must_not_become_sustained_motion();
  extent_confirmation_preserves_translation_and_accepts_persistent_geometry();
  brief_visibility_gap_must_not_accumulate_control_gain();
  geometric_rebind_must_not_restart_acquisition_gain();
  missing_background_must_not_transfer_lock_to_shifted_neighbour();
  known_large_neighbour_must_not_interrupt_smaller_visible_target();
  missing_lock_must_not_reappear_on_continuously_visible_neighbour();
  rejected_first_observation_must_not_poison_replacement_identity();
  deferred_rebind_keeps_ambiguity_confidence_and_distance_gates();
  weak_neighbour_cannot_bypass_confidence_after_lock_expiry();
  lost_neighbour_cannot_veto_fresh_visible_owner();
  known_neighbour_waits_for_retained_owner_before_switching();
  crowd_reacquisition_must_not_restart_on_ranking_fluctuation();
  invalid_pending_choice_requires_fresh_confirmation();
  retained_identity_requires_confirmation_after_hold();
  mixed_confidence_group_preserves_physical_owners();
  confirmed_stop_and_reversal_retire_old_velocity();
  check(visual_move_budget_us(100'000,140'000,250'000)==10'000,"send_budget_uses_observation_age",
      double(visual_move_budget_us(100'000,140'000,250'000)),10'000);
  check(visual_move_budget_us(100'000,150'000,250'000)==0,"stale_send_not_refreshed_by_ticket",
      double(visual_move_budget_us(100'000,150'000,250'000)),0);
  check(visual_move_budget_us(0,140'000,250'000)==0,"invalid_observation_cannot_dispatch",0,0);
  std::printf("audit_regressions failures=%d\n",failures);
  return failures ? 1 : 0;
}
