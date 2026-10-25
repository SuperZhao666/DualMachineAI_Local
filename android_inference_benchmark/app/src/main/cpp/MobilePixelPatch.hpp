#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

// Shared normalized patch/texture/subpixel math. No detector, identity,
// timestamp, thread or allocation policy belongs to this numerical kernel.
namespace vfdual_android::pixel_patch {
struct View {const std::vector<std::uint8_t>& data;int w,h;};
struct Patch {
  std::array<int,49> data{};int size{},sum{},squares{};
  // The numerical payload remains data[]. Zero-padded rows let ARM64 load
  // both supported patch widths without reading beyond the source patch.
  std::array<std::uint8_t,7*8> padded_rows{};
  int padded_radius{};
};
struct Point {float x{},y{},score{};bool valid{};};
struct Flow {float x{},y{},previous_x{},previous_y{},score{},fb{};int point_x{},point_y{};};
inline bool inside(const View& im,int x,int y,int r) {return x>=r&&y>=r&&x+r<im.w&&y+r<im.h;}
inline Patch patch(const View& im,int x,int y,int r) {
  Patch p;
  for(int dy=-r;dy<=r;++dy)for(int dx=-r;dx<=r;++dx){
    const int v=im.data[(y+dy)*im.w+x+dx];p.data[p.size++]=v;p.sum+=v;p.squares+=v*v;
    if(r==2 || r==3)p.padded_rows[(dy+r)*8+dx+r]=static_cast<std::uint8_t>(v);
  }
  if(r==2 || r==3)p.padded_radius=r;
  return p;
}
inline float ncc_score(const Patch& a,int sum,int squares,int cross) {
  const double va=double(a.squares)-double(a.sum)*a.sum/a.size;
  const double vb=double(squares)-double(sum)*sum/a.size;
  if(va<200. || vb<200.)return -2.F;
  return float((cross-double(a.sum)*sum/a.size)/std::sqrt(va*vb));
}
// Keep an explicit oracle with the original integer iteration and arithmetic.
inline float ncc_scalar(const Patch& a,const View& b,int x,int y,int r) {
  if(!inside(b,x,y,r))return -2.F;
  int sum{},squares{},cross{},i{};
  for(int dy=-r;dy<=r;++dy)for(int dx=-r;dx<=r;++dx){
    const int v=b.data[(y+dy)*b.w+x+dx];sum+=v;squares+=v*v;cross+=a.data[i++]*v;
  }
  return ncc_score(a,sum,squares,cross);
}
#if defined(__aarch64__) || defined(_M_ARM64)
inline float ncc_neon(const Patch& a,const View& b,int x,int y,int r) {
  // A radius2 row has5 values plus3 padding lanes; radius3 has7 plus1.
  // Require all8 loaded bytes to remain in the same image row. In particular,
  // the radius2 bound is stricter than x+r+1, even though masked lanes are0.
  if((r!=2 && r!=3) || a.padded_radius!=r || a.size!=(r*2+1)*(r*2+1) ||
      !inside(b,x,y,r) || x-r+7>=b.w)return ncc_scalar(a,b,x,y,r);
  const std::uint8_t lanes2[8]{255,255,255,255,255,0,0,0};
  const std::uint8_t lanes3[8]{255,255,255,255,255,255,255,0};
  const auto mask=vld1_u8(r==2?lanes2:lanes3);
  auto sums=vdupq_n_u16(0);
  auto squares=vdupq_n_u32(0),cross=vdupq_n_u32(0);
  for(int row=0;row<r*2+1;++row){
    const auto values=vand_u8(vld1_u8(b.data.data()+(y-r+row)*b.w+x-r),mask);
    const auto reference=vld1_u8(a.padded_rows.data()+row*8);
    sums=vaddq_u16(sums,vmovl_u8(values));
    squares=vaddq_u32(squares,vpaddlq_u16(vmull_u8(values,values)));
    cross=vaddq_u32(cross,vpaddlq_u16(vmull_u8(reference,values)));
  }
  // All totals are exact integers: at most49*255^2, well below int32.
  // The scalar and vector paths share the unchanged double/sqrt expression.
  return ncc_score(a,int(vaddvq_u16(sums)),int(vaddvq_u32(squares)),int(vaddvq_u32(cross)));
}
#endif
inline float ncc(const Patch& a,const View& b,int x,int y,int r) {
#if defined(__aarch64__) || defined(_M_ARM64)
  return ncc_neon(a,b,x,y,r);
#else
  return ncc_scalar(a,b,x,y,r);
#endif
}
inline float ncc_subpixel(const Patch& a,const View& b,float x,float y,int r) {
  const int ix=int(std::floor(x)),iy=int(std::floor(y));
  if(!inside(b,ix,iy,r)||ix+r+1>=b.w||iy+r+1>=b.h)return -2.F;
  const float fx=x-ix,fy=y-iy;double sum{},squares{},cross{};int k{};
  for(int dy=-r;dy<=r;++dy)for(int dx=-r;dx<=r;++dx){
    const int i=(iy+dy)*b.w+ix+dx;
    const double v=b.data[i]*(1-fx)*(1-fy)+b.data[i+1]*fx*(1-fy)+
        b.data[i+b.w]*(1-fx)*fy+b.data[i+b.w+1]*fx*fy;
    sum+=v;squares+=v*v;cross+=a.data[k++]*v;
  }
  const double va=double(a.squares)-double(a.sum)*a.sum/a.size,vb=squares-sum*sum/a.size;
  return va>200.&&vb>200.?float((cross-a.sum*sum/a.size)/std::sqrt(va*vb)):-2.F;
}
inline float peak_offset(float negative,float centre,float positive) {
  if(negative<-.5F || positive<-.5F)return 0.F;
  const float denominator=negative+positive-2.F*centre;
  return denominator<-.00001F?std::clamp(.5F*(negative-positive)/denominator,-.5F,.5F):0.F;
}
inline Point search(const View& from,const View& to,int x,int y,int patch_radius,
             int dx0,int dy0,int radius,bool require_unique) {
  if(!inside(from,x,y,patch_radius)||radius<1||radius>16)return {};
  const auto a=patch(from,x,y,patch_radius);
  // The complete active side*side prefix is written before any score read.
  std::array<float,33*33> scores;const int side=radius*2+1;
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
  // The grid already stores ncc's exact float result for interior neighbours.
  // Window-edge neighbours still need the original out-of-grid evaluation.
  const auto neighbour_score=[&](int ox,int oy) {
    const int ix=bx+ox,iy=by+oy;
    return ix>=0 && ix<side && iy>=0 && iy<side ? scores[iy*side+ix]
        : ncc(a,to,x+dx+ox,y+dy+oy,patch_radius);
  };
  const float sx=peak_offset(neighbour_score(-1,0),best,neighbour_score(1,0));
  const float sy=peak_offset(neighbour_score(0,-1),best,neighbour_score(0,1));
  const float subpixel=ncc_subpixel(a,to,x+dx+sx,y+dy+sy,patch_radius);
  const float quality=std::max(best,subpixel);
  return {x+dx+sx,y+dy+sy,quality,quality>=.78F};
}
inline std::array<Point,4> coarse_seeds(const View& from,const View& to,int x,int y,
                               int dx0,int dy0,int radius) {
  std::array<Point,4> result{};
  if(!inside(from,x,y,2))return result;
  // Peak selection/suppression only reads the fully written active prefix.
  const auto a=patch(from,x,y,2);std::array<float,33*33> scores;const int side=radius*2+1;
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
inline Point match(const View& from,const View& to,const View& coarse_from,const View& coarse_to,
            int x,int y,float expected_x,float expected_y,int coarse_radius) {
  // Quarter-scale aliases can rank a genuine half-pixel peak below a nearby
  // coarse peak. Refine a bounded set, then enforce uniqueness at fine scale.
  const auto seeds=coarse_seeds(coarse_from,coarse_to,x/2,y/2,
      int(std::lround(expected_x*.5F)),int(std::lround(expected_y*.5F)),coarse_radius);
  std::array<Point,4> fine{};Point best{};
  for(std::size_t i=0;i<seeds.size();++i)if(seeds[i].valid){
    fine[i]=search(from,to,x,y,3,int(std::lround(seeds[i].x*2))-x,int(std::lround(seeds[i].y*2))-y,3,false);
    if(fine[i].valid&&(!best.valid||fine[i].score>best.score))best=fine[i];
  }
  if(!best.valid)return {};
  for(const auto& other:fine)
    if(other.valid&&std::hypot(other.x-best.x,other.y-best.y)>1.5F&&other.score>best.score-.06F)return {};
  return best;
}
inline int texture(const View& image,int x,int y) {
  if(!inside(image,x,y,3))return 0;
  int xx{},yy{},xy{};
  for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx){
    const auto i=(y+dy)*image.w+x+dx;
    const int gx=int(image.data[i+1])-image.data[i-1],gy=int(image.data[i+image.w])-image.data[i-image.w];
    xx+=gx*gx;yy+=gy*gy;xy+=gx*gy;
  }
  return int((double(xx)*yy-double(xy)*xy)/(xx+yy+1));
}

} // namespace vfdual_android::pixel_patch
