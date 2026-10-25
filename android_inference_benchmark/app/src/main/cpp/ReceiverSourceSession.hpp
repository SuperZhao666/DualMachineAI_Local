#pragma once

#include <cstdint>
#include <optional>

namespace vfdual_android {

enum class ReceiverSourceDecision : std::uint8_t {
  accept,
  restart_source_port,
  reject_foreign,
  restart_source_sequence
};

/** Prevents fragments from concurrent host processes entering one H.264 decoder session. */
class ReceiverSourceSession final {
public:
  explicit ReceiverSourceSession(std::uint32_t expected_ipv4) noexcept;
  [[nodiscard]] ReceiverSourceDecision observe(std::uint32_t ipv4, std::uint16_t port,
                                               std::uint64_t now_us,
                                               std::optional<std::uint32_t> frame_id = std::nullopt) noexcept;
  void reset(std::uint32_t expected_ipv4) noexcept;

private:
  [[nodiscard]] bool observe_sequence(std::uint32_t frame_id, std::uint64_t now_us) noexcept;
  void clear_sequence_history() noexcept;
  std::uint32_t expected_ipv4_{};
  std::uint32_t ipv4_{};
  std::uint16_t port_{};
  std::uint64_t last_accepted_us_{};
  bool active_{};
  std::optional<std::uint32_t> latest_sequence_;
  std::optional<std::uint32_t> rewind_sequence_;
  std::uint64_t last_sequence_progress_us_{};
  std::uint64_t rewind_started_us_{};
  std::uint32_t rewind_progress_frames_{};
};

}  // namespace vfdual_android
