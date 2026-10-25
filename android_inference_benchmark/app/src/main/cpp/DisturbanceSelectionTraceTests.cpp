#include "ControlReferenceFixture.hpp"
#include "DisturbanceSelectionTrace.hpp"
#include "MobileDisturbanceVelocity.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>

namespace {
bool count_allocations{};
std::size_t allocation_count{};
}
void* operator new(std::size_t size) {
  if(count_allocations) ++allocation_count;
  if(void* pointer=std::malloc(size?size:1)) return pointer;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer,std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer,std::size_t) noexcept { std::free(pointer); }

using namespace vfdual_android;
namespace {
using Branch=MobileResponseEstimator::DisturbanceBranch;
using Window=MobileResponseEstimator::DisturbanceWindow;

MobileResponseEstimator::VisibleMotion real_boundary(float speed,bool proposal=true) {
  MobileResponseEstimator response;
  assert(response.restore(.709F,.709F,24'000,.99F,.99F));
  response.accept(64,0,125'000);
  std::array<MobileResponseEstimator::TargetObservation,6> points{};
  for (unsigned n=0;n<6;++n) {
    const auto at=100'000+n*20'000ULL;
    const float own=at>=149'000?64.F*.709F:0.F;
    points[n]={400.F+speed*float(at-100'000)/1e6F-own,208.F,at};
  }
  return response.disturbance_velocity(points,400,0,proposal);
}
void actual_interval_boundary_is_reported_without_fixing_the_jump() {
  const auto before=real_boundary(206),after=real_boundary(207);
  assert(before.x_evidence.has_interval && after.x_evidence.has_interval);
  assert(before.x_evidence.minimum>19 && before.x_evidence.minimum<20);
  assert(after.x_evidence.minimum>20 && after.x_evidence.minimum<21);
  assert(before.x_evidence.branch==Branch::proposal_fallback);
  assert(after.x_evidence.branch==Branch::conservative);
  // This is evidence of the existing discontinuity, NOT an algorithm fix.
  assert(before.x>200.F && after.x>.6F && after.x<.7F);
  assert(before.x-after.x>200.F);
  assert(before.x_valid && after.x_valid);
  assert(before.x_evidence.points==6 && before.x_evidence.span_us==100'000);
  assert(before.x_evidence.first_observed_at_us==100'000 && before.x_evidence.last_observed_at_us==200'000);
  assert(before.x_evidence.available_commands==1 && before.x_evidence.delay_hypotheses==15);
  assert(before.x_evidence.delay_low_us==0 && before.x_evidence.delay_high_us==56'000);
  assert(before.x_evidence.quality_weight==1 && after.x_evidence.quality_weight==1);
  std::printf("PASS real min20 boundary: min %.6f -> %.6f selected %.6f -> %.6f branches %u -> %u\n",
      before.x_evidence.minimum,after.x_evidence.minimum,before.x,after.x,
      unsigned(before.x_evidence.branch),unsigned(after.x_evidence.branch));
}
void true_zero_and_insufficient_are_distinct() {
  MobileResponseEstimator response;
  const std::array<MobileResponseEstimator::TargetObservation,1> single{{{12,18,100'000}}};
  const auto initial=response.disturbance_velocity(single);
  assert(!initial.x_valid && !initial.x_evidence.has_interval);
  assert(initial.x_evidence.branch==Branch::insufficient && initial.x_evidence.points==1);
  assert(response.restore(.709F,.709F,24'000,.99F,.99F));
  const std::array<MobileResponseEstimator::TargetObservation,2> fixed{{{12,18,100'000},{12,18,120'000}}};
  const auto zero=response.disturbance_velocity(fixed,400,400,true);
  assert(zero.x==0.F && zero.x_valid && zero.x_evidence.has_interval);
  assert(zero.x_evidence.minimum==0 && zero.x_evidence.maximum==0 && zero.x_evidence.branch==Branch::nearzero);
  const std::array<MobileResponseEstimator::TargetObservation,2> moving{{{12,18,100'000},{22,18,120'000}}};
  const auto short_history=response.disturbance_velocity(moving,400,0,true);
  assert(!short_history.x_valid && short_history.x_evidence.has_interval);
  assert(short_history.x_evidence.branch==Branch::insufficient && short_history.x_evidence.points==2);
  const auto uncertain=real_boundary(206,false);
  assert(uncertain.x==0.F && !uncertain.x_valid && uncertain.x_evidence.has_interval);
  assert(uncertain.x_evidence.branch==Branch::conservative);
}
void caller_retirement_reports_the_selected_window() {
  MobileResponseEstimator response;
  assert(response.restore(.709F,.709F,24'000,.99F,.99F));
  MobileDisturbanceVelocity recent;
  MobileResponseEstimator::VisibleMotion stopped;
  for (unsigned i=0;i<6;++i)
    stopped=recent.update(i?10.F:0.F,208.F,100'000+i*20'000ULL,response,1200.F,400,0);
  assert(stopped.x==0 && stopped.x_valid && (recent.retired_axes()&1U));
  assert(stopped.x_evidence.branch==Branch::nearzero);
  assert(stopped.x_evidence.window==Window::recent_four && stopped.x_evidence.points==4);
  assert(stopped.x_evidence.first_observed_at_us==140'000 && stopped.x_evidence.last_observed_at_us==200'000);
  assert(stopped.x_evidence.span_us==60'000 && std::abs(stopped.x_evidence.minimum)<1e-6 && std::abs(stopped.x_evidence.maximum)<1e-6);

  MobileDisturbanceVelocity geometry;
  MobileResponseEstimator::VisibleMotion bounded;
  for (unsigned i=0;i<4;++i)
    bounded=geometry.update(208.F,208.F+10.F*i,100'000+i*20'000ULL,response,1200.F,
                            0,400,0,-10.F*i);
  assert(bounded.y==0 && bounded.y_valid && (geometry.geometry_bounded_axes()&2U));
  assert(bounded.y_evidence.branch==Branch::nearzero);
  assert(bounded.y_evidence.window==Window::control_point && bounded.y_evidence.points==4);
  assert(std::abs(bounded.y_evidence.minimum)<1e-6 && std::abs(bounded.y_evidence.maximum)<1e-6);
}
DisturbanceSelectionTrace::Context context(std::uint64_t n=0) {
  return {.control_trace_id=17+n,.sequence=23+n,.observed_at_us=200'000+n*20'000,
      .now_us=242'000+n*20'000,.wire_frame_id=333+n,.lock_id=9,.track_id=11,
      .frame_interval_us=20'000,.current_delay_us=24'000,.restored_delay_us=24'000,
      .error_x=1.25F,.error_y=-2.5F,.response_x=.709F,.response_y=.707F};
}
void ring_fields_and_event_filter() {
  auto trace=std::make_unique<DisturbanceSelectionTrace>();
  auto snapshot=std::make_unique<DisturbanceSelectionTrace::Snapshot>();
  auto motion=real_boundary(206);
  auto c=context();
  trace->observe(c,motion,400,0,motion.x,motion.y,true,true,1,2);
  trace->drain(*snapshot);
  assert(snapshot->count==2 && snapshot->lost==0 && !snapshot->reset);
  const auto& row=snapshot->rows[0];
  assert(row.size()==35 && row[0]==1 && row[1]==17 && row[2]==23 && row[3]==200'000 && row[4]==242'000);
  assert(row[5]==333 && row[6]==9 && row[7]==11 && row[8]==0);
  assert((row[9]&DisturbanceSelectionTrace::first_context) && (row[9]&DisturbanceSelectionTrace::has_interval));
  assert((row[9]&DisturbanceSelectionTrace::estimator_valid) && (row[9]&DisturbanceSelectionTrace::trusted));
  assert(row[10]==2 && row[11]==0 && row[12]==6 && row[13]==100'000 && row[14]==200'000 && row[15]==100'000);
  assert(row[16]==0 && row[17]==56'000 && row[18]==15);
  assert(row[19]==std::llround(motion.x_evidence.minimum*1000) && row[20]==std::llround(motion.x_evidence.maximum*1000));
  assert(row[21]==std::llround(motion.x_evidence.raw_quality*1e6) && row[22]==1'000'000);
  assert(row[23]==400'000 && row[24]==std::llround(motion.x*1000) && row[25]==row[24]);
  assert(row[26]==1250 && row[27]==0 && row[28]==709'000 && row[29]==24'000 && row[30]==24'000);
  assert(row[31]==20'000 && row[32]==1 && row[33]==1 && row[34]==2);
  assert(snapshot->rows[1][8]==1 && snapshot->rows[1][26]==-2500 && snapshot->rows[1][28]==707'000);
  // Sliding point timestamps/span or quality alone must not emit every frame.
  c=context(1);
  motion.x_evidence.first_observed_at_us+=20'000;
  motion.x_evidence.last_observed_at_us+=21'000;
  motion.x_evidence.span_us+=1000;
  trace->observe(c,motion,400,0,motion.x+1,motion.y,true,true,0,0);
  trace->drain(*snapshot);
  assert(snapshot->count==0 && snapshot->lost==0 && !trace->has_pending());
  c=context(2);
  motion.x_evidence.branch=Branch::conservative;
  trace->observe(c,motion,400,0,motion.x+1,motion.y,true,true,0,0);
  trace->drain(*snapshot);
  assert(snapshot->count==1 && (snapshot->rows[0][9]&DisturbanceSelectionTrace::branch_changed));
  c=context(3);
  trace->observe(c,motion,400,0,motion.x+151,motion.y,true,true,0,0);
  trace->drain(*snapshot);
  assert(snapshot->count==1 && (snapshot->rows[0][9]&DisturbanceSelectionTrace::near_target_jump));
  c=context(4); c.error_x=30;
  trace->observe(c,motion,400,0,motion.x+351,motion.y,true,true,0,0);
  trace->drain(*snapshot);
  assert(snapshot->count==0);
  c=context(5); motion.x_evidence.points=4;
  trace->observe(c,motion,400,0,motion.x+351,motion.y,true,true,0,0);
  trace->drain(*snapshot);
  assert(snapshot->count==1 && (snapshot->rows[0][9]&DisturbanceSelectionTrace::window_changed));
  trace->reset_observation_history();
  c=context(6);
  trace->observe(c,motion,400,0,motion.x+351,motion.y,true,true,0,0);
  trace->drain(*snapshot);
  assert(snapshot->count==2 && (snapshot->rows[0][9]&DisturbanceSelectionTrace::first_context));
}
void overflow_is_explicit_and_serialization_has_exact_arity() {
  auto trace=std::make_unique<DisturbanceSelectionTrace>();
  auto snapshot=std::make_unique<DisturbanceSelectionTrace::Snapshot>();
  auto motion=real_boundary(206);
  constexpr unsigned observations=200;
  for (unsigned i=0;i<observations;++i) {
    motion.x_evidence.branch=motion.y_evidence.branch=i%2?Branch::conservative:Branch::proposal_fallback;
    trace->observe(context(i),motion,400,0,motion.x,motion.y,true,true,0,0);
  }
  trace->drain(*snapshot);
  assert(trace->total()==2*observations && snapshot->count==DisturbanceSelectionTrace::capacity);
  assert(snapshot->lost==2*observations-DisturbanceSelectionTrace::capacity);
  assert(snapshot->rows[0][0]==std::int64_t(snapshot->lost+1) && snapshot->rows.back()[0]==2*observations);
  const auto report=snapshot->report();
  assert(report.find("disturbance_selection_trace_lost=144")!=std::string::npos);
  const auto start=report.find("disturbance_selection_trace_v1=[");
  assert(start!=std::string::npos && report.back()==']');
  unsigned fields=1,rows=0;
  for (auto at=report.find('[',start)+1;at<report.size();++at) {
    if (report[at]==',') ++fields;
    if (report[at]==';' || report[at]==']') { assert(fields==35); fields=1; ++rows; }
  }
  assert(rows==DisturbanceSelectionTrace::capacity);
  trace->drain(*snapshot);
  assert(snapshot->count==0 && snapshot->lost==0);
}
void actual_core_trace_ids_and_selected_values_join() {
  auto config=reference_416_control_config();
  config.selected_class_id=config.body_class_id=0;
  config.head_body_pairing_enabled=false; config.body_fallback_y_ratio=.5F;
  config.reject_border_touching=false; config.body_fallback_min_confidence=0;
  config.body_fallback_max_center_distance_pixels=416;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2,config);
  config.observation_servo_enabled=true;
  auto core=std::make_unique<MobileControlCore>(config);
  assert(core->restore_response_calibration(.709F,.707F,24'000,.99F,.99F));
  for (unsigned i=0;i<30;++i) {
    const float x=208.F+.2F*i;
    const std::array target{YoloDetection{x-20,158,x+20,258,.95F,0}};
    const auto observed=100'000+i*8'000ULL;
    const auto output=core->process(target,{i+1,observed,observed+23'000,{},555+i});
    assert(output.has_target);
  }
  auto snapshot=std::make_unique<DisturbanceSelectionTrace::Snapshot>();
  core->drain_disturbance_selection_trace(*snapshot);
  assert(snapshot->count>=4 && snapshot->lost==0);
  const auto& control=core->control_trace();
  for (std::size_t i=0;i<snapshot->count;++i) {
    const auto& row=snapshot->rows[i];
    assert(row[1]>0 && std::uint64_t(row[1])<=control.count);
    const auto& sample=control.samples[(row[1]-1)%control.samples.size()];
    assert(row[1]==std::int64_t(sample.id) && row[2]==std::int64_t(sample.sequence));
    assert(row[3]==std::int64_t(sample.observed_at_us) && row[5]==sample.wire_frame_id);
    assert(row[6]==std::int64_t(sample.lock_id) && row[7]==std::int64_t(sample.track_id));
    const bool y=row[8]!=0;
    assert(row[23]==std::llround((y?sample.proposal_y:sample.proposal_x)*1000));
    assert(row[24]==std::llround((y?sample.disturbance_y:sample.disturbance_x)*1000));
    assert(row[25]==std::llround((y?sample.velocity_y:sample.velocity_x)*1000));
    assert(row[29]==std::int64_t(sample.response_delay_us) && row[30]==24'000);
  }
}
void numeric_observe_and_drain_allocate_nothing() {
  auto trace=std::make_unique<DisturbanceSelectionTrace>();
  auto snapshot=std::make_unique<DisturbanceSelectionTrace::Snapshot>();
  auto motion=real_boundary(206);
  allocation_count=0; count_allocations=true;
  for(unsigned i=0;i<500;++i) {
    motion.x_evidence.branch=motion.y_evidence.branch=i%2?Branch::conservative:Branch::nearzero;
    trace->observe(context(i),motion,400,0,motion.x,motion.y,true,true,0,0);
    if(i%80==79) trace->drain(*snapshot);
  }
  trace->drain(*snapshot);
  count_allocations=false;
  assert(allocation_count==0);
}
}
int main() {
  std::setvbuf(stdout,nullptr,_IONBF,0);
  actual_interval_boundary_is_reported_without_fixing_the_jump();
  true_zero_and_insufficient_are_distinct();
  caller_retirement_reports_the_selected_window();
  ring_fields_and_event_filter();
  overflow_is_explicit_and_serialization_has_exact_arity();
  actual_core_trace_ids_and_selected_values_join();
  numeric_observe_and_drain_allocate_nothing();
  std::puts("PASS disturbance selection evidence: branches, actual recent4/control-point, core joins, event filter, 35 fields and explicit overflow");
}
