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
  void accept(int x, int y, std::uint64_t at) noexcept {
    commands_[command_count_++ % commands_.size()] = {x, y, at};
  }
  void observe(const BackgroundMotion& motion, std::uint64_t at) noexcept {
    observations_[observation_count_++ % observations_.size()] = {motion, at};
    if (observation_count_ < 20 || observation_count_ % 4 != 0) return;
    auto fx = fit(false), fy = fit(true);
    // A saved same-device response is the reference. Correlated player input
    // can fit a convincing but incorrect scale during combat (observed Y=.27
    // after a .75 calibration). Do not let that fit redefine the actuator.
    if (restored_reference_) {
      if (fx.valid && std::abs(fx.response-x_.response)>x_.response*.20F) fx.valid=false;
      if (fy.valid && std::abs(fy.response-y_.response)>y_.response*.20F) fy.valid=false;
    }
    update_axis(x_, pending_x_, confirmations_x_, fx);
    update_axis(y_, pending_y_, confirmations_y_, fy);
    const auto& delay_fit = fx.valid && (!fy.valid || fx.energy >= fy.energy) ? fx : fy;
    if (delay_fit.valid && (x_.trusted || y_.trusted)) {
      delay_history_[delay_count_++ % delay_history_.size()] = delay_fit.delay_us + 4'000U;
      auto ordered = delay_history_;
      const auto count = std::min<std::size_t>(delay_count_, ordered.size());
      std::sort(ordered.begin(), ordered.begin() + count);
      // A single correlated manual-camera movement must not empty the pending
      // ledger early. Smooth the observed delay, including normal Bluetooth
      // paths whose measured response is longer than the old 36 ms search.
      const auto median = ordered[count / 2U];
      delay_us_ = delay_established_
          ? std::clamp(median, delay_us_ - std::min(delay_us_ - 8'000U, std::uint64_t{4'000}),
                       std::min<std::uint64_t>(100'000U, delay_us_ + 8'000U))
          : std::clamp<std::uint64_t>(median, 8'000U, 100'000U);
      delay_established_ = true;
    }
  }
  [[nodiscard]] const Axis& x() const noexcept { return x_; }
  [[nodiscard]] const Axis& y() const noexcept { return y_; }
  [[nodiscard]] std::uint64_t delay_us() const noexcept { return delay_us_; }
  [[nodiscard]] std::uint64_t updates() const noexcept { return updates_; }
  [[nodiscard]] bool restored_reference() const noexcept { return restored_reference_; }
  [[nodiscard]] std::uint64_t restored_delay_us() const noexcept { return restored_delay_us_; }
  [[nodiscard]] bool restore(float x, float y, std::uint64_t delay, float quality_x, float quality_y) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || x<.2F || x>4.F || y<.2F || y>4.F ||
        delay<8'000 || delay>100'000 || !std::isfinite(quality_x) || !std::isfinite(quality_y) ||
        quality_x<.82F || quality_x>1.F || quality_y<.82F || quality_y>1.F) return false;
    clear_history(); x_={x,quality_x,true}; y_={y,quality_y,true};
    delay_us_=delay; restored_delay_us_=delay; delay_history_.fill(delay); delay_count_=delay_history_.size();
    delay_established_=true; restored_reference_=true; updates_=0; return true;
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
      const auto earliest=restored_reference_?std::min(delay_us_,restored_delay_us_):delay_us_;
      const auto latest=restored_reference_?std::max(delay_us_,restored_delay_us_):delay_us_;
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
      // Every plausible response delay agrees on a near-zero relative trend.
      // This includes a player already following the person: background/world
      // motion is not a reason to inject an extra mouse movement. Two samples
      // suffice for this narrowly bounded zero check, but not a velocity lead.
      if (minimum>=-20 && maximum<=20) {
        motion.valid=true; evidence.branch=DisturbanceBranch::nearzero; return motion;
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
    return result;
  }
  void clear_history() noexcept {
    command_count_ = observation_count_ = 0;
    confirmations_x_ = confirmations_y_ = 0;
    delay_count_ = 0;
  }
 private:
  struct Command { int x{}, y{}; std::uint64_t at{}; };
  struct Observation { BackgroundMotion motion{}; std::uint64_t at{}; };
  struct Fit { float response{}, quality{}, energy{}; std::uint64_t delay_us{}; bool valid{}; };
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
    const auto available = std::min<std::uint64_t>(observation_count_, observations_.size());
    const auto begin = observation_count_ - available;
    const auto cmd_begin = command_count_ - std::min<std::uint64_t>(command_count_, commands_.size());
    for (std::uint64_t span : {2U, 4U}) for (std::uint64_t delay = 4'000; delay <= 96'000; delay += 4'000) {
      double sx{}, sy{}, sxx{}, sxy{}, syy{};
      double gram[3][3]{}, bx[3]{}, by[3]{};
      unsigned n{}, excited{};
      auto command_cursor = cmd_begin;
      for (auto i = begin + span; i < observation_count_; i += span) {
        const auto& prev = observations_[(i - span) % observations_.size()];
        const auto& last = observations_[i % observations_.size()];
        bool valid = last.at > prev.at && last.at - prev.at <= span * 18'000U;
        double displacement{};
        for (auto k = i - span + 1U; k <= i; ++k) {
          const auto& sample = observations_[k % observations_.size()];
          valid &= sample.motion.valid && sample.motion.previous_observed_at_us == observations_[(k - 1U) % observations_.size()].at;
          displacement += y_axis ? sample.motion.y : sample.motion.x;
        }
        if (!valid) continue;
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
        const double normalization = (7'143.0 * span) / double(last.at - prev.at);
        const double x = counts * normalization;
        const double y = displacement * normalization;
        sx += x; sy += y; sxx += x * x; sxy += x * y; syy += y * y; ++n;
        const double t = (double(i - begin) - double(available) * .5) / double(available);
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
      if (drift_order == 2U) {
        if (n < 12) continue;
        xx=sxx; xy=sxy; yy=syy;
        if (!remove_drift(gram,bx,by,xx,xy,yy) || xx < original_energy * .15) continue;
      }
      if (xx < 40 || yy < 10) continue;
      const double response = -xy / xx;
      const double quality = xy * xy / (xx * yy);
      if (response < .20 || response > 4.0 || quality < (drift_order ? .93 : .82)) continue;
      if (!best.valid || quality > best.quality + .0001) {
        best = {float(response), float(quality), float(xx), delay, true};
      }
      }
    }
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
};
}  // namespace vfdual_android
