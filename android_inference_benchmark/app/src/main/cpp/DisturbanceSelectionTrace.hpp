#pragma once

#include "DiagnosticTraceCursor.hpp"
#include "MobileResponseEstimator.hpp"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

namespace vfdual_android {
// Numeric, event-only diagnostics. Owner synchronizes observe/drain; report()
// belongs to the background consumer AFTER releasing the control mutex.
// No diagnostic value is read by the control or velocity-selection algorithm.
class DisturbanceSelectionTrace final {
 public:
  static constexpr std::size_t capacity=256, field_count=35;
  static constexpr float near_target_pixels=16.0F, velocity_jump_pixels_per_second=100.0F;
  // disturbance_selection_trace_v1 fields (times phone monotonic us):
  // id,controlTraceId,sequence,observed,now,wire,lock,track,axis,eventFlags,
  // branch,window,pointCount,firstPoint,lastPoint,span,delayLow,delayHigh,
  // delayHypotheses,minVelocityMilli,maxVelocityMilli,rawQualityE6,
  // qualityWeightE6,proposalMilli,disturbanceMilli,selectedVelocityMilli,
  // targetErrorMilli,previousSelectedMilli,responseE6,currentDelay,
  // restoredDelay,frameInterval,availableCommands,retiredAxes,geometryAxes.
  // axis: 0=X,1=Y. Velocity/error units are the SAME control-ROI pixels as
  // control_trace_v6 (physical desktop pixels in the production profile).
  // branch: 0=insufficient,1=nearzero,2=proposal_fallback,3=conservative.
  // window: 0=full,1=recent_four,2=control_point. has_interval distinguishes
  // absent evidence from a genuinely measured [0,0] interval.
  using Row=std::array<std::int64_t,field_count>;
  enum EventFlag : std::uint32_t {
    first_context=1U, branch_changed=2U, window_changed=4U,
    near_target_jump=8U, valid_changed=16U, proposal_limited_changed=32U,
    tracking_only=64U, has_interval=128U, estimator_valid=256U,
    trusted=512U, proposal_limited=1024U, near_target=2048U,
  };
  struct Context {
    std::uint64_t control_trace_id{}, sequence{}, observed_at_us{}, now_us{};
    std::uint64_t wire_frame_id{}, lock_id{}, track_id{}, frame_interval_us{};
    std::uint64_t current_delay_us{}, restored_delay_us{};
    float error_x{}, error_y{}, response_x{}, response_y{};
    bool tracking_only{};
  };
  struct Snapshot {
    std::array<Row,capacity> rows{};
    std::size_t count{};
    std::uint64_t lost{};
    bool reset{};
    [[nodiscard]] std::string report() const {
      std::ostringstream out;
      out << "disturbance_selection_clock=phone_monotonic_us "
          << "disturbance_selection_velocity_scale=1000 "
          << "disturbance_selection_quality_scale=1000000 "
          << "disturbance_selection_trace_lost=" << lost
          << " disturbance_selection_trace_reset=" << (reset?1:0)
          << " disturbance_selection_trace_v1=[";
      for (std::size_t i=0;i<count;++i) {
        if (i) out << ';';
        for (std::size_t j=0;j<field_count;++j) { if(j) out << ','; out << rows[i][j]; }
      }
      return out.str()+']';
    }
  };
  void observe(const Context& context,const MobileResponseEstimator::VisibleMotion& motion,
      float proposal_x,float proposal_y,float selected_x,float selected_y,
      bool trusted_x,bool trusted_y,unsigned retired_axes,unsigned geometry_axes) noexcept {
    const bool near=std::abs(context.error_x)<=near_target_pixels &&
        std::abs(context.error_y)<=near_target_pixels;
    observe_axis(context,motion.x_evidence,0,motion.x,motion.x_valid,motion.x_proposal_limited,
        proposal_x,selected_x,trusted_x,near,retired_axes,geometry_axes);
    observe_axis(context,motion.y_evidence,1,motion.y,motion.y_valid,motion.y_proposal_limited,
        proposal_y,selected_y,trusted_y,near,retired_axes,geometry_axes);
  }
  void drain(Snapshot& out) noexcept {
    const auto range=cursor_.advance(total_,capacity);
    out.count=0; out.lost=range.lost; out.reset=range.reset;
    for (auto id=range.first;id<=range.last;++id) out.rows[out.count++]=rows_[(id-1U)%capacity];
    drained_=total_;
  }
  [[nodiscard]] bool has_pending() const noexcept { return total_!=drained_; }
  [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
  // Preserve unread rows/cursor across controller resets; only forget the
  // previous event comparison so a new observation is explicitly contextual.
  void reset_observation_history() noexcept { previous_={}; }
 private:
  struct Previous {
    std::uint64_t lock{},track{},observed{};
    MobileResponseEstimator::DisturbanceBranch branch{};
    MobileResponseEstimator::DisturbanceWindow window{};
    std::uint32_t points{};
    float selected{};
    bool present{},valid{},proposal_limited{};
  };
  static std::int64_t scaled(double value,double scale) noexcept {
    if (!std::isfinite(value)) return 0;
    const long double result=static_cast<long double>(value)*scale;
    constexpr auto hi=std::numeric_limits<std::int64_t>::max();
    constexpr auto lo=std::numeric_limits<std::int64_t>::min();
    if (result>=static_cast<long double>(hi)) return hi;
    if (result<=static_cast<long double>(lo)) return lo;
    return static_cast<std::int64_t>(std::llround(result));
  }
  void observe_axis(const Context& context,
      const MobileResponseEstimator::DisturbanceAxisEvidence& evidence,unsigned axis,
      float disturbance,bool valid,bool limited,float proposal,float selected,
      bool axis_trusted,bool near,unsigned retired,unsigned geometry) noexcept {
    auto& previous=previous_[axis];
    const bool same=previous.present && previous.lock==context.lock_id &&
        previous.track==context.track_id && context.observed_at_us>previous.observed;
    unsigned event=0;
    if (!same) event|=first_context;
    else {
      if (previous.branch!=evidence.branch) event|=branch_changed;
      // Changing clock values is NOT a new event. Only the actual window type
      // or number of selected points changes this flag.
      if (previous.window!=evidence.window || previous.points!=evidence.points) event|=window_changed;
      if (previous.valid!=valid) event|=valid_changed;
      if (previous.proposal_limited!=limited) event|=proposal_limited_changed;
      if (near && std::abs(selected-previous.selected)>velocity_jump_pixels_per_second)
        event|=near_target_jump;
    }
    if (event) {
      unsigned flags=event | (context.tracking_only?tracking_only:0U) |
          (evidence.has_interval?has_interval:0U) | (valid?estimator_valid:0U) |
          (axis_trusted?trusted:0U) | (limited?proposal_limited:0U) | (near?near_target:0U);
      Row row{};
      row[0]=static_cast<std::int64_t>(++total_);
      row[1]=static_cast<std::int64_t>(context.control_trace_id);
      row[2]=static_cast<std::int64_t>(context.sequence);
      row[3]=static_cast<std::int64_t>(context.observed_at_us);
      row[4]=static_cast<std::int64_t>(context.now_us);
      row[5]=static_cast<std::int64_t>(context.wire_frame_id);
      row[6]=static_cast<std::int64_t>(context.lock_id);
      row[7]=static_cast<std::int64_t>(context.track_id);
      row[8]=axis; row[9]=flags;
      row[10]=static_cast<std::int64_t>(evidence.branch);
      row[11]=static_cast<std::int64_t>(evidence.window);
      row[12]=evidence.points;
      row[13]=static_cast<std::int64_t>(evidence.first_observed_at_us);
      row[14]=static_cast<std::int64_t>(evidence.last_observed_at_us);
      row[15]=static_cast<std::int64_t>(evidence.span_us);
      row[16]=static_cast<std::int64_t>(evidence.delay_low_us);
      row[17]=static_cast<std::int64_t>(evidence.delay_high_us);
      row[18]=evidence.delay_hypotheses;
      row[19]=scaled(evidence.minimum,1000); row[20]=scaled(evidence.maximum,1000);
      row[21]=scaled(evidence.raw_quality,1e6); row[22]=scaled(evidence.quality_weight,1e6);
      row[23]=scaled(proposal,1000); row[24]=scaled(disturbance,1000);
      row[25]=scaled(selected,1000);
      row[26]=scaled(axis?context.error_y:context.error_x,1000);
      row[27]=same?scaled(previous.selected,1000):0;
      row[28]=scaled(axis?context.response_y:context.response_x,1e6);
      row[29]=static_cast<std::int64_t>(context.current_delay_us);
      row[30]=static_cast<std::int64_t>(context.restored_delay_us);
      row[31]=static_cast<std::int64_t>(context.frame_interval_us);
      row[32]=evidence.available_commands; row[33]=retired; row[34]=geometry;
      rows_[(total_-1U)%capacity]=row;
    }
    previous={context.lock_id,context.track_id,context.observed_at_us,evidence.branch,evidence.window,
              evidence.points,selected,true,valid,limited};
  }
  std::array<Row,capacity> rows_{};
  std::array<Previous,2> previous_{};
  std::uint64_t total_{},drained_{};
  DiagnosticTraceCursor cursor_{};
};
} // namespace vfdual_android
