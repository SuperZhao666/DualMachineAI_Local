#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace vfdual_android {
inline int patch_sad7_scalar(const std::uint8_t* a, const std::uint8_t* b,
                             std::size_t stride) noexcept {
  int sum{};
  for (unsigned row=0; row<7; ++row) {
    for (unsigned col=0; col<7; ++col) {
      const int d=int(a[col])-int(b[col]);sum+=d<0?-d:d;
    }
    a+=stride;b+=stride;
  }
  return sum;
}

// Exact 7x7 byte SAD. Each row must have EIGHT readable bytes; the eighth
// lane contributes nothing. Image owners provide one extra trailing byte so
// this remains valid even when the last patch reaches the final image pixel.
inline int patch_sad7(const std::uint8_t* a, const std::uint8_t* b,
                      std::size_t stride) noexcept {
#if defined(__aarch64__) && defined(__ARM_NEON)
  uint16x8_t sum=vdupq_n_u16(0);
  for (unsigned row=0; row<7; ++row) {
    sum=vaddw_u8(sum,vabd_u8(vld1_u8(a),vld1_u8(b)));
    a+=stride;b+=stride;
  }
  sum=vsetq_lane_u16(0,sum,7);
  return int(vaddvq_u16(sum));
#elif defined(__SSE2__) || defined(_M_X64)
  const auto keep_seven=_mm_set_epi32(0,0,0x00ffffff,-1);
  auto sum=_mm_setzero_si128();
  for (unsigned row=0; row<7; ++row) {
    std::int64_t av{},bv{};
    std::memcpy(&av,a,8);std::memcpy(&bv,b,8);
    const auto x=_mm_and_si128(_mm_cvtsi64_si128(av),keep_seven);
    const auto y=_mm_and_si128(_mm_cvtsi64_si128(bv),keep_seven);
    sum=_mm_add_epi64(sum,_mm_sad_epu8(x,y));
    a+=stride;b+=stride;
  }
  return int(_mm_cvtsi128_si64(sum));
#else
  return patch_sad7_scalar(a,b,stride);
#endif
}
}  // namespace vfdual_android
