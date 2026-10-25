#include "MobileFreshPixelProjection.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>
using namespace vfdual_android;
namespace { bool count_allocations=false; unsigned allocation_count=0; }
void* operator new(std::size_t n){if(count_allocations)++allocation_count;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
namespace {
constexpr int W=640;
#if defined(__ANDROID__)
constexpr const char* runtime_scope="android_arm64_standalone_CLI_no_USB_HID_NPU";
#else
constexpr const char* runtime_scope="windows_desktop_no_phone_performance_claim";
#endif
std::vector<std::uint8_t> pattern(unsigned seed){
  std::vector<std::uint8_t> noise(W*W),out(W*W);
  for(auto& v:noise){seed=1664525U*seed+1013904223U;v=seed>>24;}
  for(int y=2;y<W-2;++y)for(int x=2;x<W-2;++x){int sum{};
    for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx)sum+=noise[(y+dy)*W+x+dx];
    out[y*W+x]=std::uint8_t(std::clamp(128+(sum/25-128)*4,0,255));}
  return out;
}
float sample(const std::vector<std::uint8_t>& p,float x,float y){
  x=std::clamp(x,0.F,float(W-2));y=std::clamp(y,0.F,float(W-2));
  const int ix=int(x),iy=int(y);const auto fx=x-ix,fy=y-iy;
  return p[iy*W+ix]*(1-fx)*(1-fy)+p[iy*W+ix+1]*fx*(1-fy)+p[(iy+1)*W+ix]*(1-fx)*fy+p[(iy+1)*W+ix+1]*fx*fy;
}
struct Actor{YoloDetection box;const std::vector<std::uint8_t>* texture;float sx=1,sy=1;};
std::vector<std::uint8_t> scene(const std::vector<std::uint8_t>& bg,std::span<const Actor> actors,float light=0){
  std::vector<std::uint8_t> out(W*W*6);
  for(int y=0;y<W;++y)for(int x=0;x<W;++x){float value=bg[y*W+x];
    for(const auto& a:actors)if(x>=a.box.x1&&x<a.box.x2&&y>=a.box.y1&&y<a.box.y2)
      value=sample(*a.texture,(x-a.box.x1)*a.sx+80,(y-a.box.y1)*a.sy+80)+light;
    const auto v=std::uint8_t(std::clamp(std::lround(value),0L,255L));
    for(int k=0;k<3;++k){out[(y*W+x)*6+2*k]=v;out[(y*W+x)*6+2*k+1]=0;}
  }return out;
}
YoloDetection shifted(YoloDetection b,float dx,float dy){b.x1+=dx;b.x2+=dx;b.y1+=dy;b.y2+=dy;return b;}

void tests(){
  const auto bg=pattern(13),one=pattern(57),two=pattern(213);const std::vector<std::uint8_t> flat(W*W,120);
  const YoloDetection original{220,180,340,420,.9F,0};const std::array boxes{original};
  const std::array actor{Actor{original,&one}};const auto first=scene(bg,actor);auto same_pixels=first;
  MobileFreshPixelProjection tracker;
  assert(!tracker.configure(416,416));
  auto r=tracker.process(first,same_pixels,boxes,{1,10,100000},{1,12,118000},0);
  assert(!r.complete&&r.projected_boxes.empty()&&r.rejection==FreshPixelRejection::not_configured);
  assert(tracker.configure(640,640));
  for(const auto [dx,dy]:std::array{std::pair{0.F,0.F},std::pair{8.F,-4.F},std::pair{-24.F,20.F},std::pair{1.4F,.7F},std::pair{40.F,-20.F}}){
    const std::array next{Actor{shifted(original,dx,dy),&one}};const auto pixels=scene(bg,next,10);
    // configure owns all buffers. Only the actual process call is counted.
    count_allocations=true;
    r=tracker.process(first,pixels,boxes,{1,10,100000},{1,12,118000},0);
    count_allocations=false;assert(allocation_count==0);
    assert(r.complete&&r.projected_boxes.size()==1&&r.targets.size()==1&&r.valid_count==1);
    assert(r.source_identity.observed_at_us==100000&&r.target_identity.observed_at_us==118000);
    const auto& e=r.targets[0];assert(e.valid&&e.pixel_observed_at_us==118000);
    assert(std::abs(e.dx_model_px-dx)<1.6&&std::abs(e.dy_model_px-dy)<1.6);
    const auto& b=r.projected_boxes[0];assert(b.class_id==original.class_id&&b.confidence==original.confidence);
    assert(b.x1==original.x1+e.dx_model_px&&b.x2==original.x2+e.dx_model_px);
    assert(std::abs((b.y1+(b.y2-b.y1)*.18F)-(original.y1+(original.y2-original.y1)*.18F)-e.dy_model_px)<.0001F);
  }
  const auto absent=scene(bg,{});r=tracker.process(first,absent,boxes,{1,10,100000},{1,12,118000},0);
  assert(!r.complete&&r.projected_boxes.empty()&&r.targets[0].pixel_observed_at_us==0);
  const std::array flat_actor{Actor{original,&flat}};const auto flat_a=scene(bg,flat_actor);auto flat_b=flat_a;
  r=tracker.process(flat_a,flat_b,boxes,{1,10,100000},{1,12,118000},0);assert(!r.complete&&r.projected_boxes.empty());
  const std::array duplicates{Actor{shifted(original,-90,0),&one},Actor{shifted(original,90,0),&one}};
  const auto confusing=scene(bg,duplicates);r=tracker.process(first,confusing,boxes,{1,10,100000},{1,12,118000},0);assert(!r.complete);
  const YoloDetection a{240,180,320,400,.9F,0},b{340,180,420,400,.9F,0};const std::array pair{a,b};
  const std::array old{Actor{a,&one},Actor{b,&two}};
  const std::array partial{Actor{shifted(a,-12,8),&one}};
  const auto old_scene=scene(bg,old),partial_scene=scene(bg,partial);
  r=tracker.process(old_scene,partial_scene,pair,{1,10,100000},{1,12,118000},0);
  assert(!r.complete&&r.projected_boxes.empty()&&r.valid_count==1&&r.targets[0].valid&&!r.targets[1].valid&&r.targets[1].pixel_observed_at_us==0);
  const std::array crossed{Actor{shifted(a,55,0),&one},Actor{shifted(b,-55,0),&two}};const auto crossed_scene=scene(bg,crossed);
  r=tracker.process(old_scene,crossed_scene,pair,{1,10,100000},{1,12,118000},0);assert(!r.complete&&r.projected_boxes.empty());
  const auto occluded=shifted(original,16,-8);const YoloDetection covering{occluded.x1,occluded.y1,occluded.x2,occluded.y1+100,.9F,0};
  const std::array masked{Actor{occluded,&one},Actor{covering,&flat}};const auto masked_scene=scene(bg,masked);
  r=tracker.process(first,masked_scene,boxes,{1,10,100000},{1,12,118000},0);assert(!r.complete);
  const std::array pose_changed{Actor{shifted(original,16,4),&two}};const auto pose_scene=scene(bg,pose_changed);
  r=tracker.process(first,pose_scene,boxes,{1,10,100000},{1,12,118000},0);assert(!r.complete);
  for(unsigned n=0;n<5;++n){FreshPixelIdentity target{1,12,118000};
    if(n==0)target.generation=2;if(n==1)target.sequence=10;if(n==2)target.observed_at_us=99000;
    if(n==3)target.observed_at_us=151000;if(n==4)target.generation=0;
    r=tracker.process(first,same_pixels,boxes,{1,10,100000},target,0);
    assert(!r.complete&&r.projected_boxes.empty()&&r.valid_count==0&&r.targets[0].pixel_observed_at_us==0);
  }
  r=tracker.process(first,first,boxes,{1,10,100000},{1,12,118000},0);assert(r.rejection==FreshPixelRejection::invalid_pixels);
  // Live small bodies below16 model pixels do not support the patch grid.
  // They must remain rejected without creating apparently fresh evidence.
  const std::array tiny_boxes{YoloDetection{310,300,321,314,.9F,0}};
  r=tracker.process(first,same_pixels,tiny_boxes,{1,10,100000},{1,12,118000},0);
  assert(!r.complete&&r.valid_count==0&&r.projected_boxes.empty());
  assert(r.rejection==FreshPixelRejection::invalid_source_box&&r.targets.size()==1);
  assert(!r.targets[0].valid&&r.targets[0].pixel_observed_at_us==0&&r.targets[0].candidates==0);
  r=tracker.process(first,same_pixels,{}, {1,10,100000},{1,12,118000},0);assert(!r.complete&&r.rejection==FreshPixelRejection::unsupported_box_count);
  std::array<YoloDetection,17> too_many;too_many.fill(original);
  r=tracker.process(first,same_pixels,too_many,{1,10,100000},{1,12,118000},0);assert(!r.complete&&r.rejection==FreshPixelRejection::unsupported_box_count);
  r=tracker.process(first,same_pixels,boxes,{1,10,100000},{1,12,118000},1);
  assert(!r.complete&&r.budget_exhausted&&r.valid_count==0&&r.projected_boxes.empty());
  for(const auto& e:r.targets)assert(!e.valid&&e.pixel_observed_at_us==0);
  // Two visually indistinguishable people exchanging places can leave exactly
  // the same pixels. A zero translation is not a proof of physical identity.
  const YoloDetection ia{100,180,220,420,.9F,0},ib{390,180,510,420,.9F,0};const std::array identical_boxes{ia,ib};
  const std::array identical_actors{Actor{ia,&one},Actor{ib,&one}};const auto identical_a=scene(bg,identical_actors);auto identical_b=identical_a;
  r=tracker.process(identical_a,identical_b,identical_boxes,{1,10,100000},{1,12,118000},0);
  assert(r.complete);for(const auto& e:r.targets)assert(std::hypot(e.dx_model_px,e.dy_model_px)<.1F);
  std::puts("known_limit_same_appearance_swap_not_identity_proof=1");
  tracker.reset();assert(tracker.configure(640,640));
  std::puts("PASS production fresh pixel projection: real RGB path, complete/partial, time, no hot allocation, budget and semantic limits");
}
}
int main(){std::setvbuf(stdout,nullptr,_IONBF,0);tests();return 0;}
