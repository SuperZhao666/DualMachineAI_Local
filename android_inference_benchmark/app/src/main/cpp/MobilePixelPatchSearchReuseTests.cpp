#include "MobilePixelPatch.hpp"
#include <bit>
#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>

using namespace vfdual_android::pixel_patch;
namespace {
// Frozen pre-optimization function, preserving candidate order and arithmetic.
Point reference_search(const View& from,const View& to,int x,int y,int patch_radius,
             int dx0,int dy0,int radius,bool require_unique) {
  if(!inside(from,x,y,patch_radius)||radius<1||radius>16)return {};
  const auto a=patch(from,x,y,patch_radius);
  std::array<float,33*33> scores{};const int side=radius*2+1;
  float best=-2.F;int bx{},by{};
  for(int iy=0;iy<side;++iy)for(int ix=0;ix<side;++ix){
    const int dx=dx0+ix-radius,dy=dy0+iy-radius;
    const float score=ncc(a,to,x+dx,y+dy,patch_radius);
    scores[iy*side+ix]=score;
    if(score>best){best=score;bx=ix;by=iy;}
  }
  if(best<.45F)return {};
  if(require_unique){
    float alternative=-2.F;
    for(int iy=0;iy<side;++iy)for(int ix=0;ix<side;++ix)
      if(std::abs(ix-bx)>1||std::abs(iy-by)>1)alternative=std::max(alternative,scores[iy*side+ix]);
    if(alternative>best-.06F)return {};
  }
  const int dx=dx0+bx-radius,dy=dy0+by-radius;
  const float sx=peak_offset(ncc(a,to,x+dx-1,y+dy,patch_radius),best,ncc(a,to,x+dx+1,y+dy,patch_radius));
  const float sy=peak_offset(ncc(a,to,x+dx,y+dy-1,patch_radius),best,ncc(a,to,x+dx,y+dy+1,patch_radius));
  const float subpixel=ncc_subpixel(a,to,x+dx+sx,y+dy+sy,patch_radius);
  const float quality=std::max(best,subpixel);
  return {x+dx+sx,y+dy+sy,quality,quality>=.78F};
}
std::array<Point,4> reference_coarse_seeds(const View& from,const View& to,int x,int y,
                               int dx0,int dy0,int radius) {
  std::array<Point,4> result{};
  if(!inside(from,x,y,2))return result;
  const auto a=patch(from,x,y,2);std::array<float,33*33> scores{};const int side=radius*2+1;
  for(int iy=0;iy<side;++iy)for(int ix=0;ix<side;++ix)
    scores[iy*side+ix]=ncc(a,to,x+dx0+ix-radius,y+dy0+iy-radius,2);
  for(auto& peak:result){
    float best=.45F;int bx=-1,by=-1;
    for(int iy=0;iy<side;++iy)for(int ix=0;ix<side;++ix)
      if(scores[iy*side+ix]>best){best=scores[iy*side+ix];bx=ix;by=iy;}
    if(bx<0)break;
    peak={float(x+dx0+bx-radius),float(y+dy0+by-radius),best,true};
    for(int iy=std::max(0,by-1);iy<=std::min(side-1,by+1);++iy)
      for(int ix=std::max(0,bx-1);ix<=std::min(side-1,bx+1);++ix)scores[iy*side+ix]=-2.F;
  }
  return result;
}
std::uint32_t seed=19;
unsigned random_number(){seed=seed*1664525U+1013904223U;return seed;}
bool equal(const Point& a,const Point& b) {
  return std::bit_cast<std::uint32_t>(a.x)==std::bit_cast<std::uint32_t>(b.x) &&
      std::bit_cast<std::uint32_t>(a.y)==std::bit_cast<std::uint32_t>(b.y) &&
      std::bit_cast<std::uint32_t>(a.score)==std::bit_cast<std::uint32_t>(b.score) && a.valid==b.valid;
}
std::vector<std::uint8_t> noise(int width,int height) {
  std::vector<std::uint8_t> image(std::size_t(width)*height);
  for(auto& value:image)value=std::uint8_t(random_number()>>24U);
  return image;
}
std::vector<std::uint8_t> translate(const std::vector<std::uint8_t>& source,int w,int h,int dx,int dy,bool inverted=false) {
  std::vector<std::uint8_t> result(source.size());
  for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
    const int xx=std::clamp(x-dx,0,w-1),yy=std::clamp(y-dy,0,h-1);
    const auto value=source[yy*w+xx];result[y*w+x]=inverted?std::uint8_t(255-value):value;
  }
  return result;
}
int failures{};unsigned cases{};
void compare(const View& a,const View& b,int x,int y,int patch_radius,int dx,int dy,int radius,bool unique) {
  const auto expected=reference_search(a,b,x,y,patch_radius,dx,dy,radius,unique);
  const auto actual=search(a,b,x,y,patch_radius,dx,dy,radius,unique);
  ++cases;
  if(!equal(actual,expected)) {
    if(failures<5)std::printf("FAIL exact radius=%d patch=%d xy=%d,%d expected=%a,%a,%a,%d actual=%a,%a,%a,%d\n",
        radius,patch_radius,x,y,expected.x,expected.y,expected.score,expected.valid,actual.x,actual.y,actual.score,actual.valid);
    ++failures;
  }
}
void random_and_boundary_contract() {
  for(unsigned iteration=0;iteration<240;++iteration) {
    const int w=32+int(random_number()%49),h=32+int(random_number()%49);
    const auto first=noise(w,h);
    const int radius=std::array{1,2,3,4,8,16}[iteration%6];
    for(auto [dx,dy]:std::array<std::pair<int,int>,7>{{{0,0},{radius,0},{-radius,0},{0,radius},{0,-radius},
        {radius,radius},{-radius,-radius}}}) {
      const auto second=translate(first,w,h,dx,dy);
      for(int patch_radius:{2,3})for(bool unique:{false,true})
        compare({first,w,h},{second,w,h},w/2,h/2,patch_radius,0,0,radius,unique);
    }
    const auto unrelated=noise(w,h),negative=translate(first,w,h,0,0,true);
    for(const auto* second:{&unrelated,&negative})for(int coordinate:{0,2,3,w/2,w-4,w-1,-1})
      compare({first,w,h},{*second,w,h},coordinate,std::clamp(coordinate,0,h-1),3,0,0,radius,iteration%2!=0);
  }
  const int w=48,h=48;
  std::vector<std::uint8_t> flat(w*h,128),periodic(w*h);
  for(int y=0;y<h;++y)for(int x=0;x<w;++x)periodic[y*w+x]=((x+y)%4)*60;
  for(const auto* image:{&flat,&periodic})for(int radius:{0,1,3,16,17})for(bool unique:{false,true})
    compare({*image,w,h},{*image,w,h},24,24,3,0,0,radius,unique);
  std::printf("exact_search_contract cases=%u mismatches=%d precision=all_Point_float_bits_and_valid\n",cases,failures);
}
void coarse_contract() {
  unsigned coarse_cases{},mismatches{};
  for(unsigned i=0;i<180;++i) {
    const int w=32+int(random_number()%49),h=32+int(random_number()%49),radius=std::array{1,2,3,4,8,16}[i%6];
    const auto a=noise(w,h),translated=translate(a,w,h,i%2?radius:0,i%3?0:-radius),
        negative=translate(a,w,h,0,0,true),other=noise(w,h);
    for(const auto* b:{&a,&translated,&negative,&other})for(int x:{-1,0,2,w/2,w-3,w-1}) {
      const auto expected=reference_coarse_seeds({a,w,h},{*b,w,h},x,h/2,0,0,radius);
      const auto actual=coarse_seeds({a,w,h},{*b,w,h},x,h/2,0,0,radius);
      for(std::size_t j=0;j<actual.size();++j)if(!equal(actual[j],expected[j]))++mismatches;
      ++coarse_cases;
    }
  }
  const int w=40,h=40;std::vector<std::uint8_t> flat(w*h,128),ties(w*h);
  for(int y=0;y<h;++y)for(int x=0;x<w;++x)ties[y*w+x]=std::uint8_t(((x+y)%4)*60);
  for(const auto* a:{&flat,&ties})for(int radius:{1,2,3,4,8,16}) {
    const auto expected=reference_coarse_seeds({*a,w,h},{*a,w,h},20,20,0,0,radius);
    const auto actual=coarse_seeds({*a,w,h},{*a,w,h},20,20,0,0,radius);
    for(std::size_t j=0;j<actual.size();++j)if(!equal(actual[j],expected[j]))++mismatches;
    ++coarse_cases;
  }
  failures+=int(mismatches);
  std::printf("exact_coarse_contract cases=%u mismatched_Points=%u precision=all_four_Points_float_bits_and_valid\n",coarse_cases,mismatches);
}
void call_count_contract() {
#ifdef VF_COUNT_NCC
  const int w=64,h=64;const auto a=noise(w,h);
  for(auto [dx,dy]:std::array<std::pair<int,int>,3>{{{0,0},{3,0},{3,3}}}) {
    const auto b=translate(a,w,h,dx,dy);
    g_ncc_calls=0;const auto expected=reference_search({a,w,h},{b,w,h},32,32,3,0,0,3,false);
    const auto reference_calls=g_ncc_calls;
    g_ncc_calls=0;const auto actual=search({a,w,h},{b,w,h},32,32,3,0,0,3,false);
    const auto optimized_calls=g_ncc_calls;
    const unsigned expected_calls=dx==0?49:dy==0?50:51;
    const bool pass=equal(expected,actual) && reference_calls==53 && optimized_calls==expected_calls;
    std::printf("%s ncc_calls translation=%d,%d reference=%llu candidate=%llu expected_candidate=%u\n",
        pass?"PASS":"FAIL",dx,dy,static_cast<unsigned long long>(reference_calls),
        static_cast<unsigned long long>(optimized_calls),expected_calls);
    if(!pass)++failures;
  }
#endif
}
void kernel_benchmark() {
#ifndef VF_COUNT_NCC
  const int w=64,h=64;const auto a=noise(w,h),b=translate(a,w,h,0,0);
  std::array<double,7> before{},after{};volatile float sink{};
  const auto measure=[&](bool reference) {
    const auto began=std::chrono::steady_clock::now();float accumulated{};
    for(unsigned i=0;i<5000;++i) {
      const int x=25+int(i%10),y=25+int(i%9);
      const auto result=reference?reference_search({a,w,h},{b,w,h},x,y,3,0,0,3,false)
                                 :search({a,w,h},{b,w,h},x,y,3,0,0,3,false);
      accumulated+=result.score;
    }
    sink=accumulated;
    return std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-began).count()/5000;
  };
  for(unsigned i=0;i<7;++i) {
    if(i%2){after[i]=measure(false);before[i]=measure(true);}
    else{before[i]=measure(true);after[i]=measure(false);}
  }
  std::sort(before.begin(),before.end());std::sort(after.begin(),after.end());
  std::printf("desktop_kernel_only iterations=5000 repeats=7 reference_median_us=%.6f candidate_median_us=%.6f sink=%.3f\n",
      before[3],after[3],double(sink));
#endif
}
}
int main(){random_and_boundary_contract();coarse_contract();call_count_contract();kernel_benchmark();
  std::printf("failures=%d scope=NCC_search_kernel_not_phone_pipeline_or_physical_latency\n",failures);return failures?1:0;}
