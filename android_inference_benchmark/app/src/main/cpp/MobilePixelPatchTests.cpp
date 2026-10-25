#include "MobilePixelPatch.hpp"
#include <cassert>
#include <bit>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <string_view>
using namespace vfdual_android::pixel_patch;
namespace {
unsigned random_word(unsigned& state){return state=1664525U*state+1013904223U;}
// Independent padded-row scalar reduction verifies every zero/masked lane on
// desktop too. On ARM64 the same cases also execute ncc_neon directly.
float padded_oracle(const Patch& p,const View& image,int x,int y,int r){
  if(!inside(image,x,y,r))return -2.F;
  int sum{},squares{},cross{};
  for(int row=0;row<2*r+1;++row)for(int column=0;column<8;++column){
    const int v=column<2*r+1?image.data[(y-r+row)*image.w+x-r+column]:0;
    sum+=v;squares+=v*v;cross+=p.padded_rows[row*8+column]*v;
  }
  const double va=double(p.squares)-double(p.sum)*p.sum/p.size;
  const double vb=double(squares)-double(sum)*sum/p.size;
  if(va<200. || vb<200.)return -2.F;
  return float((cross-double(p.sum)*sum/p.size)/std::sqrt(va*vb));
}
void exact(const Patch& p,const View& image,int x,int y,int r){
  const auto reference=std::bit_cast<std::uint32_t>(ncc_scalar(p,image,x,y,r));
  assert(std::bit_cast<std::uint32_t>(ncc(p,image,x,y,r))==reference);
  assert(std::bit_cast<std::uint32_t>(padded_oracle(p,image,x,y,r))==reference);
#if defined(__aarch64__) || defined(_M_ARM64)
  assert(std::bit_cast<std::uint32_t>(ncc_neon(p,image,x,y,r))==reference);
#endif
}
void test_exact_reductions(){
  unsigned seed=0x62d123abU,comparisons{};
  for(unsigned trial=0;trial<2497;++trial){
    const int r=2+int(trial&1),w=2*r+1+int(random_word(seed)%81),h=2*r+1+int(random_word(seed)%69);
    std::vector<std::uint8_t> a(w*h),b(w*h);
    for(int i=0;i<w*h;++i){
      switch(trial%7){
        case 0:a[i]=255;b[i]=255;break;
        case 1:a[i]=120+(random_word(seed)%3);b[i]=120+(random_word(seed)%3);break;
        case 2:a[i]=(i&1)?255:0;b[i]=(i&1)?0:255;break;
        case 3:a[i]=std::uint8_t(i%w);b[i]=a[i];break;
        default:a[i]=random_word(seed)>>24;b[i]=random_word(seed)>>24;break;
      }
    }
    const View av{a,w,h},bv{b,w,h};
    const int sx=r+int(random_word(seed)%unsigned(w-2*r)),sy=r+int(random_word(seed)%unsigned(h-2*r));
    const auto p=patch(av,sx,sy,r);
    assert(p.padded_radius==r && p.size==(r*2+1)*(r*2+1));
    for(int row=0;row<r*2+1;++row)for(int col=0;col<8;++col)
      assert(p.padded_rows[row*8+col]==(col<r*2+1?p.data[row*(r*2+1)+col]:0));
    // Exercise both image edges, including the radius2 final three masked
    // lanes, tiny image widths, last physical row and invalid coordinates.
    for(int x : {-1,0,r,w-r-4,w-r-3,w-r-2,w-r-1,w-r,w-1,w})
      for(int y : {-1,0,r,h-r-1,h-r,h-1,h}){exact(p,bv,x,y,r);++comparisons;}
    exact(p,av,sx,sy,r);++comparisons;
    for(unsigned i=0;i<24;++i){
      const int x=int(random_word(seed)%unsigned(w+2))-1,y=int(random_word(seed)%unsigned(h+2))-1;
      exact(p,bv,x,y,r);++comparisons;
    }
  }
  std::printf("PASS NCC integer/float-bit exact2497 textures; comparisons=%u backend=%s\n",comparisons,
#if defined(__aarch64__) || defined(_M_ARM64)
    "ARM64 NEON+scalar"
#else
    "desktop scalar+padded oracle"
#endif
  );
}
void benchmark(){
  std::vector<std::uint8_t> a(160*160),b(160*160);unsigned seed=73;
  for(auto& v:a)v=random_word(seed)>>24;
  for(auto& v:b)v=random_word(seed)>>24;
  const View av{a,160,160},bv{b,160,160};
  for(const int r:{2,3}){
    const auto p=patch(av,80,80,r);
    for(const bool scalar:{true,false}){
      double checksum{};const auto start=std::chrono::steady_clock::now();
      constexpr unsigned rounds=256;
      for(unsigned repeat=0;repeat<rounds;++repeat)
        for(int y=64;y<=96;++y)for(int x=64;x<=96;++x)
          checksum+=scalar?ncc_scalar(p,bv,x,y,r):ncc(p,bv,x,y,r);
      const double elapsed=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
      std::printf("NCC_PROBE radius=%d scalar=%d evaluations=%u total_us=%.3f ns_per_eval=%.3f checksum=%.17g\n",
        r,scalar,rounds*33*33,elapsed,elapsed*1000/(rounds*33*33),checksum);
    }
  }
}
}
int main(int argc,char** argv){
  std::vector<std::uint8_t> image(64*64);
  unsigned seed=57;for(auto& v:image){seed=1664525U*seed+1013904223U;v=seed>>24;}
  const View view{image,64,64};const auto p=patch(view,30,30,3);
  assert(p.size==49&&p.sum==std::accumulate(p.data.begin(),p.data.begin()+p.size,0));
  assert(std::abs(ncc(p,view,30,30,3)-1.F)<1e-6F);
  assert(std::abs(ncc_subpixel(p,view,30.F,30.F,3)-1.F)<1e-6F);
  assert(ncc(p,view,0,0,3)==-2.F&&!inside(view,0,0,3));
  std::vector<std::uint8_t> flat(64*64,120);const View fv{flat,64,64};
  assert(ncc(patch(fv,30,30,3),fv,30,30,3)==-2.F);
  assert(peak_offset(.8F,1.F,.8F)==0.F);
  assert(texture(view,30,30)>800&&texture(fv,30,30)==0);
  const auto identity=search(view,view,30,30,3,0,0,3,true);
  assert(identity.valid&&std::hypot(identity.x-30,identity.y-30)<.2F);
  auto shifted=image;
  for(unsigned y=3;y<61;++y)for(unsigned x=3;x<61;++x)shifted[y*64+x]=image[(y-2)*64+x-1];
  const View next{shifted,64,64};const auto found=search(view,next,30,30,3,0,0,4,true);
  assert(found.valid&&std::abs(found.x-31)<.2F&&std::abs(found.y-32)<.2F);
  test_exact_reductions();
  if(argc>1 && std::string_view(argv[1])=="--benchmark")benchmark();
  std::puts("PASS shared pixel patch numerical kernel");
}
