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
void translation_with_detector_shape_noise() {
  const auto bg=texture(13),body=texture(57);const YoloDetection original{140,130,220,290,.9F,0};
  for(const auto move:std::array{std::pair{0.F,0.F},std::pair{7.F,-3.F},std::pair{-24.F,18.F},std::pair{1.4F,.7F},std::pair{40.F,-20.F}}){
    MobileTargetImageMotion tracker;
    const std::array first_actor{Actor{original,&body}};const std::array first_box{original};
    (void)tracker.observe(scene(bg,0,0,first_actor),W,W,first_box,{},100'000,1);
    const auto actual=shifted(original,move.first,move.second);
    const std::array actor{Actor{actual,&body}};auto noisy=actual;noisy.y2+=35;const std::array boxes{noisy};
    const auto result=tracker.observe(scene(bg,-12,6,actor,12),W,W,boxes,{},112'000,1);
    assert(result.size()==1);const auto& m=result[0];
    std::printf("pixel motion expected=%.2f,%.2f actual=%.3f,%.3f valid=%u support=%u candidates=%u ncc=%.3f fb=%.3f\n",
      move.first,move.second,m.x,m.y,m.valid,m.support,m.candidates,m.quality,m.forward_backward_error);
    assert(m.valid&&m.previous_index==0&&m.previous_observed_at_us==100'000);
    assert(std::abs(m.x-move.first)<1.2F&&std::abs(m.y-move.second)<1.2F);
  }
}
void independent_people_and_detector_order() {
  const auto bg=texture(13),one=texture(57),two=texture(213);
  const YoloDetection a{90,120,150,260,.9F,0},b{260,120,320,260,.8F,0};
  const std::array old_actors{Actor{a,&one},Actor{b,&two}};const std::array old_boxes{a,b};
  MobileTargetImageMotion tracker;(void)tracker.observe(scene(bg,0,0,old_actors),W,W,old_boxes,{},100'000,1);
  const auto next_a=shifted(a,22,9),next_b=shifted(b,-22,-7);
  const std::array actors{Actor{next_a,&one},Actor{next_b,&two}};const std::array boxes{next_b,next_a};
  const auto result=tracker.observe(scene(bg,-10,5,actors),W,W,boxes,{},116'667,1);
  assert(result.size()==2&&result[0].valid&&result[1].valid);
  assert(result[0].previous_index==1&&result[1].previous_index==0);
  assert(std::abs(result[0].x+22)<1.2F&&std::abs(result[1].x-22)<1.2F);
}
void disappeared_flat_and_discontinuous_are_invalid() {
  const auto bg=texture(13),body=texture(57);const YoloDetection b{140,130,220,290,.9F,0};
  const std::array actors{Actor{b,&body}};const std::array boxes{b};
  MobileTargetImageMotion tracker;const auto first=scene(bg,0,0,actors);
  (void)tracker.observe(first,W,W,boxes,{},100'000,1);
  const auto gone=tracker.observe(scene(bg,0,0,{}),W,W,boxes,{},112'000,1);
  assert(!gone[0].valid);
  const std::vector<std::uint8_t> flat(W*W*6,120);
  (void)tracker.observe(flat,W,W,boxes,{},124'000,1);
  assert(!tracker.observe(flat,W,W,boxes,{},136'000,1)[0].valid);
  assert(!tracker.observe(first,W,W,boxes,{},300'000,1)[0].valid);
  assert(!tracker.observe(first,W,W,boxes,{},312'000,2)[0].valid);
  assert(!tracker.observe(first,W,W,boxes,{},312'000,2)[0].valid);
  assert(tracker.observe({},W,W,boxes,{},324'000,2).empty());
}
void benchmark() {
  const auto bg=texture(13),body=texture(57);const YoloDetection b{140,130,220,290,.9F,0};
  const std::array actors{Actor{b,&body}};const std::array boxes{b};const auto image=scene(bg,0,0,actors);
  MobileTargetImageMotion tracker;std::vector<double> times;
  for(unsigned i=0;i<40;++i){
    const auto at=100'000+i*8'000ULL;const BackgroundMotion prior{at-8'000,0,0,20,30,i>0};
    const auto start=std::chrono::steady_clock::now();const auto result=tracker.observe(image,W,W,boxes,prior,at,1);
    const auto us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
    if(i>2){assert(result[0].valid);times.push_back(us);}
  }
  std::sort(times.begin(),times.end());std::printf("pixel motion desktop benchmark median_us=%.1f p95_us=%.1f\n",times[times.size()/2],times[times.size()*95/100]);
}
void selective_work_keeps_frames_and_checks_fast_motion(){
  const auto bg=texture(13),body=texture(57);const YoloDetection b{140,130,220,290,.9F,0};
  const std::array actors{Actor{b,&body}};const std::array boxes{b};
  MobileTargetImageMotion tracker;
  const auto first=scene(bg,0,0,actors);
  (void)tracker.observe(first,W,W,boxes,{},100'000,1,.18F,0,true);
  assert(!tracker.observe(first,W,W,boxes,{},112'000,1,.18F,0,true)[0].valid);
  const auto shifted_box=shifted(b,40,-20);const std::array next_boxes{shifted_box};
  const std::array next_actors{Actor{shifted_box,&body}};const auto next=scene(bg,0,0,next_actors);
  const auto moving=tracker.observe(next,W,W,next_boxes,{},124'000,1,.18F,0,true);
  assert(moving[0].valid&&moving[0].previous_observed_at_us==112'000);
  assert(tracker.observe(next,W,W,next_boxes,{},136'000,1,.18F,0,true)[0].valid);
}
}
int main(){
  std::setvbuf(stdout,nullptr,_IONBF,0);
#ifdef _WIN32
  _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
  translation_with_detector_shape_noise();independent_people_and_detector_order();
  disappeared_flat_and_discontinuous_are_invalid();benchmark();selective_work_keeps_frames_and_checks_fast_motion();
  std::puts("PASS target image motion without extra detector");
}
