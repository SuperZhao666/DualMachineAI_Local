#include "NativeH264Decoder.hpp"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>

#include "QnnRealtimeExecutor.hpp"
#include "DecodedObservationAdmission.hpp"
#include "MakcuMoveBridge.hpp"
#include "MakcuOutputGate.hpp"
#include "MobileControlCore.hpp"
#include "MobileFreshPixelProjection.hpp"
#include "ProjectionObservationGate.hpp"
#include "vfdual/h264_access_unit.hpp"
#include "vfdual/image_preprocessor.hpp"

namespace vfdual_android {
namespace {
constexpr std::int32_t kColorFormatYuv420Flexible = 0x7f420888;
constexpr std::int32_t kColorStandardBt709 = 1;
constexpr std::int32_t kColorStandardBt601Pal = 2;
constexpr std::int32_t kColorStandardBt601Ntsc = 4;
constexpr std::int32_t kColorRangeLimited = 2;
constexpr std::int32_t kMaximumDecodedWidth = 3840;
constexpr std::int32_t kMaximumDecodedHeight = 2160;
// Match the single-machine latest-frame slot: one frame may execute while only
// the newest eligible decoded observation waits.  More pending frames add
// latency after a scheduler hiccup without increasing single-worker throughput.
constexpr std::size_t kInferenceQueueCapacity = 1;
constexpr std::size_t kRgbInputBufferCount = 4;

int requested_pixel_projection_mode() noexcept {
  char value[PROP_VALUE_MAX]{};
  if (__system_property_get("debug.visionforge.pixel_projection", value) <= 0) return 0;
  return value[0] == '1' && value[1] == 0 ? 1 : value[0] == '2' && value[1] == 0 ? 2 : 0;
}

constexpr std::int64_t kInputPollTimeoutUs = 1'000;
constexpr std::int64_t kOutputWorkerPollTimeoutUs = 1'000;

JNIEnv* current_thread_environment(JavaVM* vm) {
  if (vm == nullptr) return nullptr;
  struct ThreadAttachment final {
    JavaVM* vm{};
    JNIEnv* environment{};
    bool attached{};
    ~ThreadAttachment() {
      if (attached && vm != nullptr) vm->DetachCurrentThread();
    }
  };
  thread_local ThreadAttachment attachment;
  if (attachment.vm == vm && attachment.environment != nullptr) return attachment.environment;
  JNIEnv* environment{};
  if (vm->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6) == JNI_OK) {
    attachment = {vm, environment, false};
    return environment;
  }
  if (vm->AttachCurrentThread(&environment, nullptr) != JNI_OK) return nullptr;
  attachment.vm = vm;
  attachment.environment = environment;
  attachment.attached = true;
  return environment;
}

template <std::size_t Capacity>
void record_latency(
    FixedMetricRing<std::uint64_t, Capacity>& samples,
    std::uint64_t value) noexcept {
  (void)samples.push(value);
}



}
bool NativeH264Decoder::configure(JNIEnv* environment, jobject surface, std::int32_t width, std::int32_t height) {
  if (surface == nullptr || width <= 0 || height <= 0) return false;
  stop();
  std::scoped_lock lock(mutex_);
  output_window_ = ANativeWindow_fromSurface(environment, surface);
  if (output_window_ == nullptr) return false;
  output_window_owned_ = true;
  codec_ = AMediaCodec_createDecoderByType("video/avc");
  if (codec_ == nullptr) { ANativeWindow_release(output_window_); output_window_ = nullptr; return false; }
  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
  // The host captures the active desktop, while the model always consumes a
  // model-shaped image. Permit MediaCodec to adapt to the H.264 SPS
  // source size instead of treating the model input dimensions as video size.
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_WIDTH, kMaximumDecodedWidth);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_HEIGHT, kMaximumDecodedHeight);
  const media_status_t configured = AMediaCodec_configure(codec_, format, output_window_, nullptr, 0);
  AMediaFormat_delete(format);
  if (configured != AMEDIA_OK || AMediaCodec_start(codec_) != AMEDIA_OK) {
    AMediaCodec_delete(codec_); codec_ = nullptr;
    ANativeWindow_release(output_window_); output_window_ = nullptr;
    return false;
  }
  queued_access_units_ = 0; rendered_frames_ = 0; stale_decoded_outputs_ = 0;
  dropped_access_units_ = 0;
  input_drop_metrics_.reset();
  output_worker_running_ = true;
  output_worker_ = std::thread(&NativeH264Decoder::output_loop, this);
  return true;
}

bool NativeH264Decoder::configure_for_inference(std::int32_t width, std::int32_t height) {
  if (width <= 0 || height <= 0) return false;
  stop();
  std::scoped_lock lock(mutex_);
  if (java_vm_ == nullptr || java_decoder_object_ == nullptr ||
      java_offer_access_unit_method_ == nullptr) {
    __android_log_print(
        ANDROID_LOG_ERROR, "VisionForgeDual", "Java Image.Plane decoder bridge is not bound");
    return false;
  }
  model_dimensions_ = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 3U, 2U};
  const auto input_bytes = vfdual::model_input_bytes(model_dimensions_);
  model_input_.resize(input_bytes);
  java_image_decoder_mode_ = true;
  java_decoder_failed_ = false;
  java_decoder_failure_.clear();
  output_color_format_ = kColorFormatYuv420Flexible;
  output_color_standard_ = kColorStandardBt709;
  output_color_range_ = kColorRangeLimited;
  decoded_width_ = width;
  decoded_height_ = height;
  output_stride_ = width;
  output_slice_height_ = height;
  output_crop_left_ = output_crop_top_ = 0;
  output_crop_right_ = width;
  output_crop_bottom_ = height;
  image_crop_left_ = image_crop_top_ = 0;
  image_crop_right_ = image_crop_bottom_ = 0;
  image_y_row_stride_ = image_u_row_stride_ = image_v_row_stride_ = 0;
  image_y_pixel_stride_ = image_u_pixel_stride_ = image_v_pixel_stride_ = 0;
  acquired_images_ = image_layout_failures_ = rejected_output_images_ = 0;
  queued_access_units_ = sync_access_units_ = rendered_frames_ =
      fresh_content_outputs_ = repeated_content_outputs_ = stale_decoded_outputs_ =
      superseded_preprocessed_frames_ =
      stale_generation_inference_suppressions_ =
      stale_generation_result_suppressions_ =
      stale_generation_control_suppressions_ =
      repeated_content_inferences_ = repeated_content_inference_suppressions_ =
      repeated_content_control_suppressions_ =
      dropped_access_units_ = qnn_executions_ = qnn_failures_ = preprocess_failures_ = 0;
  consecutive_qnn_failures_ = 0;
  stale_pre_inference_frames_.store(0, std::memory_order_relaxed);
  inference_queue_high_watermark_.store(0, std::memory_order_relaxed);
  maximum_pre_inference_frame_age_us_.store(0, std::memory_order_relaxed);
  deadline_replaced_pre_inference_frames_.store(0,std::memory_order_relaxed);
  deadline_replacement_waits_.store(0,std::memory_order_relaxed);
  deadline_replacement_fallbacks_.store(0,std::memory_order_relaxed);
  deadline_maximum_replacement_wait_us_.store(0,std::memory_order_relaxed);
  deadline_backend_estimate_us_.store(0,std::memory_order_relaxed);
  report_snapshot_mutex_wait_.reset();
  report_snapshot_mutex_hold_.reset();
  submit_duration_.reset();
  submit_first_mutex_wait_.reset();
  submit_second_mutex_wait_.reset();
  submit_java_offer_duration_.reset();
  pixel_projection_mode_=0;
  pixel_projection_candidates_=pixel_projection_attempts_=pixel_projection_complete_=0;
  pixel_projection_math_rejections_=pixel_projection_gate_rejections_=0;
  pixel_projection_control_publications_=pixel_projection_old_source_skips_=0;
  pixel_projection_shadow_compared_=pixel_projection_shadow_unscored_=0;
  last_qnn_success_monotonic_us_ = last_qnn_failure_monotonic_us_ = 0;
  input_drop_metrics_.reset();
  stream_restarts_ = stream_restart_failures_ = last_stream_restart_us_ = 0;
  last_stream_restart_request_us_ = stream_restart_started_us_ = 0;
  ++stream_restart_generation_;
  activate_makcu_stream_generation(stream_restart_generation_);
  stream_restart_in_progress_ = false;
  pending_stream_restart_completion_.reset();
  pending_makcu_resume_generation_.reset();
  java_decoder_name_ = "starting";
  last_detection_count_ = 0;
  last_detection_source_generation_=last_detection_source_sequence_=last_detection_source_observed_us_=0;
  last_detection_source_wire_frame_id_=0;
  last_detections_.clear();
  pending_frames_.clear();
  last_qnn_status_ = 0xffffffffU;
  last_input_nal_type_mask_ = 0;
  preprocess_samples_us_.clear(); qnn_samples_us_.clear();
  inference_total_samples_us_.clear(); decode_queue_samples_us_.clear();
  receive_to_result_samples_us_.clear(); control_dispatch_samples_us_.clear();
  background_motion_samples_us_.clear();
  host_plus_receive_to_result_samples_us_.clear();
  receive_to_control_samples_us_.clear();
  superseded_decoded_outputs_ = 0;
  pipeline_metrics_.clear();
  pending_inference_frames_.clear();
  free_model_inputs_.clear();
  // Exactly four owned RGB inputs: producer, executing A, detached carry B,
  // and newest pending C. No input is borrowed from the producer for projection.
  free_model_inputs_.resize(kRgbInputBufferCount - 1);
  for (auto& input : free_model_inputs_) input.resize(input_bytes);
  inference_worker_running_ = true;
  inference_worker_ = std::thread(&NativeH264Decoder::inference_loop, this);
  return true;
}

void NativeH264Decoder::bind_java_decoder(JNIEnv* environment, jobject decoder) {
  if (environment == nullptr || decoder == nullptr) return;
  JavaVM* vm{};
  if (environment->GetJavaVM(&vm) != JNI_OK || vm == nullptr) return;
  // Resolve a complete replacement before discarding the working binding.
  // NewGlobalRef/GetMethodID can fail with a pending JNI exception.
  jobject replacement = environment->NewGlobalRef(decoder);
  if (replacement == nullptr) {
    if (environment->ExceptionCheck()) environment->ExceptionClear();
    return;
  }
  jclass decoder_class = environment->GetObjectClass(decoder);
  const auto resolve = [&](const char* name, const char* signature) -> jmethodID {
    if (decoder_class == nullptr || environment->ExceptionCheck()) return nullptr;
    return environment->GetMethodID(decoder_class, name, signature);
  };
  const auto offer = resolve("offerAccessUnit", "(Ljava/nio/ByteBuffer;IJZ)I");
  const auto has_fatal = resolve("hasFatalFailure", "()Z");
  const auto fatal_diagnostic = resolve("fatalDiagnostic", "()Ljava/lang/String;");
  const auto restart = resolve("requestStreamRestart", "(IIJ)Z");
  const auto input_health = resolve("inputHealthReport", "()Ljava/lang/String;");
  if (decoder_class != nullptr) environment->DeleteLocalRef(decoder_class);
  const bool failed = environment->ExceptionCheck() || offer == nullptr || has_fatal == nullptr ||
      fatal_diagnostic == nullptr || restart == nullptr || input_health == nullptr;
  if (environment->ExceptionCheck()) environment->ExceptionClear();
  if (failed) {
    environment->DeleteGlobalRef(replacement);
    return;
  }
  std::scoped_lock lock(mutex_);
  if (java_decoder_object_ != nullptr) environment->DeleteGlobalRef(java_decoder_object_);
  java_decoder_object_ = replacement;
  java_vm_ = vm;
  java_offer_access_unit_method_ = offer;
  java_has_fatal_failure_method_ = has_fatal;
  java_fatal_diagnostic_method_ = fatal_diagnostic;
  java_restart_after_discontinuity_method_ = restart;
  java_input_health_report_method_ = input_health;
  ++java_decoder_binding_generation_;
}

bool NativeH264Decoder::submit(
    std::span<const std::byte> access_unit, std::uint64_t presentation_us,
    std::uint32_t frame_id, std::uint64_t control_sequence,
    bool content_updated, std::uint64_t observed_at_us) {
  AtomicDurationScope total_submit(submit_duration_);
  if (observed_at_us == UINT64_MAX) observed_at_us = presentation_us;
  const auto first_wait_started_us = AtomicDurationSummary::now_us();
  std::unique_lock lock(mutex_);
  submit_first_mutex_wait_.record(AtomicDurationSummary::now_us() - first_wait_started_us);
  if (access_unit.empty()) return false;
  if (java_image_decoder_mode_) {
    if (java_decoder_failed_) {
      ++dropped_access_units_;
      ++input_drop_metrics_.not_running;
      return false;
    }
    last_input_nal_type_mask_ = vfdual::h264_annex_b_nal_type_mask(access_unit);
    const bool sync_frame = (last_input_nal_type_mask_ & (1U << 5U)) != 0U ||
        vfdual::h264_avcc_contains_idr(access_unit);
    const bool produces_decoded_frame =
        (last_input_nal_type_mask_ & ((1U << 1U) | (1U << 5U))) != 0U;
    if (produces_decoded_frame) {
      pending_frames_.push_back({
          static_cast<std::int64_t>(presentation_us), frame_id, control_sequence,
          content_updated, stream_restart_generation_, observed_at_us});
    }
    // Do not hold the decoder state lock across Java MediaCodec. The output
    // thread calls back into process_java_yuv420 and must not be starved by a
    // high-rate input producer.
    const auto generation = stream_restart_generation_;
    const auto binding_generation = java_decoder_binding_generation_;
    lock.unlock();
    const auto java_offer_started_us = AtomicDurationSummary::now_us();
    const auto offer_result = submit_to_java(
        access_unit, presentation_us, sync_frame, generation, binding_generation);
    submit_java_offer_duration_.record(AtomicDurationSummary::now_us() - java_offer_started_us);
    const auto second_wait_started_us = AtomicDurationSummary::now_us();
    lock.lock();
    submit_second_mutex_wait_.record(AtomicDurationSummary::now_us() - second_wait_started_us);
    if (generation != stream_restart_generation_ || binding_generation != java_decoder_binding_generation_ ||
        !java_image_decoder_mode_ || !inference_worker_running_.load(std::memory_order_acquire)) {
      // A stop/restart/rebind owns its own counters and metadata. An old offer
      // completion cannot erase a new frame with the same timestamp.
      const auto abandoned = std::find_if(pending_frames_.begin(), pending_frames_.end(),
          [=](const PendingFrame& pending) {
            return pending.presentation_us == static_cast<std::int64_t>(presentation_us) &&
                pending.stream_generation == generation && pending.control_sequence == control_sequence &&
                pending.frame_id == frame_id;
          });
      if (abandoned != pending_frames_.end()) pending_frames_.erase(abandoned);
      return false;
    }
    if (offer_result != JavaAccessUnitOfferResult::accepted) {
      if (produces_decoded_frame) {
        const auto rejected = std::find_if(
            pending_frames_.rbegin(), pending_frames_.rend(),
            [presentation_us](const PendingFrame& pending) {
              return pending.presentation_us == static_cast<std::int64_t>(presentation_us);
            });
        if (rejected != pending_frames_.rend()) {
          pending_frames_.erase(std::next(rejected).base());
        }
      }
      ++dropped_access_units_;
      input_drop_metrics_.record_offer(offer_result);
      return false;
    }
    ++queued_access_units_;
    if (sync_frame) ++sync_access_units_;
    return true;
  }
  if (codec_ == nullptr) {
    ++dropped_access_units_;
    ++input_drop_metrics_.not_running;
    return false;
  }
  // The output worker owns decode-output, preprocessing and QNN. Keep the UDP
  // receive path bounded so a slow consumer cannot starve socket reassembly.
  const ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, kInputPollTimeoutUs);
  if (index < 0) {
    ++dropped_access_units_;
    ++input_drop_metrics_.no_input_buffer;
    return false;
  }
  size_t capacity{};
  std::uint8_t* destination = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(index), &capacity);
  if (destination == nullptr || access_unit.size() > capacity) {
    ++dropped_access_units_;
    ++input_drop_metrics_.buffer_capacity;
    return false;
  }
  std::memcpy(destination, access_unit.data(), access_unit.size());
  last_input_nal_type_mask_ = vfdual::h264_annex_b_nal_type_mask(access_unit);
  const bool sync_frame = vfdual::h264_access_unit_contains_idr(access_unit);
  const std::uint32_t flags = sync_frame ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME : 0U;
  if (AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(index), 0, access_unit.size(), presentation_us, flags) != AMEDIA_OK) {
    ++dropped_access_units_;
    ++input_drop_metrics_.codec_exception;
    return false;
  }
  pending_frames_.push_back({
      static_cast<std::int64_t>(presentation_us), frame_id, control_sequence,
      content_updated, stream_restart_generation_, observed_at_us});
  ++queued_access_units_;
  if (sync_frame) ++sync_access_units_;
  return true;
}

JavaAccessUnitOfferResult NativeH264Decoder::submit_to_java(
    std::span<const std::byte> access_unit, std::uint64_t presentation_us, bool sync_frame,
    std::uint64_t stream_generation, std::uint64_t binding_generation) {
  if (access_unit.size() > INT32_MAX) return JavaAccessUnitOfferResult::bridge_failure;
  JavaVM* vm{};
  {
    std::scoped_lock lock(mutex_);
    if (stream_generation != stream_restart_generation_ ||
        binding_generation != java_decoder_binding_generation_) {
      return JavaAccessUnitOfferResult::restarting;
    }
    vm = java_vm_;
  }
  JNIEnv* environment = current_thread_environment(vm);
  if (environment == nullptr) return JavaAccessUnitOfferResult::bridge_failure;
  struct LocalDecoderReference final {
    JNIEnv* environment{};
    jobject object{};
    ~LocalDecoderReference() { if (object != nullptr) environment->DeleteLocalRef(object); }
  } reference{environment, nullptr};
  jmethodID offer_method{}, has_fatal_method{}, fatal_diagnostic_method{};
  {
    std::scoped_lock lock(mutex_);
    if (vm != java_vm_ || stream_generation != stream_restart_generation_ ||
        binding_generation != java_decoder_binding_generation_ || !java_image_decoder_mode_ ||
        !inference_worker_running_.load(std::memory_order_acquire) || stream_restart_in_progress_) {
      return JavaAccessUnitOfferResult::restarting;
    }
    if (java_decoder_object_ != nullptr && java_offer_access_unit_method_ != nullptr) {
      reference.object = environment->NewLocalRef(java_decoder_object_);
      offer_method = java_offer_access_unit_method_;
      has_fatal_method = java_has_fatal_failure_method_;
      fatal_diagnostic_method = java_fatal_diagnostic_method_;
    }
  }
  if (environment->ExceptionCheck()) {
    environment->ExceptionClear();
    return JavaAccessUnitOfferResult::bridge_failure;
  }
  if (reference.object == nullptr || offer_method == nullptr) {
    return JavaAccessUnitOfferResult::bridge_failure;
  }
  jobject buffer = environment->NewDirectByteBuffer(
      const_cast<std::byte*>(access_unit.data()), static_cast<jlong>(access_unit.size()));
  if (buffer == nullptr) {
    if (environment->ExceptionCheck()) environment->ExceptionClear();
    return JavaAccessUnitOfferResult::bridge_failure;
  }
  const jint result_code = environment->CallIntMethod(
      reference.object, offer_method, buffer,
      static_cast<jint>(access_unit.size()), static_cast<jlong>(presentation_us),
      sync_frame ? JNI_TRUE : JNI_FALSE);
  environment->DeleteLocalRef(buffer);
  if (environment->ExceptionCheck()) {
    environment->ExceptionDescribe();
    environment->ExceptionClear();
    __android_log_print(
        ANDROID_LOG_ERROR, "VisionForgeDual", "Java decoder access-unit callback failed");
    return JavaAccessUnitOfferResult::bridge_failure;
  }
  bool fatal_failure = false;
  std::string fatal_diagnostic;
  if (result_code != static_cast<jint>(JavaAccessUnitOfferResult::accepted) &&
      has_fatal_method != nullptr) {
    fatal_failure = environment->CallBooleanMethod(reference.object, has_fatal_method) == JNI_TRUE;
    if (environment->ExceptionCheck()) {
      environment->ExceptionClear();
      return JavaAccessUnitOfferResult::bridge_failure;
    }
    if (fatal_failure && fatal_diagnostic_method != nullptr) {
      auto* diagnostic = static_cast<jstring>(environment->CallObjectMethod(
          reference.object, fatal_diagnostic_method));
      // A diagnostic failure must not leave a pending exception for the next
      // attached receive-thread invocation.
      if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
        if (diagnostic != nullptr) environment->DeleteLocalRef(diagnostic);
        diagnostic = nullptr;
      }
      if (diagnostic != nullptr) {
        const char* utf = environment->GetStringUTFChars(diagnostic, nullptr);
        if (utf != nullptr) {
          fatal_diagnostic = utf;
          environment->ReleaseStringUTFChars(diagnostic, utf);
        }
        environment->DeleteLocalRef(diagnostic);
      }
    }
  }
  if (environment->ExceptionCheck()) {
    environment->ExceptionDescribe();
    environment->ExceptionClear();
  }
  if (fatal_failure) {
    std::scoped_lock lock(mutex_);
    if (stream_generation == stream_restart_generation_ &&
        binding_generation == java_decoder_binding_generation_) {
      java_decoder_failed_ = true;
      java_decoder_failure_ = fatal_diagnostic.empty() ? "unknown" : std::move(fatal_diagnostic);
    }
  }
  if (result_code < static_cast<jint>(JavaAccessUnitOfferResult::accepted) ||
      result_code > static_cast<jint>(JavaAccessUnitOfferResult::reference_recovery)) {
    return JavaAccessUnitOfferResult::bridge_failure;
  }
  return static_cast<JavaAccessUnitOfferResult>(result_code);
}

void NativeH264Decoder::output_loop() {
  while (output_worker_running_) {
    AMediaCodec* active_codec{};
    {
      std::scoped_lock lock(mutex_);
      active_codec = codec_;
    }
    if (active_codec == nullptr) break;

    AMediaCodecBufferInfo output{};
    const ssize_t output_index = AMediaCodec_dequeueOutputBuffer(
        active_codec, &output, kOutputWorkerPollTimeoutUs);
    if (output_index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      std::scoped_lock lock(mutex_);
      update_output_format_locked();
      continue;
    }
    if (output_index < 0) continue;

    {
      std::scoped_lock lock(mutex_);
      process_output_locked(static_cast<std::size_t>(output_index), output);
    }
    AMediaCodec_releaseOutputBuffer(
        active_codec, static_cast<std::size_t>(output_index), output_window_ != nullptr);
  }
}

void NativeH264Decoder::record_outcome(const PendingInferenceFrame& frame,
    PipelineOutcome reason,std::uint64_t at,std::uint64_t detail) {
  std::scoped_lock lock(outcome_mutex_);
  outcome_trace_.record(frame.control_sequence,frame.frame_id,frame.stream_generation,
                       frame.observed_at_us,at,reason,detail);
}
void NativeH264Decoder::record_outcome(const PendingFrame& frame,
    PipelineOutcome reason,std::uint64_t detail) {
  const auto at=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  std::scoped_lock lock(outcome_mutex_);
  outcome_trace_.record(frame.control_sequence,frame.frame_id,frame.stream_generation,
                       frame.observed_at_us,at,reason,detail);
}
void NativeH264Decoder::record_projection(ProjectionTimingTrace::Row row) {
  std::scoped_lock lock(projection_trace_mutex_);projection_timing_trace_.record(row);
}
void NativeH264Decoder::record_projection_target(ProjectionTimingTrace::TargetRow row) {
  std::scoped_lock lock(projection_trace_mutex_);projection_timing_trace_.record_target(row);
}

void NativeH264Decoder::inference_loop() {
  std::vector<YoloDetection> detections;
  MobileBackgroundMotion background_estimator;
  MobileTargetImageMotion image_motion_estimator;
  InferenceDeadlineAdmission deadline_admission;
  using Gate=ProjectionObservationGate;
  using Trace=ProjectionTimingTrace;
  static_assert(Gate::kMaximumAgeUs==kContinuousControlMaximumObservationAgeUs);
  Gate observation_gate;
  MobileFreshPixelProjection pixel_projector;
  bool projector_configuration_attempted=false,projector_configured=false;
  bool require_chronological_control=false;
  std::uint64_t gate_generation{};
  std::optional<PendingInferenceFrame> carry_frame;
  struct ShadowProjection {
    Gate::Source source{};
    Gate::Frame target{};
    int mode{};
    std::array<YoloDetection,MobileFreshPixelProjection::kMaximumTargets> boxes{};
    std::size_t count{};
  };
  std::optional<ShadowProjection> shadow_projection;
  const auto now_us=[] {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  };
  const auto identity=[](const PendingInferenceFrame& f)->Gate::Frame {
    return {f.stream_generation,f.control_sequence,f.frame_id,f.observed_at_us};
  };
  const auto scaled=[](double value,double scale)->std::int64_t {
    return std::isfinite(value)?static_cast<std::int64_t>(std::llround(std::clamp(value*scale,-1e12,1e12))):-1;
  };
  const auto base_row=[&](int mode,Trace::Event event,const Gate::Source& source,
      const Gate::Frame& pixels,const PendingInferenceFrame& current)->Trace::Row {
    Trace::Row row{};row[1]=mode;row[2]=static_cast<std::int64_t>(event);
    row[3]=source.frame.generation;row[4]=source.frame.sequence;row[5]=source.frame.wire_frame_id;row[6]=source.frame.observed_us;
    row[7]=pixels.sequence;row[8]=pixels.wire_frame_id;row[9]=pixels.observed_us;
    row[10]=current.control_sequence;row[11]=current.frame_id;row[12]=current.observed_at_us;
    row[17]=source.valid_until_us;row[21]=source.detection_count;row[28]=row[29]=-1;
    return row;
  };
  const auto recycle_input = [this](std::vector<std::uint8_t>& input) {
    if(input.empty())return;
    std::scoped_lock inference_lock(inference_mutex_);
    free_model_inputs_.push_back(std::move(input));
  };
  const auto retire_shadow=[&](const PendingInferenceFrame& frame,Trace::Outcome why) {
    if(!shadow_projection || shadow_projection->target!=identity(frame))return;
    auto row=base_row(shadow_projection->mode,Trace::Event::unscored,shadow_projection->source,shadow_projection->target,frame);
    row[19]=now_us();row[25]=static_cast<std::int64_t>(why);
    record_projection(row);++pixel_projection_shadow_unscored_;shadow_projection.reset();
  };
  const auto compare_shadow=[&](const PendingInferenceFrame& frame,std::span<const YoloDetection> current) {
    if(!shadow_projection || shadow_projection->target!=identity(frame))return;
    const auto& shadow=*shadow_projection;
    auto row=base_row(shadow.mode,Trace::Event::comparison,shadow.source,shadow.target,frame);
    std::array<bool,300> used{};
    double minimum_iou=1.,sum_error{};std::size_t matched{};
    const auto iou=[](const YoloDetection& a,const YoloDetection& b) {
      const double overlap=std::max(0.F,std::min(a.x2,b.x2)-std::max(a.x1,b.x1))*
          std::max(0.F,std::min(a.y2,b.y2)-std::max(a.y1,b.y1));
      const double total=std::max(0.F,a.x2-a.x1)*std::max(0.F,a.y2-a.y1)+
          std::max(0.F,b.x2-b.x1)*std::max(0.F,b.y2-b.y1)-overlap;
      return total>0?overlap/total:0.;
    };
    for(std::size_t i=0;i<shadow.count;++i) {
      const auto& projected=shadow.boxes[i];double best{};std::size_t best_index=current.size();
      for(std::size_t j=0;j<std::min(current.size(),used.size());++j) {
        if(used[j] || current[j].class_id!=projected.class_id)continue;
        const auto score=iou(projected,current[j]);if(score>best){best=score;best_index=j;}
      }
      std::int64_t error=-1;
      const bool reliable=best_index!=current.size() && best>=.50;
      minimum_iou=std::min(minimum_iou,best);
      if(best_index!=current.size()) {
        const auto& actual=current[best_index];
        const auto distance=std::hypot((projected.x1+projected.x2-actual.x1-actual.x2)*.5F,
            projected.y1+.18F*(projected.y2-projected.y1)-actual.y1-.18F*(actual.y2-actual.y1));
        error=scaled(distance,1000);
        if(reliable){used[best_index]=true;++matched;sum_error+=distance;}
      }
      Trace::TargetRow target{};target[1]=static_cast<std::int64_t>(Trace::Event::comparison);target[2]=shadow.mode;
      target[3]=shadow.source.frame.generation;target[4]=shadow.source.frame.sequence;target[5]=shadow.source.frame.wire_frame_id;
      target[6]=shadow.source.frame.observed_us;target[7]=shadow.target.sequence;target[8]=shadow.target.wire_frame_id;
      target[9]=shadow.target.observed_us;target[10]=i;target[11]=reliable;
      target[19]=best_index==current.size()?-1:static_cast<std::int64_t>(best_index);
      target[20]=best_index==current.size()?-1:scaled(best,1e6);target[21]=error;
      record_projection_target(target);
    }
    row[19]=now_us();row[25]=static_cast<std::int64_t>(Trace::Outcome::compared);row[26]=matched;row[27]=current.size();
    row[28]=shadow.count?scaled(minimum_iou,1e6):-1;row[29]=matched?scaled(sum_error/matched,1000):-1;
    record_projection(row);++pixel_projection_shadow_compared_;shadow_projection.reset();
  };
  while (true) {
    PendingInferenceFrame frame{};
    {
      std::unique_lock lock(inference_mutex_);
      inference_condition_.wait(lock, [this,&carry_frame] {
        return !inference_worker_running_.load(std::memory_order_acquire) ||
            carry_frame.has_value() || !pending_inference_frames_.empty();
      });
      if (!inference_worker_running_.load(std::memory_order_acquire) &&
          pending_inference_frames_.empty() && !carry_frame) break;
      if(carry_frame) {
        if(!pending_inference_frames_.empty()) {
          const auto& ready=pending_inference_frames_.front();
          const bool newer=ready.stream_generation>carry_frame->stream_generation ||
              (ready.stream_generation==carry_frame->stream_generation && ready.content_updated &&
               ready.control_sequence>carry_frame->control_sequence && ready.observed_at_us>carry_frame->observed_at_us);
          if(newer) {
            // No inference is forced merely to obtain a shadow score.
            retire_shadow(*carry_frame,ready.stream_generation==carry_frame->stream_generation?
                Trace::Outcome::superseded_unscored:Trace::Outcome::generation_unscored);
            record_outcome(*carry_frame,ready.stream_generation==carry_frame->stream_generation?
                PipelineOutcome::preprocessed_superseded:PipelineOutcome::generation_before_inference,
                now_us(),ready.control_sequence);
            free_model_inputs_.push_back(std::move(carry_frame->input));carry_frame.reset();
          }
        }
        if(carry_frame){frame=std::move(*carry_frame);carry_frame.reset();}
        else {frame=std::move(pending_inference_frames_.front());pending_inference_frames_.pop_front();}
      } else {
        frame=std::move(pending_inference_frames_.front());pending_inference_frames_.pop_front();
      }
    }
    if (!inference_worker_running_.load(std::memory_order_acquire)) {
      retire_shadow(frame,Trace::Outcome::stopped_unscored);
      recycle_input(frame.input);
      break;
    }

    // Prefer actual newer ready input after warmup. An already running
    // conversion may receive the policy's bounded wait; failure/timeout keeps
    // the original frame. This block performs at most one exchange.
    PendingInferenceFrame replaced{};
    bool replaced_for_deadline=false;
    {
      std::unique_lock inference_lock(inference_mutex_);
      const InferenceDeadlineAdmission::Observation selected{frame.observed_at_us,frame.stream_generation};
      const auto replacement_ready=[&] {
        return !pending_inference_frames_.empty() && pending_inference_frames_.front().content_updated &&
            pending_inference_frames_.front().control_sequence>frame.control_sequence && deadline_admission.prefer_ready(selected,
            {pending_inference_frames_.front().observed_at_us,pending_inference_frames_.front().stream_generation},now_us());
      };
      const auto wait_budget_us=replacement_ready()?0:
          deadline_admission.replacement_wait_budget_us(selected,preprocessing_input_,now_us());
      if(wait_budget_us) {
        const auto preparing=preprocessing_input_;
        const auto began=now_us();++deadline_replacement_waits_;
        inference_condition_.wait_for(inference_lock,
            std::chrono::microseconds(wait_budget_us),[&] {
          return !inference_worker_running_.load(std::memory_order_acquire) || replacement_ready() ||
              preprocessing_input_.observation.observed_us!=preparing.observation.observed_us ||
              preprocessing_input_.observation.generation!=preparing.observation.generation;
        });
        const auto elapsed=now_us()-began;
        deadline_maximum_replacement_wait_us_.store(std::max(
            deadline_maximum_replacement_wait_us_.load(std::memory_order_relaxed),elapsed),std::memory_order_relaxed);
        if(!replacement_ready())++deadline_replacement_fallbacks_;
      }
      if(replacement_ready()) {
        replaced=std::move(frame);
        frame=std::move(pending_inference_frames_.front());pending_inference_frames_.pop_front();
        free_model_inputs_.push_back(std::move(replaced.input));
        ++deadline_replaced_pre_inference_frames_;replaced_for_deadline=true;
      }
    }
    if(replaced_for_deadline) {
      retire_shadow(replaced,Trace::Outcome::superseded_unscored);
      record_outcome(replaced,PipelineOutcome::deadline_replaced_before_inference,now_us(),frame.control_sequence);
    }
    if(!inference_worker_running_.load(std::memory_order_acquire)) {
      retire_shadow(frame,Trace::Outcome::stopped_unscored);
      recycle_input(frame.input);break;
    }

    bool current_before_inference{};
    {
      std::scoped_lock lock(mutex_);
      current_before_inference =
          is_current_stream_generation_locked(frame.stream_generation);
      if (!current_before_inference) {
        ++stale_generation_inference_suppressions_;
      }
    }
    if (!current_before_inference) {
      const auto at=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
      record_outcome(frame,PipelineOutcome::generation_before_inference,at);
      retire_shadow(frame,Trace::Outcome::generation_unscored);
      recycle_input(frame.input);
      continue;
    }

    const auto inference_started = std::chrono::steady_clock::now();
    const auto inference_started_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            inference_started.time_since_epoch()).count());
    const bool invalid_or_stale =
        inference_started_us < frame.observed_at_us ||
        inference_started_us - frame.observed_at_us >
            kMobileControlMaximumFrameAgeUs;
    if (inference_started_us >= frame.observed_at_us) {
      const std::uint64_t age_us = inference_started_us - frame.observed_at_us;
      maximum_pre_inference_frame_age_us_.store(
          std::max(maximum_pre_inference_frame_age_us_.load(
                       std::memory_order_relaxed),
                   age_us),
          std::memory_order_relaxed);
    }
    if (invalid_or_stale) {
      record_outcome(frame,PipelineOutcome::stale_before_inference,inference_started_us);
      stale_pre_inference_frames_.fetch_add(1, std::memory_order_relaxed);
      retire_shadow(frame,Trace::Outcome::stale_unscored);
      recycle_input(frame.input);
      continue;
    }

    std::uint64_t qnn_us{};
    std::uint32_t graph_status{0xffffffffU};
    std::uint32_t detection_count{};
    detections.clear();
    const bool succeeded = vfdual_android::execute_realtime_inference(
        frame.input, &qnn_us, &graph_status, &detection_count, &detections);
    const auto inference_finished = std::chrono::steady_clock::now();
    const auto inference_finished_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(inference_finished.time_since_epoch()).count());
    const auto inference_total_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            inference_finished - inference_started).count());
    if(succeeded) {
      deadline_admission.record_backend_wall(inference_total_us);
      deadline_backend_estimate_us_.store(deadline_admission.backend_estimate_us(),std::memory_order_relaxed);
    }
    if (!inference_worker_running_.load(std::memory_order_acquire)) {
      record_outcome(frame,PipelineOutcome::stopped_after_inference,inference_finished_us);
      retire_shadow(frame,Trace::Outcome::stopped_unscored);
      recycle_input(frame.input);
      break;
    }
    const int projection_mode=requested_pixel_projection_mode();
    pixel_projection_mode_.store(projection_mode,std::memory_order_relaxed);
    if(gate_generation!=frame.stream_generation) {
      observation_gate.restart(frame.stream_generation);gate_generation=frame.stream_generation;
      require_chronological_control=false;
    }
    const Gate::Source source{identity(frame),frame.control_sequence,detections.size(),
        frame.observed_at_us<=UINT64_MAX-Gate::kMaximumAgeUs?frame.observed_at_us+Gate::kMaximumAgeUs:0};
    if(succeeded)static_cast<void>(observation_gate.cache_source(source));
    const bool source_cached=observation_gate.cached_source() && observation_gate.cached_source()->frame==source.frame;
    bool source_current{};
    {std::scoped_lock lock(mutex_);source_current=is_current_stream_generation_locked(frame.stream_generation);}
    if(projection_mode && succeeded && source_current && frame.content_updated && !detections.empty() &&
        now_us()>=frame.observed_at_us && now_us()-frame.observed_at_us<Gate::kMaximumAgeUs) {
      std::scoped_lock lock(inference_mutex_);
      if(!pending_inference_frames_.empty()) {
        const auto& ready=pending_inference_frames_.front();
        if(ready.stream_generation==frame.stream_generation && ready.content_updated &&
            ready.control_sequence>frame.control_sequence && ready.observed_at_us>frame.observed_at_us) {
          carry_frame.emplace(std::move(pending_inference_frames_.front()));pending_inference_frames_.pop_front();
          ++pixel_projection_candidates_;
        }
      }
    }
    const auto previous_control=observation_gate.last_published_control();
    const PendingInferenceFrame* control_frame=&frame;
    std::span<const YoloDetection> control_detections=detections;
    bool pixel_control=false,allow_control=succeeded && source_current && frame.content_updated;
    FreshPixelProjectionResult projection_result{};
    Gate::Proposal projection_proposal{};
    Trace::Outcome projection_outcome=Trace::Outcome::no_newer_ready;
    std::uint64_t projection_started{},projection_finished{};
    std::array<Gate::TargetEvidence,MobileFreshPixelProjection::kMaximumTargets> gate_targets{};
    const auto make_evidence=[&]() {
      for(std::size_t i=0;i<projection_result.targets.size() && i<gate_targets.size();++i) {
        const auto& t=projection_result.targets[i];gate_targets[i]={t.source_index,t.valid,t.pixel_observed_at_us};
      }
      return Gate::ProjectionEvidence{source.frame,carry_frame?identity(*carry_frame):Gate::Frame{},source.detection_count,
          projection_result.valid_count,projection_result.complete,projection_result.budget_exhausted,
          std::span<const Gate::TargetEvidence>(gate_targets).first(std::min(projection_result.targets.size(),gate_targets.size())),{}};
    };
    const auto project_pixels=[&](std::span<const YoloDetection> actual_source) {
      if(!carry_frame)return;
      projection_started=now_us();
      if(!projector_configuration_attempted) {
        projector_configuration_attempted=true;
        projector_configured=pixel_projector.configure(model_dimensions_.width,model_dimensions_.height);
      }
      if(!projector_configured){projection_outcome=Trace::Outcome::not_configured;projection_finished=now_us();return;}
      if(projection_started<source.frame.observed_us || projection_started-source.frame.observed_us>=Gate::kMaximumAgeUs) {
        projection_outcome=Trace::Outcome::source_expired;projection_finished=now_us();return;
      }
      ++pixel_projection_attempts_;
      projection_result=pixel_projector.process(frame.input,carry_frame->input,actual_source,
          {frame.stream_generation,frame.control_sequence,frame.observed_at_us},
          {carry_frame->stream_generation,carry_frame->control_sequence,carry_frame->observed_at_us},2'500U);
      projection_finished=now_us();
      if(projection_result.complete)++pixel_projection_complete_;
      else {++pixel_projection_math_rejections_;projection_outcome=Trace::Outcome::projector_rejected;}
    };
    if(projection_mode==2 && succeeded) {
      allow_control=allow_control && source_cached;
      compare_shadow(frame,detections);
      observation_gate.set_user_enabled(control_observation_requested(frame.stream_generation));
      if(allow_control && !detections.empty() && carry_frame && control_observation_requested(frame.stream_generation)) {
        project_pixels(detections);
        if(projection_result.complete) {
          const Gate::Pixels pixels{identity(*carry_frame),carry_frame->control_sequence,carry_frame->content_updated,true};
          projection_proposal=observation_gate.prepare_projection(pixels,make_evidence(),now_us());
          if(projection_proposal.ready()) {
            pixel_control=true;control_frame=&*carry_frame;control_detections=projection_result.projected_boxes;
            projection_outcome=Trace::Outcome::active_projected;
          } else {++pixel_projection_gate_rejections_;projection_outcome=Trace::Outcome::gate_rejected;}
        }
      }
      if(!pixel_control) {
        projection_proposal=observation_gate.prepare_detector_fallback(now_us());
        allow_control=allow_control && projection_proposal.ready();
        if(allow_control)projection_outcome=Trace::Outcome::chronological_source;
        else if(frame.observed_at_us<=previous_control.observed_us) {
          ++pixel_projection_old_source_skips_;projection_outcome=Trace::Outcome::old_source_skipped;
        }
      }
    } else if(require_chronological_control && frame.observed_at_us<=previous_control.observed_us) {
      allow_control=false;++pixel_projection_old_source_skips_;projection_outcome=Trace::Outcome::old_source_skipped;
    }
    if(detections.empty() && projection_mode && projection_outcome!=Trace::Outcome::old_source_skipped)
      projection_outcome=Trace::Outcome::empty_source;

    const auto background_started = std::chrono::steady_clock::now();
    BackgroundMotion background{};
    if (allow_control) {
      background = background_estimator.observe(control_frame->input, model_dimensions_.width,
          model_dimensions_.height, control_detections, control_frame->observed_at_us, control_frame->stream_generation);
    } else if (!succeeded) {
      background_estimator.reset();
    }
    const auto background_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - background_started).count());
    const auto image_motion_started=std::chrono::steady_clock::now();
    std::span<const TargetImageMotion> image_motion;
    if(allow_control) {
      // Reuse the same inference pixels. Correspondence is auxiliary; failure
      // or a spent CPU budget never rejects an ordinary body detection.
      image_motion=image_motion_estimator.observe(control_frame->input,model_dimensions_.width,
          model_dimensions_.height,control_detections,background,control_frame->observed_at_us,control_frame->stream_generation,.18F,2'500U,true);
    } else if(!succeeded)image_motion_estimator.reset();
    const auto image_motion_us=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-image_motion_started).count());
    const auto completed_at_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (!succeeded) {
      record_outcome(frame,PipelineOutcome::qnn_failed,completed_at_us,graph_status);
      const auto failure_count = commit_qnn_failure(
          frame.stream_generation, graph_status, completed_at_us);
      if (failure_count.has_value()) {
        __android_log_print(
            ANDROID_LOG_ERROR, "VisionForgeDual",
            "inference failure backend=%.*s count=%llu status=%u",
            static_cast<int>(realtime_inference_backend_token().size()),
            realtime_inference_backend_token().data(),
            static_cast<unsigned long long>(*failure_count), graph_status);
      }
      retire_shadow(frame,Trace::Outcome::failed_unscored);
      recycle_input(frame.input);
      continue;
    }

    const QnnSuccessCommit commit = commit_qnn_success(
        frame, qnn_us, graph_status, detection_count, inference_total_us,
        completed_at_us, detections);
    if (!commit.committed) {
      record_outcome(frame,PipelineOutcome::generation_after_inference,completed_at_us);
      retire_shadow(frame,Trace::Outcome::generation_unscored);
      if(projection_mode) {
        auto row=base_row(projection_mode,Trace::Event::attempt,source,carry_frame?identity(*carry_frame):Gate::Frame{},frame);
        row[16]=previous_control.observed_us;row[18]=projection_started;row[19]=now_us();
        row[20]=projection_result.elapsed_us;row[22]=projection_result.valid_count;
        row[23]=static_cast<std::int64_t>(Gate::Reason::wrong_generation);
        row[24]=static_cast<std::int64_t>(projection_result.rejection);row[25]=static_cast<std::int64_t>(Trace::Outcome::wrong_generation);
        record_projection(row);
        if(projection_result.complete && carry_frame) {
          row[2]=static_cast<std::int64_t>(Trace::Event::unscored);
          row[25]=static_cast<std::int64_t>(Trace::Outcome::generation_unscored);
          record_projection(row);++pixel_projection_shadow_unscored_;
        }
      }
      recycle_input(frame.input);
      continue;
    }
    if (commit.resume_generation.has_value()) {
      const bool resumed =
          resume_makcu_output_after_valid_frame(*commit.resume_generation);
      __android_log_print(
          ANDROID_LOG_INFO, "VisionForgeDual",
          "MAKCU recovery hold released=%d generation=%llu after_valid_qnn_frame=1",
          resumed ? 1 : 0,
          static_cast<unsigned long long>(*commit.resume_generation));
    }
    const auto control_started = std::chrono::steady_clock::now();
    if(!pixel_control)control_detections=last_detections_; // NNP commit swapped the REAL source A.
    bool observation_consumed=false;
    if(projection_mode==2 && allow_control) {
      // Auxiliary work may have consumed the remaining source decision budget.
      observation_gate.set_user_enabled(control_observation_requested(frame.stream_generation));
      if(pixel_control) {
        const Gate::Pixels pixels{identity(*control_frame),control_frame->control_sequence,control_frame->content_updated,true};
        projection_proposal=observation_gate.prepare_projection(pixels,make_evidence(),now_us());
      } else projection_proposal=observation_gate.prepare_detector_fallback(now_us());
      allow_control=projection_proposal.ready();
      if(!allow_control){
        background_estimator.reset();image_motion_estimator.reset();++pixel_projection_gate_rejections_;
        projection_outcome=Trace::Outcome::gate_rejected;
      }
    }
    if (allow_control && control_frame_is_actionable(commit.committed, control_frame->content_updated)) {
      publish_makcu_move_for_detections(
          control_detections, control_frame->control_sequence, control_frame->stream_generation,
          control_frame->content_updated, control_frame->observed_at_us, completed_at_us, background,
          control_frame->frame_id,image_motion,pixel_control?source.valid_until_us:0,&observation_consumed);
      if(observation_consumed) {
        // A physical unsent-deadline discard does NOT undo an observation the
        // Core already consumed. Do not recheck source expiry after receipt.
        if(projection_mode==2) {
          static_cast<void>(observation_gate.record_consumed_observation(identity(*control_frame),control_frame->control_sequence,true));
          if(pixel_control){require_chronological_control=true;++pixel_projection_control_publications_;}
        } else static_cast<void>(observation_gate.record_legacy_observation(identity(frame),frame.control_sequence,true));
      } else {background_estimator.reset();image_motion_estimator.reset();projection_outcome=Trace::Outcome::observation_not_consumed;}
    }
    const auto control_finished = std::chrono::steady_clock::now();
    const auto control_finished_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(control_finished.time_since_epoch()).count());
    {
      std::scoped_lock lock(mutex_);
      if (is_current_stream_generation_locked(frame.stream_generation)) {
        pipeline_timing_trace_.record({
            .sequence = frame.control_sequence,
            .wire_frame_id = frame.frame_id,
            .generation = frame.stream_generation,
            .observed_us = frame.observed_at_us,
            .received_us = frame.received_at_us,
            .decoded_planes_us = frame.decoded_planes_us,
            .preprocess_started_us = frame.preprocess_started_us,
            .preprocess_finished_us = frame.preprocess_finished_us,
            .enqueued_us = frame.enqueued_us,
            .inference_started_us = inference_started_us,
            .inference_finished_us = inference_finished_us,
            .result_ready_us = completed_at_us,
            .control_started_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(control_started.time_since_epoch()).count()),
            .control_finished_us = control_finished_us,
            .qnn_us = qnn_us,
            .background_us = background_us,
            .image_motion_us = image_motion_us,
            .detections = detection_count,
            .content_updated = frame.content_updated ? 1U : 0U});
        record_latency(background_motion_samples_us_, background_us);
        record_latency(image_motion_samples_us_,image_motion_us);
        image_motion_budget_exhaustions_+=image_motion_estimator.work_budget_exhausted();
        record_latency(control_dispatch_samples_us_, static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(control_finished - control_started).count()));
        if (completed_at_us >= frame.received_at_us)
          record_latency(receive_to_result_samples_us_, completed_at_us - frame.received_at_us);
        if (completed_at_us >= frame.observed_at_us)
          record_latency(host_plus_receive_to_result_samples_us_, completed_at_us - frame.observed_at_us);
        if (control_finished_us >= frame.received_at_us)
          record_latency(receive_to_control_samples_us_, control_finished_us - frame.received_at_us);
      }
    }
    if(projection_mode!=2)compare_shadow(frame,last_detections_);
    if(projection_mode==1 && carry_frame && !last_detections_.empty()) {
      project_pixels(last_detections_);
      if(projection_result.complete) {
        auto trial=observation_gate;trial.set_user_enabled(true);
        const Gate::Pixels pixels{identity(*carry_frame),carry_frame->control_sequence,carry_frame->content_updated,true};
        projection_proposal=trial.prepare_projection(pixels,make_evidence(),now_us());
        if(projection_proposal.ready())projection_outcome=Trace::Outcome::shadow_complete;
        else {projection_outcome=Trace::Outcome::gate_rejected;++pixel_projection_gate_rejections_;}
      }
    }
    if(projection_mode && projection_result.complete && carry_frame) {
      ShadowProjection shadow;shadow.source=source;shadow.target=identity(*carry_frame);shadow.mode=projection_mode;
      shadow.count=std::min(projection_result.projected_boxes.size(),shadow.boxes.size());
      std::copy_n(projection_result.projected_boxes.begin(),shadow.count,shadow.boxes.begin());
      shadow_projection=shadow;
    }
    if(projection_mode) {
      auto row=base_row(projection_mode,Trace::Event::attempt,source,carry_frame?identity(*carry_frame):Gate::Frame{},frame);
      if(observation_consumed){row[13]=control_frame->control_sequence;row[14]=control_frame->frame_id;row[15]=control_frame->observed_at_us;}
      row[16]=previous_control.observed_us;row[18]=projection_started;row[19]=projection_finished?projection_finished:now_us();
      row[20]=projection_result.elapsed_us;row[22]=projection_result.valid_count;
      row[23]=static_cast<std::int64_t>(projection_proposal.reason);row[24]=static_cast<std::int64_t>(projection_result.rejection);
      row[25]=static_cast<std::int64_t>(projection_outcome);
      row[30]=row[19]>=row[6]?row[19]-row[6]:-1;
      row[31]=row[9] && row[19]>=row[9]?row[19]-row[9]:-1;
      record_projection(row);
      for(const auto& evidence:projection_result.targets) {
        Trace::TargetRow target{};target[1]=static_cast<std::int64_t>(Trace::Event::attempt);target[2]=projection_mode;
        target[3]=frame.stream_generation;target[4]=frame.control_sequence;target[5]=frame.frame_id;target[6]=frame.observed_at_us;
        if(carry_frame){target[7]=carry_frame->control_sequence;target[8]=carry_frame->frame_id;target[9]=carry_frame->observed_at_us;}
        target[10]=evidence.source_index;target[11]=evidence.valid;target[12]=static_cast<std::int64_t>(evidence.rejection);
        target[13]=scaled(evidence.dx_model_px,1000);target[14]=scaled(evidence.dy_model_px,1000);target[15]=scaled(evidence.quality,1e6);
        target[16]=scaled(evidence.forward_backward_error_model_px,1000);target[17]=evidence.support;target[18]=evidence.candidates;
        target[19]=target[20]=target[21]=-1;record_projection_target(target);
      }
    }
    if (commit.execution_count <= 3U || commit.execution_count % 30U == 0U) {
      __android_log_print(
          ANDROID_LOG_INFO, "VisionForgeDual",
          "inference success backend=%.*s count=%llu elapsed_us=%llu detections=%u",
          static_cast<int>(realtime_inference_backend_token().size()),
          realtime_inference_backend_token().data(),
          static_cast<unsigned long long>(commit.execution_count),
          static_cast<unsigned long long>(qnn_us), detection_count);
    }
    recycle_input(frame.input);
  }
  if(carry_frame){retire_shadow(*carry_frame,Trace::Outcome::stopped_unscored);recycle_input(carry_frame->input);carry_frame.reset();}
  if(shadow_projection) {
    // Terminal accounting remains explicit even if a future lifecycle change
    // retires an owned target before the ordinary carry cleanup sees it.
    PendingInferenceFrame terminal{};
    terminal.stream_generation=shadow_projection->target.generation;
    terminal.control_sequence=shadow_projection->target.sequence;
    terminal.frame_id=static_cast<std::uint32_t>(shadow_projection->target.wire_frame_id);
    terminal.observed_at_us=shadow_projection->target.observed_us;
    retire_shadow(terminal,Trace::Outcome::stopped_unscored);
  }
}

std::optional<std::uint64_t> NativeH264Decoder::commit_qnn_failure(
    std::uint64_t generation, std::uint32_t graph_status,
    std::uint64_t failed_at_us) {
  std::scoped_lock lock(mutex_);
  if (!is_current_stream_generation_locked(generation)) {
    ++stale_generation_result_suppressions_;
    return std::nullopt;
  }
  ++qnn_failures_;
  ++consecutive_qnn_failures_;
  last_qnn_failure_monotonic_us_ = failed_at_us;
  last_qnn_status_ = graph_status;
  return qnn_failures_;
}

NativeH264Decoder::QnnSuccessCommit NativeH264Decoder::commit_qnn_success(
    const PendingInferenceFrame& frame, std::uint64_t qnn_us,
    std::uint32_t graph_status, std::uint32_t detection_count,
    std::uint64_t inference_total_us, std::uint64_t completed_at_us,
    std::vector<YoloDetection>& detections) {
  std::scoped_lock lock(mutex_);
  if (!is_current_stream_generation_locked(frame.stream_generation)) {
    ++stale_generation_result_suppressions_;
    ++stale_generation_control_suppressions_;
    return {};
  }

  QnnSuccessCommit commit{
      .committed = true,
      .resume_generation = std::nullopt,
      .execution_count = 0U};
  if (frame.content_updated && pending_makcu_resume_generation_.has_value() &&
      *pending_makcu_resume_generation_ == frame.stream_generation) {
    commit.resume_generation = pending_makcu_resume_generation_;
    pending_makcu_resume_generation_.reset();
  }
  ++qnn_executions_;
  consecutive_qnn_failures_ = 0;
  last_qnn_success_monotonic_us_ = completed_at_us;
  last_qnn_us_ = qnn_us;
  last_inference_total_us_ = inference_total_us;
  last_qnn_status_ = graph_status;
  last_detection_count_ = detection_count;
  last_detection_source_generation_=frame.stream_generation;
  last_detection_source_sequence_=frame.control_sequence;
  last_detection_source_wire_frame_id_=frame.frame_id;
  last_detection_source_observed_us_=frame.observed_at_us;
  last_detections_.swap(detections);
  record_latency(qnn_samples_us_, qnn_us);
  record_latency(inference_total_samples_us_, inference_total_us);
  (void)pipeline_metrics_.push({
      frame.frame_id, frame.decode_queue_us, frame.preprocess_us, qnn_us,
      last_detection_count_, last_qnn_status_});
  append_detection_trace_locked(frame.frame_id);
  if (!frame.content_updated) ++repeated_content_inferences_;
  commit.execution_count = qnn_executions_;
  return commit;
}

void NativeH264Decoder::process_output_locked(
    std::size_t, const AMediaCodecBufferInfo&) {
  ++rendered_frames_;
}

std::optional<NativeH264Decoder::PendingFrame> NativeH264Decoder::take_pending_frame_for_output_locked(std::int64_t presentation_us) {
  const auto match = std::find_if(pending_frames_.begin(), pending_frames_.end(),
      [presentation_us](const PendingFrame& pending) { return pending.presentation_us == presentation_us; });
  if (match == pending_frames_.end()) return std::nullopt;
  const PendingFrame pending = *match;
  pending_frames_.erase(match);
  return pending;
}

void NativeH264Decoder::append_detection_trace_locked(std::uint32_t frame_id) {
  if (!detection_trace_enabled_ || frame_id == 0 || detection_trace_frames_.size() >= detection_trace_maximum_frames_) return;
  detection_trace_frames_.push_back({frame_id, last_detections_});
}

void NativeH264Decoder::update_output_format_locked() {
  AMediaFormat* format = AMediaCodec_getOutputFormat(codec_);
  if (format == nullptr) return;
  AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &output_color_format_);
  AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_STANDARD, &output_color_standard_);
  AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_RANGE, &output_color_range_);
  AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &decoded_width_);
  AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &decoded_height_);
  __android_log_print(
      ANDROID_LOG_INFO, "VisionForgeDual",
      "surface decoder format actual=%#x standard=%d range=%d size=%dx%d",
      output_color_format_, output_color_standard_, output_color_range_,
      decoded_width_, decoded_height_);
  AMediaFormat_delete(format);
}

NativeH264Decoder::PreprocessingLease::PreprocessingLease(
    NativeH264Decoder& owner,const PendingFrame& frame,std::uint64_t started):owner_(owner) {
  // Caller holds the main mutex; publish this measured hint with the lease.
  const auto expected_duration=owner_.last_preprocess_us_;
  std::scoped_lock lock(owner_.inference_mutex_);
  owner_.preprocessing_input_={{frame.observed_at_us,frame.stream_generation},started,expected_duration};
}
NativeH264Decoder::PreprocessingLease::~PreprocessingLease() {
  {
    std::scoped_lock lock(owner_.inference_mutex_);
    owner_.preprocessing_input_={};
  }
  owner_.inference_condition_.notify_one();
}

bool NativeH264Decoder::process_java_yuv420(
    const vfdual::Yuv420PlaneView& y_plane, const vfdual::Yuv420PlaneView& u_plane,
    const vfdual::Yuv420PlaneView& v_plane, std::int32_t image_width,
    std::int32_t image_height, std::int32_t crop_left, std::int32_t crop_top,
    std::int32_t crop_right, std::int32_t crop_bottom,
    std::int64_t presentation_time_us) {
  std::scoped_lock callback_lock(java_output_callback_mutex_);
  std::unique_lock lock(mutex_);
  if (!java_image_decoder_mode_ ||
      !inference_worker_running_.load(std::memory_order_acquire)) {
    ++stale_decoded_outputs_;
    return false;
  }
  const auto pending_metadata = take_pending_frame_for_output_locked(presentation_time_us);
  if (!pending_metadata.has_value()) {
    ++stale_decoded_outputs_;
    return false;
  }
  ++acquired_images_;
  ++rendered_frames_;
  if (!pending_metadata->content_updated) {
    record_outcome(*pending_metadata,PipelineOutcome::repeated_decoded_content);
    ++repeated_content_outputs_;
    ++repeated_content_inference_suppressions_;
    ++repeated_content_control_suppressions_;
    return true;
  }
  ++fresh_content_outputs_;

  // The encoded dimensions and physical ROI are separate contracts. The UDP
  // receiver verifies the ROI; reject an older Host's 416 video here as well.
  if (crop_right-crop_left != static_cast<std::int32_t>(model_dimensions_.width) ||
      crop_bottom-crop_top != static_cast<std::int32_t>(model_dimensions_.height)) {
    ++preprocess_failures_;
    java_decoder_failed_ = true;
    java_decoder_failure_ = "encoded_size_model_contract_mismatch";
    return false;
  }

  const bool supported_standard = output_color_standard_ == kColorStandardBt709 ||
      output_color_standard_ == kColorStandardBt601Pal ||
      output_color_standard_ == kColorStandardBt601Ntsc;
  if (!supported_standard || output_color_range_ != kColorRangeLimited) {
    ++preprocess_failures_;
    java_decoder_failed_ = true;
    java_decoder_failure_ = "unsupported_colorimetry_standard=" +
        std::to_string(output_color_standard_) + "_range=" +
        std::to_string(output_color_range_);
    __android_log_print(
        ANDROID_LOG_ERROR, "VisionForgeDual", "%s", java_decoder_failure_.c_str());
    return false;
  }

  vfdual::Yuv420ImageView yuv;
  const bool layout_valid = image_width > 0 && image_height > 0 && crop_left >= 0 &&
      crop_top >= 0 && crop_right > crop_left && crop_bottom > crop_top &&
      vfdual::make_yuv420_image_view(
          y_plane, u_plane, v_plane,
          static_cast<std::uint32_t>(image_width), static_cast<std::uint32_t>(image_height),
          {static_cast<std::uint32_t>(crop_left), static_cast<std::uint32_t>(crop_top),
           static_cast<std::uint32_t>(crop_right), static_cast<std::uint32_t>(crop_bottom)}, yuv);
  if (!layout_valid) {
    ++image_layout_failures_;
    ++preprocess_failures_;
    __android_log_print(
        ANDROID_LOG_ERROR, "VisionForgeDual",
        "Java Image.Plane layout rejected image=%dx%d crop=%d,%d,%d,%d "
        "row=%u/%u/%u pixel=%u/%u/%u failures=%llu",
        image_width, image_height, crop_left, crop_top, crop_right, crop_bottom,
        y_plane.row_stride, u_plane.row_stride, v_plane.row_stride,
        y_plane.pixel_stride, u_plane.pixel_stride, v_plane.pixel_stride,
        static_cast<unsigned long long>(image_layout_failures_));
    return false;
  }
  if (image_y_row_stride_ != static_cast<std::int32_t>(y_plane.row_stride) ||
      image_u_row_stride_ != static_cast<std::int32_t>(u_plane.row_stride) ||
      image_v_row_stride_ != static_cast<std::int32_t>(v_plane.row_stride) ||
      image_y_pixel_stride_ != static_cast<std::int32_t>(y_plane.pixel_stride) ||
      image_u_pixel_stride_ != static_cast<std::int32_t>(u_plane.pixel_stride) ||
      image_v_pixel_stride_ != static_cast<std::int32_t>(v_plane.pixel_stride) ||
      image_crop_left_ != crop_left || image_crop_top_ != crop_top ||
      image_crop_right_ != crop_right || image_crop_bottom_ != crop_bottom) {
    image_y_row_stride_ = static_cast<std::int32_t>(y_plane.row_stride);
    image_u_row_stride_ = static_cast<std::int32_t>(u_plane.row_stride);
    image_v_row_stride_ = static_cast<std::int32_t>(v_plane.row_stride);
    image_y_pixel_stride_ = static_cast<std::int32_t>(y_plane.pixel_stride);
    image_u_pixel_stride_ = static_cast<std::int32_t>(u_plane.pixel_stride);
    image_v_pixel_stride_ = static_cast<std::int32_t>(v_plane.pixel_stride);
    image_crop_left_ = crop_left; image_crop_top_ = crop_top;
    image_crop_right_ = crop_right; image_crop_bottom_ = crop_bottom;
    __android_log_print(
        ANDROID_LOG_INFO, "VisionForgeDual",
        "Java Image.Plane layout verified image=%dx%d crop=%d,%d,%d,%d "
        "row=%u/%u/%u pixel=%u/%u/%u",
        image_width, image_height, crop_left, crop_top, crop_right, crop_bottom,
        y_plane.row_stride, u_plane.row_stride, v_plane.row_stride,
        y_plane.pixel_stride, u_plane.pixel_stride, v_plane.pixel_stride);
  }
  const auto now_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  const auto presentation_us = static_cast<std::uint64_t>(presentation_time_us);
  const std::uint64_t decode_queue_us = now_us >= presentation_us ? now_us - presentation_us : 0U;
  record_latency(decode_queue_samples_us_, decode_queue_us);
  // Drain old decoded pictures without spending CPU on resize/color conversion.
  // Otherwise slower resize paths can stay permanently behind MediaCodec and
  // feed only expired frames to the inference worker. The compressed reference
  // chain is preserved: only the already decoded output is skipped.
  if (!is_current_stream_generation_locked(pending_metadata->stream_generation) ||
      !decoded_observation_is_fresh(presentation_us, pending_metadata->observed_at_us,
                                   now_us, kContinuousControlMaximumObservationAgeUs)) {
    ++stale_decoded_outputs_;
    return true;
  }
  const auto matrix = output_color_standard_ == kColorStandardBt601Pal ||
          output_color_standard_ == kColorStandardBt601Ntsc
      ? vfdual::YuvColorMatrix::bt601_limited
      : vfdual::YuvColorMatrix::bt709_limited;
  if (!inference_worker_running_.load(std::memory_order_acquire)) {
    ++stale_decoded_outputs_;
    return false;
  }
  PreprocessingLease preparing(*this,*pending_metadata,static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
  lock.unlock();
  const auto preprocess_started = std::chrono::steady_clock::now();
  vfdual::LetterboxTransform transform;
  const bool preprocessed = vfdual::preprocess_yuv420_to_model_rgb(
      yuv, matrix, model_dimensions_, model_input_, transform);
  const auto preprocess_finished = std::chrono::steady_clock::now();
  const auto preprocess_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          preprocess_finished - preprocess_started).count());
  lock.lock();
  if (!preprocessed) {
    ++preprocess_failures_;
    __android_log_print(ANDROID_LOG_ERROR, "VisionForgeDual", "preprocess failed rendered=%llu", static_cast<unsigned long long>(rendered_frames_));
    return false;
  }
  if (!inference_worker_running_.load(std::memory_order_acquire) ||
      !is_current_stream_generation_locked(pending_metadata->stream_generation)) {
    ++stale_decoded_outputs_;
    return true;
  }
  last_preprocess_us_ = preprocess_us;
  record_latency(preprocess_samples_us_, last_preprocess_us_);
  {
    std::scoped_lock inference_lock(inference_mutex_);
    if (!inference_worker_running_.load(std::memory_order_acquire)) {
      ++stale_decoded_outputs_;
      return false;
    }
    PendingInferenceFrame inference_frame{};
    inference_frame.input.swap(model_input_);
    if (pending_inference_frames_.size() >= kInferenceQueueCapacity ||
        free_model_inputs_.empty()) {
      if (pending_inference_frames_.empty()) {
        model_input_.swap(inference_frame.input);
        ++preprocess_failures_;
        return false;
      }
      if (!content_frame_can_supersede(
              pending_inference_frames_.front().content_updated,
              pending_metadata->content_updated)) {
        // Keep the already queued real observation. The repeat remains useful
        // only when inference capacity is available; it cannot erase a control
        // opportunity from the queue.
        model_input_.swap(inference_frame.input);
        ++superseded_preprocessed_frames_;
        return true;
      }
      record_outcome(pending_inference_frames_.front(),PipelineOutcome::preprocessed_superseded,
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
      model_input_.swap(pending_inference_frames_.front().input);
      pending_inference_frames_.pop_front();
      ++superseded_preprocessed_frames_;
    } else {
      model_input_.swap(free_model_inputs_.back());
      free_model_inputs_.pop_back();
    }
    inference_frame.frame_id = pending_metadata->frame_id;
    inference_frame.control_sequence = pending_metadata->control_sequence;
    inference_frame.content_updated = pending_metadata->content_updated;
    inference_frame.stream_generation = pending_metadata->stream_generation;
    inference_frame.observed_at_us = pending_metadata->observed_at_us;
    inference_frame.received_at_us = presentation_us;
    inference_frame.preprocess_us = last_preprocess_us_;
    inference_frame.decode_queue_us = decode_queue_us;
    inference_frame.decoded_planes_us = now_us;
    inference_frame.preprocess_started_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(preprocess_started.time_since_epoch()).count());
    inference_frame.preprocess_finished_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(preprocess_finished.time_since_epoch()).count());
    inference_frame.enqueued_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    pending_inference_frames_.push_back(std::move(inference_frame));
    inference_queue_high_watermark_.store(
        std::max(
            inference_queue_high_watermark_.load(std::memory_order_relaxed),
            static_cast<std::uint64_t>(pending_inference_frames_.size())),
        std::memory_order_relaxed);
  }
  inference_condition_.notify_one();
  return true;
}

void NativeH264Decoder::update_java_decoder_format(
    std::string codec_name, std::int32_t color_format, std::int32_t color_standard,
    std::int32_t color_range, std::int32_t width, std::int32_t height) {
  std::scoped_lock lock(mutex_);
  java_decoder_name_ = std::move(codec_name);
  output_color_format_ = color_format;
  output_color_standard_ = color_standard;
  output_color_range_ = color_range;
  if (width > 0) decoded_width_ = width;
  if (height > 0) decoded_height_ = height;
}

bool NativeH264Decoder::has_fatal_decoder_failure() const {
  std::scoped_lock lock(mutex_);
  return java_decoder_failed_;
}

void NativeH264Decoder::discard_java_decoded_output(
    std::int64_t presentation_time_us, std::string reason, bool fatal) {
  std::scoped_lock lock(mutex_);
  if(const auto frame=take_pending_frame_for_output_locked(presentation_time_us))
    record_outcome(*frame,PipelineOutcome::decoded_discarded,fatal?1U:0U);
  ++rejected_output_images_;
  if (fatal) {
    java_decoder_failed_ = true;
    java_decoder_failure_ = reason;
  }
  if (rejected_output_images_ <= 3U ||
      (rejected_output_images_ & (rejected_output_images_ - 1U)) == 0U) {
    __android_log_print(
        fatal ? ANDROID_LOG_ERROR : ANDROID_LOG_WARN, "VisionForgeDual",
        "Java decoded output rejected count=%llu fatal=%d reason=%s",
        static_cast<unsigned long long>(rejected_output_images_), fatal,
        reason.c_str());
  }
}

void NativeH264Decoder::coalesce_java_decoded_output(std::int64_t older_us, std::int64_t newer_us) {
  std::scoped_lock lock(mutex_);
  const auto older = take_pending_frame_for_output_locked(older_us);
  if(older)record_outcome(*older,PipelineOutcome::decoded_discarded,2U);
  const auto newer = std::find_if(pending_frames_.begin(), pending_frames_.end(),
      [newer_us](const PendingFrame& frame) { return frame.presentation_us == newer_us; });
  if (older && newer != pending_frames_.end() && older->content_updated &&
      older->stream_generation == newer->stream_generation && newer_us > older_us) {
    newer->content_updated = true;
  }
  ++superseded_decoded_outputs_;
}

void NativeH264Decoder::discard_java_compressed_access_unit(
    std::int64_t presentation_time_us, std::string reason, bool fatal) {
  std::scoped_lock lock(mutex_);
  if(const auto frame=take_pending_frame_for_output_locked(presentation_time_us))
    record_outcome(*frame,PipelineOutcome::compressed_discarded,fatal?1U:0U);
  ++dropped_access_units_;
  input_drop_metrics_.record_async(reason);
  if (fatal) {
    java_decoder_failed_ = true;
    java_decoder_failure_ = reason;
  }
  const auto reason_total = input_drop_metrics_.total();
  if (reason_total <= 3U || (reason_total & (reason_total - 1U)) == 0U) {
    __android_log_print(
        fatal ? ANDROID_LOG_ERROR : ANDROID_LOG_WARN, "VisionForgeDual",
        "Java compressed access unit dropped count=%llu fatal=%d reason=%s",
        static_cast<unsigned long long>(reason_total), fatal, reason.c_str());
  }
}

bool NativeH264Decoder::restart_after_stream_discontinuity() {
  JavaVM* java_vm{};
  jmethodID restart_method{};
  std::int32_t width{};
  std::int32_t height{};
  std::uint64_t generation{};
  std::uint64_t binding_generation{};
  {
    std::scoped_lock lock(mutex_);
    // Multiple Host epoch changes may arrive while Java is recreating
    // MediaCodec.  The receiver keeps only the newest candidate epoch; native
    // therefore coalesces every overlapping request into the one in-flight
    // restart instead of treating Java's busy response as fatal.
    if (stream_restart_in_progress_) return true;
    java_vm = java_vm_;
    restart_method = java_restart_after_discontinuity_method_;
    binding_generation = java_decoder_binding_generation_;
    width = decoded_width_;
    height = decoded_height_;
    java_decoder_failed_ = false;
    java_decoder_failure_ = "stream_source_changed_restart_in_progress";
    generation = ++stream_restart_generation_;
    stream_restart_started_us_ = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    stream_restart_in_progress_ = true;
    pending_stream_restart_completion_.reset();
    pending_makcu_resume_generation_.reset();
    pending_frames_.clear();
    suspend_makcu_output_for_recovery(generation);
  }
  {
    std::scoped_lock inference_lock(inference_mutex_);
    recycle_pending_inference_frames_locked();
  }
  if (java_vm == nullptr || restart_method == nullptr ||
      width <= 0 || height <= 0) {
    std::scoped_lock lock(mutex_);
    if (generation != stream_restart_generation_ ||
        binding_generation != java_decoder_binding_generation_ || !stream_restart_in_progress_) return false;
    ++stream_restart_failures_;
    stream_restart_in_progress_ = false;
    disable_makcu_output();
    java_decoder_failure_ = "stream_source_changed_restart_bridge_unavailable";
    return false;
  }
  JNIEnv* environment = current_thread_environment(java_vm);
  if (environment == nullptr) {
    std::scoped_lock lock(mutex_);
    if (generation != stream_restart_generation_ ||
        binding_generation != java_decoder_binding_generation_ || !stream_restart_in_progress_) return false;
    ++stream_restart_failures_;
    stream_restart_in_progress_ = false;
    disable_makcu_output();
    java_decoder_failure_ = "stream_source_changed_restart_jni_unavailable";
    return false;
  }
  struct LocalDecoderReference final {
    JNIEnv* environment{};
    jobject object{};
    ~LocalDecoderReference() { if (object != nullptr) environment->DeleteLocalRef(object); }
  } reference{environment, nullptr};
  {
    std::scoped_lock lock(mutex_);
    if (java_vm != java_vm_ || generation != stream_restart_generation_ ||
        binding_generation != java_decoder_binding_generation_ || !stream_restart_in_progress_) return false;
    if (java_decoder_object_ != nullptr) reference.object = environment->NewLocalRef(java_decoder_object_);
  }
  const bool reference_failed = environment->ExceptionCheck() || reference.object == nullptr;
  if (environment->ExceptionCheck()) environment->ExceptionClear();
  if (reference_failed) {
    std::scoped_lock lock(mutex_);
    if (generation != stream_restart_generation_ ||
        binding_generation != java_decoder_binding_generation_ || !stream_restart_in_progress_) return false;
    ++stream_restart_failures_;
    stream_restart_in_progress_ = false;
    java_decoder_failed_ = true;
    java_decoder_failure_ = "stream_source_changed_restart_reference_unavailable";
    disable_makcu_output();
    return false;
  }
  const auto started = std::chrono::steady_clock::now();
  const jboolean restarted = environment->CallBooleanMethod(
      reference.object, restart_method, static_cast<jint>(width), static_cast<jint>(height),
      static_cast<jlong>(generation));
  bool callback_failed = false;
  if (environment->ExceptionCheck()) {
    environment->ExceptionDescribe();
    environment->ExceptionClear();
    callback_failed = true;
  }
  const auto elapsed_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started).count());
  const bool scheduled = restarted == JNI_TRUE && !callback_failed;
  {
    std::scoped_lock lock(mutex_);
    if (generation != stream_restart_generation_ ||
        binding_generation != java_decoder_binding_generation_) return false;
    last_stream_restart_request_us_ = elapsed_us;
    if (!scheduled && stream_restart_in_progress_) {
      ++stream_restart_failures_;
      stream_restart_in_progress_ = false;
      java_decoder_failed_ = true;
      java_decoder_failure_ = "stream_source_changed_restart_schedule_failed";
      pending_makcu_resume_generation_.reset();
      disable_makcu_output();
    }
  }
  __android_log_print(
      scheduled ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "VisionForgeDual",
      "stream decoder restart scheduled=%d generation=%llu request_us=%llu output_enabled=0",
      scheduled ? 1 : 0, static_cast<unsigned long long>(generation),
      static_cast<unsigned long long>(elapsed_us));
  return scheduled;
}

void NativeH264Decoder::complete_stream_restart(
    std::uint64_t generation, bool succeeded, std::uint64_t elapsed_us,
    std::string diagnostic) {
  std::scoped_lock lock(mutex_);
  if (generation != stream_restart_generation_ || !stream_restart_in_progress_) {
    __android_log_print(
        ANDROID_LOG_WARN, "VisionForgeDual",
        "ignored stale stream restart completion generation=%llu active=%llu",
        static_cast<unsigned long long>(generation),
        static_cast<unsigned long long>(stream_restart_generation_));
    return;
  }
  stream_restart_in_progress_ = false;
  last_stream_restart_us_ = elapsed_us;
  pending_stream_restart_completion_ = StreamRestartCompletion{
      generation, succeeded, elapsed_us};
  if (succeeded) {
    ++stream_restarts_;
    java_decoder_failed_ = false;
    java_decoder_failure_.clear();
    pending_makcu_resume_generation_ = generation;
  } else {
    ++stream_restart_failures_;
    java_decoder_failed_ = true;
    java_decoder_failure_ = diagnostic.empty()
        ? "stream_source_changed_restart_failed" : std::move(diagnostic);
    pending_makcu_resume_generation_.reset();
    disable_makcu_output();
  }
  __android_log_print(
      succeeded ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "VisionForgeDual",
      "stream decoder restart completed=%d generation=%llu elapsed_us=%llu",
      succeeded ? 1 : 0, static_cast<unsigned long long>(generation),
      static_cast<unsigned long long>(elapsed_us));
}

std::optional<NativeH264Decoder::StreamRestartCompletion>
NativeH264Decoder::take_stream_restart_completion() {
  std::scoped_lock lock(mutex_);
  auto completion = pending_stream_restart_completion_;
  pending_stream_restart_completion_.reset();
  return completion;
}

bool NativeH264Decoder::has_stream_restart_timed_out() {
  std::scoped_lock lock(mutex_);
  if (!stream_restart_in_progress_ || stream_restart_started_us_ == 0U) return false;
  const auto now_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
  if (now_us < stream_restart_started_us_ ||
      now_us - stream_restart_started_us_ < kStreamRestartTimeoutUs) return false;
  stream_restart_in_progress_ = false;
  pending_stream_restart_completion_.reset();
  pending_makcu_resume_generation_.reset();
  ++stream_restart_generation_;
  ++stream_restart_failures_;
  java_decoder_failed_ = true;
  java_decoder_failure_ = "stream_source_changed_restart_timeout_250ms";
  disable_makcu_output();
  __android_log_print(
      ANDROID_LOG_ERROR, "VisionForgeDual",
      "stream decoder restart timeout elapsed_us=%llu; full recovery required",
      static_cast<unsigned long long>(now_us - stream_restart_started_us_));
  return true;
}

void NativeH264Decoder::mark_stream_discontinuity() {
  disable_makcu_output();
  {
    std::scoped_lock inference_lock(inference_mutex_);
    recycle_pending_inference_frames_locked();
  }
  std::scoped_lock lock(mutex_);
  ++stream_restart_generation_;
  stream_restart_in_progress_ = false;
  pending_stream_restart_completion_.reset();
  pending_makcu_resume_generation_.reset();
  pending_frames_.clear();
  java_decoder_failed_ = true;
  java_decoder_failure_ = "stream_network_source_changed_full_recovery_required";
}

void NativeH264Decoder::stop() {
  disable_makcu_output();
  output_worker_running_ = false;
  inference_worker_running_ = false;
  {
    std::scoped_lock inference_lock(inference_mutex_);
    recycle_pending_inference_frames_locked();
  }
  inference_condition_.notify_all();
  if (output_worker_.joinable()) output_worker_.join();
  if (inference_worker_.joinable()) inference_worker_.join();
  std::scoped_lock callback_lock(java_output_callback_mutex_);
  std::scoped_lock lock(mutex_);
  ++stream_restart_generation_;
  stream_restart_in_progress_ = false;
  pending_stream_restart_completion_.reset();
  pending_makcu_resume_generation_.reset();
  if (codec_ != nullptr) { AMediaCodec_stop(codec_); AMediaCodec_delete(codec_); codec_ = nullptr; }
  if (output_window_ != nullptr && output_window_owned_) {
    ANativeWindow_release(output_window_);
    output_window_ = nullptr;
  }
  output_window_owned_ = false;
  java_image_decoder_mode_ = false;
  pending_frames_.clear();
}

void NativeH264Decoder::recycle_pending_inference_frames_locked() {
  while (!pending_inference_frames_.empty()) {
    free_model_inputs_.push_back(std::move(pending_inference_frames_.front().input));
    pending_inference_frames_.pop_front();
  }
}

bool NativeH264Decoder::is_current_stream_generation_locked(
    std::uint64_t generation) const noexcept {
  return inference_result_can_commit(
      generation, stream_restart_generation_, stream_restart_in_progress_,
      java_decoder_failed_);
}

void NativeH264Decoder::begin_detection_trace(std::size_t maximum_frames) {
  std::scoped_lock lock(mutex_);
  detection_trace_frames_.clear();
  detection_trace_maximum_frames_ = maximum_frames;
  detection_trace_enabled_ = maximum_frames != 0;
}

std::string NativeH264Decoder::detection_trace_csv() const {
  std::scoped_lock lock(mutex_);
  std::ostringstream value;
  value << "frame_id,class_id,confidence,x1,y1,x2,y2\n";
  for (const auto& frame : detection_trace_frames_) {
    if (frame.detections.empty()) {
      value << frame.frame_id << ",-1,0,0,0,0,0\n";
      continue;
    }
    for (const auto& detection : frame.detections) {
      value << frame.frame_id << ',' << detection.class_id << ',' << detection.confidence << ','
            << detection.x1 << ',' << detection.y1 << ',' << detection.x2 << ',' << detection.y2 << '\n';
    }
  }
  return value.str();
}

std::string NativeH264Decoder::pipeline_metrics_csv() const {
  std::scoped_lock lock(mutex_);
  std::ostringstream value;
  value << "frame_id,decode_queue_us,preprocess_us,qnn_us,detection_count,qnn_status\n";
  for (std::size_t index = 0; index < pipeline_metrics_.size(); ++index) {
    const auto& sample = pipeline_metrics_.oldest_at(index);
    value << sample.frame_id << ',' << sample.decode_queue_us << ',' << sample.preprocess_us << ',' << sample.qnn_us << ','
          << sample.detections << ',' << sample.qnn_status << '\n';
  }
  return value.str();
}

std::string NativeH264Decoder::drain_pipeline_timing_trace() {
  // Keep the bounded copy under mutex_, and all allocation/formatting outside
  // the decoder/inference lock. The snapshot is too large for the JNI stack.
  auto snapshot = std::make_unique<PipelineTimingTrace::Snapshot>();
  {
    std::scoped_lock lock(mutex_);
    pipeline_timing_trace_.drain(*snapshot);
  }
  auto outcomes=std::make_unique<PipelineOutcomeTrace::Snapshot>();
  {
    std::scoped_lock lock(outcome_mutex_);
    outcome_trace_.drain(*outcomes);
  }
  std::unique_ptr<ProjectionTimingTrace::Snapshot> projection;
  bool projection_pending{};
  {std::scoped_lock lock(projection_trace_mutex_);projection_pending=projection_timing_trace_.has_pending();}
  if(projection_pending) {
    projection=std::make_unique<ProjectionTimingTrace::Snapshot>();
    std::scoped_lock lock(projection_trace_mutex_);projection_timing_trace_.drain(*projection);
  }
  return snapshot->report()+" "+outcomes->report()+(projection?" "+projection->report():"");
}

std::string NativeH264Decoder::java_input_health_report(
    JNIEnv* environment, jobject decoder_object, jmethodID method) {
  if (environment == nullptr || decoder_object == nullptr ||
      method == nullptr) {
    return "unavailable";
  }
  auto* report = static_cast<jstring>(environment->CallObjectMethod(
      decoder_object, method));
  if (environment->ExceptionCheck()) {
    environment->ExceptionClear();
    if (report != nullptr) environment->DeleteLocalRef(report);
    return "jni_exception";
  }
  if (report == nullptr) return "unavailable";
  const char* utf = environment->GetStringUTFChars(report, nullptr);
  if (environment->ExceptionCheck()) {
    environment->ExceptionClear();
    if (utf != nullptr) environment->ReleaseStringUTFChars(report, utf);
    environment->DeleteLocalRef(report);
    return "jni_exception";
  }
  std::string value = utf == nullptr ? "unavailable" : utf;
  if (utf != nullptr) environment->ReleaseStringUTFChars(report, utf);
  environment->DeleteLocalRef(report);
  return value;
}

std::string NativeH264Decoder::report() const {
  // Allocation and potential JNI thread attachment are outside decoder state.
  auto snapshot=std::make_unique<NativeDecoderReportSnapshot>();
  snapshot->java_decoder_name_.reserve(256U);
  snapshot->java_decoder_failure_.reserve(4096U);
  JavaVM* java_vm{};
  std::uint64_t wait_us{},hold_us{};
  {
    const auto began=AtomicDurationSummary::now_us();
    std::unique_lock lock(mutex_);
    const auto acquired=AtomicDurationSummary::now_us();
    wait_us+=acquired-began;java_vm=java_vm_;
    const auto releasing=AtomicDurationSummary::now_us();
    lock.unlock();
    hold_us+=releasing-acquired;
  }
  JNIEnv* environment=current_thread_environment(java_vm);
  struct LocalDecoderReference final {
    JNIEnv* environment{};jobject object{};
    ~LocalDecoderReference(){if(environment && object)environment->DeleteLocalRef(object);}
  } reference{environment,nullptr};
  jmethodID input_health_method{};
  {
    const auto began=AtomicDurationSummary::now_us();
    std::unique_lock lock(mutex_);
    const auto acquired=AtomicDurationSummary::now_us();
    wait_us+=acquired-began;snapshot->now_us_=acquired;
    snapshot->configured_=(codec_!=nullptr || java_image_decoder_mode_) &&
        !java_decoder_failed_ && !stream_restart_in_progress_;
    snapshot->pending_access_units_=pending_frames_.size();
    snapshot->last_detections_total_=last_detections_.size();
    std::copy_n(last_detections_.begin(),std::min(last_detections_.size(),snapshot->last_detections_.size()),snapshot->last_detections_.begin());
    snapshot->requested_color_format_=kColorFormatYuv420Flexible;
    snapshot->inference_queue_capacity_=kInferenceQueueCapacity;
    snapshot->rgb_input_buffer_count_=kRgbInputBufferCount;
    snapshot->last_qnn_success_monotonic_us_=last_qnn_success_monotonic_us_;
    snapshot->last_qnn_failure_monotonic_us_=last_qnn_failure_monotonic_us_;
    snapshot->queued_access_units_=queued_access_units_;
    snapshot->sync_access_units_=sync_access_units_;
    snapshot->last_input_nal_type_mask_=last_input_nal_type_mask_;
    snapshot->decoded_width_=decoded_width_;
    snapshot->decoded_height_=decoded_height_;
    snapshot->java_image_decoder_mode_=java_image_decoder_mode_;
    snapshot->java_decoder_name_=java_decoder_name_;
    snapshot->java_decoder_failed_=java_decoder_failed_;
    snapshot->java_decoder_failure_=java_decoder_failure_;
    snapshot->stream_restarts_=stream_restarts_;
    snapshot->stream_restart_failures_=stream_restart_failures_;
    snapshot->stream_restart_in_progress_=stream_restart_in_progress_;
    snapshot->last_stream_restart_us_=last_stream_restart_us_;
    snapshot->last_stream_restart_request_us_=last_stream_restart_request_us_;
    snapshot->output_color_format_=output_color_format_;
    snapshot->output_color_standard_=output_color_standard_;
    snapshot->output_color_range_=output_color_range_;
    snapshot->image_y_row_stride_=image_y_row_stride_;
    snapshot->image_u_row_stride_=image_u_row_stride_;
    snapshot->image_v_row_stride_=image_v_row_stride_;
    snapshot->image_y_pixel_stride_=image_y_pixel_stride_;
    snapshot->image_u_pixel_stride_=image_u_pixel_stride_;
    snapshot->image_v_pixel_stride_=image_v_pixel_stride_;
    snapshot->image_crop_left_=image_crop_left_;
    snapshot->image_crop_top_=image_crop_top_;
    snapshot->image_crop_right_=image_crop_right_;
    snapshot->image_crop_bottom_=image_crop_bottom_;
    snapshot->image_layout_failures_=image_layout_failures_;
    snapshot->rejected_output_images_=rejected_output_images_;
    snapshot->acquired_images_=acquired_images_;
    snapshot->rendered_frames_=rendered_frames_;
    snapshot->fresh_content_outputs_=fresh_content_outputs_;
    snapshot->repeated_content_outputs_=repeated_content_outputs_;
    snapshot->stale_decoded_outputs_=stale_decoded_outputs_;
    snapshot->superseded_decoded_outputs_=superseded_decoded_outputs_;
    snapshot->superseded_preprocessed_frames_=superseded_preprocessed_frames_;
    snapshot->inference_queue_high_watermark_=inference_queue_high_watermark_.load(std::memory_order_relaxed);
    snapshot->stale_pre_inference_frames_=stale_pre_inference_frames_.load(std::memory_order_relaxed);
    snapshot->maximum_pre_inference_frame_age_us_=maximum_pre_inference_frame_age_us_.load(std::memory_order_relaxed);
    snapshot->deadline_replaced_pre_inference_frames_=deadline_replaced_pre_inference_frames_.load(std::memory_order_relaxed);
    snapshot->deadline_replacement_waits_=deadline_replacement_waits_.load(std::memory_order_relaxed);
    snapshot->deadline_replacement_fallbacks_=deadline_replacement_fallbacks_.load(std::memory_order_relaxed);
    snapshot->deadline_maximum_replacement_wait_us_=deadline_maximum_replacement_wait_us_.load(std::memory_order_relaxed);
    snapshot->deadline_backend_estimate_us_=deadline_backend_estimate_us_.load(std::memory_order_relaxed);
    snapshot->stale_generation_inference_suppressions_=stale_generation_inference_suppressions_;
    snapshot->stale_generation_result_suppressions_=stale_generation_result_suppressions_;
    snapshot->stale_generation_control_suppressions_=stale_generation_control_suppressions_;
    snapshot->repeated_content_inferences_=repeated_content_inferences_;
    snapshot->repeated_content_inference_suppressions_=repeated_content_inference_suppressions_;
    snapshot->repeated_content_control_suppressions_=repeated_content_control_suppressions_;
    snapshot->dropped_access_units_=dropped_access_units_;
    snapshot->input_drop_metrics_=input_drop_metrics_;
    snapshot->qnn_executions_=qnn_executions_;
    snapshot->qnn_failures_=qnn_failures_;
    snapshot->consecutive_qnn_failures_=consecutive_qnn_failures_;
    snapshot->preprocess_failures_=preprocess_failures_;
    snapshot->last_detection_count_=last_detection_count_;
    snapshot->last_detection_source_generation_=last_detection_source_generation_;
    snapshot->last_detection_source_sequence_=last_detection_source_sequence_;
    snapshot->last_detection_source_wire_frame_id_=last_detection_source_wire_frame_id_;
    snapshot->last_detection_source_observed_us_=last_detection_source_observed_us_;
    snapshot->pixel_projection_mode_=pixel_projection_mode_.load(std::memory_order_relaxed);
    snapshot->pixel_projection_candidates_=pixel_projection_candidates_.load(std::memory_order_relaxed);
    snapshot->pixel_projection_attempts_=pixel_projection_attempts_.load(std::memory_order_relaxed);
    snapshot->pixel_projection_complete_=pixel_projection_complete_.load(std::memory_order_relaxed);
    snapshot->pixel_projection_math_rejections_=pixel_projection_math_rejections_.load(std::memory_order_relaxed);
    snapshot->pixel_projection_gate_rejections_=pixel_projection_gate_rejections_.load(std::memory_order_relaxed);
    snapshot->pixel_projection_control_publications_=pixel_projection_control_publications_.load(std::memory_order_relaxed);
    snapshot->pixel_projection_old_source_skips_=pixel_projection_old_source_skips_.load(std::memory_order_relaxed);
    snapshot->pixel_projection_shadow_compared_=pixel_projection_shadow_compared_.load(std::memory_order_relaxed);
    snapshot->pixel_projection_shadow_unscored_=pixel_projection_shadow_unscored_.load(std::memory_order_relaxed);
    snapshot->last_qnn_status_=last_qnn_status_;
    snapshot->preprocess_samples_us_=preprocess_samples_us_;
    snapshot->last_preprocess_us_=last_preprocess_us_;
    snapshot->qnn_samples_us_=qnn_samples_us_;
    snapshot->last_qnn_us_=last_qnn_us_;
    snapshot->inference_total_samples_us_=inference_total_samples_us_;
    snapshot->last_inference_total_us_=last_inference_total_us_;
    snapshot->decode_queue_samples_us_=decode_queue_samples_us_;
    snapshot->receive_to_result_samples_us_=receive_to_result_samples_us_;
    snapshot->host_plus_receive_to_result_samples_us_=host_plus_receive_to_result_samples_us_;
    snapshot->control_dispatch_samples_us_=control_dispatch_samples_us_;
    snapshot->background_motion_samples_us_=background_motion_samples_us_;
    snapshot->image_motion_samples_us_=image_motion_samples_us_;
    snapshot->image_motion_budget_exhaustions_=image_motion_budget_exhaustions_;
    snapshot->receive_to_control_samples_us_=receive_to_control_samples_us_;
    // bind/release may delete the global reference after this scope. A local
    // reference keeps this exact object and its class/method alive meanwhile.
    if(environment && java_vm==java_vm_ && java_decoder_object_ && java_input_health_report_method_) {
      reference.object=environment->NewLocalRef(java_decoder_object_);
      input_health_method=java_input_health_report_method_;
    }
    const auto releasing=AtomicDurationSummary::now_us();
    lock.unlock();
    hold_us+=releasing-acquired;
  }
  report_snapshot_mutex_wait_.record(wait_us);
  report_snapshot_mutex_hold_.record(hold_us);
  std::string input_health;
  if(environment && environment->ExceptionCheck()) {
    environment->ExceptionClear();input_health="jni_exception";
  } else input_health=java_input_health_report(environment,reference.object,input_health_method);
  std::ostringstream submit_summaries;
  submit_duration_.append(submit_summaries,"native_submit");
  submit_first_mutex_wait_.append(submit_summaries,"native_submit_first_mutex_wait");
  submit_second_mutex_wait_.append(submit_summaries,"native_submit_second_mutex_wait");
  submit_java_offer_duration_.append(submit_summaries,"native_submit_java_offer");
  // These subsystems have independent snapshots. Never nest their work under
  // the decoder mutex or run formatting/sorting on a decode callback's lock.
  const auto bridge_report=makcu_move_bridge_report();
  std::ostringstream value;
  value<<format_native_decoder_report_snapshot(*snapshot,realtime_inference_backend_token(),
      input_health,bridge_report,submit_summaries.str());
  report_snapshot_mutex_wait_.append(value,"decoder_report_snapshot_mutex_wait");
  report_snapshot_mutex_hold_.append(value,"decoder_report_snapshot_mutex_hold");
  return value.str();
}

NativeH264Decoder::~NativeH264Decoder() { stop(); }
NativeH264Decoder& decoder() { static NativeH264Decoder instance; return instance; }
}  // namespace vfdual_android

extern "C" JNIEXPORT jboolean JNICALL
Java_com_visionforge_inferencebenchmark_QnnHtpBridge_configureNativeH264Decoder(
    JNIEnv* environment, jclass, jobject surface, jint width, jint height) {
  return vfdual_android::decoder().configure(environment, surface, width, height) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_visionforge_inferencebenchmark_QnnHtpBridge_stopNativeH264Decoder(JNIEnv*, jclass) {
  vfdual_android::decoder().stop();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_visionforge_inferencebenchmark_QnnHtpBridge_configureNativeH264InferenceDecoder(JNIEnv*, jclass, jint width, jint height) {
  return vfdual_android::decoder().configure_for_inference(width, height) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_visionforge_inferencebenchmark_QnnHtpBridge_bindNativeH264AccessUnitBridge(
    JNIEnv* environment, jclass, jobject decoder) {
  vfdual_android::decoder().bind_java_decoder(environment, decoder);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_visionforge_inferencebenchmark_QnnHtpBridge_offerNativeDecodedYuv420(
    JNIEnv* environment, jclass,
    jobject y_buffer, jint y_offset, jint y_remaining, jint y_row_stride, jint y_pixel_stride,
    jobject u_buffer, jint u_offset, jint u_remaining, jint u_row_stride, jint u_pixel_stride,
    jobject v_buffer, jint v_offset, jint v_remaining, jint v_row_stride, jint v_pixel_stride,
    jint image_width, jint image_height, jint crop_left, jint crop_top,
    jint crop_right, jint crop_bottom, jlong presentation_time_us) {
  const auto plane = [environment](
      jobject buffer, jint offset, jint remaining, jint row_stride,
      jint pixel_stride, vfdual::Yuv420PlaneView& output) {
    const jlong capacity = buffer == nullptr ? -1 : environment->GetDirectBufferCapacity(buffer);
    auto* address = buffer == nullptr
        ? nullptr
        : static_cast<std::uint8_t*>(environment->GetDirectBufferAddress(buffer));
    if (address == nullptr || capacity < 0 || offset < 0 || remaining <= 0 ||
        row_stride <= 0 || pixel_stride <= 0 ||
        static_cast<jlong>(offset) + remaining > capacity) {
      return false;
    }
    output = {
        address + offset, static_cast<std::size_t>(remaining),
        static_cast<std::uint32_t>(row_stride), static_cast<std::uint32_t>(pixel_stride)};
    return true;
  };
  vfdual::Yuv420PlaneView y_plane;
  vfdual::Yuv420PlaneView u_plane;
  vfdual::Yuv420PlaneView v_plane;
  const bool valid = plane(y_buffer, y_offset, y_remaining, y_row_stride, y_pixel_stride, y_plane) &&
      plane(u_buffer, u_offset, u_remaining, u_row_stride, u_pixel_stride, u_plane) &&
      plane(v_buffer, v_offset, v_remaining, v_row_stride, v_pixel_stride, v_plane);
  if (!valid) {
    // Java has already dequeued this picture. A rejected JNI plane cannot
    // produce a later callback that would retire its pending metadata.
    vfdual_android::decoder().discard_java_decoded_output(
        presentation_time_us, "native_plane_buffer_invalid", false);
    return JNI_FALSE;
  }
  return vfdual_android::decoder().process_java_yuv420(
      y_plane, u_plane, v_plane, image_width, image_height,
      crop_left, crop_top, crop_right, crop_bottom, presentation_time_us)
      ? JNI_TRUE
      : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_visionforge_inferencebenchmark_QnnHtpBridge_discardNativeDecodedOutput(
    JNIEnv* environment, jclass, jlong presentation_time_us, jstring reason, jboolean fatal) {
  const char* utf = reason == nullptr ? nullptr : environment->GetStringUTFChars(reason, nullptr);
  std::string value = utf == nullptr ? "unknown" : utf;
  if (utf != nullptr) environment->ReleaseStringUTFChars(reason, utf);
  vfdual_android::decoder().discard_java_decoded_output(
      presentation_time_us, std::move(value), fatal == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_visionforge_inferencebenchmark_QnnHtpBridge_coalesceNativeDecodedOutput(
    JNIEnv*, jclass, jlong older_us, jlong newer_us) {
  vfdual_android::decoder().coalesce_java_decoded_output(older_us, newer_us);
}

extern "C" JNIEXPORT void JNICALL
Java_com_visionforge_inferencebenchmark_QnnHtpBridge_discardNativeCompressedAccessUnit(
    JNIEnv* environment, jclass, jlong presentation_time_us, jstring reason, jboolean fatal) {
  const char* utf = reason == nullptr ? nullptr : environment->GetStringUTFChars(reason, nullptr);
  std::string value = utf == nullptr ? "unknown" : utf;
  if (utf != nullptr) environment->ReleaseStringUTFChars(reason, utf);
  vfdual_android::decoder().discard_java_compressed_access_unit(
      presentation_time_us, std::move(value), fatal == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_visionforge_inferencebenchmark_QnnHtpBridge_updateNativeH264JavaDecoderFormat(
    JNIEnv* environment, jclass, jstring codec_name, jint color_format,
    jint color_standard, jint color_range, jint width, jint height) {
  const char* utf = codec_name == nullptr ? nullptr : environment->GetStringUTFChars(codec_name, nullptr);
  std::string name = utf == nullptr ? "unknown" : utf;
  if (utf != nullptr) environment->ReleaseStringUTFChars(codec_name, utf);
  vfdual_android::decoder().update_java_decoder_format(
      std::move(name), color_format, color_standard, color_range, width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_visionforge_inferencebenchmark_QnnHtpBridge_completeNativeH264StreamRestart(
    JNIEnv* environment, jclass, jlong generation, jboolean succeeded,
    jlong elapsed_us, jstring diagnostic) {
  std::string detail;
  if (environment != nullptr && diagnostic != nullptr) {
    const char* utf = environment->GetStringUTFChars(diagnostic, nullptr);
    if (utf != nullptr) {
      detail = utf;
      environment->ReleaseStringUTFChars(diagnostic, utf);
    }
  }
  vfdual_android::decoder().complete_stream_restart(
      static_cast<std::uint64_t>(generation), succeeded == JNI_TRUE,
      elapsed_us < 0 ? 0U : static_cast<std::uint64_t>(elapsed_us), std::move(detail));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_visionforge_inferencebenchmark_QnnHtpBridge_getNativeH264DecoderReport(JNIEnv* environment, jclass) {
  const std::string value = vfdual_android::decoder().report();
  return environment->NewStringUTF(value.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_visionforge_inferencebenchmark_QnnHtpBridge_getNativeControlTraceReport(JNIEnv* environment, jclass) {
  const std::string report = vfdual_android::makcu_move_bridge_trace();
  return environment->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_visionforge_inferencebenchmark_QnnHtpBridge_getNativePipelineTraceReport(JNIEnv* environment, jclass) {
  const std::string report = vfdual_android::decoder().drain_pipeline_timing_trace();
  return environment->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_visionforge_inferencebenchmark_QnnHtpBridge_getNativePipelineMetricsCsv(JNIEnv* environment, jclass) {
  const std::string value = vfdual_android::decoder().pipeline_metrics_csv();
  return environment->NewStringUTF(value.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_visionforge_inferencebenchmark_QnnHtpBridge_beginNativeH264DetectionTrace(JNIEnv*, jclass, jint maximum_frames) {
  vfdual_android::decoder().begin_detection_trace(maximum_frames > 0 ? static_cast<std::size_t>(maximum_frames) : 0U);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_visionforge_inferencebenchmark_QnnHtpBridge_getNativeH264DetectionTraceCsv(JNIEnv* environment, jclass) {
  const std::string value = vfdual_android::decoder().detection_trace_csv();
  return environment->NewStringUTF(value.c_str());
}
