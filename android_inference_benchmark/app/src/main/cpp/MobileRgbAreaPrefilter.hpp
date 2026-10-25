#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace vfdual_android {
// Exact scale2 form of sum(77R+150G+29B)/(256*scale*scale), rounded once.
// Input is little-endian RGB U16; as in the existing path only low bytes are
// sampled. Caller supplies width*height*6 input bytes and floor(width/2)*
// floor(height/2) output bytes. Odd final rows/columns are intentionally unused.
inline void downsample_rgb_u16_area2(const std::uint8_t* rgb,
    std::size_t width,std::size_t height,std::uint8_t* gray) noexcept {
  const auto out_width=width/2U,out_height=height/2U;
  for(std::size_t y=0;y<out_height;++y){
    const auto* top=rgb+y*width*12U;const auto* bottom=top+width*6U;
    auto* output=gray+y*out_width;
    std::size_t x=0;
#if defined(__aarch64__)
    for(;x+4U<=out_width;x+=4U){
      // Eight source pixels per row form four independent2x2 areas. Only
      // the low bytes participate, exactly as in the scalar contract.
      const auto a=vld3q_u16(reinterpret_cast<const std::uint16_t*>(top+x*12U));
      const auto b=vld3q_u16(reinterpret_cast<const std::uint16_t*>(bottom+x*12U));
      const auto pairs=[](uint16x8_t values){return vpaddl_u8(vmovn_u16(values));};
      const auto red=vadd_u16(pairs(a.val[0]),pairs(b.val[0]));
      const auto green=vadd_u16(pairs(a.val[1]),pairs(b.val[1]));
      const auto blue=vadd_u16(pairs(a.val[2]),pairs(b.val[2]));
      auto sum=vmull_n_u16(red,77);
      sum=vmlal_n_u16(sum,green,150);sum=vmlal_n_u16(sum,blue,29);
      const auto pixels=vmovn_u16(vcombine_u16(
          vshrn_n_u32(vaddq_u32(sum,vdupq_n_u32(512)),10),vdup_n_u16(0)));
      const auto packed=vget_lane_u32(vreinterpret_u32_u8(pixels),0);
      std::memcpy(output+x,&packed,sizeof(packed));
    }
#endif
    for(;x<out_width;++x){
      const auto i=x*12U;
      const unsigned r=unsigned(top[i])+top[i+6U]+bottom[i]+bottom[i+6U];
      const unsigned g=unsigned(top[i+2U])+top[i+8U]+bottom[i+2U]+bottom[i+8U];
      const unsigned b=unsigned(top[i+4U])+top[i+10U]+bottom[i+4U]+bottom[i+10U];
      output[x]=std::uint8_t((77U*r+150U*g+29U*b+512U)>>10U);
    }
  }
}

// Exact 4x4 area filter used by 640 input. Accumulate channels before applying
// the integer luminance weights; there is still only one final rounding.
inline void downsample_rgb_u16_area4(const std::uint8_t* rgb,
    std::size_t width,std::size_t height,std::uint8_t* gray) noexcept {
  const auto out_width=width/4U,out_height=height/4U;
  for(std::size_t y=0;y<out_height;++y){
    auto* output=gray+y*out_width;std::size_t x=0;
#if defined(__aarch64__)
    for(;x+4U<=out_width;x+=4U){
      auto red=vdupq_n_u32(0),green=vdupq_n_u32(0),blue=vdupq_n_u32(0);
      for(unsigned row=0;row<4;++row){
        const auto* p=reinterpret_cast<const std::uint16_t*>(rgb+((y*4U+row)*width+x*4U)*6U);
        const auto a=vld3q_u16(p),b=vld3q_u16(p+24);
        const auto sum4=[](uint16x8_t low,uint16x8_t high){
          return vpaddlq_u16(vpaddlq_u8(vcombine_u8(vmovn_u16(low),vmovn_u16(high))));
        };
        red=vaddq_u32(red,sum4(a.val[0],b.val[0]));
        green=vaddq_u32(green,sum4(a.val[1],b.val[1]));
        blue=vaddq_u32(blue,sum4(a.val[2],b.val[2]));
      }
      auto sum=vmlaq_n_u32(vmulq_n_u32(red,77),green,150);
      sum=vmlaq_n_u32(sum,blue,29);sum=vaddq_u32(sum,vdupq_n_u32(2048));
      const auto pixels=vmovn_u16(vcombine_u16(vshrn_n_u32(sum,12),vdup_n_u16(0)));
      const auto packed=vget_lane_u32(vreinterpret_u32_u8(pixels),0);
      std::memcpy(output+x,&packed,sizeof(packed));
    }
#endif
    for(;x<out_width;++x){
      unsigned red{},green{},blue{};
      for(unsigned row=0;row<4;++row){
        const auto* p=rgb+((y*4U+row)*width+x*4U)*6U;
        red+=unsigned(p[0])+p[6]+p[12]+p[18];
        green+=unsigned(p[2])+p[8]+p[14]+p[20];
        blue+=unsigned(p[4])+p[10]+p[16]+p[22];
      }
      output[x]=std::uint8_t((77U*red+150U*green+29U*blue+2048U)>>12U);
    }
  }
}
}  // namespace vfdual_android
