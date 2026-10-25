#include "MobileControlCore.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace vfdual_android;
namespace {
void require(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); std::exit(1); }
}
struct Command { std::uint64_t at; int x, y; };
void production_stationary(float response, float phase, std::uint64_t cadence,
                           std::uint64_t delay, bool vertical) {
  MobileControlConfig c;
  c.body_class_id = c.selected_class_id = 0;
  c.head_class_id = vfdual::kNoModelClass;
  c.selected_class_uses_geometric_head = true;
  c.reject_border_touching = false;
  c.body_fallback_min_confidence = 0;
  c.body_fallback_max_center_distance_pixels = 640;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2, c);
  c.observation_servo_enabled = true;
  c.motion.response_gain = .60F;
  c.motion.deadzone_pixels = .5F;
  MobileControlCore core(c);
  require(core.restore_response_calibration(response,response,delay,.99F,.99F), "restore calibration");
  std::vector<Command> commands;
  const auto camera = [&](std::uint64_t at) {
    double counts{};
    for (const auto& command : commands)
      if (at >= command.at + delay) counts += vertical ? command.y : command.x;
    return counts * response;
  };
  std::uint64_t previous{};
  int positive{}, negative{};
  double maximum_tail_error{};
  for (unsigned i=0;i<480;++i) {
    const auto at=100'000U+i*cadence;
    const float error=phase-float(camera(at));
    const float x=320+(vertical?0:error), y=320+(vertical?error:0);
    const std::array detections{YoloDetection{x-20,y-18,x+20,y+82,.95F,0}};
    ControlFrameContext frame{i+1U,at,at+16'000U};
    if(previous) {
      const float background=float(camera(previous)-camera(at));
      frame.background={previous,vertical?0:background,vertical?background:0,24,32,true};
    }
    const auto output=core.process(detections,frame);
    if (output.has_move()) {
      const auto accepted=frame.now_us+1'000U;
      require(core.record_accepted_move(output.delta_x,output.delta_y,accepted), "accepted ledger");
      commands.push_back({accepted,output.delta_x,output.delta_y});
      if (i>=240) {
        const int delta=vertical?output.delta_y:output.delta_x;
        positive+=delta>0; negative+=delta<0;
      }
    }
    if(i>=240) maximum_tail_error=std::max(maximum_tail_error,std::abs(double(error)));
    previous=at;
  }
  std::printf("quantization response=%.2f phase=%.2f cadence=%llu delay=%llu axis=%c pos=%d neg=%d max=%.6f\n",
      response,phase,cadence,delay,vertical?'y':'x',positive,negative,maximum_tail_error);
  require(positive==0 && negative==0, "stationary nearest lattice must not oscillate indefinitely");
  require(maximum_tail_error<=std::max(.5F,.5F*response)+.001F, "hold nearest reachable lattice");
}
void direct_guards_and_resume() {
  MobileContinuousServo servo;
  MobileMotionPlannerConfig c;
  c.response_gain=.6F; c.response_x_px_per_count=c.response_y_px_per_count=1.4F;
  MobileMotionPlannerInput input{.target_id=1,.observation_sequence=1,.observed_at_us=100'000,
      .now_us=116'000,.error_x=.6F,.stationary_x_evidence=true,.stationary_y_evidence=true,
      .no_pending_reports=true};
  MobileMotionPlannerOutput output;
  for(unsigned i=0;i<12;++i) {
    input.observation_sequence=i+1; input.observed_at_us=100'000+i*16'667U;
    input.now_us=input.observed_at_us+16'000;
    output=servo.plan(input,c,0,0,true,true,24'000,16'667);
  }
  require(servo.diagnostic().x.resolution_stopped && !output.has_move(), "confirmed static enters terminal");
  input.observed_at_us+=16'667; input.now_us+=16'667; ++input.observation_sequence;
  input.target_velocity_x_pixels_per_second=140; input.stationary_x_evidence=false; input.error_x=0;
  output=servo.plan(input,c,0,0,true,true,24'000,16'667);
  require(output.delta_x>0 && !servo.diagnostic().x.resolution_stopped, "moving target keeps zero-error feedforward");
  MobileContinuousServo unknown;
  input.target_velocity_x_pixels_per_second=0; input.error_x=.6F;
  input.stationary_x_evidence=false;
  unsigned ordinary_moves{};
  for(unsigned i=0;i<12;++i) {
    input.observed_at_us+=16'667; input.now_us+=16'667; ++input.observation_sequence;
    ordinary_moves += unknown.plan(input,c,0,0,true,true,24'000,16'667).has_move();
  }
  require(!unknown.diagnostic().x.resolution_stopped, "unknown zero velocity is not static evidence");
  require(ordinary_moves>0, "ordinary fractional-count accumulation remains enabled");
  for(bool pending:{false,true}) {
    MobileContinuousServo guarded;
    input.stationary_x_evidence=true;
    input.no_pending_reports=!pending;
    for(unsigned i=0;i<12;++i) {
      input.observed_at_us+=16'667; input.now_us+=16'667; ++input.observation_sequence;
      (void)guarded.plan(input,c,pending?1.F:0.F,0,pending,true,24'000,16'667);
    }
    require(!guarded.diagnostic().x.resolution_stopped,
        pending?"pending commands cannot enter resolution terminal":"untrusted scale cannot enter resolution terminal");
  }
  input.no_pending_reports=true;
  input.stationary_x_evidence=true;
  input.observed_at_us+=100'000; input.now_us=input.observed_at_us+16'000; ++input.observation_sequence;
  (void)servo.plan(input,c,0,0,true,true,24'000,16'667);
  require(!servo.diagnostic().x.resolution_stopped, "a real observation gap restarts static confirmation");
  input.stationary_x_evidence=false;
  input.error_x=4;
  input.observed_at_us+=16'667; input.now_us+=16'667; ++input.observation_sequence;
  output=servo.plan(input,c,0,0,true,true,24'000,16'667);
  require(output.delta_x>0 && !servo.diagnostic().x.resolution_stopped, "manual camera displacement exits hold immediately");
}
}
int main() {
  for(float response:{.4F,.7F,1.F,1.4F,2.F,4.F})
    for(float phase:{-.6F,.6F,-1.1F,1.1F})
      for(std::uint64_t cadence:{8'333U,16'667U,33'333U})
        for(std::uint64_t delay:{16'000U,24'000U,52'000U})
          for(bool vertical:{false,true})
            production_stationary(response,phase,cadence,delay,vertical);
  direct_guards_and_resume();
}
