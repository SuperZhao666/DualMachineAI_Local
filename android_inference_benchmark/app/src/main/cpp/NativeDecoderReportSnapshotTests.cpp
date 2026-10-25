#define VF_NATIVE_DECODER_REPORT_ONLY
#include "NativeH264Decoder.hpp"
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
using namespace vfdual_android;

int main() {
  NativeDecoderReportSnapshot value;
  const auto format=[&](const NativeDecoderReportSnapshot& snapshot) {
    return format_native_decoder_report_snapshot(snapshot,"qnn_htp","contract=bounded", "output_requested=1", " native_submit_count=8");
  };
  auto empty=format(value);
  assert(empty.find("configured=0")!=std::string::npos);
  assert(empty.find("last_qnn_success_age_ms=-1 last_qnn_failure_age_ms=-1")!=std::string::npos);
  assert(empty.find("decoder_failure=none")!=std::string::npos);
  assert(empty.find("preprocess{n=0 last_us=0}")!=std::string::npos);
  assert(empty.find("last_detections=[] makcu_bridge{output_requested=1}")!=std::string::npos);
  value.now_us_=18'014'398'509'481'984ULL;
  value.last_qnn_success_monotonic_us_=value.now_us_-123'456;
  value.last_qnn_failure_monotonic_us_=value.now_us_-456'789;
  value.configured_=true;value.queued_access_units_=19;value.sync_access_units_=2;value.pending_access_units_=3;
  value.requested_color_format_=0x7f420888;value.decoded_width_=value.decoded_height_=640;
  value.java_image_decoder_mode_=true;value.java_decoder_name_="hardware_test_codec";
  value.java_decoder_failure_=std::string(9000,'f');
  value.inference_queue_capacity_=1;value.rgb_input_buffer_count_=4;value.pixel_projection_mode_=2;
  value.input_drop_metrics_.codec_exception=7;value.last_detection_count_=7;
  value.last_detection_source_observed_us_=value.now_us_-18'000;
  for(unsigned i=0;i<1000;++i)(void)value.preprocess_samples_us_.push(i);
  for(auto* ring:{&value.qnn_samples_us_,&value.inference_total_samples_us_,&value.decode_queue_samples_us_,
      &value.receive_to_result_samples_us_,&value.host_plus_receive_to_result_samples_us_,&value.control_dispatch_samples_us_,
      &value.background_motion_samples_us_,&value.image_motion_samples_us_,&value.receive_to_control_samples_us_})
    for(unsigned i=0;i<20;++i)(void)ring->push(i*100);
  value.last_preprocess_us_=999;value.last_detections_total_=7;
  for(unsigned i=0;i<5;++i)value.last_detections_[i]={float(i),2,3,4,.75F,0};
  const auto original=value;const auto text=format(value);
  assert(text.find("configured=1 queued_access_units=19 sync_access_units=2 pending_access_units=3")!=std::string::npos);
  assert(text.find("last_qnn_success_age_ms=123 last_qnn_failure_age_ms=456")!=std::string::npos);
  assert(text.find(value.java_decoder_failure_)!=std::string::npos);
  assert(text.find("preprocess{n=300 p50_us=849 p95_us=984 p99_us=996 last_us=999}")!=std::string::npos);
  assert(text.find("input_drop_codec_exception=7")!=std::string::npos);
  assert(text.find("input_health={contract=bounded} native_submit_count=8 last_detections=[")!=std::string::npos);
  assert(text.find(",...] makcu_bridge{")!=std::string::npos);
  assert(value.preprocess_samples_us_.oldest_at(0)==700 && value.preprocess_samples_us_.oldest_at(299)==999);
  const char* original_fields[]={
"acquired_images",
"class",
"color_range",
"color_standard",
"confidence",
"configured",
"consecutive_qnn_failures",
"deadline_backend_estimate_us",
"deadline_maximum_replacement_wait_us",
"deadline_replaced_pre_inference_frames",
"deadline_replacement_fallbacks",
"deadline_replacement_waits",
"decoded_size",
"decoder_failed",
"decoder_failure",
"decoder_name",
"dropped_access_units",
"fresh_content_outputs",
"image_crop",
"image_layout_failures",
"image_pixel_stride",
"image_row_stride",
"inference_backend",
"inference_queue_capacity",
"inference_queue_high_watermark",
"input_drop_bridge_failure",
"input_drop_buffer_capacity",
"input_drop_codec_exception",
"input_drop_fatal_flush",
"input_drop_invalid",
"input_drop_no_buffer",
"input_drop_not_running",
"input_drop_other",
"input_drop_queue_capacity",
"input_drop_recovery_flush",
"input_drop_reference_recovery",
"input_drop_restart",
"input_drop_stale_before_codec",
"input_drop_state_exception",
"input_health",
"last_detection_count",
"last_detection_source_generation",
"last_detection_source_observed_us",
"last_detection_source_sequence",
"last_detection_source_wire_frame_id",
"last_detections",
"last_input_nal_type_mask",
"last_qnn_failure_age_ms",
"last_qnn_status",
"last_qnn_success_age_ms",
"last_stream_restart_request_us",
"last_stream_restart_us",
"last_us",
"maximum_pre_inference_frame_age_us",
"output_color_format",
"output_path",
"pending_access_units",
"pixel_projection_attempts",
"pixel_projection_candidates",
"pixel_projection_complete",
"pixel_projection_control_publications",
"pixel_projection_gate_rejections",
"pixel_projection_math_rejections",
"pixel_projection_mode",
"pixel_projection_old_source_skips",
"pixel_projection_rgb_pool_buffers",
"pixel_projection_shadow_compared",
"pixel_projection_shadow_unscored",
"preprocess_failures",
"qnn_executions",
"qnn_failures",
"queued_access_units",
"rejected_output_images",
"rendered_frames",
"repeated_content_control_suppressions",
"repeated_content_inference_suppressions",
"repeated_content_inferences",
"repeated_content_outputs",
"requested_color_format",
"stale_decoded_outputs",
"stale_generation_control_suppressions",
"stale_generation_inference_suppressions",
"stale_generation_result_suppressions",
"stale_pre_inference_frames",
"stream_restart_failures",
"stream_restart_in_progress",
"stream_restarts",
"superseded_decoded_outputs",
"superseded_preprocessed_frames",
"sync_access_units",
"target_image_motion_budget_exhaustions",
"target_image_motion_budget_us",
"timing_scope",
"xyxy"
  };
  for(const auto* field:original_fields)assert(text.find(std::string(field)+'=')!=std::string::npos);
  // Detached snapshot is immutable under formatting and independent of the
  // producer's subsequent clear/rewrite, including strings and wrapped rings.
  value.java_decoder_failure_.clear();value.preprocess_samples_us_.clear();value.last_detections_total_=0;
  assert(format(original)==text);
  std::vector<std::thread> readers;
  for(unsigned i=0;i<4;++i)readers.emplace_back([&]{for(unsigned j=0;j<10;++j)assert(format(original)==text);});
  for(auto& thread:readers)thread.join();
  value=original;value.now_us_=value.last_qnn_success_monotonic_us_-1;
  assert(format(value).find("last_qnn_success_age_ms=-1")!=std::string::npos);
  const auto unavailable=format_native_decoder_report_snapshot({},"qnn_htp","unavailable",""," ");
  assert(unavailable.find("input_health={unavailable}")!=std::string::npos);
  std::printf("PASS pure decoder report: %zu original fields, wrapped percentiles, empty/future ages, complete diagnostics, five-box limit, detached/concurrent formatting\n",std::size(original_fields));
}
