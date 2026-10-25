#pragma once
#include "InferenceDeadlineAdmission.hpp"
#include "DecoderCallbackOwnership.hpp"

#ifndef VF_NATIVE_DECODER_REPORT_ONLY
#include <jni.h>
#include <media/NdkMediaCodec.h>
#endif

#include <algorithm>
#include <array>
#include <sstream>
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "DecoderInputDropMetrics.hpp"
#include "AtomicDurationSummary.hpp"
#include "FixedMetricRing.hpp"
#include "InferenceGenerationPolicy.hpp"
#include "PipelineTimingTrace.hpp"
#include "PipelineOutcomeTrace.hpp"
#include "ProjectionTimingTrace.hpp"
#include "YoloPostprocessor.hpp"
#include "vfdual/image_preprocessor.hpp"

struct ANativeWindow;

namespace vfdual_android {

/** Decoder-state copy: formatting, Java method invocation and Bridge work run
 * outside mutex_. The required JNI local-reference pin stays inside its hold. */
struct NativeDecoderReportSnapshot final {
  std::uint64_t now_us_{};
  std::uint64_t last_qnn_success_monotonic_us_{};
  std::uint64_t last_qnn_failure_monotonic_us_{};
  bool configured_{};
  std::uint64_t queued_access_units_{};
  std::uint64_t sync_access_units_{};
  std::size_t pending_access_units_{};
  std::uint32_t last_input_nal_type_mask_{};
  std::int32_t decoded_width_{};
  std::int32_t decoded_height_{};
  bool java_image_decoder_mode_{};
  std::string java_decoder_name_{};
  bool java_decoder_failed_{};
  std::string java_decoder_failure_{};
  std::uint64_t stream_restarts_{};
  std::uint64_t stream_restart_failures_{};
  bool stream_restart_in_progress_{};
  std::uint64_t last_stream_restart_us_{};
  std::uint64_t last_stream_restart_request_us_{};
  std::int32_t requested_color_format_{};
  std::int32_t output_color_format_{};
  std::int32_t output_color_standard_{};
  std::int32_t output_color_range_{};
  std::int32_t image_y_row_stride_{};
  std::int32_t image_u_row_stride_{};
  std::int32_t image_v_row_stride_{};
  std::int32_t image_y_pixel_stride_{};
  std::int32_t image_u_pixel_stride_{};
  std::int32_t image_v_pixel_stride_{};
  std::int32_t image_crop_left_{};
  std::int32_t image_crop_top_{};
  std::int32_t image_crop_right_{};
  std::int32_t image_crop_bottom_{};
  std::uint64_t image_layout_failures_{};
  std::uint64_t rejected_output_images_{};
  std::uint64_t acquired_images_{};
  std::uint64_t rendered_frames_{};
  std::uint64_t fresh_content_outputs_{};
  std::uint64_t repeated_content_outputs_{};
  std::uint64_t stale_decoded_outputs_{};
  std::uint64_t superseded_decoded_outputs_{};
  std::uint64_t superseded_preprocessed_frames_{};
  std::size_t inference_queue_capacity_{};
  std::uint64_t inference_queue_high_watermark_{};
  std::uint64_t stale_pre_inference_frames_{};
  std::uint64_t maximum_pre_inference_frame_age_us_{};
  std::uint64_t deadline_replaced_pre_inference_frames_{};
  std::uint64_t deadline_replacement_waits_{};
  std::uint64_t deadline_replacement_fallbacks_{};
  std::uint64_t deadline_maximum_replacement_wait_us_{};
  std::uint64_t deadline_backend_estimate_us_{};
  std::uint64_t stale_generation_inference_suppressions_{};
  std::uint64_t stale_generation_result_suppressions_{};
  std::uint64_t stale_generation_control_suppressions_{};
  std::uint64_t repeated_content_inferences_{};
  std::uint64_t repeated_content_inference_suppressions_{};
  std::uint64_t repeated_content_control_suppressions_{};
  std::uint64_t dropped_access_units_{};
  DecoderInputDropMetrics input_drop_metrics_{};
  std::uint64_t qnn_executions_{};
  std::uint64_t qnn_failures_{};
  std::uint64_t consecutive_qnn_failures_{};
  std::uint64_t preprocess_failures_{};
  std::uint32_t last_detection_count_{};
  std::uint64_t last_detection_source_generation_{};
  std::uint64_t last_detection_source_sequence_{};
  std::uint32_t last_detection_source_wire_frame_id_{};
  std::uint64_t last_detection_source_observed_us_{};
  std::uint64_t pixel_projection_mode_{};
  std::size_t rgb_input_buffer_count_{};
  std::uint64_t pixel_projection_candidates_{};
  std::uint64_t pixel_projection_attempts_{};
  std::uint64_t pixel_projection_complete_{};
  std::uint64_t pixel_projection_math_rejections_{};
  std::uint64_t pixel_projection_gate_rejections_{};
  std::uint64_t pixel_projection_control_publications_{};
  std::uint64_t pixel_projection_old_source_skips_{};
  std::uint64_t pixel_projection_shadow_compared_{};
  std::uint64_t pixel_projection_shadow_unscored_{};
  std::uint32_t last_qnn_status_{};
  FixedMetricRing<std::uint64_t,300U> preprocess_samples_us_{};
  std::uint64_t last_preprocess_us_{};
  FixedMetricRing<std::uint64_t,300U> qnn_samples_us_{};
  std::uint64_t last_qnn_us_{};
  FixedMetricRing<std::uint64_t,300U> inference_total_samples_us_{};
  std::uint64_t last_inference_total_us_{};
  FixedMetricRing<std::uint64_t,300U> decode_queue_samples_us_{};
  FixedMetricRing<std::uint64_t,300U> receive_to_result_samples_us_{};
  FixedMetricRing<std::uint64_t,300U> host_plus_receive_to_result_samples_us_{};
  FixedMetricRing<std::uint64_t,300U> control_dispatch_samples_us_{};
  FixedMetricRing<std::uint64_t,300U> background_motion_samples_us_{};
  FixedMetricRing<std::uint64_t,300U> image_motion_samples_us_{};
  std::uint64_t image_motion_budget_exhaustions_{};
  FixedMetricRing<std::uint64_t,300U> receive_to_control_samples_us_{};
  std::size_t last_detections_total_{};
  std::array<YoloDetection,5U> last_detections_{};
};

template <std::size_t Capacity>
inline std::string native_decoder_report_percentile_summary(
    const FixedMetricRing<std::uint64_t, Capacity>& samples) {
  if (samples.empty()) return "n=0";
  std::array<std::uint64_t, Capacity> sorted{};
  for (std::size_t index = 0; index < samples.size(); ++index) {
    sorted[index] = samples.oldest_at(index);
  }
  std::sort(sorted.begin(), sorted.begin() + samples.size());
  const auto percentile = [&sorted, sample_count = samples.size()](
                              std::size_t numerator) {
    return sorted[(sample_count - 1U) * numerator / 100U];
  };
  return "n=" + std::to_string(samples.size()) + " p50_us=" + std::to_string(percentile(50)) +
         " p95_us=" + std::to_string(percentile(95)) +
         " p99_us=" + std::to_string(percentile(99));
}

/** Pure text contract. External strings are snapshots from independent subsystems. */
inline std::string format_native_decoder_report_snapshot(
    const NativeDecoderReportSnapshot& snapshot, std::string_view inference_backend,
    std::string_view java_input_health, std::string_view bridge_report,
    std::string_view submit_summaries) {
  const auto now_us=snapshot.now_us_;
  const auto qnn_success_age_ms = snapshot.last_qnn_success_monotonic_us_ == 0 ||
          now_us < snapshot.last_qnn_success_monotonic_us_
      ? -1LL
      : static_cast<long long>((now_us - snapshot.last_qnn_success_monotonic_us_) / 1'000U);
  const auto qnn_failure_age_ms = snapshot.last_qnn_failure_monotonic_us_ == 0 ||
          now_us < snapshot.last_qnn_failure_monotonic_us_
      ? -1LL
      : static_cast<long long>((now_us - snapshot.last_qnn_failure_monotonic_us_) / 1'000U);
  std::ostringstream value;
  value << "configured=" << snapshot.configured_
        << " queued_access_units=" << snapshot.queued_access_units_
        << " sync_access_units=" << snapshot.sync_access_units_
        << " pending_access_units=" << snapshot.pending_access_units_
        << " last_input_nal_type_mask=" << snapshot.last_input_nal_type_mask_
        << " decoded_size=" << snapshot.decoded_width_ << 'x' << snapshot.decoded_height_
        << " output_path=" << (snapshot.java_image_decoder_mode_ ? "java_getOutputImage" : "surface")
        << " decoder_name=" << snapshot.java_decoder_name_
        << " decoder_failed=" << snapshot.java_decoder_failed_
        << " decoder_failure=" << (snapshot.java_decoder_failure_.empty() ? "none" : snapshot.java_decoder_failure_)
        << " stream_restarts=" << snapshot.stream_restarts_
        << " stream_restart_failures=" << snapshot.stream_restart_failures_
        << " stream_restart_in_progress=" << snapshot.stream_restart_in_progress_
        << " last_stream_restart_us=" << snapshot.last_stream_restart_us_
        << " last_stream_restart_request_us=" << snapshot.last_stream_restart_request_us_
        << " requested_color_format=" << snapshot.requested_color_format_
        << " output_color_format=" << snapshot.output_color_format_
        << " color_standard=" << snapshot.output_color_standard_ << " color_range=" << snapshot.output_color_range_
        << " image_row_stride=" << snapshot.image_y_row_stride_ << '/' << snapshot.image_u_row_stride_ << '/'
        << snapshot.image_v_row_stride_ << " image_pixel_stride=" << snapshot.image_y_pixel_stride_ << '/'
        << snapshot.image_u_pixel_stride_ << '/' << snapshot.image_v_pixel_stride_
        << " image_crop=" << snapshot.image_crop_left_ << '/' << snapshot.image_crop_top_ << '/'
        << snapshot.image_crop_right_ << '/' << snapshot.image_crop_bottom_
        << " image_layout_failures=" << snapshot.image_layout_failures_
        << " rejected_output_images=" << snapshot.rejected_output_images_
         << " acquired_images=" << snapshot.acquired_images_
         << " rendered_frames=" << snapshot.rendered_frames_
         << " fresh_content_outputs=" << snapshot.fresh_content_outputs_
         << " repeated_content_outputs=" << snapshot.repeated_content_outputs_
         << " stale_decoded_outputs=" << snapshot.stale_decoded_outputs_
        << " superseded_decoded_outputs=" << snapshot.superseded_decoded_outputs_
        << " superseded_preprocessed_frames=" << snapshot.superseded_preprocessed_frames_
        << " inference_queue_capacity=" << snapshot.inference_queue_capacity_
        << " inference_queue_high_watermark="
        << snapshot.inference_queue_high_watermark_
        << " stale_pre_inference_frames="
        << snapshot.stale_pre_inference_frames_
        << " maximum_pre_inference_frame_age_us="
        << snapshot.maximum_pre_inference_frame_age_us_
        << " deadline_replaced_pre_inference_frames=" << snapshot.deadline_replaced_pre_inference_frames_
        << " deadline_replacement_waits=" << snapshot.deadline_replacement_waits_
        << " deadline_replacement_fallbacks=" << snapshot.deadline_replacement_fallbacks_
        << " deadline_maximum_replacement_wait_us=" << snapshot.deadline_maximum_replacement_wait_us_
        << " deadline_backend_estimate_us=" << snapshot.deadline_backend_estimate_us_
        << " stale_generation_inference_suppressions="
        << snapshot.stale_generation_inference_suppressions_
        << " stale_generation_result_suppressions="
        << snapshot.stale_generation_result_suppressions_
         << " stale_generation_control_suppressions="
         << snapshot.stale_generation_control_suppressions_
         << " repeated_content_inferences=" << snapshot.repeated_content_inferences_
         << " repeated_content_inference_suppressions="
         << snapshot.repeated_content_inference_suppressions_
         << " repeated_content_control_suppressions="
         << snapshot.repeated_content_control_suppressions_
        << " dropped_access_units=" << snapshot.dropped_access_units_
        << " input_drop_no_buffer=" << snapshot.input_drop_metrics_.no_input_buffer
        << " input_drop_stale_before_codec="
        << snapshot.input_drop_metrics_.stale_before_codec
        << " input_drop_queue_capacity=" << snapshot.input_drop_metrics_.queue_capacity
        << " input_drop_buffer_capacity=" << snapshot.input_drop_metrics_.buffer_capacity
        << " input_drop_codec_exception=" << snapshot.input_drop_metrics_.codec_exception
        << " input_drop_state_exception=" << snapshot.input_drop_metrics_.state_exception
        << " input_drop_restart=" << snapshot.input_drop_metrics_.restart
        << " input_drop_reference_recovery=" << snapshot.input_drop_metrics_.reference_recovery
        << " input_drop_not_running=" << snapshot.input_drop_metrics_.not_running
        << " input_drop_invalid=" << snapshot.input_drop_metrics_.invalid
        << " input_drop_bridge_failure=" << snapshot.input_drop_metrics_.bridge_failure
        << " input_drop_fatal_flush=" << snapshot.input_drop_metrics_.fatal_flush
        << " input_drop_recovery_flush=" << snapshot.input_drop_metrics_.recovery_flush
        << " input_drop_other=" << snapshot.input_drop_metrics_.other
         << " inference_backend=" << inference_backend
         << " qnn_executions=" << snapshot.qnn_executions_ << " qnn_failures=" << snapshot.qnn_failures_
        << " consecutive_qnn_failures=" << snapshot.consecutive_qnn_failures_
        << " last_qnn_success_age_ms=" << qnn_success_age_ms
        << " last_qnn_failure_age_ms=" << qnn_failure_age_ms
        << " preprocess_failures=" << snapshot.preprocess_failures_ << " last_detection_count=" << snapshot.last_detection_count_
        << " last_detection_source_generation=" << snapshot.last_detection_source_generation_
        << " last_detection_source_sequence=" << snapshot.last_detection_source_sequence_
        << " last_detection_source_wire_frame_id=" << snapshot.last_detection_source_wire_frame_id_
        << " last_detection_source_observed_us=" << snapshot.last_detection_source_observed_us_
        << " pixel_projection_mode=" << snapshot.pixel_projection_mode_
        << " pixel_projection_rgb_pool_buffers=" << snapshot.rgb_input_buffer_count_
        << " pixel_projection_candidates=" << snapshot.pixel_projection_candidates_
        << " pixel_projection_attempts=" << snapshot.pixel_projection_attempts_
        << " pixel_projection_complete=" << snapshot.pixel_projection_complete_
        << " pixel_projection_math_rejections=" << snapshot.pixel_projection_math_rejections_
        << " pixel_projection_gate_rejections=" << snapshot.pixel_projection_gate_rejections_
        << " pixel_projection_control_publications=" << snapshot.pixel_projection_control_publications_
        << " pixel_projection_old_source_skips=" << snapshot.pixel_projection_old_source_skips_
        << " pixel_projection_shadow_compared=" << snapshot.pixel_projection_shadow_compared_
        << " pixel_projection_shadow_unscored=" << snapshot.pixel_projection_shadow_unscored_
        << " last_qnn_status=" << snapshot.last_qnn_status_ << " preprocess{" << native_decoder_report_percentile_summary(snapshot.preprocess_samples_us_)
        << " last_us=" << snapshot.last_preprocess_us_ << "} qnn{" << native_decoder_report_percentile_summary(snapshot.qnn_samples_us_)
        << " last_us=" << snapshot.last_qnn_us_ << "} inference_total{"
        << native_decoder_report_percentile_summary(snapshot.inference_total_samples_us_)
        << " last_us=" << snapshot.last_inference_total_us_ << "} decode_queue{"
        << native_decoder_report_percentile_summary(snapshot.decode_queue_samples_us_) << "}"
        << " receive_to_result{" << native_decoder_report_percentile_summary(snapshot.receive_to_result_samples_us_) << "}"
        << " host_plus_receive_to_result{" << native_decoder_report_percentile_summary(snapshot.host_plus_receive_to_result_samples_us_) << "}"
        << " control_dispatch{" << native_decoder_report_percentile_summary(snapshot.control_dispatch_samples_us_) << "}"
        << " background_motion{" << native_decoder_report_percentile_summary(snapshot.background_motion_samples_us_) << "}"
        << " target_image_motion{" << native_decoder_report_percentile_summary(snapshot.image_motion_samples_us_) << "}"
        << " target_image_motion_budget_us=2500 target_image_motion_budget_exhaustions=" << snapshot.image_motion_budget_exhaustions_
        << " receive_to_control{" << native_decoder_report_percentile_summary(snapshot.receive_to_control_samples_us_) << "}"
        << " timing_scope=phone_receive_to_local_control_dispatch_not_host_or_physical_ack"
        << " input_health={" << java_input_health << "}";
  value << submit_summaries;
  value << " last_detections=[";
  constexpr std::size_t kReportDetectionLimit = 5;
  for (std::size_t index = 0; index < std::min(snapshot.last_detections_total_, kReportDetectionLimit); ++index) {
    const auto& detection = snapshot.last_detections_[index];
    if (index != 0) value << ',';
    value << "{class=" << detection.class_id << ",confidence=" << detection.confidence
          << ",xyxy=" << detection.x1 << '/' << detection.y1 << '/' << detection.x2 << '/' << detection.y2 << '}';
  }
  if (snapshot.last_detections_total_ > kReportDetectionLimit) value << ",...";
  value << ']';
  value << " makcu_bridge{" << bridge_report << '}';
  return value.str();
}

#ifndef VF_NATIVE_DECODER_REPORT_ONLY
/** Surface decoder consumer. Network code only sees submit(), never MediaCodec state. */
class NativeH264Decoder final {
public:
  static constexpr std::uint64_t kStreamRestartTimeoutUs = 250'000U;

  struct StreamRestartCompletion final {
    std::uint64_t generation{};
    bool succeeded{};
    std::uint64_t elapsed_us{};
  };

  [[nodiscard]] bool configure(JNIEnv* environment, jobject surface, std::int32_t width, std::int32_t height);
  [[nodiscard]] bool configure_for_inference(std::int32_t width, std::int32_t height);
  void bind_java_decoder(JNIEnv* environment, jobject decoder);
  [[nodiscard]] bool submit(
      std::span<const std::byte> access_unit, std::uint64_t presentation_us,
      std::uint32_t frame_id, std::uint64_t control_sequence,
      bool content_updated, std::uint64_t observed_at_us = UINT64_MAX);
  [[nodiscard]] bool process_java_yuv420(
      const vfdual::Yuv420PlaneView& y_plane, const vfdual::Yuv420PlaneView& u_plane,
      const vfdual::Yuv420PlaneView& v_plane, std::int32_t image_width,
      std::int32_t image_height, std::int32_t crop_left, std::int32_t crop_top,
      std::int32_t crop_right, std::int32_t crop_bottom,
      std::int64_t presentation_time_us);
  void update_java_decoder_format(
      std::string codec_name, std::int32_t color_format, std::int32_t color_standard,
      std::int32_t color_range, std::int32_t width, std::int32_t height,
      std::optional<DecoderCallbackToken> token=std::nullopt);
  [[nodiscard]] std::array<std::uint64_t,2> java_callback_context() const;
  [[nodiscard]] bool publish_java_codec(DecoderCallbackToken token);
  [[nodiscard]] bool has_fatal_decoder_failure() const;
  void discard_java_decoded_output(
      std::int64_t presentation_time_us, std::string reason, bool fatal,std::optional<DecoderCallbackToken> token=std::nullopt);
  void discard_java_compressed_access_unit(
      std::int64_t presentation_time_us, std::string reason, bool fatal,std::optional<DecoderCallbackToken> token=std::nullopt);
  /** Recreates only MediaCodec; QNN and the bound UDP socket stay live. */
  [[nodiscard]] bool restart_after_stream_discontinuity();
  void complete_stream_restart(
      std::uint64_t generation, bool succeeded, std::uint64_t elapsed_us,
      std::string diagnostic);
  /** Consumes the current-generation completion edge exactly once. */
  [[nodiscard]] std::optional<StreamRestartCompletion> take_stream_restart_completion();
  [[nodiscard]] bool has_stream_restart_timed_out();
  /** Fails closed before a full pipeline rebuild, without touching Java inline. */
  void mark_stream_discontinuity();
  void stop();
  [[nodiscard]] std::string report() const;
  /**
   * Enables a bounded, explicitly requested offline trace. It is intentionally disabled by
   * default so the real-time control path never serializes per-frame detection data.
   */
  void begin_detection_trace(std::size_t maximum_frames);
  [[nodiscard]] std::string detection_trace_csv() const;
  [[nodiscard]] std::string pipeline_metrics_csv() const;
  [[nodiscard]] std::string drain_pipeline_timing_trace();
  ~NativeH264Decoder();

  void coalesce_java_decoded_output(std::int64_t older_us, std::int64_t newer_us);
private:
  void output_loop();
  void inference_loop();
  void process_output_locked(std::size_t output_index, const AMediaCodecBufferInfo& output);
  [[nodiscard]] JavaAccessUnitOfferResult submit_to_java(
      std::span<const std::byte> access_unit, std::uint64_t presentation_us, bool sync_frame,
      std::uint64_t stream_generation, std::uint64_t binding_generation);
  /** Caller owns a thread-local JNI reference; no decoder-state lock is held. */
  [[nodiscard]] static std::string java_input_health_report(JNIEnv*, jobject, jmethodID);
  void update_output_format_locked();

  struct PendingFrame {
    std::int64_t presentation_us{};
    std::uint32_t frame_id{};
    std::uint64_t control_sequence{};
    bool content_updated{};
    std::uint64_t stream_generation{};
    std::uint64_t observed_at_us{};
  };

  struct DetectionTraceFrame {
    std::uint32_t frame_id{};
    std::vector<YoloDetection> detections;
  };

  struct PendingInferenceFrame {
    std::vector<std::uint8_t> input;
    std::uint32_t frame_id{};
    std::uint64_t control_sequence{};
    bool content_updated{};
    std::uint64_t stream_generation{};
    std::uint64_t observed_at_us{};
    std::uint64_t received_at_us{};
    std::uint64_t preprocess_us{};
    std::uint64_t decode_queue_us{};
    std::uint64_t decoded_planes_us{};
    std::uint64_t preprocess_started_us{};
    std::uint64_t preprocess_finished_us{};
    std::uint64_t enqueued_us{};
  };

  // The output callback is serialized by java_output_callback_mutex_. The
  // marker is protected by inference_mutex_ and exists only during preprocessing.
  class PreprocessingLease final {
   public:
    PreprocessingLease(NativeH264Decoder& owner,const PendingFrame& frame,std::uint64_t started);
    ~PreprocessingLease();
    PreprocessingLease(const PreprocessingLease&)=delete;
    PreprocessingLease& operator=(const PreprocessingLease&)=delete;
   private:
    NativeH264Decoder& owner_;
  };

  struct PipelineMetric {
    std::uint32_t frame_id{};
    std::uint64_t decode_queue_us{};
    std::uint64_t preprocess_us{};
    std::uint64_t qnn_us{};
    std::uint32_t detections{};
    std::uint32_t qnn_status{};
  };
  void record_outcome(const PendingInferenceFrame& frame, PipelineOutcome reason,
                      std::uint64_t at, std::uint64_t detail=0);
  void record_outcome(const PendingFrame& frame, PipelineOutcome reason,
                      std::uint64_t detail=0);
  void record_projection(ProjectionTimingTrace::Row row);
  void record_projection_target(ProjectionTimingTrace::TargetRow row);
  mutable std::mutex outcome_mutex_;
  PipelineOutcomeTrace outcome_trace_;
  mutable std::mutex projection_trace_mutex_;
  ProjectionTimingTrace projection_timing_trace_;

  struct QnnSuccessCommit final {
    bool committed{};
    std::optional<std::uint64_t> resume_generation;
    std::uint64_t execution_count{};
  };

  static constexpr std::size_t kLatencySampleCapacity = 300U;
  static constexpr std::size_t kPipelineMetricCapacity = 600U;

  [[nodiscard]] std::optional<PendingFrame> take_pending_frame_for_output_locked(std::int64_t presentation_us);
  void append_detection_trace_locked(std::uint32_t frame_id);
  /** mutex_ must be held; restart/failure state is part of the generation contract. */
  [[nodiscard]] bool is_current_stream_generation_locked(
      std::uint64_t generation) const noexcept;
  /** Atomically rejects a late old-generation failure or commits current state. */
  [[nodiscard]] std::optional<std::uint64_t> commit_qnn_failure(
      std::uint64_t generation, std::uint32_t graph_status,
      std::uint64_t failed_at_us);
  /** Atomically publishes metrics/detections only for the active generation. */
  [[nodiscard]] QnnSuccessCommit commit_qnn_success(
      const PendingInferenceFrame& frame, std::uint64_t qnn_us,
      std::uint32_t graph_status, std::uint32_t detection_count,
      std::uint64_t inference_total_us, std::uint64_t completed_at_us,
      std::vector<YoloDetection>& detections);
  /** inference_mutex_ must be held; preserves every reusable input allocation. */
  void recycle_pending_inference_frames_locked();

  mutable std::mutex mutex_;
  /** Serializes Java output-buffer use with stop/reconfigure, not UDP state access. */
  std::mutex java_output_callback_mutex_;
  std::thread output_worker_;
  std::atomic_bool output_worker_running_{false};
  std::thread inference_worker_;
  std::atomic_bool inference_worker_running_{false};
  std::mutex inference_mutex_;
  std::condition_variable inference_condition_;
  AMediaCodec* codec_{};
  ANativeWindow* output_window_{};
  bool output_window_owned_{};
  JavaVM* java_vm_{};
  std::uint64_t java_decoder_binding_generation_{};
  DecoderCallbackOwnership java_callback_ownership_{};
  jobject java_decoder_object_{};
  jmethodID java_offer_access_unit_method_{};
  jmethodID java_has_fatal_failure_method_{};
  jmethodID java_fatal_diagnostic_method_{};
  jmethodID java_restart_after_discontinuity_method_{};
  jmethodID java_input_health_report_method_{};
  bool java_image_decoder_mode_{};
  bool java_decoder_failed_{};
  std::string java_decoder_name_{"unconfigured"};
  std::string java_decoder_failure_{};
  vfdual::ModelInputDimensions model_dimensions_{};
  std::vector<std::uint8_t> model_input_;
  std::deque<PendingInferenceFrame> pending_inference_frames_;
  std::vector<std::vector<std::uint8_t>> free_model_inputs_;
  InferenceDeadlineAdmission::Preparing preprocessing_input_{};
  std::int32_t output_color_format_{};
  std::int32_t output_color_standard_{1};
  std::int32_t output_color_range_{2};
  std::int32_t decoded_width_{};
  std::int32_t decoded_height_{};
  std::int32_t output_stride_{};
  std::int32_t output_slice_height_{};
  std::int32_t output_crop_left_{};
  std::int32_t output_crop_top_{};
  std::int32_t output_crop_right_{};
  std::int32_t output_crop_bottom_{};
  std::int32_t image_crop_left_{};
  std::int32_t image_crop_top_{};
  std::int32_t image_crop_right_{};
  std::int32_t image_crop_bottom_{};
  std::int32_t image_y_row_stride_{};
  std::int32_t image_u_row_stride_{};
  std::int32_t image_v_row_stride_{};
  std::int32_t image_y_pixel_stride_{};
  std::int32_t image_u_pixel_stride_{};
  std::int32_t image_v_pixel_stride_{};
  std::uint64_t acquired_images_{};
  std::uint64_t image_layout_failures_{};
  std::uint64_t rejected_output_images_{};
  std::uint64_t queued_access_units_{};
  std::uint64_t sync_access_units_{};
  std::uint64_t rendered_frames_{};
  std::uint64_t fresh_content_outputs_{};
  std::uint64_t repeated_content_outputs_{};
  std::uint64_t stale_decoded_outputs_{};
  std::uint64_t superseded_decoded_outputs_{};
  std::uint64_t superseded_preprocessed_frames_{};
  std::atomic_uint64_t stale_pre_inference_frames_{};
  std::atomic_uint64_t inference_queue_high_watermark_{};
  std::atomic_uint64_t maximum_pre_inference_frame_age_us_{};
  std::atomic_uint64_t deadline_replaced_pre_inference_frames_{};
  std::atomic_uint64_t deadline_replacement_waits_{};
  std::atomic_uint64_t deadline_replacement_fallbacks_{};
  std::atomic_uint64_t deadline_maximum_replacement_wait_us_{};
  std::atomic_uint64_t deadline_backend_estimate_us_{};
  mutable AtomicDurationSummary report_snapshot_mutex_wait_, report_snapshot_mutex_hold_;
  AtomicDurationSummary submit_duration_, submit_first_mutex_wait_, submit_second_mutex_wait_, submit_java_offer_duration_;
  std::atomic_uint64_t pixel_projection_mode_{};
  std::atomic_uint64_t pixel_projection_candidates_{}, pixel_projection_attempts_{}, pixel_projection_complete_{};
  std::atomic_uint64_t pixel_projection_math_rejections_{}, pixel_projection_gate_rejections_{};
  std::atomic_uint64_t pixel_projection_control_publications_{}, pixel_projection_old_source_skips_{};
  std::atomic_uint64_t pixel_projection_shadow_compared_{}, pixel_projection_shadow_unscored_{};
  std::uint64_t stale_generation_inference_suppressions_{};
  std::uint64_t stale_generation_result_suppressions_{};
  std::uint64_t stale_generation_control_suppressions_{};
  std::uint64_t repeated_content_inferences_{};
  std::uint64_t repeated_content_inference_suppressions_{};
  std::uint64_t repeated_content_control_suppressions_{};
  std::uint64_t dropped_access_units_{};
  DecoderInputDropMetrics input_drop_metrics_{};
  std::uint64_t qnn_executions_{};
  std::uint64_t qnn_failures_{};
  std::uint64_t consecutive_qnn_failures_{};
  std::uint64_t last_qnn_success_monotonic_us_{};
  std::uint64_t last_qnn_failure_monotonic_us_{};
  std::uint64_t preprocess_failures_{};
  std::uint64_t stream_restarts_{};
  std::uint64_t stream_restart_failures_{};
  std::uint64_t last_stream_restart_us_{};
  std::uint64_t last_stream_restart_request_us_{};
  std::uint64_t stream_restart_generation_{};
  std::uint64_t stream_restart_started_us_{};
  bool stream_restart_in_progress_{};
  std::optional<StreamRestartCompletion> pending_stream_restart_completion_;
  std::optional<std::uint64_t> pending_makcu_resume_generation_;
  FixedMetricRing<std::uint64_t, kLatencySampleCapacity> preprocess_samples_us_;
  FixedMetricRing<std::uint64_t, kLatencySampleCapacity> qnn_samples_us_;
  FixedMetricRing<std::uint64_t, kLatencySampleCapacity> inference_total_samples_us_;
  FixedMetricRing<std::uint64_t, kLatencySampleCapacity> decode_queue_samples_us_;
  FixedMetricRing<std::uint64_t, kLatencySampleCapacity> receive_to_result_samples_us_;
  FixedMetricRing<std::uint64_t, kLatencySampleCapacity> host_plus_receive_to_result_samples_us_;
  FixedMetricRing<std::uint64_t, kLatencySampleCapacity> control_dispatch_samples_us_;
  FixedMetricRing<std::uint64_t, kLatencySampleCapacity> background_motion_samples_us_;
  FixedMetricRing<std::uint64_t, kLatencySampleCapacity> image_motion_samples_us_;
  std::uint64_t image_motion_budget_exhaustions_{};
  FixedMetricRing<std::uint64_t, kLatencySampleCapacity> receive_to_control_samples_us_;
  std::uint64_t last_preprocess_us_{};
  std::uint64_t last_qnn_us_{};
  std::uint64_t last_inference_total_us_{};
  std::uint32_t last_input_nal_type_mask_{};
  std::uint32_t last_detection_count_{};
  std::uint64_t last_detection_source_generation_{}, last_detection_source_sequence_{}, last_detection_source_observed_us_{};
  std::uint32_t last_detection_source_wire_frame_id_{};
  std::vector<YoloDetection> last_detections_;
  std::uint32_t last_qnn_status_{0xffffffffU};
  std::deque<PendingFrame> pending_frames_;
  bool detection_trace_enabled_{};
  std::size_t detection_trace_maximum_frames_{};
  std::vector<DetectionTraceFrame> detection_trace_frames_;
  FixedMetricRing<PipelineMetric, kPipelineMetricCapacity> pipeline_metrics_;
  PipelineTimingTrace pipeline_timing_trace_;
};

NativeH264Decoder& decoder();
#endif  // VF_NATIVE_DECODER_REPORT_ONLY

}  // namespace vfdual_android
