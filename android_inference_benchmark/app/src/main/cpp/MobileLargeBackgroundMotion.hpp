#pragma once
#include "MobileBackgroundMotion.hpp"
#include "MobilePatchSad.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <span>

namespace vfdual_android {
// Recovery for camera steps beyond the ordinary +/-16 half-resolution search.
// Coarse pixels propose a displacement; independent fine patches must confirm
// it before it can shift identities or enter actuator system identification.
namespace large_background {
struct Plane {std::span<const std::uint8_t> image,mask;int w,h;};
struct Fit {float x{},y{};unsigned support{},candidates{};bool valid{};};
inline bool inside(const Plane& p,int x,int y) {return x>=3&&y>=3&&x+3<p.w&&y+3<p.h;}
inline int sad(const Plane& a,const Plane& b,int x,int y,int dx,int dy) {
  return patch_sad7(a.image.data()+(y-3)*a.w+x-3,
                    b.image.data()+(y+dy-3)*b.w+x+dx-3,std::size_t(a.w));
}
inline Fit fit(const Plane& a,const Plane& b,int prior_x,int prior_y,int radius,
               std::chrono::steady_clock::time_point deadline,bool bounded) {
  struct Point {float dx,dy;int x,y;};
  std::array<Point,49> points{};unsigned n{};Fit out;
  for(int gy=0;gy<7;++gy)for(int gx=0;gx<7;++gx) {
    if(bounded&&std::chrono::steady_clock::now()>=deadline)return {};
    const int x=6+(a.w-12)*(2*gx+1)/14,y=6+(a.h-12)*(2*gy+1)/14;
    if(a.mask[y*a.w+x])continue;
    // Low-texture patches cannot establish correspondence after reduction.
    int sum=0,squares=0;
    for(int yy=-3;yy<=3;++yy)for(int xx=-3;xx<=3;++xx) {
      int v=a.image[(y+yy)*a.w+x+xx];sum+=v;squares+=v*v;
    }
    if(squares-sum*sum/49<600)continue;
    ++out.candidates;
    int lx=std::max(prior_x-radius,3-x),hx=std::min(prior_x+radius,a.w-4-x);
    int ly=std::max(prior_y-radius,3-y),hy=std::min(prior_y+radius,a.h-4-y);
    const int full_lx=lx,full_hx=hx,full_ly=ly,full_hy=hy;
    if(n>=10) {
      std::array<float,49> xs{},ys{};
      for(unsigned i=0;i<n;++i){xs[i]=points[i].dx;ys[i]=points[i].dy;}
      std::sort(xs.begin(),xs.begin()+n);std::sort(ys.begin(),ys.begin()+n);
      unsigned support=0;for(unsigned i=0;i<n;++i)
        support+=std::hypot(points[i].dx-xs[n/2],points[i].dy-ys[n/2])<=1.5F;
      if(support>=8&&support*4>=n*3) {
        lx=std::max(lx,int(std::lround(xs[n/2]))-3);hx=std::min(hx,int(std::lround(xs[n/2]))+3);
        ly=std::max(ly,int(std::lround(ys[n/2]))-3);hy=std::min(hy,int(std::lround(ys[n/2]))+3);
      }
    }
    int best=std::numeric_limits<int>::max(),bx=0,by=0;
    for(int dy=ly;dy<=hy;++dy)for(int dx=lx;dx<=hx;++dx) {
      if(b.mask[(y+dy)*b.w+x+dx])continue;
      int score=sad(a,b,x,y,dx,dy);if(score<best){best=score;bx=dx;by=dy;}
    }
    if(best>30*49||std::abs(bx-prior_x)==radius||std::abs(by-prior_y)==radius||
       !inside(b,x+bx-1,y+by-1)||!inside(b,x+bx+1,y+by+1))continue;
    int alternative=std::numeric_limits<int>::max();
    const int step=radius>8?4:1;
    for(int dy=full_ly;dy<=full_hy;dy+=step)for(int dx=full_lx;dx<=full_hx;dx+=step) {
      if(std::abs(dx-bx)<=2&&std::abs(dy-by)<=2)continue;
      if(b.mask[(y+dy)*b.w+x+dx])continue;
      alternative=std::min(alternative,sad(a,b,x,y,dx,dy));
    }
    if(alternative<best+2*49)continue;
    const auto sub=[](int lo,int mid,int hi) {
      const float d=float(lo+hi-2*mid);return d>0?std::clamp(.5F*float(lo-hi)/d,-.5F,.5F):0.F;
    };
    points[n++]={bx+sub(sad(a,b,x,y,bx-1,by),best,sad(a,b,x,y,bx+1,by)),
                 by+sub(sad(a,b,x,y,bx,by-1),best,sad(a,b,x,y,bx,by+1)),x,y};
  }
  if(n<8)return {};
  std::array<float,49> xs{},ys{};for(unsigned i=0;i<n;++i){xs[i]=points[i].dx;ys[i]=points[i].dy;}
  std::sort(xs.begin(),xs.begin()+n);std::sort(ys.begin(),ys.begin()+n);
  int minx=a.w,maxx=0,miny=a.h,maxy=0;
  for(unsigned i=0;i<n;++i)if(std::hypot(points[i].dx-xs[n/2],points[i].dy-ys[n/2])<=1.5F) {
    out.x+=points[i].dx;out.y+=points[i].dy;++out.support;
    minx=std::min(minx,points[i].x);maxx=std::max(maxx,points[i].x);
    miny=std::min(miny,points[i].y);maxy=std::max(maxy,points[i].y);
  }
  if(out.support<8||out.support*3<n*2||maxx-minx<=a.w/4||maxy-miny<=a.h/4)return {};
  out.x/=out.support;out.y/=out.support;out.valid=true;return out;
}
inline BackgroundMotion recover(const Plane& a,const Plane& b,unsigned scale,
                                std::uint64_t previous_at,std::uint64_t budget_us) {
  if(a.w!=b.w||a.h!=b.h||a.w>208||a.h>208||a.w<64||a.h<64)return {};
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::microseconds(budget_us);
  // One readable padding byte is required by the exact SIMD SAD kernel.
  std::array<std::uint8_t,104*104+1> ac{},bc{},am{},bm{};
  const int w=a.w/2,h=a.h/2;
  for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
    unsigned av=0,bv=0,ma=0,mb=0;
    for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx) {
      int i=(y*2+dy)*a.w+x*2+dx;av+=a.image[i];bv+=b.image[i];ma|=a.mask[i];mb|=b.mask[i];
    }
    ac[y*w+x]=std::uint8_t((av+2)/4);bc[y*w+x]=std::uint8_t((bv+2)/4);
    am[y*w+x]=std::uint8_t(ma);bm[y*w+x]=std::uint8_t(mb);
  }
  const auto coarse=fit({ac,am,w,h},{bc,bm,w,h},0,0,32,deadline,budget_us!=0);
  if(!coarse.valid||std::max(std::abs(coarse.x),std::abs(coarse.y))*2*scale<24.F)return {};
  const auto fine=fit(a,b,int(std::lround(coarse.x*2)),int(std::lround(coarse.y*2)),4,deadline,budget_us!=0);
  if(!fine.valid||std::hypot(fine.x-coarse.x*2,fine.y-coarse.y*2)>3.F||
      std::max(std::abs(fine.x),std::abs(fine.y))*scale>kMobileBackgroundMaximumShiftPixels)return {};
  return {previous_at,fine.x*scale,fine.y*scale,fine.support,fine.candidates,true,true};
}
} // namespace large_background
} // namespace vfdual_android
