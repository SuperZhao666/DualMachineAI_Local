#include "MakcuMoveBridge.hpp"

#include "ControlBlockerState.hpp"
#include "MobileControlCore.hpp"
#include "MobileControlDeadline.hpp"
#include "MakcuOutputGate.hpp"
#include "DiagnosticTraceCursor.hpp"
#include "DetectionTrace.hpp"
#include "ControlSpaceTransform.hpp"
#include "vfdual/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <memory>
#include <sstream>

namespace vfdual_android {
namespace {
constexpr float kMinimumRelativeGain = 0.01F;
constexpr float kMaximumRelativeGain = 0.95F;
constexpr float kMaximumDeadzonePixels = 64.0F;
constexpr std::int32_t kMinimumAxisDelta = 1;
constexpr std::int32_t kMaximumAxisDelta = 127;
constexpr std::uint32_t kMinimumSwitchConfirmationMs = 10U;
constexpr std::uint32_t kMaximumSwitchConfirmationMs = 100U;
// Java delivery owns a bounded 25 ms USB write and 50 ms exact-response wait.
// Include executor/JNI/USB scheduling headroom while keeping a short,
// fail-closed end-to-end command lifetime. The remaining budget is forwarded
// to Java so both layers enforce the same terminal deadline.
constexpr std::uint64_t kMaximumMoveCompletionAgeUs = 250'000U;
// Receiver fresh-content recovery requests an IDR after 150 ms and backs off
// from 500 ms. Two seconds admits bounded recovery without allowing an ACK
// barrier to remain armed for the lifetime of the process.
constexpr std::uint64_t kMaximumPostAckVisibilityAgeUs = 2'000'000U;

std::mutex g_bridge_mutex;
std::mutex g_control_mutex;
std::mutex g_publish_mutex;
JavaVM* g_vm{};
jclass g_controller_class{};
jmethodID g_offer_move{};
jmethodID g_suspend_delivery{};
jmethodID g_resume_delivery{};
jmethodID g_fail_closed_delivery{};
MobileControlCore g_control_core{screen_pixel_control_config()};
struct DispatchDiagnostic {
  std::uint64_t id{}, sequence{}, ticket{}, dispatched_us{}, returned_us{}, budget_us{};
  std::int32_t dx{}, dy{};
  unsigned outcome{}; // 1 accepted, 2 disabled, 3 expired, 4 JNI unavailable, 5 rejected.
  std::uint64_t control_trace_id{};
};
std::array<DispatchDiagnostic, 1024> g_dispatch_trace{};
std::uint64_t g_dispatch_trace_count{};
DiagnosticTraceCursor g_control_trace_cursor{}, g_dispatch_trace_cursor{};
DetectionTrace g_detection_trace{};
DiagnosticTraceCursor g_detection_trace_cursor{};
void record_dispatch(std::uint64_t sequence, std::uint64_t ticket, std::uint64_t at,
    std::uint64_t returned, std::uint64_t budget, const MobileControlOutput& output, unsigned outcome) {
  std::scoped_lock lock(g_control_mutex);
  const auto id = ++g_dispatch_trace_count;
  g_dispatch_trace[(id - 1) % g_dispatch_trace.size()] =
      {id, sequence, ticket, at, returned, budget, output.delta_x, output.delta_y, outcome,
       g_control_core.control_trace().count};
}
std::array<double,7> g_saved_calibration{};
std::string g_calibration_scope;
bool g_preserve_current_calibration{};
std::atomic_uint64_t g_processed_frames{};
std::atomic_uint64_t g_candidates{};
MakcuStreamGenerationGate g_stream_generation_gate{};
std::atomic_uint64_t g_stale_stream_generation_suppressed{};
std::atomic_uint64_t g_delivery_fail_close_requests{};
std::atomic_uint64_t g_stream_lifecycle_close_requests{};
std::atomic_uint64_t g_output_disabled_suppressed{};
std::atomic_uint64_t g_java_offer_acceptances{};
std::atomic_uint64_t g_java_offer_rejections{};
std::atomic_uint64_t g_deadzone_suppressed{};
std::atomic_uint64_t g_held_targets{};
std::atomic_uint64_t g_target_switches{};
std::atomic_uint64_t g_no_targets{};
std::atomic_uint64_t g_java_dispatch_failures{};
std::atomic_uint64_t g_move_completion_pending_suppressed{};
std::atomic_uint64_t g_device_ack_completions{};
std::atomic_uint64_t g_device_ack_failures{};
std::atomic_uint64_t g_move_completion_timeouts{};
std::atomic_uint64_t g_pre_send_discards{};
std::atomic_uint64_t g_stale_device_feedback{};
std::atomic_uint64_t g_last_device_ack_us{};
std::atomic_uint64_t g_last_device_ack_at_us{};
std::atomic_uint64_t g_last_device_ack_interval_us{};
std::atomic_uint64_t g_bluetooth_hid_api_acceptances{};
std::atomic_uint64_t g_stale_bluetooth_hid_api_acceptances{};
std::atomic_uint64_t g_last_bluetooth_hid_api_acceptance_us{};
std::atomic_uint64_t g_post_completion_visibility_suppressed{};
std::atomic_uint64_t g_post_completion_visibility_timeouts{};
std::atomic_uint64_t g_no_move_suppressed{};
std::atomic_uint64_t g_response_guard_suppressed{};
std::atomic_uint64_t g_device_resolution_suppressed{};
std::atomic_uint64_t g_settle_guard_suppressed{};
std::atomic_uint64_t g_direction_flip_suppressed{};
std::atomic_uint64_t g_motion_invalid_suppressed{};
std::atomic_uint64_t g_detected_not_control_eligible_suppressed{};
std::atomic_uint64_t g_last_detection_count{};
std::atomic<float> g_last_max_detection_confidence{};
std::atomic<std::int32_t> g_last_offered_delta_x{};
std::atomic<std::int32_t> g_last_offered_delta_y{};
std::atomic<std::int32_t> g_last_nonzero_offered_direction_x{};
std::atomic<std::int32_t> g_last_nonzero_offered_direction_y{};
std::atomic_uint64_t g_offered_absolute_x_counts{};
std::atomic_uint64_t g_offered_absolute_y_counts{};
std::atomic_uint64_t g_maximum_offered_absolute_axis_delta{};
std::atomic_uint64_t g_offered_direction_flips_x{};
std::atomic_uint64_t g_offered_direction_flips_y{};
std::atomic_uint64_t g_last_offer_at_us{};
std::atomic_uint64_t g_last_offer_interval_us{};
std::atomic<float> g_relative_gain{0.36F};
std::atomic<float> g_deadzone_pixels{0.5F};
std::atomic<std::int32_t> g_maximum_axis_delta{kMaximumAxisDelta};
std::atomic_bool g_java_bridge_ready{};
MakcuOutputGate g_output_gate{};
MakcuMoveCommitGate g_move_commit_gate{};
MakcuMoveVisibilityGate g_move_visibility_gate{};
ControlBlockerState g_control_blocker{};

enum class JavaDeliveryCall {
  suspend,
  resume,
  fail_closed,
};

std::uint64_t monotonic_microseconds() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

void update_control_blocker(
    ControlBlockerReason reason, std::uint64_t now_us = 0U) noexcept {
  g_control_blocker.update(
      reason, now_us == 0U ? monotonic_microseconds() : now_us);
}

ControlBlockerReason blocker_for_control_output(
    const MobileControlOutput& output) noexcept {
  switch (output.suppression_reason) {
    case ControlSuppressionReason::none:
      return output.has_move()
          ? ControlBlockerReason::runnable
          : ControlBlockerReason::response_guard;
    case ControlSuppressionReason::invalid_time:
    case ControlSuppressionReason::stale_frame:
    case ControlSuppressionReason::non_monotonic_frame:
    case ControlSuppressionReason::motion_invalid:
      return ControlBlockerReason::motion_invalid;
    case ControlSuppressionReason::no_valid_target:
      return ControlBlockerReason::no_valid_target;
    case ControlSuppressionReason::detected_not_control_eligible:
      return ControlBlockerReason::detected_not_control_eligible;
    case ControlSuppressionReason::lock_held:
      return ControlBlockerReason::target_held;
    case ControlSuppressionReason::switch_pending:
      return ControlBlockerReason::switch_pending;
    case ControlSuppressionReason::deadzone:
      return ControlBlockerReason::deadzone;
    case ControlSuppressionReason::settle_guard:
      return ControlBlockerReason::settle_guard;
    case ControlSuppressionReason::response_guard:
      return ControlBlockerReason::response_guard;
    case ControlSuppressionReason::device_resolution_guard:
      return ControlBlockerReason::device_resolution_guard;
    case ControlSuppressionReason::direction_flip:
      return ControlBlockerReason::direction_flip;
  }
  return ControlBlockerReason::motion_invalid;
}

void observe_no_move(
    const MobileControlOutput& output, std::uint64_t now_us) noexcept {
  ++g_no_move_suppressed;
  switch (output.suppression_reason) {
    case ControlSuppressionReason::deadzone:
      ++g_deadzone_suppressed;
      break;
    case ControlSuppressionReason::response_guard:
      ++g_response_guard_suppressed;
      break;
    case ControlSuppressionReason::device_resolution_guard:
      ++g_device_resolution_suppressed;
      break;
    case ControlSuppressionReason::settle_guard:
      ++g_settle_guard_suppressed;
      break;
    case ControlSuppressionReason::direction_flip:
      ++g_direction_flip_suppressed;
      break;
    case ControlSuppressionReason::invalid_time:
    case ControlSuppressionReason::stale_frame:
    case ControlSuppressionReason::non_monotonic_frame:
    case ControlSuppressionReason::motion_invalid:
      ++g_motion_invalid_suppressed;
      break;
    case ControlSuppressionReason::none:
    case ControlSuppressionReason::no_valid_target:
    case ControlSuppressionReason::lock_held:
    case ControlSuppressionReason::switch_pending:
      break;
    case ControlSuppressionReason::detected_not_control_eligible:
      ++g_detected_not_control_eligible_suppressed;
      break;
  }
  update_control_blocker(blocker_for_control_output(output), now_us);
}

std::uint64_t absolute_counts(std::int32_t delta) noexcept {
  const std::int64_t widened = delta;
  return static_cast<std::uint64_t>(widened < 0 ? -widened : widened);
}

void update_maximum(
    std::atomic_uint64_t& destination, std::uint64_t candidate) noexcept {
  std::uint64_t current = destination.load(std::memory_order_relaxed);
  while (current < candidate &&
         !destination.compare_exchange_weak(
             current, candidate, std::memory_order_relaxed)) {
  }
}

void observe_interval(
    std::uint64_t observed_at_us, std::atomic_uint64_t& previous_at_us,
    std::atomic_uint64_t& latest_interval_us) noexcept {
  const std::uint64_t previous =
      previous_at_us.exchange(observed_at_us, std::memory_order_relaxed);
  if (previous != 0U && observed_at_us >= previous) {
    latest_interval_us.store(
        observed_at_us - previous, std::memory_order_relaxed);
  }
}

void observe_axis_direction(
    std::int32_t delta, std::atomic<std::int32_t>& previous_direction,
    std::atomic_uint64_t& direction_flips) noexcept {
  const std::int32_t direction = delta > 0 ? 1 : delta < 0 ? -1 : 0;
  if (direction == 0) return;
  const std::int32_t previous =
      previous_direction.exchange(direction, std::memory_order_relaxed);
  if (previous != 0 && previous != direction) {
    direction_flips.fetch_add(1U, std::memory_order_relaxed);
  }
}

void observe_accepted_move(std::int32_t delta_x, std::int32_t delta_y) noexcept {
  const std::uint64_t absolute_x = absolute_counts(delta_x);
  const std::uint64_t absolute_y = absolute_counts(delta_y);
  g_last_offered_delta_x.store(delta_x, std::memory_order_relaxed);
  g_last_offered_delta_y.store(delta_y, std::memory_order_relaxed);
  g_offered_absolute_x_counts.fetch_add(absolute_x, std::memory_order_relaxed);
  g_offered_absolute_y_counts.fetch_add(absolute_y, std::memory_order_relaxed);
  update_maximum(
      g_maximum_offered_absolute_axis_delta,
      std::max(absolute_x, absolute_y));
  observe_axis_direction(
      delta_x, g_last_nonzero_offered_direction_x,
      g_offered_direction_flips_x);
  observe_axis_direction(
      delta_y, g_last_nonzero_offered_direction_y,
      g_offered_direction_flips_y);
  observe_interval(
      monotonic_microseconds(), g_last_offer_at_us,
      g_last_offer_interval_us);
}

void observe_matching_device_ack() noexcept {
  observe_interval(
      monotonic_microseconds(), g_last_device_ack_at_us,
      g_last_device_ack_interval_us);
}

std::uint64_t remaining_move_budget_us(
    std::uint64_t started_at_us, std::uint64_t now_us) noexcept {
  if (started_at_us == 0U || now_us < started_at_us) {
    return kMaximumMoveCompletionAgeUs;
  }
  const std::uint64_t elapsed = now_us - started_at_us;
  return elapsed >= kMaximumMoveCompletionAgeUs
      ? 0U
      : kMaximumMoveCompletionAgeUs - elapsed;
}

void reset_move_telemetry_continuity() noexcept {
  g_last_offered_delta_x.store(0, std::memory_order_relaxed);
  g_last_offered_delta_y.store(0, std::memory_order_relaxed);
  g_last_nonzero_offered_direction_x.store(0, std::memory_order_relaxed);
  g_last_nonzero_offered_direction_y.store(0, std::memory_order_relaxed);
  g_last_offer_at_us.store(0U, std::memory_order_relaxed);
  g_last_offer_interval_us.store(0U, std::memory_order_relaxed);
  g_last_device_ack_at_us.store(0U, std::memory_order_relaxed);
  g_last_device_ack_interval_us.store(0U, std::memory_order_relaxed);
}

void clear_jni_exception(JNIEnv* environment) noexcept {
  if (environment != nullptr && environment->ExceptionCheck()) {
    environment->ExceptionDescribe();
    environment->ExceptionClear();
  }
}

jint attach_bridge_thread(JavaVM* vm,JNIEnv** environment) noexcept {
#if defined(_WIN32)
  return vm->AttachCurrentThread(reinterpret_cast<void**>(environment),nullptr);
#else
  return vm->AttachCurrentThread(environment,nullptr);
#endif
}

void clear_binding_locked(JNIEnv* environment) noexcept {
  if (g_controller_class != nullptr && environment != nullptr) {
    environment->DeleteGlobalRef(g_controller_class);
  }
  g_vm = nullptr;
  g_controller_class = nullptr;
  g_offer_move = nullptr;
  g_suspend_delivery = nullptr;
  g_resume_delivery = nullptr;
  g_fail_closed_delivery = nullptr;
  g_java_bridge_ready.store(false, std::memory_order_release);
}

jmethodID delivery_method_locked(JavaDeliveryCall call) noexcept {
  switch (call) {
    case JavaDeliveryCall::suspend: return g_suspend_delivery;
    case JavaDeliveryCall::resume: return g_resume_delivery;
    case JavaDeliveryCall::fail_closed: return g_fail_closed_delivery;
  }
  return nullptr;
}

bool invoke_java_delivery_gate(JavaDeliveryCall call, std::uint64_t generation = 0U) noexcept {
  JavaVM* vm{};
  {
    std::scoped_lock lock(g_bridge_mutex);
    if (!g_java_bridge_ready.load(std::memory_order_acquire) || g_vm == nullptr) return false;
    vm = g_vm;
  }

  JNIEnv* environment{};
  bool attached = false;
  const jint environment_status = vm->GetEnv(
      reinterpret_cast<void**>(&environment), JNI_VERSION_1_6);
  if (environment_status == JNI_EDETACHED) {
    if (attach_bridge_thread(vm,&environment) != JNI_OK) return false;
    attached = true;
  } else if (environment_status != JNI_OK || environment == nullptr) {
    return false;
  }

  jclass controller_class{};
  jmethodID method{};
  {
    std::scoped_lock lock(g_bridge_mutex);
    if (g_java_bridge_ready.load(std::memory_order_acquire) && vm == g_vm &&
        g_controller_class != nullptr) {
      method = delivery_method_locked(call);
      controller_class = static_cast<jclass>(environment->NewGlobalRef(g_controller_class));
    }
  }
  if (environment->ExceptionCheck() || controller_class == nullptr || method == nullptr) {
    clear_jni_exception(environment);
    if (controller_class != nullptr) environment->DeleteGlobalRef(controller_class);
    if (attached) vm->DetachCurrentThread();
    return false;
  }

  const jboolean accepted = call == JavaDeliveryCall::fail_closed
      ? environment->CallStaticBooleanMethod(controller_class, method)
      : environment->CallStaticBooleanMethod(
          controller_class, method, static_cast<jlong>(generation));
  const bool call_succeeded = !environment->ExceptionCheck() && accepted == JNI_TRUE;
  clear_jni_exception(environment);
  environment->DeleteGlobalRef(controller_class);
  if (attached) vm->DetachCurrentThread();
  return call_succeeded;
}

void fail_closed_native_state(
    MakcuStreamCloseScope scope,
    ControlBlockerReason blocker =
        ControlBlockerReason::transport_failure) noexcept {
  g_stream_generation_gate.fail_closed(scope);
  if (scope == MakcuStreamCloseScope::delivery_failure) {
    ++g_delivery_fail_close_requests;
  } else {
    ++g_stream_lifecycle_close_requests;
  }
  g_output_gate.fail_closed();
  g_move_commit_gate.fail_closed();
  g_move_visibility_gate.fail_closed();
  {
    std::scoped_lock lock(g_control_mutex);
    g_control_core.reset();
  }
  reset_move_telemetry_continuity();
  update_control_blocker(
      scope == MakcuStreamCloseScope::stream_lifecycle
          ? ControlBlockerReason::user_disabled
          : blocker);
}

void fail_closed_bridge(
    MakcuStreamCloseScope scope =
        MakcuStreamCloseScope::delivery_failure,
    ControlBlockerReason blocker =
        ControlBlockerReason::transport_failure) noexcept {
  fail_closed_native_state(scope, blocker);
  static_cast<void>(invoke_java_delivery_gate(JavaDeliveryCall::fail_closed));
}

bool complete_matching_move(
    std::uint64_t ticket, std::atomic_uint64_t& stale_counter,
    std::uint64_t minimum_visibility_us) noexcept {
  const std::uint64_t acknowledged_at_us = monotonic_microseconds();
  const MakcuMoveCompletion completion = g_move_commit_gate.complete_with(
      ticket, [&](MakcuFrameIdentity source_identity) noexcept {
        g_move_visibility_gate.arm(
            source_identity, acknowledged_at_us, minimum_visibility_us);
      });
  if (!completion.matched) {
    ++stale_counter;
    return false;
  }
  update_control_blocker(
      ControlBlockerReason::post_ack_visibility, acknowledged_at_us);
  return true;
}
}  // namespace

bool bind_makcu_move_bridge(JNIEnv* environment, jclass controller_class) {
  fail_closed_bridge(
      MakcuStreamCloseScope::delivery_failure,
      ControlBlockerReason::initializing);
  g_java_bridge_ready.store(false, std::memory_order_release);
  if (environment == nullptr || controller_class == nullptr || environment->ExceptionCheck()) {
    clear_jni_exception(environment);
    return false;
  }

  JavaVM* candidate_vm{};
  if (environment->GetJavaVM(&candidate_vm) != JNI_OK || candidate_vm == nullptr) {
    std::scoped_lock lock(g_bridge_mutex);
    clear_binding_locked(environment);
    return false;
  }
  jclass candidate_class = static_cast<jclass>(environment->NewGlobalRef(controller_class));
  if (candidate_class == nullptr || environment->ExceptionCheck()) {
    clear_jni_exception(environment);
    if (candidate_class != nullptr) environment->DeleteGlobalRef(candidate_class);
    std::scoped_lock lock(g_bridge_mutex);
    clear_binding_locked(environment);
    return false;
  }

  const jmethodID candidate_offer =
      environment->GetStaticMethodID(
          candidate_class, "offerNativeMoveBeforeDeadline", "(IIJJ)Z");
  const jmethodID candidate_suspend = environment->ExceptionCheck() ? nullptr
      : environment->GetStaticMethodID(
          candidate_class, "suspendNativeDeliveryForRecovery", "(J)Z");
  const jmethodID candidate_resume = environment->ExceptionCheck() ? nullptr
      : environment->GetStaticMethodID(
          candidate_class, "resumeNativeDeliveryAfterRecovery", "(J)Z");
  const jmethodID candidate_fail_closed = environment->ExceptionCheck() ? nullptr
      : environment->GetStaticMethodID(candidate_class, "failClosedNativeDelivery", "()Z");
  if (environment->ExceptionCheck() || candidate_offer == nullptr || candidate_suspend == nullptr ||
      candidate_resume == nullptr || candidate_fail_closed == nullptr) {
    clear_jni_exception(environment);
    environment->DeleteGlobalRef(candidate_class);
    std::scoped_lock lock(g_bridge_mutex);
    clear_binding_locked(environment);
    return false;
  }

  {
    std::scoped_lock lock(g_bridge_mutex);
    clear_binding_locked(environment);
    g_vm = candidate_vm;
    g_controller_class = candidate_class;
    g_offer_move = candidate_offer;
    g_suspend_delivery = candidate_suspend;
    g_resume_delivery = candidate_resume;
    g_fail_closed_delivery = candidate_fail_closed;
    g_java_bridge_ready.store(true, std::memory_order_release);
  }
  return true;
}

bool configure_makcu_control_profile(const MakcuControlProfile& profile) {
  // A profile generation must not cross an observation/offer transaction.
  // Java closes and drains physical delivery before calling the disabled and
  // enabled phases, while this lock prevents a rejected old offer from
  // fail-closing or resetting the newly configured native generation.
  std::scoped_lock publish_lock(g_publish_mutex);
  const vfdual::MobileModelContract* model =
      vfdual::find_mobile_model(profile.model_token);
  const bool target_policy_valid = model != nullptr &&
      std::isfinite(profile.target_y_ratio) &&
      vfdual::matches_mobile_aim_target_contract(
          *model, profile.body_class_id, profile.head_class_id,
          profile.selected_class_id, profile.selected_class_uses_head_box,
          profile.selected_class_uses_geometric_head,
          profile.target_y_ratio);
  const bool personal_scalars_valid =
      std::isfinite(profile.personal_speed_scale) &&
      std::isfinite(profile.personal_stability_scale) &&
      std::isfinite(profile.personal_variation_scale) &&
      std::isfinite(profile.personal_median_duration_ms) &&
      std::isfinite(profile.personal_peak_time_fraction) &&
      std::isfinite(profile.personal_bell_correlation) &&
      std::isfinite(profile.personal_jitter_amplitude_pixels) &&
      std::isfinite(profile.personal_jitter_maximum_pixels) &&
      std::isfinite(profile.personal_minimum_error_pixels);
  const bool personal_envelope_valid =
      profile.personal_speed_envelope_count <=
          profile.personal_speed_envelope.size() &&
      std::all_of(
          profile.personal_speed_envelope.begin(),
          profile.personal_speed_envelope.begin() +
              static_cast<std::ptrdiff_t>(
                  profile.personal_speed_envelope_count),
          [](float value) {
            return std::isfinite(value) && value >= 0.0F && value <= 2.0F;
          });
  if (!std::isfinite(profile.relative_gain) || !std::isfinite(profile.deadzone_pixels) ||
      profile.relative_gain < kMinimumRelativeGain || profile.relative_gain > kMaximumRelativeGain ||
      profile.deadzone_pixels < 0.0F || profile.deadzone_pixels > kMaximumDeadzonePixels ||
      profile.maximum_axis_delta < kMinimumAxisDelta ||
      profile.maximum_axis_delta > kMaximumAxisDelta ||
      profile.switch_confirmation_ms < kMinimumSwitchConfirmationMs ||
      profile.switch_confirmation_ms > kMaximumSwitchConfirmationMs) {
    fail_closed_bridge();
    return false;
  }
  if (!target_policy_valid || !personal_scalars_valid || !personal_envelope_valid ||
      (profile.personal_trajectory_enabled &&
       profile.personal_speed_envelope_count !=
           profile.personal_speed_envelope.size())) {
    fail_closed_bridge();
    return false;
  }
  if (profile.output_enabled && !g_java_bridge_ready.load(std::memory_order_acquire)) {
    fail_closed_bridge();
    return false;
  }
  if (profile.output_enabled && g_output_gate.recovery_suspended()) return false;
  if (!profile.output_enabled) {
    static_cast<void>(g_output_gate.set_user_requested(false));
  }
  MobileControlConfig control_config=screen_pixel_control_config();
  control_config.body_class_id = profile.body_class_id;
  control_config.head_class_id = profile.head_class_id;
  control_config.selected_class_id = profile.selected_class_id;
  control_config.model_width = vfdual::kProductionCaptureWidth;
  control_config.model_height = vfdual::kProductionCaptureHeight;
  control_config.model_center_x = control_config.model_width * 0.5F;
  control_config.model_center_y = control_config.model_height * 0.5F;
  control_config.tracker.high_confidence_threshold =
      model->tracking_confidence.high_confidence_threshold;
  control_config.tracker.low_confidence_threshold =
      model->tracking_confidence.low_confidence_threshold;
  control_config.tracker.new_track_confidence_threshold =
      model->tracking_confidence.new_track_confidence_threshold;
  control_config.selected_class_uses_head_box =
      profile.selected_class_uses_head_box;
  control_config.selected_class_uses_geometric_head =
      profile.selected_class_uses_geometric_head;
  control_config.head_body_pairing_enabled =
      profile.selected_class_uses_head_box;
  control_config.body_fallback_y_ratio = profile.target_y_ratio;
  control_config.head_confidence_threshold = 0.0F;
  control_config.body_confidence_threshold = 0.0F;
  control_config.paired_body_min_confidence = 0.0F;
  control_config.head_only_min_confidence = 0.0F;
  control_config.body_fallback_min_confidence = 0.0F;
  control_config.head_only_max_center_distance_pixels =
      control_config.model_width;
  control_config.body_fallback_max_center_distance_pixels =
      control_config.model_width;
  control_config.reject_border_touching = false;
  if (!profile.selected_class_uses_head_box) {
    control_config.body_min_width = 2.0F;
    control_config.body_min_height = 2.0F;
    control_config.body_max_width = control_config.model_width;
    control_config.body_max_height = control_config.model_height;
    control_config.body_min_aspect_ratio = 0.02F;
    control_config.body_max_aspect_ratio = 50.0F;
  }
  control_config.motion.response_gain = profile.relative_gain;
  control_config.motion.deadzone_pixels = profile.deadzone_pixels;
  control_config.motion.maximum_axis_delta = profile.maximum_axis_delta;
  control_config.switch_confirmation_duration_us =
      static_cast<std::uint64_t>(profile.switch_confirmation_ms) * 1'000U;
  // User/profile timing is authoritative. Model tuning may derive dependent
  // time contracts (CS2 lost-target hold) but must not replace this value.
  apply_mobile_model_control_tuning(*model, control_config);
  control_config.observation_servo_enabled = true;
  control_config.motion.maximum_prediction_pixels = kMobileObservationPredictionLimitPixels;
  // Brief skill effects must not hand a live lock to an adjacent enemy.
  // Preserve identity through 350 ms of missing observations; no movement is
  // emitted for the invisible target, and a replacement still needs confirmation.
  control_config.lost_target_hold_duration_us = 350'000U;
  control_config.tracker.lost_track_duration_us = 500'000U;
  control_config.motion.uncalibrated_maximum_axis_delta = std::min(
      control_config.motion.uncalibrated_maximum_axis_delta,
      profile.maximum_axis_delta);
  control_config.motion.maximum_step_counts_per_tick = std::min(
      control_config.motion.maximum_step_counts_per_tick,
      static_cast<float>(profile.maximum_axis_delta));
  control_config.motion.maximum_jerk_counts_per_tick2 = std::min(
      control_config.motion.maximum_jerk_counts_per_tick2,
      control_config.motion.maximum_step_counts_per_tick);
  control_config.motion.settle_maximum_axis_delta =
      std::min(
          control_config.motion.settle_maximum_axis_delta,
          profile.maximum_axis_delta);
  control_config.motion.personal_trajectory_enabled =
      profile.personal_trajectory_enabled;
  control_config.motion.personal_profile_seed =
      profile.personal_profile_seed;
  control_config.motion.personal_speed_scale =
      profile.personal_speed_scale;
  control_config.motion.personal_stability_scale =
      profile.personal_stability_scale;
  control_config.motion.personal_variation_scale =
      profile.personal_variation_scale;
  control_config.motion.personal_median_duration_ms =
      profile.personal_median_duration_ms;
  control_config.motion.personal_peak_time_fraction =
      profile.personal_peak_time_fraction;
  control_config.motion.personal_bell_correlation =
      profile.personal_bell_correlation;
  control_config.motion.personal_jitter_amplitude_pixels =
      profile.personal_jitter_amplitude_pixels;
  control_config.motion.personal_jitter_maximum_pixels =
      profile.personal_jitter_maximum_pixels;
  control_config.motion.personal_maximum_extra_counts =
      profile.personal_maximum_extra_counts;
  control_config.motion.personal_minimum_error_pixels =
      profile.personal_minimum_error_pixels;
  control_config.motion.personal_speed_envelope =
      profile.personal_speed_envelope;
  control_config.motion.personal_speed_envelope_count =
      profile.personal_speed_envelope_count;
  bool control_configured{};
  {
    std::scoped_lock lock(g_control_mutex);
    const auto& previous=g_control_core.response_estimator();
    const bool matching_scope = g_calibration_scope.starts_with(profile.model_token + "|");
    if (matching_scope && g_preserve_current_calibration && previous.x().trusted && previous.y().trusted) {
      g_saved_calibration={previous.x().response,previous.y().response,double(previous.delay_us()),
          previous.x().quality,previous.y().quality,1.0,double(previous.updates())};
    }
    control_configured = g_control_core.configure(control_config);
    if (control_configured && matching_scope && g_saved_calibration[5] != 0.0) {
      (void)g_control_core.restore_response_calibration(float(g_saved_calibration[0]),float(g_saved_calibration[1]),
          std::uint64_t(g_saved_calibration[2]),float(g_saved_calibration[3]),float(g_saved_calibration[4]));
    }
    g_preserve_current_calibration = control_configured && matching_scope;
  }
  if (!control_configured) {
    fail_closed_bridge();
    return false;
  }
  g_relative_gain.store(profile.relative_gain, std::memory_order_relaxed);
  g_deadzone_pixels.store(profile.deadzone_pixels, std::memory_order_relaxed);
  g_maximum_axis_delta.store(profile.maximum_axis_delta, std::memory_order_relaxed);
  if (!profile.output_enabled) {
    g_move_commit_gate.fail_closed();
    g_move_visibility_gate.fail_closed();
  }
  if (!g_output_gate.set_user_requested(profile.output_enabled)) return false;
  update_control_blocker(
      profile.output_enabled
          ? ControlBlockerReason::runnable
          : ControlBlockerReason::user_disabled);
  return true;
}

void activate_makcu_stream_generation(std::uint64_t generation) noexcept {
  std::scoped_lock publish_lock(g_publish_mutex);
  g_stream_generation_gate.activate(generation);
}

void suspend_makcu_output_for_recovery(std::uint64_t generation) noexcept {
  // Linearize generation rollover with the complete observation/control
  // transaction. A result that passed the decoder check immediately before
  // recovery cannot repopulate the freshly reset tracker afterward.
  std::scoped_lock publish_lock(g_publish_mutex);
  g_stream_generation_gate.activate(generation);
  g_output_gate.suspend_for_recovery(generation);
  update_control_blocker(ControlBlockerReason::recovery_suspended);
  // Java suspension first closes its offer gate and waits for transport I/O
  // already in progress. Keep the exact native ticket valid during that wait
  // so a legitimate final ACK cannot be misclassified as stale and cancel an
  // otherwise recoverable decoder restart.
  const bool java_suspended =
      invoke_java_delivery_gate(JavaDeliveryCall::suspend, generation);
  g_move_commit_gate.fail_closed();
  g_move_visibility_gate.fail_closed();
  {
    std::scoped_lock lock(g_control_mutex);
    g_control_core.reset();
  }
  if (!java_suspended ||
      !g_output_gate.recovery_suspended_for(generation)) {
    fail_closed_bridge(
        MakcuStreamCloseScope::delivery_failure,
        ControlBlockerReason::transport_failure);
  }
}

bool resume_makcu_output_after_valid_frame(std::uint64_t generation) noexcept {
  // Keep the native completion and its Java resume in the same lifecycle
  // transaction as suspend/configure/disable. Otherwise a newer suspension
  // may replace the Java token after native completion, and the rejected old
  // Java resume would fail-close that newer, healthy recovery generation.
  std::scoped_lock publish_lock(g_publish_mutex);
  const MakcuRecoveryCompletion completion =
      g_output_gate.complete_after_valid_frame(generation);
  if (!completion.matched) return false;
  const bool java_output_enabled =
      invoke_java_delivery_gate(JavaDeliveryCall::resume, generation);
  if (completion.output_enabled && java_output_enabled) {
    update_control_blocker(ControlBlockerReason::runnable);
    return true;
  }
  fail_closed_bridge(
      MakcuStreamCloseScope::delivery_failure,
      ControlBlockerReason::transport_failure);
  return false;
}

void fail_closed_makcu_delivery() noexcept {
  fail_closed_bridge();
}

void disable_makcu_output() noexcept {
  std::scoped_lock publish_lock(g_publish_mutex);
  fail_closed_bridge(MakcuStreamCloseScope::stream_lifecycle);
}

bool control_observation_requested(std::uint64_t generation) noexcept {
  return g_stream_generation_gate.accepts(generation) &&
      g_output_gate.user_requested() && g_stream_generation_gate.accepts(generation);
}

bool control_observation_generation_active(std::uint64_t generation) noexcept {
  return g_stream_generation_gate.accepts(generation);
}

void publish_makcu_move_for_detections(
    std::span<const YoloDetection> model_detections, std::uint64_t frame_sequence,
  std::uint64_t stream_generation, bool content_updated,
  std::uint64_t observed_at_us, std::uint64_t result_ready_us, const BackgroundMotion& model_background, std::uint32_t wire_frame_id,
  std::span<const TargetImageMotion> model_image_motion,
  std::uint64_t source_decision_deadline_us, bool* observation_consumed) {
  if(observation_consumed)*observation_consumed=false;
  const auto record_consumption=[&](ControlSuppressionReason reason) {
    if(observation_consumed) *observation_consumed=
        reason!=ControlSuppressionReason::invalid_time &&
        reason!=ControlSuppressionReason::stale_frame &&
        reason!=ControlSuppressionReason::non_monotonic_frame;
  };
  std::scoped_lock publish_lock(g_publish_mutex);
  // A result-ready timestamp precedes commit/recovery work and can precede
  // waiting for this transaction. Planning at that old time underestimates
  // observation age and target lead. Keep source/deadline timestamps intact;
  // sample the control clock only after acquiring the transaction lock.
  const auto now_us = monotonic_microseconds();
  if (!g_stream_generation_gate.accepts(stream_generation)) {
    ++g_stale_stream_generation_suppressed;
    update_control_blocker(
        ControlBlockerReason::stale_stream_generation, now_us);
    return;
  }
  ++g_processed_frames;
  ControlSpaceFrame transformed;
  if(!transformed.transform(model_detections,model_image_motion,model_background)){
    fail_closed_bridge();return;
  }
  const auto detections=std::span<const YoloDetection>(transformed.boxes.data(),transformed.box_count);
  const auto image_motion=std::span<const TargetImageMotion>(transformed.motions.data(),transformed.motion_count);
  const auto& background=transformed.background;
  {
    std::scoped_lock lock(g_control_mutex);
    g_detection_trace.record(detections, {frame_sequence, observed_at_us, now_us, background, wire_frame_id},image_motion);
  }
  g_last_detection_count.store(
      static_cast<std::uint64_t>(detections.size()),
      std::memory_order_relaxed);
  float maximum_detection_confidence = 0.0F;
  for (const YoloDetection& detection : detections) {
    if (std::isfinite(detection.confidence)) {
      maximum_detection_confidence = std::max(
          maximum_detection_confidence, detection.confidence);
    }
  }
  g_last_max_detection_confidence.store(
      maximum_detection_confidence, std::memory_order_relaxed);
  if (g_move_commit_gate.expire_if_older(
          now_us, kMaximumMoveCompletionAgeUs)) {
    ++g_move_completion_timeouts;
    fail_closed_bridge(
        MakcuStreamCloseScope::delivery_failure,
        ControlBlockerReason::ticket_deadline_expired);
    return;
  }
  if (g_move_commit_gate.pending()) {
    {
      std::scoped_lock lock(g_control_mutex);
      record_consumption(g_control_core.observe_tracking_only(
          detections, {frame_sequence, observed_at_us, now_us, background, wire_frame_id},image_motion));
    }
    ++g_move_completion_pending_suppressed;
    update_control_blocker(ControlBlockerReason::ticket_pending, now_us);
    return;
  }
  bool observation_servo{};
  bool accepted_history_valid = true;
  {
    std::scoped_lock lock(g_control_mutex);
    observation_servo = g_control_core.observation_servo_enabled();
    if (observation_servo) {
      const auto completed = g_move_visibility_gate.snapshot();
      if (completed.armed) {
        accepted_history_valid = g_control_core.record_accepted_move(
            g_last_offered_delta_x.load(std::memory_order_relaxed),
            g_last_offered_delta_y.load(std::memory_order_relaxed), completed.armed_at_us);
        g_move_visibility_gate.fail_closed();
      }
    }
  }
  if (!accepted_history_valid) {
    fail_closed_bridge(MakcuStreamCloseScope::delivery_failure, ControlBlockerReason::post_ack_visibility_timeout);
    return;
  }
  const MakcuMoveVisibilityDecision visibility_decision =
      g_move_visibility_gate.evaluate(
          {stream_generation, frame_sequence}, observed_at_us, content_updated, now_us,
          kMaximumPostAckVisibilityAgeUs);
  if (visibility_decision == MakcuMoveVisibilityDecision::timed_out) {
    ++g_post_completion_visibility_timeouts;
    fail_closed_bridge(
        MakcuStreamCloseScope::delivery_failure,
        ControlBlockerReason::post_ack_visibility_timeout);
    return;
  }
  const bool completed_move_became_visible =
      visibility_decision == MakcuMoveVisibilityDecision::became_visible;
  if (visibility_decision == MakcuMoveVisibilityDecision::waiting) {
    {
      std::scoped_lock lock(g_control_mutex);
      record_consumption(g_control_core.observe_tracking_only(
          detections, {frame_sequence, observed_at_us, now_us, background, wire_frame_id},image_motion));
    }
    ++g_post_completion_visibility_suppressed;
    update_control_blocker(
        ControlBlockerReason::post_ack_visibility, now_us);
    return;
  }

  MobileControlOutput output{};
  {
    std::scoped_lock lock(g_control_mutex);
    if (completed_move_became_visible) {
      g_control_core.apply_visible_ego_motion(
          g_last_offered_delta_x.load(std::memory_order_relaxed),
          g_last_offered_delta_y.load(std::memory_order_relaxed));
    }
    output = g_control_core.process(detections, {frame_sequence, observed_at_us, now_us, background, wire_frame_id},image_motion);
    record_consumption(output.suppression_reason);
  }
  if (!output.has_target) ++g_no_targets;
  if (output.held) ++g_held_targets;
  if (output.switched) ++g_target_switches;
  if (!output.has_move()) {
    observe_no_move(output, now_us);
    return;
  }
  ++g_candidates;
  if (!g_output_gate.output_enabled()) {
    record_dispatch(frame_sequence, 0, now_us, now_us, 0, output, 2);
    ++g_output_disabled_suppressed;
    update_control_blocker(ControlBlockerReason::output_disabled, now_us);
    return;
  }
  const std::uint64_t ticket =
      g_move_commit_gate.begin(now_us, stream_generation, frame_sequence);
  if (ticket == 0U) {
    ++g_move_completion_pending_suppressed;
    update_control_blocker(ControlBlockerReason::ticket_begin_failed, now_us);
    std::scoped_lock lock(g_control_mutex);
    g_control_core.reset();
    return;
  }

  const auto dispatch_now_us = monotonic_microseconds();
  const auto transport_budget_us = remaining_move_budget_us(result_ready_us, dispatch_now_us);
  std::uint64_t dispatch_budget_us = observation_servo
      ? visual_move_budget_us(observed_at_us,dispatch_now_us,transport_budget_us)
      : transport_budget_us;
  // A newer independently matched pixel observation may improve the control
  // position, but it cannot renew the neural source's original decision life.
  if(source_decision_deadline_us) {
    dispatch_budget_us=dispatch_now_us>=source_decision_deadline_us ? 0U :
        std::min(dispatch_budget_us,source_decision_deadline_us-dispatch_now_us);
  }
  const std::uint64_t dispatch_deadline_us=dispatch_now_us+dispatch_budget_us;
  if (dispatch_budget_us == 0U) {
    record_dispatch(frame_sequence, ticket, dispatch_now_us, dispatch_now_us, 0, output, 3);
    // No transport call happened. Discard this old decision, preserving the
    // live target/response history and the healthy output session. Revoking
    // the entire session here caused a 250ms recovery gap after a single late
    // frame, followed by reacquisition of a target we had already tracked.
    report_move_discarded_before_send(ticket);
    return;
  }

  bool java_offer_accepted = false;
  bool java_dispatch_completed = false;
  bool offer_authorized = false;
  offer_authorized = g_output_gate.execute_if_enabled([&] {
    std::scoped_lock lock(g_bridge_mutex);
    if (!g_java_bridge_ready.load(std::memory_order_acquire) || g_vm == nullptr ||
        g_controller_class == nullptr || g_offer_move == nullptr) {
      ++g_java_dispatch_failures;
      return;
    }
    JNIEnv* environment{};
    bool attached = false;
    const jint environment_status = g_vm->GetEnv(
        reinterpret_cast<void**>(&environment), JNI_VERSION_1_6);
    if (environment_status == JNI_EDETACHED) {
      if (attach_bridge_thread(g_vm,&environment) != JNI_OK) {
        ++g_java_dispatch_failures;
        return;
      }
      attached = true;
    } else if (environment_status != JNI_OK || environment == nullptr) {
      ++g_java_dispatch_failures;
      return;
    }
    const jboolean accepted = environment->CallStaticBooleanMethod(
        g_controller_class, g_offer_move, output.delta_x, output.delta_y,
        static_cast<jlong>(ticket),
        static_cast<jlong>(dispatch_deadline_us));
    if (environment->ExceptionCheck()) {
      environment->ExceptionClear();
      ++g_java_dispatch_failures;
    } else {
      java_dispatch_completed = true;
      java_offer_accepted = accepted == JNI_TRUE;
      if (java_offer_accepted) ++g_java_offer_acceptances;
      else ++g_java_offer_rejections;
    }
    if (attached) g_vm->DetachCurrentThread();
  });
  record_dispatch(frame_sequence, ticket, dispatch_now_us, monotonic_microseconds(), dispatch_budget_us, output,
      !offer_authorized ? 2U : !java_dispatch_completed ? 4U : java_offer_accepted ? 1U : 5U);
  if (offer_authorized && java_dispatch_completed && java_offer_accepted) {
    {
      std::scoped_lock lock(g_control_mutex);
      if (!observation_servo) g_control_core.begin_move_delivery();
    }
    observe_accepted_move(output.delta_x, output.delta_y);
    update_control_blocker(ControlBlockerReason::ticket_pending, now_us);
    return;
  }

  static_cast<void>(g_move_commit_gate.complete(ticket));
  {
    std::scoped_lock lock(g_control_mutex);
    g_control_core.reset();
  }
  if (!offer_authorized) {
    ++g_output_disabled_suppressed;
    update_control_blocker(ControlBlockerReason::output_disabled, now_us);
    return;
  }
  fail_closed_bridge(
      MakcuStreamCloseScope::delivery_failure,
      java_dispatch_completed
          ? ControlBlockerReason::dispatch_rejected
          : ControlBlockerReason::dispatch_unavailable);
}

void report_move_discarded_before_send(std::uint64_t ticket) noexcept {
  // Only pre-write/pre-API deadline paths may call this. Partial writes,
  // missing/mismatched ACKs and exceptions still take the failure path.
  const auto completion=g_move_commit_gate.complete(ticket);
  if(!completion.matched){++g_stale_device_feedback;return;}
  ++g_pre_send_discards;
  update_control_blocker(ControlBlockerReason::ticket_deadline_expired,monotonic_microseconds());
}

void report_makcu_move_result(
    std::uint64_t ticket, bool device_acknowledged,
    std::uint64_t acknowledgement_us) noexcept {
  if (g_move_commit_gate.expire_if_deadline_reached(
          monotonic_microseconds(), kMaximumMoveCompletionAgeUs)) {
    ++g_move_completion_timeouts;
    fail_closed_bridge(
        MakcuStreamCloseScope::delivery_failure,
        ControlBlockerReason::ticket_deadline_expired);
    return;
  }
  if (!device_acknowledged) {
    // Claim the exact ticket and close native authorization before pending is
    // cleared. Do not acquire g_publish_mutex here: the serial reader may hold
    // serialIoLock while decoder recovery owns the publish lock and waits for
    // that same I/O to finish.
    const MakcuMoveCompletion completion = g_move_commit_gate.complete_with(
        ticket, [](std::uint64_t) noexcept {
          g_output_gate.revoke_immediately();
        });
    const MakcuMoveFeedbackAction action = classify_makcu_move_feedback(
        completion.matched, false);
    if (action == MakcuMoveFeedbackAction::ignore_stale) {
      ++g_stale_device_feedback;
      return;
    }
    ++g_device_ack_failures;
    fail_closed_bridge();
    return;
  }
  if (!complete_matching_move(
          ticket, g_stale_device_feedback,
          kPostAcknowledgementAdditionalDelayUs)) {
    return;
  }
  observe_matching_device_ack();
  ++g_device_ack_completions;
  g_last_device_ack_us.store(acknowledgement_us, std::memory_order_relaxed);
}

bool report_bluetooth_hid_move_accepted(
    std::uint64_t ticket, std::uint64_t api_acceptance_us) noexcept {
  if (g_move_commit_gate.expire_if_deadline_reached(
          monotonic_microseconds(), kMaximumMoveCompletionAgeUs)) {
    ++g_move_completion_timeouts;
    fail_closed_bridge(
        MakcuStreamCloseScope::delivery_failure,
        ControlBlockerReason::ticket_deadline_expired);
    return false;
  }
  if (!complete_matching_move(
          ticket, g_stale_bluetooth_hid_api_acceptances,
          kPostAcknowledgementAdditionalDelayUs)) {
    return false;
  }
  ++g_bluetooth_hid_api_acceptances;
  g_last_bluetooth_hid_api_acceptance_us.store(
      api_acceptance_us, std::memory_order_relaxed);
  return true;
}

std::string makcu_move_bridge_report() {
  const std::uint64_t report_now_us = monotonic_microseconds();
  const MakcuMoveCommitSnapshot commit_snapshot =
      g_move_commit_gate.snapshot();
  const MakcuMoveVisibilitySnapshot visibility_snapshot =
      g_move_visibility_gate.snapshot();
  const ControlBlockerSnapshot blocker_snapshot =
      g_control_blocker.snapshot();
  const std::uint64_t commit_age_us =
      commit_snapshot.pending() && commit_snapshot.started_at_us != 0U &&
              report_now_us >= commit_snapshot.started_at_us
          ? report_now_us - commit_snapshot.started_at_us
          : 0U;
  const std::uint64_t visibility_age_us =
      visibility_snapshot.armed && visibility_snapshot.armed_at_us != 0U &&
              report_now_us >= visibility_snapshot.armed_at_us
          ? report_now_us - visibility_snapshot.armed_at_us
          : 0U;
  const std::uint64_t blocker_age_us =
      blocker_snapshot.since_us != 0U && report_now_us >= blocker_snapshot.since_us
          ? report_now_us - blocker_snapshot.since_us
          : 0U;
  ControlCoreMetrics control_metrics{};
  MobileResponseEstimator::Axis response_x{}, response_y{};
  std::uint64_t response_delay_us{}, response_estimate_updates{};
  MobileResponseEstimator::DelayRange response_delay_range{};
  MobileResponseEstimator::LearningState response_learning_state{};
  bool observation_servo{};
  MobileTargetTrackerMetrics tracker_metrics{};
  MobileMotionPlannerMetrics motion_metrics{};
  MobileMotionPlannerConfig motion_config{};
  MobileMotionPlannerOutput motion_snapshot{};
  std::uint64_t switch_confirmation_duration_us{};
  float missing_switch_min_confidence{};
  float missing_switch_max_jump_pixels{};
  std::uint64_t lost_target_hold_duration_us{};
  std::uint64_t lost_track_duration_us{};
  float tracker_new_track_confidence_threshold{};
  float tracker_low_confidence_threshold{};
  {
    std::scoped_lock lock(g_control_mutex);
    control_metrics = g_control_core.metrics();
    response_x = g_control_core.response_estimator().x();
    response_y = g_control_core.response_estimator().y();
    response_delay_us = g_control_core.response_estimator().delay_us();
    response_estimate_updates = g_control_core.response_estimator().updates();
    response_delay_range = g_control_core.response_estimator().delay_range();
    response_learning_state = g_control_core.response_estimator().learning_state();
    observation_servo = g_control_core.observation_servo_enabled();
    tracker_metrics = g_control_core.tracker_metrics();
    motion_metrics = g_control_core.motion_metrics();
    motion_config = g_control_core.motion_config();
    motion_snapshot = g_control_core.motion_snapshot();
    switch_confirmation_duration_us =
        g_control_core.switch_confirmation_duration_us();
    missing_switch_min_confidence =
        g_control_core.missing_switch_min_confidence();
    missing_switch_max_jump_pixels =
        g_control_core.missing_switch_max_jump_pixels();
    lost_target_hold_duration_us =
        g_control_core.lost_target_hold_duration_us();
    lost_track_duration_us = g_control_core.lost_track_duration_us();
    tracker_new_track_confidence_threshold =
        g_control_core.tracker_new_track_confidence_threshold();
    tracker_low_confidence_threshold =
        g_control_core.tracker_low_confidence_threshold();
  }
  std::ostringstream value;
  value << "processing_enabled=1"
        << " processed_frames=" << g_processed_frames.load()
        << " active_stream_generation="
        << g_stream_generation_gate.active()
        << " native_delivery_fail_close_requests="
        << g_delivery_fail_close_requests.load()
        << " native_stream_lifecycle_close_requests="
        << g_stream_lifecycle_close_requests.load()
        << " stale_stream_generation_suppressed="
        << g_stale_stream_generation_suppressed.load()
        << " candidate_moves=" << g_candidates.load()
        << " java_offer_acceptances=" << g_java_offer_acceptances.load()
        << " java_offer_rejections=" << g_java_offer_rejections.load()
        << " native_move_completion_pending=" << g_move_commit_gate.pending()
        << " native_move_completion_pending_ticket="
        << commit_snapshot.ticket
        << " native_move_completion_pending_source_generation="
        << commit_snapshot.source_generation
        << " native_move_completion_pending_source_sequence="
        << commit_snapshot.source_sequence
        << " native_move_completion_pending_age_us=" << commit_age_us
        << " native_move_completion_deadline_us="
        << kMaximumMoveCompletionAgeUs
        << " native_move_completion_pending_suppressed="
        << g_move_completion_pending_suppressed.load()
        << " native_move_completion_timeouts="
        << g_move_completion_timeouts.load()
        << " native_pre_send_discards=" << g_pre_send_discards.load()
        << " native_device_ack_completions="
        << g_device_ack_completions.load()
        << " native_device_ack_failures=" << g_device_ack_failures.load()
        << " stale_device_feedback=" << g_stale_device_feedback.load()
        << " native_last_device_ack_latency_us=" << g_last_device_ack_us.load()
        << " native_last_device_ack_interval_us="
        << g_last_device_ack_interval_us.load()
        << " native_last_offered_delta_x=" << g_last_offered_delta_x.load()
        << " native_last_offered_delta_y=" << g_last_offered_delta_y.load()
        << " native_offered_absolute_x_counts="
        << g_offered_absolute_x_counts.load()
        << " native_offered_absolute_y_counts="
        << g_offered_absolute_y_counts.load()
        << " native_maximum_offered_absolute_axis_delta="
        << g_maximum_offered_absolute_axis_delta.load()
        << " native_offered_direction_flips_x="
        << g_offered_direction_flips_x.load()
        << " native_offered_direction_flips_y="
        << g_offered_direction_flips_y.load()
        << " native_last_offer_interval_us="
        << g_last_offer_interval_us.load()
        << " native_bluetooth_hid_api_acceptances="
        << g_bluetooth_hid_api_acceptances.load()
        << " stale_bluetooth_hid_api_acceptances="
        << g_stale_bluetooth_hid_api_acceptances.load()
        << " native_last_bluetooth_hid_api_acceptance_latency_us="
        << g_last_bluetooth_hid_api_acceptance_us.load()
        << " bluetooth_hid_api_acceptance_semantics="
           "android_hid_stack_accepted_not_host_or_physical_ack"
        << " post_completion_visibility_armed="
        << visibility_snapshot.armed
        << " post_completion_visibility_source_generation="
        << visibility_snapshot.source_generation
        << " post_completion_visibility_source_sequence="
        << visibility_snapshot.source_sequence
        << " post_completion_visibility_age_us=" << visibility_age_us
        << " post_completion_visibility_timeout_us="
        << kMaximumPostAckVisibilityAgeUs
        << " post_completion_visibility_timeouts="
        << g_post_completion_visibility_timeouts.load()
        << " makcu_post_completion_visibility_min_us="
        << kPostAcknowledgementAdditionalDelayUs
        << " bluetooth_hid_post_completion_visibility_min_us="
        << kPostAcknowledgementAdditionalDelayUs
        << " post_completion_visibility_suppressed="
        << g_post_completion_visibility_suppressed.load()
        << " control_blocker="
        << control_blocker_reason_name(blocker_snapshot.reason)
        << " control_blocker_age_us=" << blocker_age_us
        << " control_blocker_transition_id="
        << blocker_snapshot.transition_id
        << " java_bridge_ready=" << g_java_bridge_ready.load()
        << " output_disabled_suppressed=" << g_output_disabled_suppressed.load()
        << " output_requested=" << g_output_gate.user_requested()
        << " output_recovery_suspended=" << g_output_gate.recovery_suspended()
        << " output_enabled=" << g_output_gate.output_enabled()
        << " relative_gain=" << g_relative_gain.load()
        << " relative_gain_contract=error_fraction_per_60hz"
        << " relative_gain_min=" << kMinimumRelativeGain
        << " relative_gain_max=" << kMaximumRelativeGain
        << " deadzone_pixels=" << g_deadzone_pixels.load()
        << " servo_resolution_rule=confirmed_stationary_half_count"
        << " servo_resolution_radius_x_px=" << std::max(motion_config.deadzone_pixels, .5F * response_x.response)
        << " servo_resolution_radius_y_px=" << std::max(motion_config.deadzone_pixels, .5F * response_y.response)
        << " last_motion_resolution_stopped=" << motion_snapshot.settle_stopped
        << " maximum_axis_delta=" << g_maximum_axis_delta.load()
        << " switch_confirmation_duration_us="
        << switch_confirmation_duration_us
        << " missing_switch_min_confidence="
        << missing_switch_min_confidence
        << " missing_switch_max_jump_pixels="
        << missing_switch_max_jump_pixels
        << " lost_target_hold_duration_us="
        << lost_target_hold_duration_us
        << " tracker_lost_duration_us=" << lost_track_duration_us
        << " last_detection_count=" << g_last_detection_count.load()
        << " last_max_detection_confidence="
        << g_last_max_detection_confidence.load()
        << " tracker_new_track_confidence_threshold="
        << tracker_new_track_confidence_threshold
        << " tracker_low_confidence_threshold="
        << tracker_low_confidence_threshold
        << " deadzone_suppressed=" << g_deadzone_suppressed.load()
        << " no_move_suppressed=" << g_no_move_suppressed.load()
        << " response_guard_suppressed="
        << g_response_guard_suppressed.load()
        << " device_resolution_suppressed="
        << g_device_resolution_suppressed.load()
        << " settle_guard_suppressed="
        << g_settle_guard_suppressed.load()
        << " direction_flip_suppressed="
        << g_direction_flip_suppressed.load()
        << " motion_invalid_suppressed="
        << g_motion_invalid_suppressed.load()
        << " detected_not_control_eligible_suppressed="
        << g_detected_not_control_eligible_suppressed.load()
        << " held_targets=" << g_held_targets.load()
        << " target_switches=" << g_target_switches.load()
        << " no_targets=" << g_no_targets.load()
        << " stale_frames=" << control_metrics.stale_frames
        << " non_monotonic_frames=" << control_metrics.non_monotonic_frames
        << " invalid_time_frames=" << control_metrics.invalid_time_frames
        << " geometry_non_finite_rejected=" << control_metrics.rejected_non_finite
        << " geometry_invalid_box_rejected=" << control_metrics.rejected_invalid_box
        << " geometry_unsupported_class_rejected=" << control_metrics.rejected_unsupported_class
        << " geometry_confidence_rejected=" << control_metrics.rejected_confidence
        << " geometry_out_of_bounds_rejected=" << control_metrics.rejected_out_of_bounds
        << " partially_visible_body_observations=" << control_metrics.partially_visible_body_observations
        << " control_revision=20261019-motion-integrity"
        << " retinal_velocity_constraints_x=" << control_metrics.retinal_velocity_constraints_x
        << " retinal_velocity_constraints_y=" << control_metrics.retinal_velocity_constraints_y
        << " pursuit_boosted_x_frames=" << control_metrics.pursuit_boosted_x_frames
        << " pursuit_boosted_y_frames=" << control_metrics.pursuit_boosted_y_frames
        << " body_shape_motion_resets=" << control_metrics.body_shape_motion_resets
        << " confirmed_stop_velocity_resets_x=" << control_metrics.confirmed_stop_velocity_resets_x
        << " confirmed_stop_velocity_resets_y=" << control_metrics.confirmed_stop_velocity_resets_y
        << " geometry_border_rejected=" << control_metrics.rejected_border
        << " geometry_size_rejected=" << control_metrics.rejected_size
        << " geometry_aspect_rejected=" << control_metrics.rejected_aspect_ratio
        << " detected_not_control_eligible_frames="
        << control_metrics.detected_not_control_eligible_frames
        << " head_preemptions=" << control_metrics.head_preemptions
        << " head_body_projection_continuations="
        << control_metrics.head_body_projection_continuations
        << " head_body_projection_recoveries="
        << control_metrics.head_body_projection_recoveries
        << " maximum_head_body_projection_recovery_pixels="
        << control_metrics.maximum_head_body_projection_recovery_pixels
        << " rejected_head_body_projection_geometry="
        << control_metrics.rejected_head_body_projection_geometry
        << " switch_pending_frames=" << control_metrics.switch_pending_frames
        << " reacquisition_pending_frames="
        << control_metrics.reacquisition_pending_frames
        << " reacquisition_candidate_retained_frames="
        << control_metrics.reacquisition_candidate_retained_frames
        << " switch_confirmation_restarts=" << control_metrics.switch_confirmation_restarts
        << " reacquisition_confirmations="
        << control_metrics.reacquisition_confirmations
        << " crowd_identity_retention_frames=" << control_metrics.crowd_identity_retention_frames
        << " rejected_missing_switch_confidence="
        << control_metrics.rejected_missing_switch_confidence
        << " rejected_missing_switch_jump="
        << control_metrics.rejected_missing_switch_jump
        << " lock_exact_track_matches="
        << control_metrics.lock_exact_track_matches
        << " lock_observation_track_matches="
        << control_metrics.lock_observation_track_matches
        << " lock_geometric_matches="
        << control_metrics.lock_geometric_matches
        << " lock_giou_matches="
        << control_metrics.lock_giou_matches
        << " rejected_exact_track_geometry="
        << control_metrics.rejected_exact_track_geometry
        << " ego_motion_adjustments="
        << control_metrics.ego_motion_adjustments
        << " measured_ego_motion_adjustments="
        << control_metrics.measured_ego_motion_adjustments
        << " rejected_measured_ego_motion_axes="
        << control_metrics.rejected_measured_ego_motion_axes
        << " ego_motion_response_waits="
        << control_metrics.ego_motion_response_waits
        << " ego_motion_response_timeouts="
        << control_metrics.ego_motion_response_timeouts
        << " maximum_ego_motion_shift_pixels="
        << control_metrics.maximum_ego_motion_shift_pixels
        << " maximum_ego_motion_prediction_error_pixels="
        << control_metrics.maximum_ego_motion_prediction_error_pixels
        << " maximum_measured_ego_motion_support="
        << control_metrics.maximum_measured_ego_motion_support
        << " track_id_rebinds=" << control_metrics.track_id_rebinds
        << " rejected_known_track_rebinds="
        << control_metrics.rejected_known_track_rebinds
        << " rejected_track_id_mismatches="
        << control_metrics.rejected_track_id_mismatches
        << " rejected_ambiguous_rebinds="
        << control_metrics.rejected_ambiguous_rebinds
        << " rejected_rebind_geometry="
        << control_metrics.rejected_rebind_geometry
        << " tracking_only_frames=" << control_metrics.tracking_only_frames
        << " tracking_only_lock_updates="
        << control_metrics.tracking_only_lock_updates
        << " settle_suppressions=" << control_metrics.settle_suppressions
        << " motion_invalid_suppressions="
        << control_metrics.motion_invalid_suppressions
        << " motion_plans=" << motion_metrics.plans
        << " motion_phase=" << mobile_motion_phase_name(motion_snapshot.phase)
        << " motion_suppression="
        << motion_planner_suppression_name(
               motion_snapshot.suppression_reason)
        << " motion_predicted_error_x="
        << motion_snapshot.predicted_error_x
        << " motion_predicted_error_y="
        << motion_snapshot.predicted_error_y
        << " motion_acceleration_x="
        << motion_snapshot.target_acceleration_x_pixels_per_second2
        << " motion_acceleration_y="
        << motion_snapshot.target_acceleration_y_pixels_per_second2
        << " motion_response_budget_x="
        << motion_snapshot.response_budget_x_counts
        << " motion_response_budget_y="
        << motion_snapshot.response_budget_y_counts
        << " motion_lock_age_ms=" << motion_snapshot.lock_age_ms
        << " motion_phase_age_ms=" << motion_snapshot.phase_age_ms
        << " motion_response_x_px_per_count="
        << motion_config.response_x_px_per_count
        << " motion_response_y_px_per_count="
        << motion_config.response_y_px_per_count
        << " motion_response_upper_x_px_per_count="
        << motion_config.response_upper_x_px_per_count
        << " motion_response_upper_y_px_per_count="
        << motion_config.response_upper_y_px_per_count
        << " motion_response_limited="
        << motion_snapshot.response_limited
        << " motion_jerk_limited=" << motion_snapshot.jerk_limited
        << " motion_step_limited=" << motion_snapshot.step_limited
        << " motion_settle_stopped=" << motion_snapshot.settle_stopped
        << " motion_uncalibrated_maximum_axis_delta="
        << motion_config.uncalibrated_maximum_axis_delta
        << " motion_maximum_step_counts_per_tick="
        << motion_config.maximum_step_counts_per_tick
        << " motion_maximum_jerk_counts_per_tick2="
        << motion_config.maximum_jerk_counts_per_tick2
        << " personal_trajectory_enabled="
        << motion_config.personal_trajectory_enabled
        << " personal_trajectory_applied="
        << motion_snapshot.personal_trajectory_applied
        << " personal_speed_scale="
        << motion_config.personal_speed_scale
        << " personal_stability_scale="
        << motion_config.personal_stability_scale
        << " personal_variation_scale="
        << motion_config.personal_variation_scale
        << " personal_envelope_points="
        << motion_config.personal_speed_envelope_count
        << " personal_trajectory_plans="
        << motion_metrics.personal_trajectory_plans
        << " personal_variation_packets="
        << motion_metrics.personal_trajectory_variation_packets
        << " motion_invalid_inputs=" << motion_metrics.invalid_inputs
        << " motion_invalid_times=" << motion_metrics.invalid_times
        << " motion_stale_observations="
        << motion_metrics.stale_observations
        << " motion_non_monotonic_observations="
        << motion_metrics.non_monotonic_observations
        << " motion_direction_flip_suppressions="
        << motion_metrics.direction_flip_suppressions
        << " motion_direction_reorientations="
        << motion_metrics.direction_reorientations
        << " motion_response_guard_suppressions="
        << motion_metrics.response_guard_suppressions
        << " motion_subcount_resolution_suppressions="
        << motion_metrics.subcount_resolution_suppressions
        << " motion_settle_guard_suppressions="
        << motion_metrics.settle_guard_suppressions
        << " motion_settle_quantization_rescues="
        << motion_metrics.settle_quantization_rescues
        << " motion_settle_stop_latches="
        << motion_metrics.settle_stop_latches
        << " motion_settle_stop_holds="
        << motion_metrics.settle_stop_holds
        << " motion_response_limited_axes="
        << motion_metrics.response_limited_axes
        << " motion_jerk_limited_axes="
        << motion_metrics.jerk_limited_axes
        << " motion_step_limited_axes="
        << motion_metrics.step_limited_axes
        << " paired_heads=" << control_metrics.paired_heads
        << " head_only_acceptances=" << control_metrics.head_only_acceptances
        << " unvalidated_heads_rejected=" << control_metrics.rejected_unvalidated_heads
        << " body_fallbacks_rejected=" << control_metrics.rejected_body_fallbacks
        << " tracker_tracks_created=" << tracker_metrics.tracks_created
        << " tracker_first_stage_matches=" << tracker_metrics.first_stage_matches
        << " tracker_second_stage_matches=" << tracker_metrics.second_stage_matches
        << " tracker_center_matches=" << tracker_metrics.center_distance_matches
        << " tracker_ambiguous_center_matches_rejected="
        << tracker_metrics.ambiguous_center_matches_rejected
        << " tracker_association_shift_frames="
        << tracker_metrics.association_shift_frames
        << " tracker_association_shift_rejections="
        << tracker_metrics.association_shift_rejections
        << " tracker_image_constraints_applied="
        << tracker_metrics.image_constraints_applied
        << " tracker_image_constraints_rejected="
        << tracker_metrics.image_constraints_rejected
        << " tracker_image_disallowed_pairs_skipped="
        << tracker_metrics.image_disallowed_pairs_skipped
        << " tracker_known_owner_conflicts_resolved="
        << tracker_metrics.known_owner_conflicts_resolved
        << " tracker_lost_track_claims_blocked="
        << tracker_metrics.lost_track_claims_blocked
        << " tracker_global_assignment_calls=" << tracker_metrics.global_assignment_calls
        << " tracker_global_assignment_matches=" << tracker_metrics.global_assignment_matches
        << " tracker_global_assignment_size_fallbacks=" << tracker_metrics.global_assignment_size_fallbacks
        << " tracker_joint_assignment_acceptances=" << tracker_metrics.joint_assignment_acceptances
        << " tracker_joint_assignment_rejections=" << tracker_metrics.joint_assignment_rejections
        << " tracker_mixed_confidence_joint_frames=" << tracker_metrics.mixed_confidence_joint_frames
        << " tracker_mixed_confidence_joint_rejections=" << tracker_metrics.mixed_confidence_joint_rejections
        << " tracker_tracks_expired=" << tracker_metrics.tracks_expired
        << " tracker_control_extent_confirmations=" << tracker_metrics.control_extent_confirmations
        << " tracker_control_filter_blends="
        << tracker_metrics.control_filter_blends
        << " tracker_control_filter_bypasses="
        << tracker_metrics.control_filter_bypasses
        << " tracker_time_based_velocity_updates="
        << tracker_metrics.time_based_velocity_updates
        << " tracker_maximum_observation_interval_us="
        << tracker_metrics.maximum_observation_interval_us
        << " tracker_maximum_control_innovation_pixels="
        << tracker_metrics.maximum_control_innovation_pixels
        << " java_dispatch_failures=" << g_java_dispatch_failures.load();
  value << " observation_feedback_reconciliations=" << control_metrics.observation_feedback_reconciliations;
  value << " visible_response_trace_v1=[";
  const auto total = control_metrics.visible_response_samples;
  const auto count = std::min<std::uint64_t>(total, control_metrics.visible_responses.size());
  for (auto id = total - count + 1U; id <= total; ++id) {
    const auto& sample = control_metrics.visible_responses[(id - 1U) % control_metrics.visible_responses.size()];
    if (id != total - count + 1U) value << ';';
    value << sample.id << ',' << sample.observed_at_us << ','
          << sample.delta_x << ',' << sample.delta_y << ','
          << sample.raw_x << ',' << sample.raw_y << ','
          << sample.resolved_x << ',' << sample.resolved_y << ','
          << sample.error_x << ',' << sample.error_y << ','
          << sample.support << ',' << sample.wait_us << ',' << sample.flags;
  }
  value << ']';
  value << " observation_servo=" << observation_servo
        << " motion_controller=" << (observation_servo ? "continuous_velocity_feedforward" : "legacy_phased")
        << " response_x_estimate=" << response_x.response << " response_x_trusted=" << response_x.trusted
        << " response_x_fit_quality=" << response_x.quality
        << " response_y_estimate=" << response_y.response << " response_y_trusted=" << response_y.trusted
        << " response_y_fit_quality=" << response_y.quality
        << " response_visibility_estimate_us=" << response_delay_us
        << " response_delay_min_us=" << response_delay_range.minimum_us
        << " response_delay_max_us=" << response_delay_range.maximum_us
        << " response_delay_candidate_count=" << response_delay_range.candidates
        << " response_delay_identifiable=" << response_delay_range.identifiable
        << " response_learning_state=" << static_cast<unsigned>(response_learning_state)
        << " response_estimate_updates=" << response_estimate_updates
        << " background_motion_frames=" << control_metrics.background_motion_frames
        << " background_motion_unavailable_frames=" << control_metrics.background_motion_unavailable_frames
        << " image_identity_rebinds=" << control_metrics.image_identity_rebinds
        << " image_identity_pending_frames=" << control_metrics.image_identity_pending_frames
        << " pending_motion_budget_holds=" << control_metrics.pending_motion_budget_holds;
  return value.str();
}

std::string makcu_move_bridge_trace() {
  // Large snapshot buffers belong to the metrics worker heap, not JNI's stack.
  auto control_snapshot = std::make_unique<ControlTrace>();
  auto detection_snapshot = std::make_unique<DetectionTrace>();
  auto disturbance_snapshot = std::make_unique<DisturbanceSelectionTrace::Snapshot>();
  auto& control_trace = *control_snapshot;
  auto& detection_trace = *detection_snapshot;
  std::array<DispatchDiagnostic, 1024> dispatch_trace{};
  DiagnosticTraceCursor::Range control_range{}, dispatch_range{}, detection_range{};
  {
    std::scoped_lock lock(g_control_mutex);
    control_trace = g_control_core.control_trace();
    dispatch_trace = g_dispatch_trace;
    detection_trace = g_detection_trace;
    g_control_core.drain_disturbance_selection_trace(*disturbance_snapshot);
    control_range = g_control_trace_cursor.advance(control_trace.count, control_trace.samples.size());
    dispatch_range = g_dispatch_trace_cursor.advance(g_dispatch_trace_count, dispatch_trace.size());
    detection_range = g_detection_trace_cursor.advance(detection_trace.count, detection_trace.samples.size());
  }
  std::ostringstream value;
  value << "trace_clock=monotonic_us trace_coordinate_space=screen_roi_pixels"
        << " model_width=" << vfdual::kDefaultMobileModel.input.width
        << " model_height=" << vfdual::kDefaultMobileModel.input.height
        << " capture_width=" << vfdual::kProductionCaptureWidth
        << " capture_height=" << vfdual::kProductionCaptureHeight
        << " coordinate_width=" << vfdual::kProductionCaptureWidth
        << " coordinate_height=" << vfdual::kProductionCaptureHeight
        << " control_trace_lost=" << control_range.lost
        << " detection_trace_lost=" << detection_range.lost
        << " dispatch_trace_lost=" << dispatch_range.lost << " control_trace_reset=" << control_range.reset
        << " control_trace_v6=[";
  for (auto id = control_range.first; id <= control_range.last; ++id) {
    const auto& s = control_trace.samples[(id - 1U) % control_trace.samples.size()];
    if (id != control_range.first) value << ';';
    value << s.id << ',' << s.sequence << ',' << s.observed_at_us << ',' << s.now_us << ','
          << s.lock_id << ',' << s.track_id << ',' << s.target_x << ',' << s.target_y << ','
          << s.delta_x << ',' << s.delta_y << ',' << s.reason << ',' << s.flags << ','
          << s.background_x << ',' << s.background_y << ',' << s.background_support << ','
          << s.pending_x << ',' << s.pending_y << ',' << s.pending_reports << ','
          << s.accepted_at_us << ',' << s.accepted_dx << ',' << s.accepted_dy << ','
          << s.velocity_x << ',' << s.velocity_y << ',' << s.predicted_error_x << ',' << s.predicted_error_y << ','
          << s.response_x << ',' << s.response_y << ',' << s.response_delay_us << ','
          << s.wire_frame_id << ',' << s.box_x1 << ',' << s.box_y1 << ',' << s.box_x2 << ',' << s.box_y2 << ',' << s.confidence << ','
          << s.detection_count << ',' << s.candidate_count << ',' << s.alternative_track << ','
          << s.alternative_x << ',' << s.alternative_y << ',' << s.alternative_confidence << ','
          << s.pending_track << ',' << s.pending_frames << ',' << s.pending_since_us << ',' << s.missing_us << ','
          << s.selection_flags << ',' << s.velocity_flags << ',' << s.motion_flags << ','
          << s.proposal_x << ',' << s.proposal_y << ',' << s.disturbance_x << ',' << s.disturbance_y << ','
          << s.response_quality_x << ',' << s.response_quality_y << ','
          << s.servo.dt_seconds << ',' << s.servo.horizon_seconds << ','
          << s.servo.x.lead << ',' << s.servo.y.lead << ',' << s.servo.x.feedback << ',' << s.servo.y.feedback << ','
          << s.servo.x.feedforward << ',' << s.servo.y.feedforward << ','
          << s.servo.x.desired << ',' << s.servo.y.desired << ',' << s.servo.x.residual << ',' << s.servo.y.residual;
  }
  value << "] dispatch_trace_v1=[";
  for (auto id = dispatch_range.first; id <= dispatch_range.last; ++id) {
    const auto& s = dispatch_trace[(id - 1) % dispatch_trace.size()];
    if (id != dispatch_range.first) value << ';';
    value << s.id << ',' << s.sequence << ',' << s.ticket << ',' << s.dispatched_us << ','
          << s.returned_us << ',' << s.budget_us << ',' << s.dx << ',' << s.dy << ',' << s.outcome << ',' << s.control_trace_id;
  }
  value << "] detection_trace_v1=[";
  for (auto id = detection_range.first; id <= detection_range.last; ++id) {
    const auto& s = detection_trace.samples[(id - 1) % detection_trace.samples.size()];
    if (id != detection_range.first) value << ';';
    value << s.id << ',' << s.frame.sequence << ',' << s.frame.wire_frame_id << ','
          << s.frame.observed_at_us << ',' << s.frame.now_us << ','
          << s.frame.background.previous_observed_at_us << ',' << s.frame.background.valid << ','
          << s.frame.background.x << ',' << s.frame.background.y << ',' << s.frame.background.support << ','
          << s.total << ',' << s.kept;
    for (unsigned n = 0; n < s.kept; ++n) {
      const auto& b = s.boxes[n];
      value << ',' << b.class_id << ',' << b.x1 << ',' << b.y1 << ',' << b.x2 << ',' << b.y2 << ',' << b.confidence;
    }
  }
  value << "] image_motion_trace_v1=[";
  bool first_image=true;
  for(auto id=detection_range.first;id<=detection_range.last;++id){
    const auto& s=detection_trace.samples[(id-1U)%detection_trace.samples.size()];
    for(unsigned n=0;n<s.image_kept;++n){
      const auto& m=s.image_motion[n];if(!first_image)value<<';';first_image=false;
      value<<s.id<<','<<s.frame.sequence<<','<<s.frame.observed_at_us<<','<<m.previous_observed_at_us<<','
           <<m.current_index<<','<<m.previous_index<<','<<m.x<<','<<m.y<<','<<m.quality<<','
           <<m.forward_backward_error<<','<<m.support<<','<<m.candidates;
    }
  }
  value << "] " << disturbance_snapshot->report();
  return value.str();
}

void set_control_calibration_scope(const std::string& scope) {
  std::scoped_lock lock(g_control_mutex);
  if (scope == g_calibration_scope) return;
  g_calibration_scope = scope;
  g_saved_calibration = {};
  // The next profile application must not copy the previous route's fit.
  g_preserve_current_calibration = false;
}

bool restore_control_calibration(float x,float y,std::uint64_t delay,float qx,float qy) {
  std::scoped_lock lock(g_control_mutex);
  if (g_calibration_scope.empty() || !g_control_core.restore_response_calibration(x,y,delay,qx,qy)) return false;
  g_saved_calibration={x,y,double(delay),qx,qy,1.0,0.0};
  g_preserve_current_calibration = true;
  return true;
}

std::array<double,11> control_calibration_snapshot() {
  std::scoped_lock lock(g_control_mutex);
  const auto& r=g_control_core.response_estimator();
  const auto& range = r.delay_range();
  return {r.x().response,r.y().response,double(r.delay_us()),r.x().quality,r.y().quality,
      r.x().trusted && r.y().trusted ? 1.0 : 0.0,double(r.updates()),
      range.identifiable?1.0:0.0,double(range.minimum_us),double(range.maximum_us),double(range.candidates)};
}

}  // namespace vfdual_android
