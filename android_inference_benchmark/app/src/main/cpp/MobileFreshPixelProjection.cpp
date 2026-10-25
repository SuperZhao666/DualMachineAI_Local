#include "MobileFreshPixelProjection.hpp"
#include "MobilePixelPatch.hpp"
#include "MobileRgbAreaPrefilter.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <new>

namespace vfdual_android {
namespace {
using pixel_patch::View;
using pixel_patch::Flow;
using pixel_patch::match;
using pixel_patch::texture;
constexpr unsigned kScale=2, kGrayWidth=320, kGrayHeight=320;
bool valid_body(const YoloDetection& b) noexcept {
  return std::isfinite(b.x1)&&std::isfinite(b.y1)&&std::isfinite(b.x2)&&std::isfinite(b.y2)&&
      std::isfinite(b.confidence)&&b.confidence>=.2F&&b.x1>=0&&b.y1>=0&&b.x2<=640&&b.y2<=640&&
      b.x2-b.x1>=8&&b.y2-b.y1>=16;
}
float overlap(const YoloDetection& a,const YoloDetection& b) noexcept {
  const float intersection=std::max(0.F,std::min(a.x2,b.x2)-std::max(a.x1,b.x1))*
      std::max(0.F,std::min(a.y2,b.y2)-std::max(a.y1,b.y1));
  const float total=(a.x2-a.x1)*(a.y2-a.y1)+(b.x2-b.x1)*(b.y2-b.y1)-intersection;
  return total>0?intersection/total:0;
}
}

const char* fresh_pixel_rejection_name(FreshPixelRejection r) noexcept {
  switch(r){
    case FreshPixelRejection::none:return "none";
    case FreshPixelRejection::not_configured:return "not_configured";
    case FreshPixelRejection::invalid_pixels:return "invalid_pixels";
    case FreshPixelRejection::invalid_identity:return "invalid_identity";
    case FreshPixelRejection::generation_mismatch:return "generation_mismatch";
    case FreshPixelRejection::not_newer:return "not_newer";
    case FreshPixelRejection::gap_over50ms:return "projection_gap_over50ms";
    case FreshPixelRejection::unsupported_box_count:return "unsupported_box_count";
    case FreshPixelRejection::invalid_source_box:return "invalid_source_box";
    case FreshPixelRejection::low_texture_or_support:return "low_texture_or_support";
    case FreshPixelRejection::ambiguous_or_low_ncc:return "ambiguous_or_low_ncc";
    case FreshPixelRejection::forward_backward:return "forward_backward";
    case FreshPixelRejection::insufficient_matches:return "insufficient_matches";
    case FreshPixelRejection::incoherent_or_narrow_support:return "incoherent_or_narrow_support";
    case FreshPixelRejection::low_quality:return "low_quality";
    case FreshPixelRejection::translation_out_of_range:return "translation_out_of_range";
    case FreshPixelRejection::projected_box_outside_frame:return "projected_box_outside_frame";
    case FreshPixelRejection::projected_people_merge:return "projected_people_merge";
    case FreshPixelRejection::work_budget:return "work_budget";
  }
  return "unknown";
}

void MobileFreshPixelProjection::reset() noexcept {
  boxes_.fill({});evidence_.fill({});
}

bool MobileFreshPixelProjection::configure(std::uint32_t width,std::uint32_t height) {
  reset();configured_=false;
  if(width!=640||height!=640)return false;
  try {
    for(auto* p:{&source_,&latest_}){
      p->gray.resize(kGrayWidth*kGrayHeight);
      p->coarse.resize((kGrayWidth/2)*(kGrayHeight/2));
    }
  }catch(const std::bad_alloc&){return false;}
  width_=width;height_=height;configured_=true;return true;
}

FreshPixelProjectionResult MobileFreshPixelProjection::process(
    std::span<const std::uint8_t> source_rgb,std::span<const std::uint8_t> latest_rgb,
    std::span<const YoloDetection> input_boxes,FreshPixelIdentity source_id,
    FreshPixelIdentity latest_id,std::uint64_t maximum_work_us) noexcept {
  const auto began=std::chrono::steady_clock::now();reset();
  FreshPixelProjectionResult result;
  result.source_identity=source_id;result.target_identity=latest_id;
  const auto count=std::min(input_boxes.size(),kMaximumTargets);
  for(unsigned i=0;i<count;++i)evidence_[i].source_index=i;
  const auto elapsed=[&]{return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now()-began).count());};
  const auto exhausted=[&]{return maximum_work_us&&elapsed()>=maximum_work_us;};
  const auto finish=[&]() {
    result.elapsed_us=elapsed();
    if(maximum_work_us&&result.elapsed_us>=maximum_work_us){
      result.budget_exhausted=true;result.rejection=FreshPixelRejection::work_budget;
      for(unsigned i=0;i<count;++i){auto& e=evidence_[i];
        e.valid=false;e.pixel_observed_at_us=0;e.rejection=FreshPixelRejection::work_budget;}
    }
    result.valid_count=0;
    for(unsigned i=0;i<count;++i)result.valid_count+=evidence_[i].valid;
    result.complete=count!=0&&count==input_boxes.size()&&result.valid_count==count&&!result.budget_exhausted;
    result.targets={evidence_.data(),count};
    if(result.complete){result.projected_boxes={boxes_.data(),count};result.rejection=FreshPixelRejection::none;}
    else if(result.rejection==FreshPixelRejection::none){
      for(unsigned i=0;i<count;++i)if(!evidence_[i].valid){result.rejection=evidence_[i].rejection;break;}
    }
    return result;
  };
  const auto reject=[&](FreshPixelRejection reason){
    result.rejection=reason;for(unsigned i=0;i<count;++i)evidence_[i].rejection=reason;return finish();};
  if(!configured_)return reject(FreshPixelRejection::not_configured);
  const std::size_t bytes=std::size_t(width_)*height_*6;
  if(source_rgb.size()!=bytes||latest_rgb.size()!=bytes)return reject(FreshPixelRejection::invalid_pixels);
  const auto sa=reinterpret_cast<std::uintptr_t>(source_rgb.data()),ta=reinterpret_cast<std::uintptr_t>(latest_rgb.data());
  if(sa<ta+bytes&&ta<sa+bytes)return reject(FreshPixelRejection::invalid_pixels);
  if(!source_id.generation||!source_id.sequence||!source_id.observed_at_us||
      !latest_id.generation||!latest_id.sequence||!latest_id.observed_at_us)return reject(FreshPixelRejection::invalid_identity);
  if(source_id.generation!=latest_id.generation)return reject(FreshPixelRejection::generation_mismatch);
  if(latest_id.sequence<=source_id.sequence||latest_id.observed_at_us<=source_id.observed_at_us)return reject(FreshPixelRejection::not_newer);
  if(latest_id.observed_at_us-source_id.observed_at_us>50'000U)return reject(FreshPixelRejection::gap_over50ms);
  if(input_boxes.empty()||input_boxes.size()>kMaximumTargets)return reject(FreshPixelRejection::unsupported_box_count);
  // An all-invalid source batch can never publish a projection. Reject it
  // before touching both full RGB tensors; keep per-box evidence identical
  // and preserve the existing geometric thresholds and all-or-none contract.
  if(std::none_of(input_boxes.begin(),input_boxes.end(),valid_body))
    return reject(FreshPixelRejection::invalid_source_box);
  const auto pyramid=[&](std::span<const std::uint8_t> rgb,Pyramid& p){
    downsample_rgb_u16_area2(rgb.data(),width_,height_,p.gray.data());
    for(unsigned y=0;y<kGrayHeight/2;++y)for(unsigned x=0;x<kGrayWidth/2;++x){
      const auto i=2*y*kGrayWidth+2*x;
      p.coarse[y*(kGrayWidth/2)+x]=std::uint8_t((unsigned(p.gray[i])+p.gray[i+1]+p.gray[i+kGrayWidth]+p.gray[i+kGrayWidth+1])/4);
    }
  };
  // Both conversions and every following check are inside the same budget.
  pyramid(source_rgb,source_);
  if(exhausted())return reject(FreshPixelRejection::work_budget);
  pyramid(latest_rgb,latest_);
  if(exhausted())return reject(FreshPixelRejection::work_budget);
  const View before{source_.gray,kGrayWidth,kGrayHeight},after{latest_.gray,kGrayWidth,kGrayHeight};
  const View bc{source_.coarse,kGrayWidth/2,kGrayHeight/2},ac{latest_.coarse,kGrayWidth/2,kGrayHeight/2};
  for(unsigned index=0;index<count;++index){
    auto& out=evidence_[index];const auto& box=input_boxes[index];
    if(exhausted()){out.rejection=FreshPixelRejection::work_budget;result.budget_exhausted=true;continue;}
    if(!valid_body(box)){out.rejection=FreshPixelRejection::invalid_source_box;continue;}
    std::array<Flow,9> flows{};std::array<std::pair<int,int>,9> points{};
    unsigned point_count{},matched{},ambiguous{},bad_fb{};
    const float anchor=box.y1+(box.y2-box.y1)*.18F;
    for(int gy=0;gy<3;++gy)for(int gx=0;gx<3;++gx){
      if(exhausted()){result.budget_exhausted=true;continue;}
      const int sx=int(std::lround((box.x1+(box.x2-box.x1)*(.25F+.25F*gx))/kScale));
      const int sy=int(std::lround((anchor+(box.y2-box.y1)*(.12F*(gy-1)))/kScale));
      int px=sx,py=sy,best{};
      for(int y=sy-2;y<=sy+2;++y)for(int x=sx-2;x<=sx+2;++x){
        if(std::abs(x-int(kGrayWidth/2))<=5&&std::abs(y-int(kGrayHeight/2))<=5)continue;
        if(x*int(kScale)<box.x1||x*int(kScale)>box.x2||y*int(kScale)<box.y1||y*int(kScale)>box.y1+(box.y2-box.y1)*.55F)continue;
        const auto score=texture(before,x,y);if(score>best){best=score;px=x;py=y;}
      }
      if(best<800)continue;
      bool duplicate{};for(unsigned i=0;i<point_count;++i)duplicate|=std::hypot(float(px-points[i].first),float(py-points[i].second))<3;
      if(duplicate)continue;points[point_count++]={px,py};++out.candidates;
      const auto forward=match(before,after,bc,ac,px,py,0,0,16);
      if(!forward.valid){++ambiguous;continue;}
      const auto back=match(after,before,ac,bc,int(std::lround(forward.x)),int(std::lround(forward.y)),0,0,16);
      if(!back.valid){++ambiguous;continue;}
      const float fb=std::hypot(back.x-px,back.y-py);if(fb>1.5F){++bad_fb;continue;}
      flows[matched++]={forward.x-px,forward.y-py,float(px),float(py),std::min(forward.score,back.score),fb,px,py};
    }
    if(exhausted())result.budget_exhausted=true;
    if(result.budget_exhausted){out.rejection=FreshPixelRejection::work_budget;continue;}
    if(matched<5){
      out.rejection=out.candidates<5?FreshPixelRejection::low_texture_or_support:
          bad_fb>=3?FreshPixelRejection::forward_backward:ambiguous?FreshPixelRejection::ambiguous_or_low_ncc:FreshPixelRejection::insufficient_matches;
      continue;
    }
    std::array<float,9> xs{},ys{};
    for(unsigned i=0;i<matched;++i){xs[i]=flows[i].x;ys[i]=flows[i].y;}
    std::sort(xs.begin(),xs.begin()+matched);std::sort(ys.begin(),ys.begin()+matched);
    const auto mx=xs[matched/2],my=ys[matched/2];float x{},y{},quality{},fb{};
    int minx=kGrayWidth,maxx{},miny=kGrayHeight,maxy{};
    for(unsigned i=0;i<matched;++i){const auto& f=flows[i];
      if(std::hypot(f.x-mx,f.y-my)>1.5F)continue;
      ++out.support;x+=f.x;y+=f.y;quality+=f.score;fb+=f.fb;
      minx=std::min(minx,f.point_x);maxx=std::max(maxx,f.point_x);miny=std::min(miny,f.point_y);maxy=std::max(maxy,f.point_y);
    }
    if(out.support<5||out.support*3<out.candidates*2||out.support*3<matched*2||maxx-minx<3||maxy-miny<3){out.rejection=FreshPixelRejection::incoherent_or_narrow_support;continue;}
    out.dx_model_px=x*kScale/out.support;out.dy_model_px=y*kScale/out.support;
    out.quality=quality/out.support;out.forward_backward_error_model_px=fb*kScale/out.support;
    if(out.quality<.90F){out.rejection=FreshPixelRejection::low_quality;continue;}
    if(std::hypot(out.dx_model_px,out.dy_model_px)>128){out.rejection=FreshPixelRejection::translation_out_of_range;continue;}
    auto translated=box;translated.x1+=out.dx_model_px;translated.x2+=out.dx_model_px;translated.y1+=out.dy_model_px;translated.y2+=out.dy_model_px;
    if(translated.x1<0||translated.y1<0||translated.x2>width_||translated.y2>height_){out.rejection=FreshPixelRejection::projected_box_outside_frame;continue;}
    boxes_[index]=translated;out.valid=true;out.pixel_observed_at_us=latest_id.observed_at_us;out.rejection=FreshPixelRejection::none;
  }
  std::array<bool,kMaximumTargets> conflicts{};
  for(unsigned i=0;i<count;++i)for(unsigned j=i+1;j<count;++j)
    if(evidence_[i].valid&&evidence_[j].valid&&overlap(boxes_[i],boxes_[j])>.55F)conflicts[i]=conflicts[j]=true;
  for(unsigned i=0;i<count;++i)if(conflicts[i]){auto& e=evidence_[i];e.valid=false;e.pixel_observed_at_us=0;e.rejection=FreshPixelRejection::projected_people_merge;}
  return finish();
}

} // namespace vfdual_android
