#pragma once
#include "MobileControlCore.hpp"
#include "vfdual/protocol.hpp"
#include <array>
#include <span>

namespace vfdual_android {
inline constexpr float kModelToControlPixelScaleX =
    float(vfdual::kProductionCaptureWidth) / vfdual::kDefaultMobileModel.input.width;
inline constexpr float kModelToControlPixelScaleY =
    float(vfdual::kProductionCaptureHeight) / vfdual::kDefaultMobileModel.input.height;
static_assert(kModelToControlPixelScaleX == kModelToControlPixelScaleY,
              "Image correspondence and evidence limits use one isotropic pixel scale");
static_assert(kModelToControlPixelScaleX == 1.F && kModelToControlPixelScaleY == 1.F,
              "Production capture and detector must keep a one-to-one pixel domain");
// Neural tensors stay 640-space. Control runs in physical pixels relative to
// the screen crop, preserving calibrated mouse response and pixel tolerances.
inline MobileControlConfig screen_pixel_control_config() {
  MobileControlConfig c;
  c.model_width=vfdual::kProductionCaptureWidth;
  c.model_height=vfdual::kProductionCaptureHeight;
  c.model_center_x=c.model_width/2.F;c.model_center_y=c.model_height/2.F;
  c.background_motion_maximum_shift_pixels *= kModelToControlPixelScaleX;
  c.background_motion_ordinary_shift_pixels *= kModelToControlPixelScaleX;
  c.image_motion_maximum_displacement_pixels *= kModelToControlPixelScaleX;
  c.image_constraint_maximum_forward_backward_error_pixels *= kModelToControlPixelScaleX;
  c.image_identity_maximum_forward_backward_error_pixels *= kModelToControlPixelScaleX;
  return c;
}
struct ControlSpaceFrame {
  std::array<YoloDetection,300> boxes{};
  std::array<TargetImageMotion,16> motions{};
  std::size_t box_count{},motion_count{};
  BackgroundMotion background{};
  bool transform(std::span<const YoloDetection> input,std::span<const TargetImageMotion> image,
                 const BackgroundMotion& bg) noexcept {
    if(input.size()>boxes.size() || image.size()>motions.size())return false;
    constexpr float sx=kModelToControlPixelScaleX;
    constexpr float sy=kModelToControlPixelScaleY;
    box_count=input.size();motion_count=image.size();background=bg;background.x*=sx;background.y*=sy;
    for(std::size_t i=0;i<box_count;++i){boxes[i]=input[i];boxes[i].x1*=sx;boxes[i].x2*=sx;boxes[i].y1*=sy;boxes[i].y2*=sy;}
    for(std::size_t i=0;i<motion_count;++i){motions[i]=image[i];motions[i].x*=sx;motions[i].y*=sy;motions[i].forward_backward_error*=sx;}
    return true;
  }
};
}
