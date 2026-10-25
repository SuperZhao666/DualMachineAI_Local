#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace vfdual_android {

// Serial observation gate. No image math, targets, boxes, hardware,
// clocks, allocations or ownership of producer buffers live in this class.
class ProjectionObservationGate final {
 public:
  static constexpr std::uint64_t kMaximumAgeUs = 50'000;
  static constexpr std::size_t kMaximumSourceTargets = 300;

  struct Frame {
    std::uint64_t generation{}, sequence{}, wire_frame_id{}, observed_us{};
    friend bool operator==(const Frame&, const Frame&) = default;
  };
  struct Source {
    Frame frame{};
    std::uint64_t independent_content_id{};
    std::size_t detection_count{};
    // Original immutable source-A deadline, not now+50ms or target-B+50ms.
    std::uint64_t valid_until_us{};
    friend bool operator==(const Source&, const Source&) = default;
  };
  struct Pixels {
    Frame frame{};
    std::uint64_t independent_content_id{};
    bool content_updated{}, independent_acquisition{};
  };
  struct TargetEvidence {
    std::size_t source_index{};
    bool valid{};
    // Projector keeps0on failure. A valid result must name the real B time.
    std::uint64_t pixel_observed_us{};
  };
  struct BackgroundHint {
    bool used{};
    std::uint64_t generation{}, source_observed_us{}, target_observed_us{};
  };
  struct ProjectionEvidence {
    Frame source{}, target{};
    std::size_t source_count{}, valid_count{};
    bool complete{}, work_budget_exhausted{};
    std::span<const TargetEvidence> targets{};
    BackgroundHint background{};
  };
  enum class Reason {
    ready, no_stream, user_disabled, no_source, invalid_source,
    wrong_generation, source_mismatch, target_mismatch, empty_source,
    source_expired, future_observation, target_expired, not_newer_than_source,
    would_rewind_or_repeat, non_monotonic_sequence, repeated_pixels,
    incomplete_projection, duplicate_source_index, invalid_target_evidence,
    misaligned_background, invalid_ticket
  };
  enum class CacheResult { updated, unchanged, older_ignored, invalid, wrong_generation, conflicting_same_frame };
  enum class Kind { skip, projected_targets, detector_targets, detector_empty_observation };
  struct Proposal {
    Reason reason{Reason::no_source};
    Kind kind{Kind::skip};
    std::uint64_t ticket{};
    Frame source{}, control{};
    std::uint64_t previous_control_observed_us{}, source_deadline_us{};
    std::size_t source_detection_count{};
    bool may_emit_target{};
    [[nodiscard]] bool ready() const noexcept { return reason == Reason::ready; }
  };
  struct Publication {
    Reason reason{Reason::invalid_ticket};
    Proposal observation{};
    [[nodiscard]] bool published() const noexcept { return reason == Reason::ready; }
  };

  // A stream epoch must advance. Retrying restart with the same generation
  // cannot erase L and republish already-consumed pixels. Restart disables;
  // caller explicitly restores the user's request after stream readiness.
  bool restart(std::uint64_t generation) noexcept {
    if (!generation || generation <= generation_) return false;
    generation_ = generation;
    enabled_ = false;
    source_.reset();
    source_frontier_ = {};
    pending_.reset();
    last_published_ = {};
    last_content_id_ = 0;
    return true;
  }
  void set_user_enabled(bool enabled) noexcept {
    if (enabled_ != enabled) pending_.reset();
    enabled_ = enabled;
  }

  // NNP cache progresses independently of published control time L. A newer
  // detection A may be <= L because L came from pixels B. Cache it; never rewind L.
  CacheResult cache_source(const Source& next) noexcept {
    if (!generation_ || next.frame.generation != generation_) return CacheResult::wrong_generation;
    if (!valid_frame(next.frame) || !next.independent_content_id ||
        next.detection_count > kMaximumSourceTargets ||
        next.frame.observed_us > std::numeric_limits<std::uint64_t>::max()-kMaximumAgeUs ||
        next.valid_until_us <= next.frame.observed_us ||
        next.valid_until_us > next.frame.observed_us+kMaximumAgeUs) return CacheResult::invalid;
    if (source_frontier_.sequence) {
      if (next.frame.sequence < source_frontier_.sequence || next.frame.observed_us < source_frontier_.observed_us)
        return CacheResult::older_ignored;
      if (next.frame.sequence == source_frontier_.sequence || next.frame.observed_us == source_frontier_.observed_us) {
        if (source_ && next == *source_) return CacheResult::unchanged;
        // Conflicting facts for one source frame cannot refresh its deadline.
        // A genuinely newer source must restore this invalidated cache.
        pending_.reset();
        source_.reset();
        return CacheResult::conflicting_same_frame;
      }
    }
    source_ = next;
    source_frontier_ = next.frame;
    pending_.reset();
    return CacheResult::updated;
  }

  Proposal prepare_projection(const Pixels& pixels, const ProjectionEvidence& evidence,
                              std::uint64_t now_us) noexcept {
    pending_.reset();
    if (const auto reason = common(now_us); reason != Reason::ready) return rejected(reason);
    const auto& source = *source_;
    if (source.detection_count == 0) return rejected(Reason::empty_source);
    if (pixels.frame.generation != generation_ || evidence.source.generation != generation_ ||
        evidence.target.generation != generation_) return rejected(Reason::wrong_generation);
    if (evidence.source != source.frame || evidence.source_count != source.detection_count)
      return rejected(Reason::source_mismatch);
    if (!valid_frame(pixels.frame) || evidence.target != pixels.frame) return rejected(Reason::target_mismatch);
    if (pixels.frame.observed_us <= source.frame.observed_us || pixels.frame.sequence <= source.frame.sequence)
      return rejected(Reason::not_newer_than_source);
    if (pixels.frame.observed_us <= last_published_.observed_us) return rejected(Reason::would_rewind_or_repeat);
    if (pixels.frame.sequence <= last_published_.sequence) return rejected(Reason::non_monotonic_sequence);
    if (pixels.frame.observed_us > now_us) return rejected(Reason::future_observation);
    if (now_us-pixels.frame.observed_us >= kMaximumAgeUs) return rejected(Reason::target_expired);
    if (!pixels.content_updated || !pixels.independent_acquisition || !pixels.independent_content_id ||
        pixels.independent_content_id == source.independent_content_id ||
        pixels.independent_content_id == last_content_id_) return rejected(Reason::repeated_pixels);
    if (!evidence.complete || evidence.work_budget_exhausted ||
        evidence.valid_count != source.detection_count || evidence.targets.size() != source.detection_count)
      return rejected(Reason::incomplete_projection);
    std::array<bool, kMaximumSourceTargets> seen{};
    for (const auto& target : evidence.targets) {
      if (target.source_index >= source.detection_count || !target.valid ||
          target.pixel_observed_us != pixels.frame.observed_us) return rejected(Reason::invalid_target_evidence);
      if (seen[target.source_index]) return rejected(Reason::duplicate_source_index);
      seen[target.source_index] = true;
    }
    if (evidence.background.used && (evidence.background.generation != generation_ ||
        evidence.background.source_observed_us != source.frame.observed_us ||
        evidence.background.target_observed_us != pixels.frame.observed_us))
      return rejected(Reason::misaligned_background);
    return issue(Kind::projected_targets, pixels.frame, pixels.independent_content_id);
  }

  // After failed projection, source-A is a fallback only if A>L. A genuine
  // empty NNP frame can publish negative evidence, never a synthetic target.
  Proposal prepare_detector_fallback(std::uint64_t now_us) noexcept {
    pending_.reset();
    if (const auto reason = common(now_us); reason != Reason::ready) return rejected(reason);
    if (source_->frame.observed_us <= last_published_.observed_us) return rejected(Reason::would_rewind_or_repeat);
    if (source_->frame.sequence <= last_published_.sequence) return rejected(Reason::non_monotonic_sequence);
    if (source_->independent_content_id == last_content_id_) return rejected(Reason::repeated_pixels);
    return issue(source_->detection_count ? Kind::detector_targets : Kind::detector_empty_observation,
                 source_->frame, source_->independent_content_id);
  }

  // Optional pre-consumption commit API for a fully serialized caller. Once
  // Core has already consumed an observation, use record_consumed_observation:
  // later physical expiry cannot undo that fact. This is not a HID receipt.
  // No state advances
  // when a proposal merely exists, fails, expires or is disabled.
  Publication publish(std::uint64_t ticket, std::uint64_t now_us) noexcept {
    if (!pending_ || !ticket || ticket != pending_->proposal.ticket) return {};
    const auto pending = *pending_;
    pending_.reset();
    if (const auto reason = common(now_us); reason != Reason::ready) return {reason, {}};
    if (pending.proposal.source != source_->frame || pending.proposal.source_deadline_us != source_->valid_until_us)
      return {Reason::source_mismatch, {}};
    const auto& control = pending.proposal.control;
    if (control.generation != generation_) return {Reason::wrong_generation, {}};
    if (control.observed_us > now_us) return {Reason::future_observation, {}};
    if (control.observed_us <= last_published_.observed_us) return {Reason::would_rewind_or_repeat, {}};
    if (control.sequence <= last_published_.sequence) return {Reason::non_monotonic_sequence, {}};
    if (now_us-control.observed_us >= kMaximumAgeUs) return {Reason::target_expired, {}};
    last_published_ = control;
    last_content_id_ = pending.content_id;
    return {Reason::ready, pending.proposal};
  }

  // Observe an already-consumed legacy A without authorizing physical output.
  // Caller proves the Bridge accepted the generation/observation transaction.
  // User-disabled legacy tracking remains legal; projection proposals stay closed.
  bool record_consumed_observation(const Frame& frame, std::uint64_t content_id,
                                   bool observation_consumed) noexcept {
    if (!observation_consumed || !valid_frame(frame) || frame.generation != generation_ ||
        !content_id || frame.observed_us <= last_published_.observed_us ||
        frame.sequence <= last_published_.sequence) return false;
    pending_.reset();
    last_published_ = frame;
    last_content_id_ = content_id;
    return true;
  }
  bool record_legacy_observation(const Frame& frame, std::uint64_t content_id,
                                 bool observation_consumed) noexcept {
    return record_consumed_observation(frame, content_id, observation_consumed);
  }

  [[nodiscard]] Frame last_published_control() const noexcept { return last_published_; }
  [[nodiscard]] const std::optional<Source>& cached_source() const noexcept { return source_; }

 private:
  struct Pending { Proposal proposal{}; std::uint64_t content_id{}; };
  static bool valid_frame(const Frame& frame) noexcept {
    return frame.generation && frame.sequence && frame.wire_frame_id && frame.observed_us;
  }
  Reason common(std::uint64_t now) const noexcept {
    if (!generation_) return Reason::no_stream;
    if (!enabled_) return Reason::user_disabled;
    if (!source_) return Reason::no_source;
    if (source_->frame.generation != generation_) return Reason::wrong_generation;
    if (now < source_->frame.observed_us) return Reason::future_observation;
    if (now >= source_->valid_until_us || now-source_->frame.observed_us >= kMaximumAgeUs)
      return Reason::source_expired;
    return Reason::ready;
  }
  static Proposal rejected(Reason reason) noexcept { Proposal out; out.reason = reason; return out; }
  Proposal issue(Kind kind, const Frame& control, std::uint64_t content_id) noexcept {
    Proposal proposal{Reason::ready, kind, ++ticket_sequence_, source_->frame, control,
                     last_published_.observed_us, source_->valid_until_us, source_->detection_count,
                     kind != Kind::detector_empty_observation};
    pending_ = Pending{proposal, content_id};
    return proposal;
  }
  std::uint64_t generation_{}, ticket_sequence_{}, last_content_id_{};
  bool enabled_{};
  std::optional<Source> source_{};
  std::optional<Pending> pending_{};
  Frame last_published_{};
  Frame source_frontier_{};
};
}  // namespace vfdual_android
