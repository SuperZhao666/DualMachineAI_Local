#include "MobileTargetImageMotion.hpp"
#include "MobileRgbAreaPrefilter.hpp"
#include "MobilePixelPatch.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace vfdual_android {
namespace {
using pixel_patch::View;
using pixel_patch::Patch;
using pixel_patch::Point;
using pixel_patch::Flow;
using pixel_patch::inside;
using pixel_patch::patch;
using pixel_patch::ncc;
using pixel_patch::ncc_subpixel;
using pixel_patch::peak_offset;
using pixel_patch::search;
using pixel_patch::coarse_seeds;
using pixel_patch::match;
using pixel_patch::texture;
bool valid_box(const YoloDetection& b,float width,float height) {
  return std::isfinite(b.x1)&&std::isfinite(b.y1)&&std::isfinite(b.x2)&&std::isfinite(b.y2)&&
      std::isfinite(b.confidence)&&b.confidence>=.2F&&b.x2-b.x1>=8.F&&b.y2-b.y1>=16.F&&
      b.x2-b.x1<=width*2&&b.y2-b.y1<=height*2;
}
float cx(const YoloDetection& b){return (b.x1+b.x2)*.5F;}
float anchor_y(const YoloDetection& b,float ratio){return b.y1+(b.y2-b.y1)*ratio;}
bool compatible(const YoloDetection& a,const YoloDetection& b) {
  const float w=(a.x2-a.x1)/(b.x2-b.x1),h=(a.y2-a.y1)/(b.y2-b.y1);
  return w>=.4F&&w<=2.5F&&h>=.4F&&h<=2.5F;
}
}

std::span<const TargetImageMotion> MobileTargetImageMotion::observe(
    std::span<const std::uint8_t> rgb,std::uint32_t width,std::uint32_t height,
    std::span<const YoloDetection> boxes,const BackgroundMotion& background,
    std::uint64_t at,std::uint64_t generation,float ratio,std::uint64_t maximum_work_us,bool selective) {
  const auto began=std::chrono::steady_clock::now();work_budget_exhausted_=false;
  const auto over_budget=[&]{
    if(maximum_work_us && std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-began).count()>=maximum_work_us)work_budget_exhausted_=true;
    return work_budget_exhausted_;
  };
  const auto count=std::min(boxes.size(),kMaximumTargets);motions_.fill({});
  if(width<64||height<64||width>1024||height>1024||rgb.size()!=std::size_t(width)*height*6||
      !at||!generation||!std::isfinite(ratio)||ratio<0.F||ratio>1.F){reset();return {};}
  // A frame with no candidate boxes cannot supply an image-identity match.
  // The old path retained zero previous boxes anyway. Reset the image history
  // and prime it again on the first nonempty frame, without reading RGB here.
  if(count==0U){reset();return {};}
  const auto scale=std::max(2U,(std::max(width,height)+207U)/208U);
  const int w=int(width/scale),h=int(height/scale),cw=w/2,ch=h/2;
  const bool continuous=width_==unsigned(w)&&height_==unsigned(h)&&scale_==scale&&
      generation_==generation&&at>observed_at_us_&&observed_at_us_&&at-observed_at_us_<=50'000U;
  if(!continuous)continuation_frames_=0;
  bool identity_risk=false;
  if(continuous && previous_count_ && count){
    const bool camera=background.valid&&background.previous_observed_at_us==observed_at_us_;
    std::array<bool,kMaximumTargets> claimed{};
    identity_risk=count!=previous_count_;
    for(std::size_t i=0;i<std::min(count,std::size_t(4));++i){
      const auto& box=boxes[i];if(!valid_box(box,float(width),float(height)))continue;
      unsigned nearby{};std::size_t previous{};
      for(std::size_t j=0;j<previous_count_;++j){
        const auto& old=previous_boxes_[j];
        if(box.class_id!=old.class_id || !valid_box(old,float(width),float(height)) || !compatible(box,old))continue;
        const float dx=cx(box)-cx(old)-(camera?background.x:0.F);
        const float dy=anchor_y(box,ratio)-anchor_y(old,ratio)-(camera?background.y:0.F);
        if(std::hypot(dx,dy)<=16.F){++nearby;previous=j;}
      }
      identity_risk|=nearby!=1U || (nearby==1U&&claimed[previous]);
      if(nearby==1U)claimed[previous]=true;
    }
  }
  if(identity_risk)continuation_frames_=3U;
  const bool evaluate=!selective || continuation_frames_!=0U;
  if(!identity_risk && continuation_frames_)--continuation_frames_;
  current_.resize(std::size_t(w)*h);current_coarse_.resize(std::size_t(cw)*ch);
  if(scale==2U){
    downsample_rgb_u16_area2(rgb.data(),width,height,current_.data());
  }else if(scale==4U){
    downsample_rgb_u16_area4(rgb.data(),width,height,current_.data());
  }else for(int y=0;y<h;++y)for(int x=0;x<w;++x){
    // Area prefilter before decimation: nearest sampling aliases odd-pixel
    // translations and can destroy a real patch correspondence.
    unsigned sum{};
    for(unsigned dy=0;dy<scale;++dy)for(unsigned dx=0;dx<scale;++dx){
      const auto i=(std::size_t(y*scale+dy)*width+x*scale+dx)*6;
      sum+=77U*rgb[i]+150U*rgb[i+2]+29U*rgb[i+4];
    }
    const unsigned denominator=256U*scale*scale;
    current_[y*w+x]=std::uint8_t((sum+denominator/2)/denominator);
  }
  for(int y=0;y<ch;++y)for(int x=0;x<cw;++x){
    const auto i=2*y*w+2*x;
    current_coarse_[y*cw+x]=std::uint8_t((unsigned(current_[i])+current_[i+1]+current_[i+w]+current_[i+w+1])/4U);
  }
  if(continuous && evaluate){
    const View previous{previous_,w,h},current{current_,w,h},pc{previous_coarse_,cw,ch},cc{current_coarse_,cw,ch};
    const bool known_camera=background.valid&&background.previous_observed_at_us==observed_at_us_;
    const float expected_x=known_camera?-background.x/scale:0.F,expected_y=known_camera?-background.y/scale:0.F;
    const int search_radius=known_camera?std::clamp(int(std::ceil((float(at-observed_at_us_)*.0012F+8.F)/(2*scale))),4,16):16;
    for(std::size_t index=0;index<std::min(count,std::size_t(4));++index){
      if(over_budget())break;
      auto& motion=motions_[index];motion.current_index=std::uint32_t(index);motion.previous_observed_at_us=observed_at_us_;
      const auto& b=boxes[index];if(!valid_box(b,float(width),float(height)))continue;
      std::array<Flow,9> flows{};std::size_t matched{};
      std::array<std::pair<int,int>,9> points{};std::size_t point_count{};
      for(int gy=0;gy<3;++gy)for(int gx=0;gx<3;++gx){
        if(over_budget())continue;
        const float xf=b.x1+(b.x2-b.x1)*(.25F+.25F*gx);
        const float yf=anchor_y(b,ratio)+(b.y2-b.y1)*(.12F*(gy-1));
        const int sx=int(std::lround(xf/scale)),sy=int(std::lround(yf/scale));
        int px=sx,py=sy,best_texture{};
        for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx){
          const int x=sx+dx,y=sy+dy;
          if(std::abs(x-w/2)<=5&&std::abs(y-h/2)<=5)continue; // reticle overlay
          const int t=texture(current,x,y);if(t>best_texture){best_texture=t;px=x;py=y;}
        }
        if(best_texture<800)continue;
        bool duplicate{};for(std::size_t i=0;i<point_count;++i)duplicate|=std::hypot(float(px-points[i].first),float(py-points[i].second))<3.F;
        if(duplicate)continue;points[point_count++]={px,py};++motion.candidates;
        const auto back=match(current,previous,cc,pc,px,py,expected_x,expected_y,search_radius);
        if(!back.valid)continue;
        const auto forward=match(previous,current,pc,cc,int(std::lround(back.x)),int(std::lround(back.y)),-expected_x,-expected_y,search_radius);
        if(!forward.valid)continue;
        const float fb=std::hypot(forward.x-px,forward.y-py);if(fb>1.5F)continue;
        flows[matched++]={float(px)-back.x,float(py)-back.y,back.x,back.y,std::min(back.score,forward.score),fb,px,py};
      }
      if(matched<4)continue;
      std::array<float,9> xs{},ys{};for(std::size_t i=0;i<matched;++i){xs[i]=flows[i].x;ys[i]=flows[i].y;}
      std::sort(xs.begin(),xs.begin()+matched);std::sort(ys.begin(),ys.begin()+matched);
      const float mx=xs[matched/2],my=ys[matched/2];
      float sumx{},sumy{},score{},fb{};int minx=w,maxx{},miny=h,maxy{};
      std::array<unsigned,kMaximumTargets> votes{};
      for(std::size_t i=0;i<matched;++i){
        const auto& f=flows[i];if(std::hypot(f.x-mx,f.y-my)>1.5F)continue;
        ++motion.support;sumx+=f.x;sumy+=f.y;score+=f.score;fb+=f.fb;
        minx=std::min(minx,f.point_x);maxx=std::max(maxx,f.point_x);miny=std::min(miny,f.point_y);maxy=std::max(maxy,f.point_y);
        for(std::size_t j=0;j<previous_count_;++j){
          const auto& old=previous_boxes_[j];const float x=f.previous_x*scale,y=f.previous_y*scale;
          if(old.class_id==b.class_id&&valid_box(old,float(width),float(height))&&compatible(b,old)&&
              x>=old.x1-6&&x<=old.x2+6&&y>=old.y1-8&&y<=old.y1+(old.y2-old.y1)*.55F+8)++votes[j];
        }
      }
      if(motion.support<4||motion.support*2<motion.candidates||motion.support*3<matched*2||maxx-minx<3||maxy-miny<3)continue;
      std::size_t best=previous_count_;unsigned best_votes{},second_votes{};
      for(std::size_t j=0;j<previous_count_;++j){
        if(votes[j]>best_votes){second_votes=best_votes;best_votes=votes[j];best=j;}
        else second_votes=std::max(second_votes,votes[j]);
      }
      if(best==previous_count_||best_votes*4<motion.support*3||best_votes<second_votes+2)continue;
      motion.x=sumx*scale/motion.support;motion.y=sumy*scale/motion.support;
      motion.quality=score/motion.support;motion.forward_backward_error=fb*scale/motion.support;
      motion.previous_index=std::uint32_t(best);motion.valid=true;
    }
    // Multiple current boxes may not claim one previous identity through flow.
    std::array<bool,kMaximumTargets> conflicted{};
    for(std::size_t i=0;i<count;++i)for(std::size_t j=i+1;j<count;++j)
      if(motions_[i].valid&&motions_[j].valid&&motions_[i].previous_index==motions_[j].previous_index)conflicted[i]=conflicted[j]=true;
    for(std::size_t i=0;i<count;++i)if(conflicted[i])motions_[i].valid=false;
  }
  previous_.swap(current_);previous_coarse_.swap(current_coarse_);
  previous_count_=count;std::copy_n(boxes.begin(),count,previous_boxes_.begin());
  width_=w;height_=h;scale_=scale;observed_at_us_=at;generation_=generation;
  return {motions_.data(),count};
}
void MobileTargetImageMotion::reset() noexcept {observed_at_us_=generation_=0;previous_count_=0;motions_.fill({});work_budget_exhausted_=false;continuation_frames_=0;}
}
