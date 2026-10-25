#include "MobilePatchSad.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

static void test_last_vector_load_stays_inside_padded_allocation() {
#if defined(_WIN32)
  SYSTEM_INFO info{};GetSystemInfo(&info);
  const auto page=std::size_t(info.dwPageSize);
  for (unsigned stride : {7U,31U,208U,512U}) {
    auto* a=static_cast<std::uint8_t*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    auto* b=static_cast<std::uint8_t*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    assert(a&&b);DWORD old{};
    assert(VirtualProtect(a+page,page,PAGE_NOACCESS,&old));
    assert(VirtualProtect(b+page,page,PAGE_NOACCESS,&old));
    const auto bytes=7U*stride+1U;assert(bytes<page);
    auto* x=a+page-bytes;auto* y=b+page-bytes;
    for(unsigned i=0;i<bytes;++i){x[i]=std::uint8_t(i*13U);y[i]=std::uint8_t(i*31U);}
    x+=stride-7U;y+=stride-7U;
    assert(vfdual_android::patch_sad7(x,y,stride)==vfdual_android::patch_sad7_scalar(x,y,stride));
    assert(VirtualFree(a,0,MEM_RELEASE));assert(VirtualFree(b,0,MEM_RELEASE));
  }
#endif
}
int main() {
  using namespace vfdual_android;
  std::uint32_t state=0x81214U;unsigned tested{};
  const auto random=[&]{state^=state<<13;state^=state>>17;state^=state<<5;return state;};
  for (unsigned stride : {7U,8U,31U,64U,129U,208U,341U,512U}) {
    std::vector<std::uint8_t> a(stride*9+1),b(a.size());
    for (unsigned trial=0;trial<1000;++trial) {
      for(auto& v:a)v=std::uint8_t(random());for(auto& v:b)v=std::uint8_t(random());
      // End-aligned seven rows force the last vector read to use the single
      // readable padding byte. Unequal padding must never affect the score.
      const auto offset=(9U-7U)*stride+stride-7U;
      const auto expected=patch_sad7_scalar(a.data()+offset,b.data()+offset,stride);
      assert(patch_sad7(a.data()+offset,b.data()+offset,stride)==expected);
      a.back()=0;b.back()=255;
      assert(patch_sad7(a.data()+offset,b.data()+offset,stride)==expected);
      ++tested;
    }
    std::fill(a.begin(),a.end(),0);std::fill(b.begin(),b.end(),255);
    assert(patch_sad7(a.data(),b.data(),stride)==49*255);
    assert(patch_sad7(a.data(),a.data(),stride)==0);
  }
  test_last_vector_load_stays_inside_padded_allocation();
  std::printf("PASS %u random strided/end-aligned SAD equivalence cases, padding and extrema\n",tested);
}
