#include "MobileRgbAreaPrefilter.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <chrono>
#include <string_view>
using namespace vfdual_android;
void scalar(const std::uint8_t* rgb,unsigned width,unsigned height,std::uint8_t* out){
  for(unsigned y=0;y<height/2;++y)for(unsigned x=0;x<width/2;++x){
    unsigned sum{};
    for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx){
      const auto i=((2*y+dy)*width+2*x+dx)*6;
      sum+=77U*rgb[i]+150U*rgb[i+2]+29U*rgb[i+4];
    }
    out[y*(width/2)+x]=std::uint8_t((sum+512U)/1024U);
  }
}
void scalar4(const std::uint8_t* rgb,unsigned width,unsigned height,std::uint8_t* out,unsigned scale){
  for(unsigned y=0;y<height/scale;++y)for(unsigned x=0;x<width/scale;++x){
    unsigned sum{};
    for(unsigned dy=0;dy<scale;++dy)for(unsigned dx=0;dx<scale;++dx){
      const auto i=((scale*y+dy)*width+scale*x+dx)*6;
      sum+=77U*rgb[i]+150U*rgb[i+2]+29U*rgb[i+4];
    }
    out[y*(width/scale)+x]=std::uint8_t((sum+128U*scale*scale)/(256U*scale*scale));
  }
}
int main(int argc,char** argv){
  std::uint32_t seed=0xe11eU;const auto random=[&]{seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;};
  unsigned cases{};
  for(unsigned w:{1U,2U,3U,63U,64U,65U,129U,207U,208U,415U,416U,639U,640U,641U})
  for(unsigned h:{1U,2U,3U,63U,64U,65U,129U,207U,208U,415U,416U,639U,640U,641U}){
    std::vector<std::uint8_t> rgb(w*h*6),a((w/2)*(h/2)+1),b(a.size());
    for(unsigned pattern=0;pattern<7;++pattern){
      for(unsigned i=0;i<rgb.size();++i)rgb[i]=pattern==0?0:pattern==1?255:pattern==2?std::uint8_t(i%6<2?255:0):pattern==3?std::uint8_t(i%6==2||i%6==3?255:0):pattern==4?std::uint8_t(i%6>=4?255:0):std::uint8_t(random());
      a.back()=b.back()=91;scalar(rgb.data(),w,h,a.data());downsample_rgb_u16_area2(rgb.data(),w,h,b.data());
      assert(a==b&&b.back()==91);
      // Upper bytes are not part of the original low-byte view.
      for(unsigned i=1;i<rgb.size();i+=2)rgb[i]^=255;
      downsample_rgb_u16_area2(rgb.data(),w,h,b.data());assert(a==b);
      ++cases;
    }
  }
  for(unsigned w:{1U,3U,4U,15U,16U,17U,63U,64U,65U,127U,208U,416U,639U,640U,641U})
  for(unsigned h:{1U,3U,4U,15U,16U,17U,63U,64U,65U,127U,208U,416U,639U,640U,641U}){
    std::vector<std::uint8_t> rgb(w*h*6),a((w/4)*(h/4)+1),b(a.size());
    for(unsigned pattern=0;pattern<5;++pattern){
      for(unsigned i=0;i<rgb.size();++i)rgb[i]=pattern==0?0:pattern==1?255:std::uint8_t(random());
      a.back()=b.back()=91;scalar4(rgb.data(),w,h,a.data(),4);downsample_rgb_u16_area4(rgb.data(),w,h,b.data());
      assert(a==b && b.back()==91);
      for(unsigned i=1;i<rgb.size();i+=2)rgb[i]^=255;
      downsample_rgb_u16_area4(rgb.data(),w,h,b.data());assert(a==b);++cases;
    }
  }
  if(argc>1 && std::string_view(argv[1])=="--benchmark"){
    {
      std::vector<std::uint8_t> rgb(640*640*6),out(320*320);
      for(auto& value:rgb)value=std::uint8_t(random());
      std::uint64_t checksum{};
      for(bool optimized:{false,true}){
        std::vector<double> times;
        for(unsigned n=0;n<230;++n){
          const auto began=std::chrono::steady_clock::now();
          if(optimized)downsample_rgb_u16_area2(rgb.data(),640,640,out.data());
          else scalar(rgb.data(),640,640,out.data());
          const auto us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-began).count();
          checksum+=out[n%out.size()];rgb[n%rgb.size()]^=1;
          if(n>=30)times.push_back(us);
        }
        std::sort(times.begin(),times.end());
        std::printf("area2 optimized=%d p50_us=%.3f p95_us=%.3f samples=%zu checksum=%llu\n",optimized,times[100],times[189],times.size(),static_cast<unsigned long long>(checksum));
      }
    }
    std::vector<std::uint8_t> rgb(640*640*6),out(160*160);for(auto& v:rgb)v=std::uint8_t(random());
    volatile unsigned scale=4;std::uint64_t checksum{};
    for(bool optimized:{false,true}){
      std::vector<double> times;
      for(unsigned n=0;n<230;++n){
        const auto began=std::chrono::steady_clock::now();
        if(optimized)downsample_rgb_u16_area4(rgb.data(),640,640,out.data());else scalar4(rgb.data(),640,640,out.data(),scale);
        const auto us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-began).count();
        checksum+=out[n%out.size()];rgb[n%rgb.size()]^=1;if(n>=30)times.push_back(us);
      }
      std::sort(times.begin(),times.end());std::printf("area4 optimized=%d p50_us=%.3f p95_us=%.3f samples=%zu checksum=%llu\n",optimized,times[100],times[189],times.size(),static_cast<unsigned long long>(checksum));
    }
  }
  std::printf("PASS %u RGB area-prefilter cases: exact rounding, colors, odd sizes, ignored high bytes and output bounds\n",cases);
}
