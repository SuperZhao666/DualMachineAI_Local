#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

namespace vfdual {
inline std::uint8_t color_byte(int x) noexcept {
    return static_cast<std::uint8_t>(std::clamp(x,0,255));
}

// Exact BT.709 limited-range integer contract. Even dimensions are required;
// row padding and alpha never enter the conversion.
inline void bgra_to_nv12_scalar(const std::uint8_t* source,std::size_t pitch,
    unsigned width,unsigned height,std::uint8_t* out) noexcept {
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
        const auto* p=source+y*pitch+x*4;
        out[std::size_t(y)*width+x]=color_byte(((47*p[2]+157*p[1]+16*p[0]+128)>>8)+16);
    }
    auto* uv=out+std::size_t(width)*height;
    for(unsigned y=0;y<height;y+=2)for(unsigned x=0;x<width;x+=2){
        int b=0,g=0,r=0;
        for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx){
            const auto* p=source+(y+dy)*pitch+(x+dx)*4;b+=p[0];g+=p[1];r+=p[2];
        }
        b/=4;g/=4;r/=4;
        auto* p=uv+std::size_t(y/2)*width+x;
        p[0]=color_byte(((-26*r-87*g+112*b+128)>>8)+128);
        p[1]=color_byte(((112*r-102*g-10*b+128)>>8)+128);
    }
}

inline __m128i color_weighted4(__m128i lo,__m128i hi,__m128i coefficients) noexcept {
    lo=_mm_madd_epi16(lo,coefficients);hi=_mm_madd_epi16(hi,coefficients);
    lo=_mm_add_epi32(lo,_mm_shuffle_epi32(lo,_MM_SHUFFLE(2,3,0,1)));
    hi=_mm_add_epi32(hi,_mm_shuffle_epi32(hi,_MM_SHUFFLE(2,3,0,1)));
    lo=_mm_shuffle_epi32(lo,_MM_SHUFFLE(2,0,2,0));
    hi=_mm_shuffle_epi32(hi,_MM_SHUFFLE(2,0,2,0));
    return _mm_unpacklo_epi64(lo,hi);
}
inline __m128i color_average_pairs(const std::uint8_t* a,const std::uint8_t* b) noexcept {
    const auto zero=_mm_setzero_si128();
    const auto x=_mm_loadu_si128(reinterpret_cast<const __m128i*>(a));
    const auto y=_mm_loadu_si128(reinterpret_cast<const __m128i*>(b));
    auto lo=_mm_add_epi16(_mm_unpacklo_epi8(x,zero),_mm_unpacklo_epi8(y,zero));
    auto hi=_mm_add_epi16(_mm_unpackhi_epi8(x,zero),_mm_unpackhi_epi8(y,zero));
    lo=_mm_srli_epi16(_mm_add_epi16(lo,_mm_srli_si128(lo,8)),2);
    hi=_mm_srli_epi16(_mm_add_epi16(hi,_mm_srli_si128(hi,8)),2);
    return _mm_unpacklo_epi64(lo,hi);
}

inline void bgra_to_nv12_sse2(const std::uint8_t* source,std::size_t pitch,
    unsigned width,unsigned height,std::uint8_t* out) noexcept {
    const auto zero=_mm_setzero_si128(),round=_mm_set1_epi32(128);
    const auto yc=_mm_setr_epi16(16,157,47,0,16,157,47,0);
    const auto uc=_mm_setr_epi16(112,-87,-26,0,112,-87,-26,0);
    const auto vc=_mm_setr_epi16(-10,-102,112,0,-10,-102,112,0);
    for(unsigned y=0;y<height;++y){
        const auto* row=source+y*pitch;auto* dst=out+std::size_t(y)*width;unsigned x=0;
        for(;x+4<=width;x+=4){
            const auto p=_mm_loadu_si128(reinterpret_cast<const __m128i*>(row+x*4));
            auto sum=color_weighted4(_mm_unpacklo_epi8(p,zero),_mm_unpackhi_epi8(p,zero),yc);
            sum=_mm_add_epi32(_mm_srli_epi32(_mm_add_epi32(sum,round),8),_mm_set1_epi32(16));
            const auto bytes=_mm_packus_epi16(_mm_packs_epi32(sum,zero),zero);
            const auto word=static_cast<std::uint32_t>(_mm_cvtsi128_si32(bytes));
            std::memcpy(dst+x,&word,4);
        }
        for(;x<width;++x){const auto* p=row+x*4;dst[x]=color_byte(((47*p[2]+157*p[1]+16*p[0]+128)>>8)+16);}
    }
    auto* uv=out+std::size_t(width)*height;
    for(unsigned y=0;y<height;y+=2){
        const auto* a=source+y*pitch;const auto* b=a+pitch;auto* dst=uv+std::size_t(y/2)*width;unsigned x=0;
        for(;x+8<=width;x+=8){
            const auto lo=color_average_pairs(a+x*4,b+x*4);
            const auto hi=color_average_pairs(a+x*4+16,b+x*4+16);
            auto u=color_weighted4(lo,hi,uc),v=color_weighted4(lo,hi,vc);
            u=_mm_add_epi32(_mm_srai_epi32(_mm_add_epi32(u,round),8),round);
            v=_mm_add_epi32(_mm_srai_epi32(_mm_add_epi32(v,round),8),round);
            const auto pairs=_mm_packs_epi32(_mm_unpacklo_epi32(u,v),_mm_unpackhi_epi32(u,v));
            _mm_storel_epi64(reinterpret_cast<__m128i*>(dst+x),_mm_packus_epi16(pairs,zero));
        }
        for(;x<width;x+=2){
            const auto* p=a+x*4;const auto* q=b+x*4;
            const int blue=(p[0]+p[4]+q[0]+q[4])/4;
            const int green=(p[1]+p[5]+q[1]+q[5])/4;
            const int red=(p[2]+p[6]+q[2]+q[6])/4;
            dst[x]=color_byte(((-26*red-87*green+112*blue+128)>>8)+128);
            dst[x+1]=color_byte(((112*red-102*green-10*blue+128)>>8)+128);
        }
    }
}
}
