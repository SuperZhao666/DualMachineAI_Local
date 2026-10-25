#pragma once
#include "MobileResponseEstimator.hpp"
#include "VelocitySampleContinuity.hpp"
#include <array>
#include <algorithm>
#include <cmath>

namespace vfdual_android {
// Screen motion with only our own HID response removed. Evaluate a range of
// plausible response delays: uncertainty may reduce feedforward, never invent
// a velocity because a pending report appeared a frame earlier or later.
class MobileDisturbanceVelocity final {
public:
  MobileResponseEstimator::VisibleMotion update(float x, float y, std::uint64_t at,
      const MobileResponseEstimator& response, float maximum,
      float proposal_x = 0.0F, float proposal_y = 0.0F,
      float control_offset_x = 0.0F, float control_offset_y = 0.0F,
      bool near_vertical_aim_point = false,
      std::uint64_t adjacent_previous = 0U) noexcept {
    retired_axes_ = 0U;
    geometry_bounded_axes_ = 0U;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(control_offset_x) ||
        !std::isfinite(control_offset_y) || !std::isfinite(maximum) || maximum<=0 || !at) {
      reset(VelocityHistoryState::invalid_input);return {};
    }
    if(size_)state_=VelocityHistoryState::insufficient;
    if(size_) {
      const auto interval=velocity_sample_interval(points_[size_-1].at,at,adjacent_previous);
      if(interval!=VelocityHistoryState::insufficient) {
        reset(interval);
        if(interval==VelocityHistoryState::invalid_input)return {};
      }
    }
    if (size_ == points_.size()) {
      for (unsigned i=1;i<size_;++i) {
        points_[i-1]=points_[i]; control_points_[i-1]=control_points_[i];
      }
      --size_;
    }
    control_points_[size_]={x+control_offset_x,y+control_offset_y,at};
    points_[size_++]={x,y,at};
    bool sparse=false;
    for(unsigned i=1;i<size_;++i)sparse=sparse || points_[i].at-points_[i-1].at>30'000U;
    const auto minimum_span=at-points_[size_>=4U?size_-4U:0U].at;
    const auto window=velocity_history_window(sparse,minimum_span);
    while(size_>1U && at-points_[0].at>window) {
      for(unsigned i=1;i<size_;++i) {points_[i-1]=points_[i];control_points_[i-1]=control_points_[i];}
      --size_;
    }
    // Preserve the insufficient-history zero output; expose its real one-point
    // history rather than an evidence-free default diagnostic.
    if (size_<2) return response.disturbance_velocity({points_.data(),size_});
    auto result=response.disturbance_velocity({points_.data(),size_},proposal_x,proposal_y,true);
    if (size_ > 4U) {
      // Six points suppress box noise, but carry an old slope past a real stop.
      // Four recent points may ONLY retire it when every plausible HID delay
      // agrees on a near-zero trend. A short-window reversal/new velocity is
      // too sensitive to delivery jitter and must keep the full-window fit.
      const auto recent = response.disturbance_velocity(
          {points_.data()+size_-4U,4U});
      if (recent.x_valid && recent.x == 0.F) {
        if (result.x != 0.F) retired_axes_ |= 1U;
        result.x=0.F; result.x_valid=true;
        result.x_stationary=recent.x_stationary;
        result.x_proposal_limited=recent.x_proposal_limited;
        result.x_evidence=recent.x_evidence;
        result.x_evidence.window=MobileResponseEstimator::DisturbanceWindow::recent_four;
      }
      if (recent.y_valid && recent.y == 0.F) {
        if (result.y != 0.F) retired_axes_ |= 2U;
        result.y=0.F; result.y_valid=true;
        result.y_stationary=recent.y_stationary;
        result.y_proposal_limited=recent.y_proposal_limited;
        result.y_evidence=recent.y_evidence;
        result.y_evidence.window=MobileResponseEstimator::DisturbanceWindow::recent_four;
      }
    }
    // A geometric head and the body's centre are different moving points when
    // the detected extent changes. A shrinking lower edge can move the centre
    // upward while the requested aim point stays still. Retire that prediction
    // only when the requested point stays inside a two-pixel envelope AND
    // every plausible HID schedule agrees it is stationary. A near-zero
    // fitted slope alone can also be a reversal and is insufficient here.
    // An uncertain or moving point keeps the existing pursuit.
    // This guard cannot create, reverse or amplify a velocity command.
    const auto stationary_point_with_changing_offset = [&](bool y_axis) {
      float low=0.0F, high=0.0F;
      float control_low=0.0F, control_high=0.0F;
      for (unsigned i=0;i<size_;++i) {
        const float offset=y_axis ? control_points_[i].y-points_[i].y
                                  : control_points_[i].x-points_[i].x;
        if (!i) low=high=offset;
        else { low=std::min(low,offset); high=std::max(high,offset); }
        const float value=y_axis?control_points_[i].y:control_points_[i].x;
        if (!i) control_low=control_high=value;
        else { control_low=std::min(control_low,value); control_high=std::max(control_high,value); }
      }
      // The real detector has approximately pixel-scale anchor noise even
      // on a stationary training-room person. A half-pixel range rejected
      // that evidence and let body-height changes seed a persistent lead.
      // The separate all-HID-delay zero-trend test below remains mandatory;
      // this spatial envelope alone cannot classify a moving point as stopped.
      const float envelope=y_axis && near_vertical_aim_point ? 2.0F : .5F;
      return high-low>1.0F && control_high-control_low<=envelope;
    };
    const bool changing_x=stationary_point_with_changing_offset(false);
    const bool changing_y=stationary_point_with_changing_offset(true);
    if (size_>=4U && (changing_x || changing_y)) {
      const auto control=response.disturbance_velocity(
          {control_points_.data(),size_});
      const auto retire = [&](float& value, float alternative, bool valid, unsigned flag) {
        if (valid && alternative==0.0F && value!=0.0F) {
          geometry_bounded_axes_|=flag;
          value=0.0F;
        }
      };
      if (changing_x) retire(result.x,control.x,control.x_valid,1U);
      if (changing_y) retire(result.y,control.y,control.y_valid,2U);
      if (geometry_bounded_axes_ & 1U) {
        result.x_stationary=control.x_stationary;
        result.x_evidence=control.x_evidence;
        result.x_evidence.window=MobileResponseEstimator::DisturbanceWindow::control_point;
      }
      if (geometry_bounded_axes_ & 2U) {
        result.y_stationary=control.y_stationary;
        result.y_evidence=control.y_evidence;
        result.y_evidence.window=MobileResponseEstimator::DisturbanceWindow::control_point;
      }
    }
    if(adjacent_previous && size_>=2U && at-points_[size_-2U].at>30'000U) {
      const auto current_direction=response.disturbance_velocity({points_.data()+size_-2U,2U});
      if((result.x>0.F && current_direction.x_negative_supported) ||
          (result.x<0.F && current_direction.x_positive_supported)) {
        result.x=0.F;result.x_valid=result.x_stationary=false;
      }
      if((result.y>0.F && current_direction.y_negative_supported) ||
          (result.y<0.F && current_direction.y_positive_supported)) {
        result.y=0.F;result.y_valid=result.y_stationary=false;
      }
    }
    result.x=std::clamp(result.x,-maximum,maximum);
    result.y=std::clamp(result.y,-maximum,maximum);
    state_=result.x_stationary && result.y_stationary?VelocityHistoryState::stationary:
        (result.x_valid && result.x!=0.F) || (result.y_valid && result.y!=0.F)?
            VelocityHistoryState::moving:size_<4U?VelocityHistoryState::insufficient:VelocityHistoryState::uncertain;
    return result;
  }
  [[nodiscard]] unsigned retired_axes() const noexcept { return retired_axes_; }
  [[nodiscard]] unsigned geometry_bounded_axes() const noexcept { return geometry_bounded_axes_; }
  void reset(VelocityHistoryState reason=VelocityHistoryState::insufficient) noexcept {
    size_=0;retired_axes_=0;geometry_bounded_axes_=0;state_=reason;
  }
  [[nodiscard]] VelocityHistoryState state()const noexcept {return state_;}
  [[nodiscard]] unsigned sample_count()const noexcept {return size_;}
  [[nodiscard]] std::uint64_t span_us()const noexcept {return size_?points_[size_-1U].at-points_[0].at:0U;}
private:
  std::array<MobileResponseEstimator::TargetObservation,6> points_{};
  std::array<MobileResponseEstimator::TargetObservation,6> control_points_{};
  unsigned size_{};
  unsigned retired_axes_{};
  unsigned geometry_bounded_axes_{};
  VelocityHistoryState state_{VelocityHistoryState::insufficient};
};
}
