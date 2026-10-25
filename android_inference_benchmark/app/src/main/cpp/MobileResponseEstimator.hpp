#pragma once
#include "MobileBackgroundMotion.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace vfdual_android {

// Online system identification from background motion and accepted reports.
// Fits sensitivity plus a constant manual-camera drift over a short window;
// tests multiple effective delays. Moving person boxes never enter this fit.
// A fit is an estimate, not proof of physical HID completion.
class MobileResponseEstimator final {
 public:
  struct Axis { float response{1.0F}; float quality{}; bool trusted{}; };
  enum class LearningState : std::uint8_t {
    warming_up, invalid_background, discontinuous_time, insufficient_excitation,
    background_disagreement, restored_scale_disagreement, ambiguous_delay, identified,
  };
  struct DelayRange {
    std::uint64_t minimum_us{8'000U}, maximum_us{100'000U};
    std::uint32_t candidates{};
    // Fresh observations identify a narrow single mode; restored calibration
    // remains a scoped prior, not proof of current physical HID completion.
    bool identifiable{};
  };
  // Same-scope in-memory migration is not a cold, validated file restore.
  // Keep its known uncertainty and identification qualification as well as
  // the small nominal-delay prior; old frame/command histories are not moved.
  struct RuntimeCalibration {
    Axis x{},y{};
    std::uint64_t delay_us{};
    DelayRange range{};
    bool restored_reference{},delay_established{};
    std::uint64_t restored_delay_us{},updates{},delay_updates{};
    std::array<std::uint64_t,5> delay_history{};
    std::size_t delay_count{};
    LearningState learning_state{LearningState::warming_up};
  };
  [[nodiscard]] RuntimeCalibration runtime_calibration()const noexcept {
    return {x_,y_,delay_us_,delay_range_,restored_reference_,delay_established_,
        restored_delay_us_,updates_,delay_updates_,delay_history_,delay_count_,learning_state_};
  }
  [[nodiscard]] bool restore_runtime_calibration(const RuntimeCalibration& saved) noexcept {
    if(!saved.x.trusted || !saved.y.trusted ||
        (saved.range.identifiable && saved.range.candidates==0U) ||
        saved.range.candidates>24U ||
        saved.range.minimum_us<8'000U || saved.range.maximum_us>100'000U ||
        saved.range.minimum_us>saved.delay_us || saved.range.maximum_us<saved.delay_us ||
        (saved.restored_reference && (saved.restored_delay_us<8'000U || saved.restored_delay_us>100'000U)))return false;
    for(std::size_t i=0;i<std::min(saved.delay_count,saved.delay_history.size());++i)
      if(saved.delay_history[i]<8'000U || saved.delay_history[i]>100'000U)return false;
    if(!restore(saved.x.response,saved.y.response,saved.delay_us,saved.x.quality,saved.y.quality))return false;
    delay_range_=saved.range;restored_reference_=saved.restored_reference;
    restored_delay_us_=saved.restored_delay_us;delay_established_=saved.delay_established;
    delay_history_=saved.delay_history;delay_count_=saved.delay_count;
    updates_=saved.updates;delay_updates_=saved.delay_updates;learning_state_=saved.learning_state;
    return true;
  }
  void accept(int x, int y, std::uint64_t at) noexcept {
    commands_[command_count_++ % commands_.size()] = {x, y, at};
  }
  void observe(const BackgroundMotion& motion, std::uint64_t at) noexcept {
    if (at == 0 || (observation_count_ &&
        at <= observations_[(observation_count_-1U)%observations_.size()].at)) {
      clear_history(); learning_state_=LearningState::discontinuous_time;
      delay_range_.identifiable=false;
      return;
    }
    const bool previous = observation_count_ != 0;
    const auto previous_at = previous ? observations_[(observation_count_-1U)%observations_.size()].at : 0U;
    const bool continuous = !previous || (motion.previous_observed_at_us == previous_at &&
        at-previous_at <= 60'000U);
    if (!motion.valid || !continuous) {
      confirmations_x_=confirmations_y_=0; delay_count_=0;
      observations_[observation_count_++%observations_.size()]={motion,at};
      learning_state_=!continuous?LearningState::discontinuous_time:LearningState::invalid_background;
      delay_range_.identifiable=false;
      return;
    }
    observations_[observation_count_++ % observations_.size()] = {motion, at};
    if (observation_count_ < 20) { learning_state_=LearningState::warming_up; return; }
    if (observation_count_ % 4 != 0) return;
    auto fx = fit(false), fy = fit(true);
    // A saved same-device response is the reference. Correlated player input
    // can fit a convincing but incorrect scale during combat (observed Y=.27
    // after a .75 calibration). Do not let that fit redefine the actuator.
    if (restored_reference_) {
      if (fx.valid && std::abs(fx.response-x_.response)>x_.response*.20F) {
        fx.valid=false; fx.reason=LearningState::restored_scale_disagreement;
      }
      if (fy.valid && std::abs(fy.response-y_.response)>y_.response*.20F) {
        fy.valid=false; fy.reason=LearningState::restored_scale_disagreement;
      }
    }
    update_axis(x_, pending_x_, confirmations_x_, fx);
    update_axis(y_, pending_y_, confirmations_y_, fy);
    const bool has_x=fx.valid && x_.trusted, has_y=fy.valid && y_.trusted;
    std::uint32_t plausible = has_x ? fx.delay_mask : 0U;
    bool conflicting_axes=false;
    if (has_y) {
      if (has_x) {
        const auto common=plausible & fy.delay_mask;
        conflicting_axes=common==0U;
        plausible=common?common:(plausible|fy.delay_mask);
      } else plausible=fy.delay_mask;
    }
    if (!plausible) {
      learning_state_=fx.valid||fy.valid?LearningState::warming_up:
          (fx.reason==LearningState::insufficient_excitation && fy.reason==LearningState::insufficient_excitation
              ?LearningState::insufficient_excitation:LearningState::background_disagreement);
      if (fx.reason==LearningState::restored_scale_disagreement || fy.reason==LearningState::restored_scale_disagreement)
        learning_state_=LearningState::restored_scale_disagreement;
      delay_range_.identifiable=false;
      return;
    }
    unsigned first=24U,last{},count{};
    for(unsigned index{};index<24U;++index) if(plausible&(1U<<index)) {
      first=std::min(first,index); last=index; ++count;
    }
    const std::uint64_t grid_low=4'000U+first*4'000U, grid_high=4'000U+last*4'000U;
    // Candidate lags are a sampled grid, not exact timestamps. Retain the
    // adjacent grid cells so an off-grid true delay cannot fall out of risk.
    const std::uint64_t low=std::max<std::uint64_t>(8'000U,grid_low-4'000U);
    const std::uint64_t high=std::min<std::uint64_t>(100'000U,grid_high+4'000U);
    const bool identifiable=!conflicting_axes && count==last-first+1U && grid_high-grid_low<=8'000U;
    delay_range_={std::min<std::uint64_t>(low,delay_us_),std::max<std::uint64_t>(high,delay_us_),count,identifiable};
    if (!identifiable) {
      // A periodic alias is not resolved by picking its first/highest R² peak.
      // Keep the nominal prior, expose every plausible mode to the risk ledger.
      learning_state_=LearningState::ambiguous_delay;
      delay_count_=0;
      return;
    }
    learning_state_=LearningState::identified;
    {
      const auto identified_delay=delay_established_ && delay_us_>=low && delay_us_<=high
          ?delay_us_:(low+high)/2U;
      delay_history_[delay_count_++ % delay_history_.size()] = identified_delay;
      auto ordered = delay_history_;
      const auto history_count = std::min<std::size_t>(delay_count_, ordered.size());
      std::sort(ordered.begin(), ordered.begin() + history_count);
      // A single correlated manual-camera movement must not empty the pending
      // ledger early. Smooth the observed delay, including normal Bluetooth
      // paths whose measured response is longer than the old 36 ms search.
      const auto median = ordered[history_count / 2U];
      delay_us_ = delay_established_
          ? std::clamp(median, delay_us_ - std::min(delay_us_ - 8'000U, std::uint64_t{4'000}),
                       std::min<std::uint64_t>(100'000U, delay_us_ + 8'000U))
          : std::clamp<std::uint64_t>(median, 8'000U, 100'000U);
      delay_established_ = true;
      ++delay_updates_;
      delay_range_.minimum_us=std::min<std::uint64_t>(low,delay_us_);
      delay_range_.maximum_us=std::max<std::uint64_t>(high,delay_us_);
    }
  }
  [[nodiscard]] const Axis& x() const noexcept { return x_; }
  [[nodiscard]] const Axis& y() const noexcept { return y_; }
  [[nodiscard]] std::uint64_t delay_us() const noexcept { return delay_us_; }
  [[nodiscard]] const DelayRange& delay_range() const noexcept { return delay_range_; }
  [[nodiscard]] LearningState learning_state() const noexcept { return learning_state_; }
  [[nodiscard]] static const char* learning_state_name(LearningState state) noexcept {
    switch(state) {
      case LearningState::warming_up:return "warming_up";
      case LearningState::invalid_background:return "invalid_background";
      case LearningState::discontinuous_time:return "discontinuous_time";
      case LearningState::insufficient_excitation:return "insufficient_excitation";
      case LearningState::background_disagreement:return "background_disagreement";
      case LearningState::restored_scale_disagreement:return "restored_scale_disagreement";
      case LearningState::ambiguous_delay:return "ambiguous_delay";
      case LearningState::identified:return "identified";
    }
    return "unknown";
  }
  [[nodiscard]] std::uint64_t delay_updates() const noexcept { return delay_updates_; }
  [[nodiscard]] std::uint64_t updates() const noexcept { return updates_; }
  [[nodiscard]] bool restored_reference() const noexcept { return restored_reference_; }
  [[nodiscard]] std::uint64_t restored_delay_us() const noexcept { return restored_delay_us_; }
  [[nodiscard]] bool restore(float x, float y, std::uint64_t delay, float quality_x, float quality_y) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || x<.2F || x>4.F || y<.2F || y>4.F ||
        delay<8'000 || delay>100'000 || !std::isfinite(quality_x) || !std::isfinite(quality_y) ||
        quality_x<.82F || quality_x>1.F || quality_y<.82F || quality_y>1.F) return false;
    clear_history(); x_={x,quality_x,true}; y_={y,quality_y,true};
    delay_us_=delay; restored_delay_us_=delay; delay_history_.fill(delay); delay_count_=delay_history_.size();
    delay_established_=true; restored_reference_=true; updates_=delay_updates_=0;
    delay_range_={delay,delay,0U,false}; learning_state_=LearningState::warming_up;
    return true;
  }
  enum class DisturbanceBranch : std::uint8_t {
    insufficient=0, nearzero=1, proposal_fallback=2, conservative=3,
  };
  enum class DisturbanceWindow : std::uint8_t {
    full=0, recent_four=1, control_point=2,
  };
  // Write-only evidence of the EXISTING selection. None of these fields is
  // read by an estimator/control decision. A zero interval needs has_interval.
  struct DisturbanceAxisEvidence {
    double minimum{}, maximum{}, raw_quality{}, quality_weight{};
    std::uint64_t first_observed_at_us{}, last_observed_at_us{}, span_us{};
    std::uint64_t delay_low_us{}, delay_high_us{};
    std::uint32_t points{}, available_commands{}, delay_hypotheses{};
    DisturbanceBranch branch{DisturbanceBranch::insufficient};
    DisturbanceWindow window{DisturbanceWindow::full};
    bool has_interval{};
  };
  struct VisibleMotion {
    float x{}, y{};
    // A measured near-zero trend is different from an interval whose possible
    // velocities straddle zero. Callers must not use numeric zero as validity.
    bool x_valid{}, y_valid{};
    bool x_proposal_limited{}, y_proposal_limited{};
    DisturbanceAxisEvidence x_evidence{}, y_evidence{};
    // Semantic evidence: every plausible delay agrees on a near-zero trend.
    // An uncertain proposal clamped to zero must never acquire this meaning.
    bool x_stationary{}, y_stationary{};
    // Semantic interval signs, independent of whether enough samples support
    // a velocity fit. Diagnostics remain write-only; these are explicit input
    // validity facts usable to retire an old contrary estimate.
    bool x_positive_supported{},x_negative_supported{},y_positive_supported{},y_negative_supported{};
  };
  struct TargetObservation { float x{}, y{}; std::uint64_t at{}; };
  [[nodiscard]] VisibleMotion disturbance_velocity(std::span<const TargetObservation> points,
      float proposal_x = 0.0F, float proposal_y = 0.0F, bool has_proposal = false) const noexcept {
    VisibleMotion result{};
    result.x_evidence.points=static_cast<std::uint32_t>(points.size());
    result.x_evidence.available_commands=static_cast<std::uint32_t>(
        std::min<std::uint64_t>(command_count_,commands_.size()));
    if (!points.empty()) {
      result.x_evidence.first_observed_at_us=points.front().at;
      result.x_evidence.last_observed_at_us=points.back().at;
      if (points.back().at>=points.front().at)
        result.x_evidence.span_us=points.back().at-points.front().at;
    }
    result.y_evidence=result.x_evidence;
    if (points.size()<2) return result;
    const auto begin=command_count_-std::min<std::uint64_t>(command_count_,commands_.size());
    double st{}, stt{};
    for (const auto& p:points) { const double t=double(p.at-points.front().at)/1e6; st+=t; stt+=t*t; }
    const double variance=stt-st*st/points.size();
    if (variance<1e-9) return result;
    struct AxisMotion {
      float velocity{}; bool valid{}, proposal_limited{};
      DisturbanceAxisEvidence evidence{};
      bool stationary{};
      bool positive_supported{},negative_supported{};
    };
    const auto axis=[&](bool y) -> AxisMotion {
      AxisMotion motion{.evidence=result.x_evidence};
      auto& evidence=motion.evidence;
      const auto& axis=y?y_:x_;
      if (!axis.trusted) return motion;
      double minimum=std::numeric_limits<double>::max(), maximum=-minimum, best_quality{};
      // A single average delay is insufficient for queued HID delivery.
      // The former +/-16ms interval invented target motion in the stationary
      // target / +/-24ms FIFO-delay regression. Require agreement across the
      // wider response interval; zero means uncertain, not a measured stop.
      // A fitted median does not prove the saved response interval has become
      // impossible. Preserve both intervals when separating HID and motion.
      const auto earliest=std::min(delay_range_.minimum_us,
          restored_reference_?std::min(delay_us_,restored_delay_us_):delay_us_);
      const auto latest=std::max(delay_range_.maximum_us,
          restored_reference_?std::max(delay_us_,restored_delay_us_):delay_us_);
      const auto low=earliest>32'000?earliest-32'000:0;
      evidence.delay_low_us=low;
      evidence.delay_high_us=latest+32'000;
      for (auto delay=low;delay<=latest+32'000;delay+=4'000) {
        ++evidence.delay_hypotheses;
        auto first=begin, last=command_count_;
        while (first<last && commands_[first%commands_.size()].at+delay+4'000<=points.front().at) ++first;
        while (last>first && commands_[(last-1)%commands_.size()].at+delay>=points.back().at+4'000) --last;
        double sp{}, stp{}, spp{};
        for (const auto& p:points) {
          double own{};
          for (auto i=first;i<last;++i) {
            const auto& c=commands_[i%commands_.size()];
            const auto elapsed=static_cast<std::int64_t>(p.at)-static_cast<std::int64_t>(c.at)-static_cast<std::int64_t>(delay);
            own+=(y?c.y:c.x)*std::clamp((double(elapsed)+4'000)/8'000,0.0,1.0);
          }
          const double position=(y?p.y:p.x)+own*axis.response;
          const double t=double(p.at-points.front().at)/1e6;
          sp+=position; stp+=t*position; spp+=position*position;
        }
        const double cross=stp-st*sp/points.size(), energy=spp-sp*sp/points.size();
        const double velocity=cross/variance;
        minimum=std::min(minimum,velocity); maximum=std::max(maximum,velocity);
        if (energy>1e-6) best_quality=std::max(best_quality,cross*cross/(variance*energy));
      }
      evidence.minimum=minimum; evidence.maximum=maximum;
      evidence.raw_quality=best_quality;
      evidence.quality_weight=std::clamp((best_quality-.30)/.50,0.0,1.0);
      evidence.has_interval=true;
      motion.positive_supported=minimum>20;
      motion.negative_supported=maximum<-20;
      // Every plausible response delay agrees on a near-zero relative trend.
      // This includes a player already following the person: background/world
      // motion is not a reason to inject an extra mouse movement. Two samples
      // suffice for this narrowly bounded zero check, but not a velocity lead.
      if (minimum>=-20 && maximum<=20) {
        motion.valid=true; motion.stationary=true;
        evidence.branch=DisturbanceBranch::nearzero; return motion;
      }
      if (points.size()<4) return motion;
      // Roughly one pixel of box uncertainty over this short observation
      // window must not become a persistent velocity command.
      const double velocity=minimum>20?minimum-20:(maximum<-20?maximum+20:0);
      const double quality=std::clamp((best_quality-.30)/.50,0.0,1.0);
      // A camera-compensated target velocity is only a proposal: the player
      // may also be moving the camera. During ambiguous HID delivery, admit
      // only the part compatible with the SCREEN-relative observations.
      // An unconstrained fallback can jump to the opposite direction.
      if (velocity==0 && has_proposal) {
        const double proposal=y?proposal_y:proposal_x;
        const auto bounded=float(std::clamp(proposal,std::min(0.0,minimum*quality),
                                                    std::max(0.0,maximum*quality)));
        motion.velocity=bounded; motion.valid=true;
        motion.proposal_limited=std::abs(bounded-proposal)>.01;
        evidence.branch=DisturbanceBranch::proposal_fallback;
        return motion;
      }
      const auto estimate=float(velocity*quality);
      motion.velocity=estimate; motion.valid=estimate!=0.0F;
      evidence.branch=DisturbanceBranch::conservative;
      return motion;
    };
    const auto x=axis(false), y=axis(true);
    result.x=x.velocity; result.y=y.velocity;
    result.x_valid=x.valid; result.y_valid=y.valid;
    result.x_proposal_limited=x.proposal_limited;
    result.y_proposal_limited=y.proposal_limited;
    result.x_evidence=x.evidence; result.y_evidence=y.evidence;
    result.x_stationary=x.stationary; result.y_stationary=y.stationary;
    result.x_positive_supported=x.positive_supported;result.x_negative_supported=x.negative_supported;
    result.y_positive_supported=y.positive_supported;result.y_negative_supported=y.negative_supported;
    return result;
  }
  void clear_history() noexcept {
    command_count_ = observation_count_ = 0;
    confirmations_x_ = confirmations_y_ = 0;
    delay_count_ = 0;
    delay_range_.identifiable=false;
    learning_state_=LearningState::warming_up;
  }
 private:
  struct Command { int x{}, y{}; std::uint64_t at{}; };
  struct Observation { BackgroundMotion motion{}; std::uint64_t at{}; };
  struct Fit {
    float response{}, quality{}, energy{}; std::uint64_t delay_us{}; bool valid{};
    std::uint32_t delay_mask{};
    LearningState reason{LearningState::insufficient_excitation};
  };
  static bool remove_drift(double gram[3][3], const double bx[3], const double by[3],
                           double& xx, double& xy, double& yy) noexcept {
    double matrix[3][5]{};
    for (unsigned i=0;i<3;++i) {
      for (unsigned j=0;j<3;++j) matrix[i][j]=gram[i][j];
      matrix[i][3]=bx[i]; matrix[i][4]=by[i];
    }
    for (unsigned i=0;i<3;++i) {
      unsigned pivot=i;
      for (unsigned j=i+1;j<3;++j) if (std::abs(matrix[j][i])>std::abs(matrix[pivot][i])) pivot=j;
      if (std::abs(matrix[pivot][i])<1e-9) return false;
      for (unsigned k=i;k<5;++k) std::swap(matrix[i][k],matrix[pivot][k]);
      const double scale=matrix[i][i];
      for (unsigned k=i;k<5;++k) matrix[i][k]/=scale;
      for (unsigned j=0;j<3;++j) if (j!=i) {
        const double factor=matrix[j][i];
        for (unsigned k=i;k<5;++k) matrix[j][k]-=factor*matrix[i][k];
      }
    }
    for (unsigned i=0;i<3;++i) { xx-=bx[i]*matrix[i][3]; xy-=bx[i]*matrix[i][4]; yy-=by[i]*matrix[i][4]; }
    return true;
  }
  [[nodiscard]] Fit fit(bool y_axis) const noexcept {
    Fit best{};
    std::array<Fit,24> by_delay{};
    auto begin = observation_count_ - std::min<std::uint64_t>(observation_count_, observations_.size());
    const auto newest_at=observations_[(observation_count_-1U)%observations_.size()].at;
    // A bounded physical-time window, not "N frames at 140 Hz". At sparse
    // rates this retains enough independent bins without fitting seconds of
    // stale manual motion; at high rates the existing bounded ring still wins.
    while(begin+1U<observation_count_ && newest_at-observations_[begin%observations_.size()].at>1'200'000U) ++begin;
    std::array<std::uint64_t,64> intervals{};
    std::size_t interval_count{};
    for(auto i=begin+1U;i<observation_count_;++i)
      intervals[interval_count++]=observations_[i%observations_.size()].at-observations_[(i-1U)%observations_.size()].at;
    if (!interval_count) return best;
    std::sort(intervals.begin(),intervals.begin()+interval_count);
    const auto reference_interval=intervals[interval_count/2U];
    const auto first_at=observations_[begin%observations_.size()].at;
    const double window_seconds=double(newest_at-first_at)/1e6;
    if (!reference_interval || reference_interval>60'000U || window_seconds<=0) return best;
    const auto cmd_begin = command_count_ - std::min<std::uint64_t>(command_count_, commands_.size());
    bool had_excitation=false;
    for (std::uint64_t span : {1U, 2U, 4U}) for (std::uint64_t delay = 4'000; delay <= 96'000; delay += 4'000) {
      double sx{}, sy{}, sxx{}, sxy{}, syy{};
      double gram[3][3]{}, bx[3]{}, by[3]{};
      unsigned n{}, excited{};
      auto command_cursor = cmd_begin;
      for (auto i = begin + span; i < observation_count_; i += span) {
        const auto& prev = observations_[(i - span) % observations_.size()];
        const auto& last = observations_[i % observations_.size()];
        bool valid = last.at > prev.at;
        double displacement{};
        for (auto k = i - span + 1U; k <= i; ++k) {
          const auto& sample = observations_[k % observations_.size()];
          const auto before=observations_[(k-1U)%observations_.size()].at;
          valid &= sample.motion.valid && sample.motion.previous_observed_at_us == before &&
              sample.at>before && sample.at-before<=60'000U;
          displacement += y_axis ? sample.motion.y : sample.motion.x;
        }
        if (!valid || (command_count_>commands_.size() &&
            commands_[cmd_begin%commands_.size()].at+delay>prev.at)) continue;
        double counts{};
        // Consecutive bins do not overlap. Traverse each bounded command
        // history once per delay, rather than scanning it again for every bin.
        while (command_cursor < command_count_ &&
               commands_[command_cursor % commands_.size()].at + delay <= prev.at) ++command_cursor;
        while (command_cursor < command_count_) {
          const auto& command = commands_[command_cursor % commands_.size()];
          if (command.at + delay > last.at) break;
          counts += y_axis ? command.y : command.x;
          ++command_cursor;
        }
        // Normalize unequal durations to the actual median cadence. Using a
        // fixed 7143-us scale made the energy gate rate dependent at 20–50 ms.
        const double normalization = (double(reference_interval) * span) / double(last.at - prev.at);
        const double x = counts * normalization;
        const double y = displacement * normalization;
        sx += x; sy += y; sxx += x * x; sxy += x * y; syy += y * y; ++n;
        const double t = double(last.at-first_at)/1e6/window_seconds-.5;
        const double basis[]{1.0, t, t*t};
        for (unsigned j=0;j<3;++j) {
          bx[j]+=basis[j]*x; by[j]+=basis[j]*y;
          for (unsigned k=0;k<3;++k) gram[j][k]+=basis[j]*basis[k];
        }
        if (std::abs(counts) >= 2) ++excited;
      }
      if (n < 9 || excited < 5) continue;
      for (unsigned drift_order : {0U, 2U}) {
      double xx = sxx - sx * sx / n, xy = sxy - sx * sy / n, yy = syy - sy * sy / n;
      const double original_energy = xx;
      if(xx>=40) had_excitation=true;
      if (drift_order == 2U) {
        if (n < 12) continue;
        xx=sxx; xy=sxy; yy=syy;
        if (!remove_drift(gram,bx,by,xx,xy,yy) || xx < original_energy * .15) continue;
      }
      if (xx < 40 || yy < 10) continue;
      const double response = -xy / xx;
      const double quality = xy * xy / (xx * yy);
      if (response < .20 || response > 4.0 || quality < (drift_order ? .93 : .82)) continue;
      auto& candidate=by_delay[(delay-4'000U)/4'000U];
      if (!candidate.valid || quality > candidate.quality + .0001) {
        candidate = {float(response), float(quality), float(xx), delay, true};
      }
      if (!best.valid || quality > best.quality + .0001) {
        best = candidate;
      }
      }
    }
    if (!best.valid) {
      best.reason=had_excitation?LearningState::background_disagreement:LearningState::insufficient_excitation;
      return best;
    }
    for(unsigned index{};index<by_delay.size();++index) {
      const auto& candidate=by_delay[index];
      if(candidate.valid && candidate.quality>=best.quality-.005F &&
          std::abs(candidate.response-best.response)<=best.response*.20F)
        best.delay_mask|=1U<<index;
    }
    best.reason=LearningState::identified;
    return best;
  }
  void update_axis(Axis& axis, float& pending, unsigned& confirmations, const Fit& fit) noexcept {
    if (!fit.valid) { confirmations = 0; return; }
    if (confirmations == 0 || std::abs(fit.response - pending) > pending * .20F) {
      pending = fit.response; confirmations = 1; return;
    }
    pending = (pending + fit.response) * .5F;
    if (++confirmations < 2) return;
    if (!restored_reference_) axis.response = axis.trusted
        ? axis.response * .75F + std::clamp(pending, axis.response * .8F, axis.response * 1.2F) * .25F
        : pending;
    axis.quality = fit.quality; axis.trusted = true; ++updates_;
  }
  std::array<Command, 192> commands_{};
  std::array<Observation, 64> observations_{};
  std::array<std::uint64_t, 5> delay_history_{};
  std::size_t delay_count_{};
  bool delay_established_{};
  bool restored_reference_{};
  std::uint64_t command_count_{}, observation_count_{}, updates_{};
  Axis x_{}, y_{};
  float pending_x_{}, pending_y_{};
  unsigned confirmations_x_{}, confirmations_y_{};
  std::uint64_t delay_us_{80'000U};
  std::uint64_t restored_delay_us_{};
  DelayRange delay_range_{};
  LearningState learning_state_{LearningState::warming_up};
  std::uint64_t delay_updates_{};
};
}  // namespace vfdual_android
