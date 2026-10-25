#include "MobileControlCore.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <vector>

using namespace vfdual_android;
namespace {
int failures{};
void check(bool ok,const char* message) {
  if(!ok) {++failures;std::fprintf(stderr,"FAIL %s\n",message);}
}
MobileControlConfig configuration(bool feedforward=true) {
  MobileControlConfig c;
  c.body_class_id=c.selected_class_id=0;c.head_class_id=vfdual::kNoModelClass;
  c.selected_class_uses_geometric_head=true;c.reject_border_touching=false;
  c.body_fallback_min_confidence=0;c.body_fallback_max_center_distance_pixels=640;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
  c.observation_servo_enabled=true;c.model_based_motion_feedforward=feedforward;
  c.lost_target_hold_duration_us=350'000U;c.tracker.lost_track_duration_us=500'000U;
  c.motion.response_gain=.60F;c.motion.deadzone_pixels=.5F;
  return c;
}
struct Command {std::uint64_t at{};int x{},y{};};
struct Plant {
  std::vector<Command> commands;
  float rx{1},ry{1};std::uint64_t delay{24'000U};
  std::array<float,2> camera(std::uint64_t at)const {
    std::array<float,2> result{};
    for(const auto& c:commands)if(at>=c.at+delay) {result[0]+=c.x*rx;result[1]+=c.y*ry;}
    return result;
  }
};
YoloDetection person(float x,float y) {return{x-20,y-18,x+20,y+82,.95F,0};}
void low_fps_core_velocity() {
  for(auto cadence:{25'000U,29'000U,30'000U,30'001U,33'333U,40'000U,50'000U})
    for(int direction:{-1,1})for(bool vertical:{false,true})for(bool model:{false,true}) {
      MobileControlCore core(configuration(model));Plant plant;
      check(core.restore_response_calibration(1,1,24'000,.99F,.99F),"C1 calibration");
      std::uint64_t previous{};double velocity_sum{},ff_sum{};unsigned valid{};
      for(unsigned i=0;i<160;++i) {
        const auto at=1'000'000ULL+i*cadence;const auto camera=plant.camera(at);
        const float travel=direction*(24.F+150.F*float(at-1'000'000U)/1e6F);
        const std::array boxes{person(320+(vertical?0:travel)-camera[0],320+(vertical?travel:0)-camera[1])};
        ControlFrameContext frame{i+1U,at,at+16'000U};
        if(previous) {const auto before=plant.camera(previous);frame.background={previous,before[0]-camera[0],before[1]-camera[1],24,32,true};}
        const auto out=core.process(boxes,frame);
        const auto& trace=core.control_trace();const auto& row=trace.samples[(trace.count-1)%trace.samples.size()];
        if(i>=120) {
          const auto velocity=vertical?row.velocity_y:row.velocity_x;
          const auto ff=vertical?row.servo.y.feedforward:row.servo.x.feedforward;
          velocity_sum+=direction*velocity;ff_sum+=direction*ff;
          valid+=direction*velocity>20 && direction*ff>.01F;
          check(core.target_velocity_history_span_us()<=150'000U &&
              (!model || core.disturbance_velocity_history_span_us()<=150'000U),
              "C1 supported sparse history retains the original physical-time upper bound");
          check(core.target_velocity_history_points()>=3U &&
              (!model || core.disturbance_velocity_history_points()>=4U),
              "C1 admitted velocity retains each fit's minimum independent sample count");
        }
        if(out.has_move()) {const auto accepted=frame.now_us+1'000U;
          check(core.record_accepted_move(out.delta_x,out.delta_y,accepted),"C1 pending acceptance");
          plant.commands.push_back({accepted,out.delta_x,out.delta_y});}
        previous=at;
      }
      std::printf("C1 cadence=%u direction=%d axis=%c model=%d mean_velocity=%.6f mean_ff=%.6f valid=%u/40\n",
          cadence,direction,vertical?'y':'x',model,velocity_sum/40,ff_sum/40,valid);
      check(velocity_sum/40>40 && ff_sum/40>.1 && valid>=20,"C1 actual Core must provide measured continuous velocity and feedforward");
    }
}
void jittered_core_velocity() {
  constexpr std::array<std::uint64_t,6> cadence{29'000U,30'000U,30'001U,33'333U,40'000U,50'000U};
  for(int direction:{-1,1})for(bool vertical:{false,true})for(bool model:{false,true}) {
    MobileControlCore core(configuration(model));Plant plant;
    check(core.restore_response_calibration(1,1,24'000,.99F,.99F),"C1 jitter calibration");
    std::uint64_t at=1'000'000U,previous{};double velocity_sum{},ff_sum{};unsigned valid{};
    for(unsigned i=0;i<160;++i) {
      if(i)at+=cadence[(i-1)%cadence.size()];const auto camera=plant.camera(at);
      const float travel=direction*(24.F+150.F*float(at-1'000'000U)/1e6F);
      const std::array boxes{person(320+(vertical?0:travel)-camera[0],320+(vertical?travel:0)-camera[1])};
      ControlFrameContext frame{i+1U,at,at+16'000U};
      if(previous){const auto before=plant.camera(previous);frame.background={previous,before[0]-camera[0],before[1]-camera[1],24,32,true};}
      const auto out=core.process(boxes,frame);const auto& trace=core.control_trace();const auto& row=trace.samples[(trace.count-1U)%trace.samples.size()];
      if(i>=120){const auto v=vertical?row.velocity_y:row.velocity_x;const auto ff=vertical?row.servo.y.feedforward:row.servo.x.feedforward;
        velocity_sum+=direction*v;ff_sum+=direction*ff;valid+=direction*v>20 && direction*ff>.01F;}
      if(out.has_move()){const auto accepted=frame.now_us+1'000U;check(core.record_accepted_move(out.delta_x,out.delta_y,accepted),"C1 jitter pending acceptance");plant.commands.push_back({accepted,out.delta_x,out.delta_y});}
      previous=at;
    }
    std::printf("C1 jitter direction=%d axis=%c model=%d mean_velocity=%.6f mean_ff=%.6f valid=%u/40\n",direction,vertical?'y':'x',model,velocity_sum/40,ff_sum/40,valid);
    check(velocity_sum/40>40 && ff_sum/40>.1 && valid>=20,"C1 real jittered cadence preserves supported forward velocity");
  }
}
void retinal_velocity_without_background() {
  for(auto cadence:{33'333U,50'000U})for(int direction:{-1,1})for(bool vertical:{false,true}) {
    MobileControlCore core(configuration());Plant plant;
    check(core.restore_response_calibration(1,1,24'000,.99F,.99F),"C1 independent retinal model prior");
    double velocity_sum{},ff_sum{};unsigned valid{};
    for(unsigned i=0;i<160;++i) {
      const auto at=1'000'000ULL+i*cadence;const auto camera=plant.camera(at);
      const float travel=direction*(24.F+150.F*float(at-1'000'000U)/1e6F);
      const std::array boxes{person(320+(vertical?0:travel)-camera[0],320+(vertical?travel:0)-camera[1])};
      const auto out=core.process(boxes,{i+1U,at,at+16'000U});
      const auto& trace=core.control_trace();const auto& row=trace.samples[(trace.count-1U)%trace.samples.size()];
      if(i>=120) {const auto v=vertical?row.velocity_y:row.velocity_x;const auto ff=vertical?row.servo.y.feedforward:row.servo.x.feedforward;
        velocity_sum+=direction*v;ff_sum+=direction*ff;valid+=direction*v>20 && direction*ff>.01F;
        check(core.target_velocity_history_state()==VelocityHistoryState::invalid_background,
            "C1 unavailable world motion is not used as retinal evidence");}
      if(out.has_move()){const auto accepted=at+17'000U;check(core.record_accepted_move(out.delta_x,out.delta_y,accepted),"C1 no-background pending acceptance");plant.commands.push_back({accepted,out.delta_x,out.delta_y});}
    }
    std::printf("C1 no_background cadence=%u direction=%d axis=%c mean_velocity=%.6f mean_ff=%.6f valid=%u/40\n",cadence,direction,vertical?'y':'x',velocity_sum/40,ff_sum/40,valid);
    check(velocity_sum/40>40 && ff_sum/40>.1 && valid>=20,
        "C1 explicit Core-source continuity supports clear retinal/HID motion independently of background availability");
  }
}
void cross_axis_quantization() {
  for(float response:{1.4F,2.F,4.F})for(float phase:{-.6F,.6F})
    for(bool vertical:{false,true})for(bool moving_later:{false,true}) {
      MobileControlCore core(configuration());Plant plant;
      plant.rx=vertical?.7F:response;plant.ry=vertical?response:.7F;
      check(core.restore_response_calibration(plant.rx,plant.ry,plant.delay,.99F,.99F),"C2 calibration");
      constexpr std::uint64_t cadence=8'333U;
      unsigned positive{},negative{},moving_reports{},moving_moves{};std::uint64_t previous{};
      double maximum_error{};
      for(unsigned i=0;i<480;++i) {
        const auto at=1'000'000ULL+i*cadence;const auto camera=plant.camera(at);
        const auto start=moving_later?160U:0U;
        const float travel=i>=start?120.F*float((i-start)*cadence)/1e6F:0.F;
        const float x=320+(vertical?travel:phase)-camera[0];
        const float y=320+(vertical?phase:travel)-camera[1];
        const std::array boxes{person(x,y)};ControlFrameContext frame{i+1U,at,at+16'000U};
        if(previous) {const auto before=plant.camera(previous);frame.background={previous,before[0]-camera[0],before[1]-camera[1],24,32,true};}
        const auto out=core.process(boxes,frame);
        if(i>=240) {
          const int fixed=vertical?out.delta_y:out.delta_x,moving=vertical?out.delta_x:out.delta_y;
          positive+=fixed>0;negative+=fixed<0;moving_moves+=moving!=0;
          const auto& trace=core.control_trace();const auto& row=trace.samples[(trace.count-1)%trace.samples.size()];
          moving_reports+=row.pending_reports>0;
          maximum_error=std::max(maximum_error,std::abs(double(vertical?y:x)-320));
        }
        if(out.has_move()) {const auto accepted=frame.now_us+1'000U;
          check(core.record_accepted_move(out.delta_x,out.delta_y,accepted),"C2 pending acceptance");
          plant.commands.push_back({accepted,out.delta_x,out.delta_y});}
        previous=at;
      }
      std::printf("C2 response=%.1f phase=%.1f static=%c later=%d positive=%u negative=%u max=%.6f moving_moves=%u pending=%u/240\n",
          response,phase,vertical?'y':'x',moving_later,positive,negative,maximum_error,moving_moves,moving_reports);
      check(positive==0 && negative==0,"C2 unrelated moving-axis reports must not cause stationary-axis oscillation");
      check(maximum_error<=response*.5+.001,"C2 stationary axis remains at nearest reachable lattice");
      check(moving_moves>100 && moving_reports>=200,"C2 physical moving-axis traffic remains active");
    }
}
void continuity_and_uncertainty_guards() {
  for(const auto gap:{70'000U,100'000U}) {
    MobileTargetVelocity target;MobileDisturbanceVelocity disturbance;MobileResponseEstimator response;
    check(response.restore(1,1,24'000,.99F,.99F),"C1 guard calibration");
    std::uint64_t at=1'000'000U;float x{};
    for(unsigned i=0;i<10;++i) {
      const auto before=at;at+=33'333U;x+=5;
      (void)target.update(5,0,before,at,1200,before);
      (void)disturbance.update(x,0,at,response,1200,0,0,0,0,false,before);
    }
    check(target.state()==VelocityHistoryState::moving && disturbance.state()==VelocityHistoryState::moving,
        "C1 both histories contain measured motion before a real gap");
    const auto before=at;at+=gap;x+=1;
    const auto t=target.update(1,0,before,at,1200,before);
    const auto d=disturbance.update(x,0,at,response,1200,0,0,0,0,false,before);
    check(t.x==0 && !d.x_valid && target.state()==VelocityHistoryState::unsupported_interval &&
        disturbance.state()==VelocityHistoryState::unsupported_interval,"C1 real 70/100 ms gaps reject borrowed motion history");
    const auto next=at+33'333U;
    check(target.update(5,0,at,next,1200,at).x==0,"C1 target history must warm up after a gap");
    check(!disturbance.update(x+5,0,next,response,1200,0,0,0,0,false,at).x_valid,
        "C1 disturbance history must warm up after a gap");
    check(target.update(5,0,next,next,1200,next).x==0 &&
        target.state()==VelocityHistoryState::invalid_input,"C1 repeated sample time is invalid");
    check(!disturbance.update(x,0,next-1,response,1200).x_valid &&
        disturbance.state()==VelocityHistoryState::invalid_input,"C1 backward sample time is invalid");
  }
  for(const auto gap:{70'000U,100'000U})for(bool model:{false,true}) {
    MobileControlCore core(configuration(model));check(core.restore_response_calibration(1,1,24'000,.99F,.99F),"C1 real gap calibration");
    std::uint64_t at=1'000'000U;unsigned sequence=1;float x=350;
    for(unsigned i=0;i<14;++i) {
      const auto before=at;if(i)at+=33'333U;x+=5;
      const std::array boxes{person(x,320)};ControlFrameContext frame{sequence++,at,at+16'000U};
      if(i)frame.background={before,0,0,24,32,true};(void)core.process(boxes,frame);
    }
    const auto before=at;at+=gap;x+=5;const std::array boxes{person(x,320)};
    (void)core.process(boxes,{sequence++,at,at+16'000U,{before,0,0,24,32,true}});
    const auto& trace=core.control_trace();auto row=trace.samples[(trace.count-1U)%trace.samples.size()];
    check(row.velocity_x==0 && row.servo.x.feedforward==0,
        "C1 actual Core cannot convert a 70/100 ms source gap into velocity or feedforward debt");
    for(unsigned i=0;i<5;++i) {
      const auto previous=at;at+=33'333U;x+=5;const std::array next{person(x,320)};
      (void)core.process(next,{sequence++,at,at+16'000U,{previous,0,0,24,32,true}});
      row=trace.samples[(trace.count-1U)%trace.samples.size()];
      if(i==0)check(row.servo.x.feedforward==0,"C1 actual Core warms up after interruption instead of reusing its prior slope");
    }
    check(row.velocity_x>20 && row.servo.x.feedforward>0,"C1 actual Core reacquires motion after enough new continuous evidence");
  }
  for(unsigned mode=0;mode<7;++mode) {
    MobileControlCore core(configuration());
    if(mode!=2)check(core.restore_response_calibration(1,1,24'000,.99F,.99F),"C1 Core negative calibration");
    double maximum_ff{};MobileControlOutput latest;
    for(unsigned i=0;i<25;++i) {
      const auto at=1'000'000ULL+i*33'333U;
      const bool pan=mode==1 || mode==2;
      float x=pan?320.F:320.F+float(i)*5.F;
      if(mode==3)x=320+(i%2?2.F:-2.F);
      if(mode==4 && i==12)x-=12;
      auto box=person(x,320);if(mode==5 && i==12)box.x2+=60;
      const std::array boxes{box};
      ControlFrameContext frame{i+1U,at,at+(mode==6 && i==12?50'000U:16'000U)};
      if(i)frame.background={at-33'333U,pan?-5.F:0.F,0,24,32,mode!=0};
      latest=core.process(mode==0 && i%3==0?std::span<const YoloDetection>{}:std::span<const YoloDetection>(boxes),frame);
      const auto& trace=core.control_trace();const auto& row=trace.samples[(trace.count-1U)%trace.samples.size()];
      if(mode<=3 && i>=6)maximum_ff=std::max(maximum_ff,std::abs(double(row.servo.x.feedforward)));
      if(mode==4 && i>=6)check(row.servo.x.feedforward>=0,"C1 one reverse outlier cannot establish sustained reverse feedforward");
      if(mode==5 && i==12)check(row.servo.x.feedforward==0,"C1 abrupt body extent cannot seed motion feedforward");
      if(mode==6 && i==12)check(!latest.has_move() && row.servo.x.feedforward==0,
          "C1 50 ms old current observation cannot generate an output or motion debt");
    }
    if(mode<=3)check(maximum_ff<.001,"C1 interrupted retinal samples manual following untrusted calibration and alternating noise do not invent feedforward");
  }
  // Explicit retained multi-mode uncertainty: enough low-FPS image history
  // may not turn timing ambiguity of a stationary world's own commands into
  // enemy velocity. This passes through the actual Core and pending ledger.
  MobileControlCore uncertain(configuration());MobileResponseEstimator reference;
  check(reference.restore(1,1,80'000,.99F,.99F),"C1 ambiguous prior calibration");
  auto prior=reference.runtime_calibration();prior.range={24'000,100'000,6,false};
  check(uncertain.restore_runtime_response_calibration(prior),"C1 retained ambiguous runtime state");
  Plant plant;plant.delay=96'000;
  for(unsigned i=0;i<18;++i) {
    const auto at=1'000'000ULL+i*33'333U;const auto camera=plant.camera(at);
    const int dx=i%2?8:-8;check(uncertain.record_accepted_move(dx,0,at),"C1 periodic accepted command");
    plant.commands.push_back({at,dx,0});const std::array boxes{person(320-camera[0],320)};
    ControlFrameContext frame{i+1U,at,at+1'000U};
    if(i){const auto before=plant.camera(at-33'333U);frame.background={at-33'333U,before[0]-camera[0],0,24,32,true};}
    (void)uncertain.process(boxes,frame);
    const auto& trace=uncertain.control_trace();const auto& row=trace.samples[(trace.count-1U)%trace.samples.size()];
    check(std::abs(row.servo.x.feedforward)<.001F,"C1 ambiguous own-HID periodic motion is not target feedforward");
  }
}
void per_axis_pending_guards() {
  MobilePendingMotion pending;check(pending.accept(1,0,100'000),"C2 positive pending");
  check(pending.accept(-1,0,100'000),"C2 negative pending");
  check(pending.accept(0,3,100'000),"C2 moving-axis pending");
  auto risk=pending.predict(140'000,24'000,8'000,80'000);
  check(risk.x==0 && risk.minimum_x==0 && risk.maximum_x==0 && risk.reports_x==2 && risk.reports_y==1,
      "C2 mixed opposite reports remain axis risk despite every signed sum being zero");
  const auto narrow=pending.predict(140'000,24'000,24'000,24'000);
  check(narrow.reports_x==0 && risk.reports_x>0,"C2 expanding feasible delay reveals axis risk after nominal visibility");
  MobileContinuousServo servo;MobileMotionPlannerConfig c;c.response_gain=.6F;
  c.response_x_px_per_count=c.response_y_px_per_count=1.4F;
  MobileMotionPlannerInput in{.target_id=1,.error_x=.6F,.stationary_x_evidence=true,
      .stationary_y_evidence=true};
  for(unsigned i=0;i<8;++i) {
    in.observation_sequence=i+1;in.observed_at_us=110'000U+i*8'333U;in.now_us=in.observed_at_us+16'000U;
    risk=pending.predict(in.observed_at_us,24'000,8'000,80'000);
    in.no_pending_x_reports=risk.reports_x==0;in.no_pending_y_reports=risk.reports_y==0;
    (void)servo.plan(in,c,risk.x,risk.y,true,true,24'000,8'333);
    check(!servo.diagnostic().x.resolution_stopped,"C2 uncertain cancelling X reports block X terminal");
  }
  for(unsigned i=8;i<11;++i) {
    in.observation_sequence=i+1;in.observed_at_us=110'000U+i*8'333U;in.now_us=in.observed_at_us+16'000U;
    check(pending.accept(0,3,in.observed_at_us-1U),"C2 continuing Y report");
    risk=pending.predict(in.observed_at_us,24'000,8'000,80'000);
    in.no_pending_x_reports=risk.reports_x==0;in.no_pending_y_reports=risk.reports_y==0;
    (void)servo.plan(in,c,risk.x,risk.y,true,true,24'000,8'333);
  }
  check(servo.diagnostic().x.resolution_stopped,"C2 Y risk alone does not block an established static X terminal");
  in.observation_sequence++;in.observed_at_us+=8'333;in.now_us+=8'333;
  in.target_velocity_x_pixels_per_second=140;in.stationary_x_evidence=false;in.error_x=0;
  const auto resumed=servo.plan(in,c,0,3,true,true,24'000,8'333);
  check(resumed.delta_x>0 && !servo.diagnostic().x.resolution_stopped,"C2 real X motion immediately exits its terminal and preserves feedforward");
}
}
int main(int argc,char** argv) {
  const std::string_view group=argc>1?argv[1]:"all";
  if(group=="all" || group=="C1"){low_fps_core_velocity();jittered_core_velocity();retinal_velocity_without_background();}
  if(group=="all" || group=="C2")cross_axis_quantization();
  if(group=="all" || group=="guards"){continuity_and_uncertainty_guards();per_axis_pending_guards();}
  std::printf("round3_control_failures=%d\n",failures);
  return failures?1:0;
}
