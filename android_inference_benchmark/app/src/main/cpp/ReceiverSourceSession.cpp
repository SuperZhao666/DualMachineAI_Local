#include "ReceiverSourceSession.hpp"
#include "vfdual/protocol.hpp"

namespace vfdual_android {
namespace {
constexpr std::uint64_t kSourcePortRestartSilenceUs = 300'000;
constexpr std::uint64_t kSequenceRestartConfirmationUs = 50'000;
constexpr std::uint32_t kSequenceRestartConfirmationFrames = 4;
constexpr std::uint32_t kMaximumCandidateFrameGap = 128;
}

ReceiverSourceSession::ReceiverSourceSession(std::uint32_t expected_ipv4) noexcept
    : expected_ipv4_(expected_ipv4) {}

ReceiverSourceDecision ReceiverSourceSession::observe(std::uint32_t ipv4, std::uint16_t port,
                                                      std::uint64_t now_us,
                                                      std::optional<std::uint32_t> frame_id) noexcept {
  if (expected_ipv4_ == 0U || ipv4 != expected_ipv4_) {
    return ReceiverSourceDecision::reject_foreign;
  }
  if (!active_) {
    ipv4_ = ipv4;
    port_ = port;
    last_accepted_us_ = now_us;
    active_ = true;
    if (frame_id) (void)observe_sequence(*frame_id, now_us);
    return ReceiverSourceDecision::accept;
  }
  if (ipv4 == ipv4_ && port == port_) {
    last_accepted_us_ = now_us;
    if (frame_id && observe_sequence(*frame_id, now_us)) {
      return ReceiverSourceDecision::restart_source_sequence;
    }
    return ReceiverSourceDecision::accept;
  }
  if (now_us >= last_accepted_us_ &&
      now_us - last_accepted_us_ >= kSourcePortRestartSilenceUs && ipv4 == ipv4_) {
    port_ = port;
    last_accepted_us_ = now_us;
    clear_sequence_history();
    if (frame_id) (void)observe_sequence(*frame_id, now_us);
    return ReceiverSourceDecision::restart_source_port;
  }
  return ReceiverSourceDecision::reject_foreign;
}

bool ReceiverSourceSession::observe_sequence(std::uint32_t frame_id,
                                             std::uint64_t now_us) noexcept {
  const auto sequence = vfdual::video_logical_frame_sequence(frame_id);
  if (!latest_sequence_ || vfdual::video_logical_frame_sequence_is_newer(
          sequence, *latest_sequence_)) {
    latest_sequence_ = sequence;
    last_sequence_progress_us_ = now_us;
    rewind_sequence_.reset();
    rewind_progress_frames_ = 0;
    return false;
  }
  if (sequence == *latest_sequence_ || now_us < last_sequence_progress_us_) {
    rewind_sequence_.reset();
    rewind_progress_frames_ = 0;
    return false;
  }
  // Host always binds port 5002, including after a process/stream restart.
  // A single old packet is not a new session: require several distinct lower
  // frame numbers progressing over time while the former stream has stopped
  // progressing. Fragment duplicates and repeated-content flags do not count.
  if (!rewind_sequence_) {
    rewind_sequence_ = sequence;
    rewind_started_us_ = now_us;
    rewind_progress_frames_ = 1;
    return false;
  }
  if (!vfdual::video_logical_frame_sequence_is_newer(sequence, *rewind_sequence_)) {
    return false;
  }
  const auto gap = (sequence - *rewind_sequence_) & vfdual::kVideoFrameSequenceMask;
  rewind_sequence_ = sequence;
  if (gap > kMaximumCandidateFrameGap || now_us < rewind_started_us_) {
    rewind_started_us_ = now_us;
    rewind_progress_frames_ = 1;
    return false;
  }
  if (rewind_progress_frames_ < kSequenceRestartConfirmationFrames) ++rewind_progress_frames_;
  if (now_us - last_sequence_progress_us_ < kSourcePortRestartSilenceUs ||
      now_us - rewind_started_us_ < kSequenceRestartConfirmationUs ||
      rewind_progress_frames_ < kSequenceRestartConfirmationFrames) return false;
  latest_sequence_ = sequence;
  last_sequence_progress_us_ = now_us;
  rewind_sequence_.reset();
  rewind_progress_frames_ = 0;
  return true;
}

void ReceiverSourceSession::clear_sequence_history() noexcept {
  latest_sequence_.reset();
  rewind_sequence_.reset();
  last_sequence_progress_us_ = rewind_started_us_ = 0;
  rewind_progress_frames_ = 0;
}

void ReceiverSourceSession::reset(std::uint32_t expected_ipv4) noexcept {
  expected_ipv4_ = expected_ipv4;
  ipv4_ = 0U;
  port_ = 0U;
  last_accepted_us_ = 0U;
  active_ = false;
  clear_sequence_history();
}

}  // namespace vfdual_android
