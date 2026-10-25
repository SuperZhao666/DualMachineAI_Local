#include "MobileBackgroundMotion.hpp"
#include "MobileTargetImageMotion.hpp"
#include "MobileControlCore.hpp"
#include "ControlSpaceTransform.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
using namespace vfdual_android;
namespace {
constexpr unsigned W=640;
int failures{};unsigned checks{};
void check(bool value,const char* name,double actual,double expected) {
  ++checks;if(!value)++failures;
  std::printf("%s %s actual=%.9f expected=%.9f\n",value?"PASS":"FAIL",name,actual,expected);
}
std::vector<std::uint8_t> texture(unsigned seed) {
  std::vector<std::uint8_t> noise(W*W),out(W*W);
  for(auto& v:noise){seed=1664525U*seed+1013904223U;v=std::uint8_t(seed>>24U);}
  for(unsigned y=2;y<W-2;++y)for(unsigned x=2;x<W-2;++x) {
    unsigned sum{};for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx)sum+=noise[(y+dy)*W+x+dx];
    out[y*W+x]=std::uint8_t(std::clamp(128+(int(sum/25)-128)*4,0,255));
  }
  return out;
}
std::vector<std::uint8_t> image(const std::vector<std::uint8_t>& pattern,unsigned shift) {
  std::vector<std::uint8_t> result(W*W*6);
  for(unsigned y=0;y<W;++y)for(unsigned x=0;x<W;++x) {
    const auto value=pattern[y*W+(x>=shift?x-shift:0)];
    std::fill_n(result.data()+(std::size_t(y)*W+x)*6,6,value);
  }
  return result;
}
MobileControlConfig config() {
  auto c=screen_pixel_control_config();c.selected_class_id=c.body_class_id=0;
  c.selected_class_uses_geometric_head=true;c.selected_class_uses_head_box=c.head_body_pairing_enabled=false;
  c.reject_border_touching=false;c.body_fallback_min_confidence=.4F;c.body_fallback_max_center_distance_pixels=c.model_width;
  apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);c.observation_servo_enabled=true;
  c.lost_target_hold_duration_us=350'000;c.tracker.lost_track_duration_us=500'000;return c;
}
struct Observation {
  BackgroundMotion background{};TargetImageMotion motion{};bool budget_exhausted{},core_background{},held{};
};
struct Pipeline {
  MobileBackgroundMotion background;MobileTargetImageMotion pixels;
  std::unique_ptr<MobileControlCore> core=std::make_unique<MobileControlCore>(config());
  std::uint64_t sequence{};
  Observation tick(const std::vector<std::uint8_t>& rgb,unsigned shift,std::uint64_t at,
                   std::uint64_t image_budget=0,std::uint64_t background_budget=0,bool selective=false,bool empty=false) {
    const std::array boxes{YoloDetection{200.F+shift,200,300.F+shift,400,.9F,0}};
    const auto detections=empty?std::span<const YoloDetection>{}:std::span<const YoloDetection>(boxes);
    const auto bg=background.observe(rgb,W,W,detections,at,1,background_budget);
    const auto motion=pixels.observe(rgb,W,W,detections,bg,at,1,.18F,image_budget,selective);
    const auto first=motion.empty()?TargetImageMotion{}:motion.front();
    ControlSpaceFrame frame;frame.transform(detections,motion,bg);
    const auto output=core->process({frame.boxes.data(),frame.box_count},{++sequence,at,at+20'000,frame.background},
        {frame.motions.data(),frame.motion_count});
    const auto& trace=core->control_trace();const auto& last=trace.samples[(trace.count-1)%trace.samples.size()];
    return {bg,first,pixels.work_budget_exhausted(),bool(last.flags&8),output.held};
  }
  void neural_failure(){background.reset();pixels.reset();}
};
void invalid_background_advances_both_pixel_references() {
  const auto a=texture(13),b=texture(57);Pipeline p;
  (void)p.tick(image(a,0),0,100'000);
  const auto invalid=p.tick(image(b,0),0,108'000);
  const auto next=p.tick(image(b,4),4,116'000);
  check(!invalid.background.valid,"unrelated_frame_background_is_invalid",invalid.background.valid,0);
  check(next.background.valid && next.background.previous_observed_at_us==108'000 && std::abs(next.background.x-4)<1,
      "background_after_failure_refers_to_actual_immediate_pixels",double(next.background.previous_observed_at_us),108'000);
  check(next.motion.valid && next.motion.previous_observed_at_us==108'000 && std::abs(next.motion.x-4)<1,
      "image_motion_after_failure_refers_to_actual_immediate_pixels",double(next.motion.previous_observed_at_us),108'000);
  check(next.core_background,"core_accepts_only_matching_current_background_reference",next.core_background,1);
}
void budget_interruption_keeps_new_pixels_as_next_reference() {
  const auto a=texture(13);Pipeline p;
  (void)p.tick(image(a,0),0,100'000,0,0,true);
  // A large camera step forces BG recovery and selective image checking.
  // Tiny test budgets force interruption without changing production budgets.
  const auto spent=p.tick(image(a,96),96,108'000,1,1,true);
  const auto next=p.tick(image(a,100),100,116'000,0,0,true);
  check(!spent.background.valid && spent.budget_exhausted && !spent.motion.valid,
      "forced_budget_interruption_is_observed_without_image_evidence",spent.budget_exhausted,1);
  check(next.background.valid && next.background.previous_observed_at_us==108'000 && std::abs(next.background.x-4)<1,
      "background_after_spent_budget_is_not_cumulative_from_old_frame",next.background.x,4);
  check(next.motion.valid && next.motion.previous_observed_at_us==108'000 && std::abs(next.motion.x-4)<1,
      "selective_image_motion_after_spent_budget_uses_new_reference",next.motion.x,4);
  check(next.core_background,"core_background_after_budget_matches_raw_history_time",next.core_background,1);
}
void neural_failure_primes_before_reusing_history() {
  const auto a=texture(13);Pipeline p;(void)p.tick(image(a,0),0,100'000);
  p.neural_failure(); // NativeH264Decoder's actual failed-inference policy.
  const auto primed=p.tick(image(a,12),12,116'000);
  const auto next=p.tick(image(a,16),16,124'000);
  check(!primed.background.valid && !primed.motion.valid && !primed.core_background,
      "first_neural_success_after_reset_does_not_claim_a_stale_reference",primed.core_background,0);
  check(next.background.valid && next.background.previous_observed_at_us==116'000 && std::abs(next.background.x-4)<1,
      "neural_failure_background_ref_is_new_prime_not_old_pre_failure",double(next.background.previous_observed_at_us),116'000);
  check(next.motion.valid && next.motion.previous_observed_at_us==116'000 && std::abs(next.motion.x-4)<1,
      "neural_failure_image_ref_is_new_prime_not_old_pre_failure",double(next.motion.previous_observed_at_us),116'000);
  check(next.core_background,"core_matches_new_prime_timestamp_after_neural_failure",next.core_background,1);
}
void skipped_input_preserves_the_full_observation_delta() {
  const auto a=texture(13);Pipeline p;(void)p.tick(image(a,0),0,100'000);
  const auto next=p.tick(image(a,12),12,116'000); // No observe call for a skipped/superseded input.
  check(next.background.valid && next.background.previous_observed_at_us==100'000 && std::abs(next.background.x-12)<1,
      "skipped_input_bg_uses_full_delta_from_last_real_observation",next.background.x,12);
  check(next.motion.valid && next.motion.previous_observed_at_us==100'000 && std::abs(next.motion.x-12)<1,
      "skipped_input_image_uses_full_delta_from_last_real_observation",next.motion.x,12);
  check(next.core_background,"core_time_reference_agrees_when_input_was_not_observed",next.core_background,1);
}
void empty_detection_and_long_gap_reprime() {
  const auto a=texture(13);Pipeline p;(void)p.tick(image(a,0),0,100'000);
  (void)p.tick(image(a,4),4,108'000,0,0,false,true);
  const auto primed=p.tick(image(a,8),8,116'000);
  const auto next=p.tick(image(a,12),12,124'000);
  check(!primed.motion.valid,"empty_detection_retires_image_identity_history",primed.motion.valid,0);
  check(next.motion.valid && next.motion.previous_observed_at_us==116'000,
      "returned_detection_image_history_is_newly_primed",double(next.motion.previous_observed_at_us),116'000);
  const auto gap=p.tick(image(a,16),16,200'000);
  const auto after=p.tick(image(a,20),20,208'000);
  check(!gap.background.valid && !gap.motion.valid && !gap.core_background,
      "long_source_gap_does_not_stitch_old_images",gap.core_background,0);
  check(after.background.valid && after.motion.valid && after.background.previous_observed_at_us==200'000 &&
      after.motion.previous_observed_at_us==200'000 && after.core_background,
      "images_and_core_agree_after_long_gap_prime",double(after.motion.previous_observed_at_us),200'000);
}
}
int main(){invalid_background_advances_both_pixel_references();budget_interruption_keeps_new_pixels_as_next_reference();
  neural_failure_primes_before_reusing_history();skipped_input_preserves_the_full_observation_delta();empty_detection_and_long_gap_reprime();
  std::printf("history_contract checks=%u failures=%d scope=offline_reference_pairing_not_device_latency\n",checks,failures);return failures?1:0;}
