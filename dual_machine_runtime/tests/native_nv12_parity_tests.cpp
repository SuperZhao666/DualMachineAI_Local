#include "../shared/src/image_preprocessor.cpp"
#include <cstdio>
#include <vector>
#include <random>
#include <chrono>

int main() {
#if !defined(__aarch64__) && !defined(__ARM_NEON)
  std::puts("UNSUPPORTED ARM NEON parity requires ARM hardware");return 77;
#else
  std::mt19937 random(0x160640U);
  std::size_t mismatches=0,cases=0,bytes=0,guard_failures=0;
  const auto started=std::chrono::steady_clock::now();
  const auto run=[&](unsigned side,unsigned pad,bool parity,bool bt709) {
    const auto yp=side+pad,uvp=side+pad;
    std::vector<std::uint8_t> y(std::size_t(yp)*side),uv(std::size_t(uvp)*side/2);
    for(auto& v:y)v=parity?64:std::uint8_t(random());
    for(auto& v:uv)v=parity?128:std::uint8_t(random());
    if(parity)uv[uvp+2]=129; // UL/UR/LL/LR U=128,128,128,129.
    vfdual::Yuv420ImageView view{y.data(),uv.data(),uv.data()+1,side,side,yp,uvp,uvp,2,2};
    const auto count=std::size_t(side)*side*6;
    std::vector<std::uint8_t> scalar(count+128,0xa5),neon(count+128,0xa5);
    const auto matrix=bt709?vfdual::YuvColorMatrix::bt709_limited:vfdual::YuvColorMatrix::bt601_limited;
    vfdual::preprocess_native_square_scalar(view,matrix,side,{scalar.data()+64,count});
    if(!vfdual::preprocess_native_square_neon(view,matrix,side,{neon.data()+64,count})){
      std::puts("FAIL expected supported NV12 NEON path");++guard_failures;return;
    }
    for(std::size_t i=0;i<count;++i)if(scalar[i+64]!=neon[i+64]) {
      if(mismatches<8)std::printf("DIFF side=%u parity=%d bt709=%d byte=%zu scalar=%u neon=%u\n",side,parity,bt709,i,unsigned(scalar[i+64]),unsigned(neon[i+64]));
      ++mismatches;
    }
    for(unsigned i=0;i<64;++i)guard_failures+=scalar[i]!=0xa5||neon[i]!=0xa5||scalar[count+64+i]!=0xa5||neon[count+64+i]!=0xa5;
    bytes+=count;++cases;
  };
  run(16,0,true,false);run(16,0,true,true);
  for(unsigned i=0;i<64;++i)run(16,i%2?16:0,false,i%2);
  for(unsigned i=0;i<2;++i)run(640,i?32:0,false,i);
  const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();
  std::printf("cases=%zu compared_bytes=%zu mismatched_bytes=%zu guard_failures=%zu elapsed_us=%lld scope=CPU_math_no_UI_NPU_USB_HID_socket\n",cases,bytes,mismatches,guard_failures,(long long)us);
  return mismatches||guard_failures?1:0;
#endif
}
