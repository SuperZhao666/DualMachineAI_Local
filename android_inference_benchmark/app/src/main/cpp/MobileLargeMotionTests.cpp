#include "MobileBackgroundMotion.hpp"
#include "MobileControlCore.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>
using namespace vfdual_android;
namespace {
constexpr int W=416;
std::vector<std::uint8_t> image(unsigned seed) {
  std::vector<std::uint8_t> noise(W*W),out(W*W);
  for(auto& v:noise){seed=1664525U*seed+1013904223U;v=seed>>24U;}
  for(int y=2;y<W-2;++y)for(int x=2;x<W-2;++x) {
    int s=0;for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx)s+=noise[(y+dy)*W+x+dx];
    out[y*W+x]=std::uint8_t(std::clamp(128+(s/25-128)*3,0,255));
  }return out;
}
std::vector<std::uint8_t> translated(const std::vector<std::uint8_t>& im,float dx,float dy) {
  std::vector<std::uint8_t> out(W*W*6);
  for(int y=0;y<W;++y)for(int x=0;x<W;++x) {
    float xx=std::clamp(x-dx,0.F,float(W-2)),yy=std::clamp(y-dy,0.F,float(W-2));
    int ix=int(xx),iy=int(yy);float fx=xx-ix,fy=yy-iy;
    auto v=std::uint8_t(im[iy*W+ix]*(1-fx)*(1-fy)+im[iy*W+ix+1]*fx*(1-fy)+
        im[(iy+1)*W+ix]*(1-fx)*fy+im[(iy+1)*W+ix+1]*fx*fy);
    std::fill_n(out.begin()+(y*W+x)*6,6,v);
  }return out;
}
BackgroundMotion observe(MobileBackgroundMotion& b,const std::vector<std::uint8_t>& im,
    std::span<const YoloDetection> boxes,std::uint64_t at,std::uint64_t budget=0) {
#ifdef VF_TEST_BASELINE
  return b.observe(im,W,W,boxes,at,1);
#else
  return b.observe(im,W,W,boxes,at,1,budget);
#endif
}
MobileControlConfig config() {
  MobileControlConfig c;c.selected_class_id=c.body_class_id=0;c.selected_class_uses_geometric_head=true;
  c.head_body_pairing_enabled=c.selected_class_uses_head_box=false;c.reject_border_touching=false;
  c.body_fallback_min_confidence=.4F;c.body_fallback_max_center_distance_pixels=416;
  c.motion.response_gain=.6F;c.motion.maximum_prediction_pixels=kMobileObservationPredictionLimitPixels;
  c.switch_confirmation_duration_us=45'000;apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
  c.observation_servo_enabled=true;c.lost_target_hold_duration_us=350'000;c.tracker.lost_track_duration_us=500'000;return c;
}
void translations(bool report) {
  unsigned passed=0,total=0;double maximum_error=0;
  for(unsigned seed:{7U,51U,777U})for(auto shift: {
      std::pair{34.F,0.F},{55.F,0.F},{64.F,0.F},{96.F,0.F},{112.F,0.F},{120.F,0.F},
      {-34.F,0.F},{-55.F,0.F},{-64.F,0.F},{-96.F,0.F},{-112.F,0.F},{-120.F,0.F},
      {0.F,55.F},{0.F,-55.F},{80.F,60.F},{-80.F,-60.F},{113.F,43.F},{-63.F,15.F}}) {
    auto base=image(seed);MobileBackgroundMotion b;
    (void)observe(b,translated(base,0,0),{},100'000);
    const auto result=observe(b,translated(base,shift.first,shift.second),{},108'000);
    float error=std::hypot(result.x-shift.first,result.y-shift.second);
    bool good=result.valid&&error<1.2F;passed+=good;++total;
    if(good)maximum_error=std::max(maximum_error,double(error));
    std::printf("translation seed=%u dx=%.0f dy=%.0f valid=%d support=%u error=%.3f\n",seed,shift.first,shift.second,result.valid,result.support,error);
    if(!report)assert(good);
  }
  std::printf("large_translation matched=%u total=%u max_error=%.3f\n",passed,total,maximum_error);
}
void camera_step_keeps_owner(bool report) {
  for(bool crowd:{false,true}) {
    MobileControlCore core(config());MobileBackgroundMotion bg;const auto texture=image(19);
    const std::array first{YoloDetection{51,140,85,198,.9F,0},YoloDetection{2,145,30,197,.85F,0}};
    auto n=crowd?2U:1U;
    const auto m1=observe(bg,translated(texture,0,0),std::span(first).first(n),100'000);
    const auto owner=core.process(std::span(first).first(n),{1,100'000,120'000,m1});assert(owner.has_target);
    auto next=first;for(auto& b:next){b.x1+=113;b.x2+=113;b.y1+=43;b.y2+=43;}
    const auto m2=observe(bg,translated(texture,113,43),std::span(next).first(n),108'000);
    const auto after=core.process(std::span(next).first(n),{2,108'000,128'000,m2});
    bool kept=after.has_target&&!after.held&&after.lock_id==owner.lock_id&&after.track_id==owner.track_id;
    std::printf("camera_step crowd=%d measured=%d same_owner=%d reason=%u\n",crowd,m2.valid,kept,unsigned(after.suppression_reason));
    if(!report)assert(kept);
  }
}
void rejects_unrelated_scenes() {
  for(unsigned seed=0;seed<12;++seed) {
    MobileBackgroundMotion b;(void)observe(b,translated(image(seed+1),0,0),{},100'000);
    assert(!observe(b,translated(image(seed+777),0,0),{},108'000).valid);
  }
  MobileBackgroundMotion b;std::vector<std::uint8_t> flat(W*W*6,114);
  (void)observe(b,flat,{},100'000);assert(!observe(b,flat,{},108'000).valid);
  // Repeated patterns have multiple equally good translations. Never turn
  // their distant correlation peak into camera motion.
  auto periodic=image(1);
  for(int y=0;y<W;++y)for(int x=0;x<W;++x)periodic[y*W+x]=((x/8+y/8)%2)?220:30;
  MobileBackgroundMotion repeating;
  (void)observe(repeating,translated(periodic,0,0),{},100'000);
  assert(!observe(repeating,translated(periodic,55,0),{},108'000).valid);
  auto first=translated(image(31),0,0),second=translated(image(31),55,0);
  const std::array cover{YoloDetection{0,0,416,416,.9F,0}};
  (void)observe(b,first,cover,116'000);assert(!observe(b,second,cover,124'000).valid);
  assert(!observe(b,first,{},250'000).valid); // a time gap cannot be stitched
}
void benchmark() {
  auto texture=image(71),a=translated(texture,0,0),b=translated(texture,55,0);
  MobileBackgroundMotion m;std::vector<long long> times;unsigned matched=0;
  for(unsigned i=0;i<80;++i) {
    const auto begin=std::chrono::steady_clock::now();
    const auto result=observe(m,i%2?b:a,{},100'000+i*8'000,2'500);
    auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count();
    if(i){times.push_back(us);matched+=result.valid;}
  }
  std::sort(times.begin(),times.end());std::printf("bounded_recovery valid=%u/79 p50_us=%lld p95_us=%lld\n",matched,times[times.size()/2],times[times.size()*95/100]);
}
}
int main(int argc,char** argv) {
#ifdef _WIN32
  _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
  std::setvbuf(stdout,nullptr,_IONBF,0);bool report=argc>1&&std::string_view(argv[1])=="--report";
  translations(report);camera_step_keeps_owner(report);rejects_unrelated_scenes();benchmark();
  std::puts("PASS large camera motion recovery");
}
