#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

namespace vfdual {
// Bounded center-aligned bilinear downscale. This CPU alternative avoids
// scheduling a VideoProcessorBlt before staging readback on a busy game GPU.
class BgraResizePlan final {
    struct Sample { unsigned first{},second{},weight{}; };
    std::array<Sample,1024> xs_{},ys_{};
    unsigned width_{},height_{};
    static void axis(std::array<Sample,1024>& samples,unsigned source,unsigned target) noexcept {
        for(unsigned i=0;i<target;++i){
            const double p=std::clamp((i+.5)*source/target-.5,0.0,double(source-1));
            const auto first=static_cast<unsigned>(p);
            samples[i]={first,std::min(first+1,source-1),static_cast<unsigned>(std::round((p-first)*256))};
        }
    }
    static __m128i pixel(const std::uint8_t* p) noexcept {
        std::uint32_t word;std::memcpy(&word,p,4);
        return _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(word)),_mm_setzero_si128());
    }
    static __m128i blend(__m128i a,__m128i b,unsigned weight) noexcept {
        // Preserve all horizontal fractional bits; 255*256 fits unsigned16.
        return _mm_add_epi16(_mm_mullo_epi16(a,_mm_set1_epi16(static_cast<short>(256-weight))),
                             _mm_mullo_epi16(b,_mm_set1_epi16(static_cast<short>(weight))));
    }
    static __m128i finish(__m128i a,__m128i b,unsigned weight) noexcept {
        const auto wa=_mm_set1_epi16(static_cast<short>(256-weight)),wb=_mm_set1_epi16(static_cast<short>(weight));
        const auto left=_mm_unpacklo_epi16(_mm_mullo_epi16(a,wa),_mm_mulhi_epu16(a,wa));
        const auto right=_mm_unpacklo_epi16(_mm_mullo_epi16(b,wb),_mm_mulhi_epu16(b,wb));
        const auto sum=_mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(left,right),_mm_set1_epi32(32768)),16);
        return _mm_packus_epi16(_mm_packs_epi32(sum,_mm_setzero_si128()),_mm_setzero_si128());
    }
public:
    bool configure(unsigned sw,unsigned sh,unsigned dw,unsigned dh) noexcept {
        if(!sw || !sh || !dw || !dh || dw>1024 || dh>1024 || dw>sw || dh>sh)return false;
        axis(xs_,sw,dw);axis(ys_,sh,dh);width_=dw;height_=dh;return true;
    }
    void run(const std::uint8_t* source,std::size_t pitch,std::uint8_t* destination) const noexcept {
        for(unsigned y=0;y<height_;++y){
            const auto sy=ys_[y];const auto* a=source+sy.first*pitch;const auto* b=source+sy.second*pitch;
            auto* dst=destination+std::size_t(y)*width_*4;
            for(unsigned x=0;x<width_;++x){
                const auto sx=xs_[x];
                const auto top=blend(pixel(a+sx.first*4),pixel(a+sx.second*4),sx.weight);
                const auto bottom=blend(pixel(b+sx.first*4),pixel(b+sx.second*4),sx.weight);
                const auto value=finish(top,bottom,sy.weight);
                const auto word=static_cast<std::uint32_t>(_mm_cvtsi128_si32(value));std::memcpy(dst+x*4,&word,4);
            }
        }
    }
};
}
