#include "PipelineTimingTrace.hpp"
#include "PipelineOutcomeTrace.hpp"
#include <cassert>
#include <cstdio>
#include <memory>

using vfdual_android::PipelineTimingSample;
using vfdual_android::PipelineTimingTrace;

int main() {
  auto outcomes=std::make_unique<vfdual_android::PipelineOutcomeTrace>();
  auto dropped=std::make_unique<vfdual_android::PipelineOutcomeTrace::Snapshot>();
  for(unsigned i=0;i<vfdual_android::PipelineOutcomeTrace::capacity+3;++i)
    outcomes->record(i,17,2,100,200,vfdual_android::PipelineOutcome::stale_before_inference);
  outcomes->drain(*dropped);
  assert(dropped->lost==3 && dropped->count==vfdual_android::PipelineOutcomeTrace::capacity);
  assert(dropped->rows[0][0]==4 && dropped->rows[0][2]==17 && dropped->rows[0][6]==2);
  outcomes->drain(*dropped);assert(dropped->count==0 && dropped->lost==0);
  auto trace = std::make_unique<PipelineTimingTrace>();
  auto snapshot = std::make_unique<PipelineTimingTrace::Snapshot>();
  trace->drain(*snapshot);
  assert(snapshot->count == 0 && snapshot->lost == 0);

  // A stream restart may reuse a wire frame ID; it must not overwrite an
  // unconsumed measurement or make the drain cursor repeat prior records.
  PipelineTimingSample first{.sequence=1, .wire_frame_id=7, .generation=10,
      .observed_us=100, .received_us=200, .decoded_planes_us=400,
      .preprocess_started_us=450, .preprocess_finished_us=500, .enqueued_us=600,
      .inference_started_us=900, .inference_finished_us=1900, .result_ready_us=2100,
      .control_started_us=2200, .control_finished_us=2300,
      .qnn_us=990, .background_us=100, .image_motion_us=90, .detections=2, .content_updated=1};
  trace->record(first);
  first.generation=11; first.sequence=2;
  trace->record(first);
  trace->drain(*snapshot);
  assert(snapshot->count == 2 && snapshot->lost == 0);
  assert(snapshot->samples[0].id == 1 && snapshot->samples[1].id == 2);
  assert(snapshot->samples[0].generation == 10 && snapshot->samples[1].generation == 11);
  assert(snapshot->report().find("1,1,7,10,100,200,400,450,500,600,900,1900,2100,2200,2300,990,100,90,2,1") != std::string::npos);
  trace->drain(*snapshot);
  assert(snapshot->count == 0 && snapshot->lost == 0);

  // Consumer delay is explicit data loss, not a repeated or fabricated row.
  for (std::uint64_t i=0; i<PipelineTimingTrace::capacity+9; ++i) {
    first.sequence=100+i;
    trace->record(first);
  }
  trace->drain(*snapshot);
  assert(snapshot->count == PipelineTimingTrace::capacity && snapshot->lost == 9);
  assert(snapshot->samples[0].id == 12 && snapshot->samples[0].sequence == 109);
  assert(snapshot->samples[snapshot->count-1].id == 1035);
  assert(snapshot->report().find("pipeline_trace_lost=9") != std::string::npos);
  trace->record(first); trace->drain(*snapshot);
  assert(snapshot->count == 1 && snapshot->lost == 0 && snapshot->samples[0].id == 1036);
  std::puts("PASS pipeline timing chronology schema, generation reuse, single drain and bounded loss");
}
