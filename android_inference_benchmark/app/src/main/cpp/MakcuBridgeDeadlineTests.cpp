// Compile the real bridge in this test TU to exercise its internal gate and
// retained controller state. No JVM is instantiated and no device I/O occurs.
#include "MakcuMoveBridge.cpp"
#include <array>
#include <iostream>
#include <stdexcept>

#define REQUIRE(x) do { if(!(x))throw std::runtime_error(std::to_string(__LINE__)+": " #x); } while(false)
using namespace vfdual_android;

namespace {
// Same physical reticle error as the former1024 fixture, translated into the
// native640 ROI. Sensitivity and relative movement are not rescaled.
const std::array<YoloDetection,1> target{{{348,278,398,408,.99F,0U}}};
std::uint64_t base{};
MobileControlOutput seed() {
    fail_closed_native_state(MakcuStreamCloseScope::delivery_failure);
    auto config=screen_pixel_control_config();
    config.switch_confirmation_duration_us=0;
    config.observation_servo_enabled=true;
    REQUIRE(g_control_core.configure(config));
    REQUIRE(g_control_core.restore_response_calibration(.709F,.707F,24000,.99F,.99F));
    g_stream_generation_gate.activate(7);
    REQUIRE(g_output_gate.set_user_requested(true));
    base=monotonic_microseconds()-230000;
    MobileControlOutput out;
    for(std::uint64_t i=1;i<=8;++i)out=g_control_core.process(target,{i,base+i*20000,base+i*20000+1000});
    REQUIRE(out.has_target && out.lock_id!=0);
    return out;
}
MobileControlOutput next(){return g_control_core.process(target,{9,base+180000,base+181000});}

void processing_clock_includes_time_since_result_ready() {
    seed();
    REQUIRE(g_output_gate.set_user_requested(false)); // No JNI/device calls.
    auto model=target;
    const auto before=monotonic_microseconds();
    const auto observed=before-30000U;
    // The result was ready20ms ago; it is not the current processing time.
    bool consumed=false;
    publish_makcu_move_for_detections(model,9,7,true,observed,before-20000U,{},98,{},0,&consumed);
    const auto after=monotonic_microseconds();
    const auto& trace=g_control_core.control_trace();
    const auto& row=trace.samples[(trace.count-1U)%trace.samples.size()];
    REQUIRE(consumed && row.sequence==9U);
    REQUIRE(row.now_us>=before && row.now_us<=after);
    REQUIRE(row.now_us-row.observed_at_us>=30000U);
    REQUIRE(row.servo.horizon_seconds>=.054F);
    REQUIRE(!g_output_gate.output_enabled());
    REQUIRE(!g_move_commit_gate.pending());
}

void old_failure_path_reproduces_session_and_identity_loss() {
    const auto before=seed();
    const auto ticket=g_move_commit_gate.begin(monotonic_microseconds(),7,8);
    REQUIRE(ticket!=0);
    // This is exactly the old Java callback for an entirely unwritten expiry.
    report_makcu_move_result(ticket,false,0);
    REQUIRE(!g_output_gate.output_enabled() && !g_output_gate.user_requested());
    const auto after=next();
    REQUIRE(!after.has_target || after.lock_id!=before.lock_id);
}
void unsent_expiry_retains_session_target_and_immediate_next_ticket() {
    const auto before=seed();
    for(unsigned i=0;i<32;++i){
        const auto ticket=g_move_commit_gate.begin(monotonic_microseconds(),7,8);
        REQUIRE(ticket!=0);
        report_move_discarded_before_send(ticket);
        REQUIRE(!g_move_commit_gate.pending());
        REQUIRE(g_output_gate.user_requested() && g_output_gate.output_enabled());
        REQUIRE(!g_move_visibility_gate.snapshot().armed);
    }
    const auto after=next();
    std::cerr<<"discard continuity before="<<before.lock_id<<" after="<<after.lock_id<<" target="<<after.has_target
        <<" reason="<<static_cast<unsigned>(after.suppression_reason)<<'\n';
    REQUIRE(after.has_target && after.lock_id==before.lock_id);
    REQUIRE(g_move_commit_gate.begin(monotonic_microseconds(),7,9)!=0);
    g_move_commit_gate.fail_closed();
}
void stale_duplicate_and_user_disabled_cancellations_stay_isolated() {
    seed();
    REQUIRE(control_observation_requested(7));
    REQUIRE(!control_observation_requested(8));
    REQUIRE(control_observation_generation_active(7));
    REQUIRE(!control_observation_generation_active(8));
    const auto first=g_move_commit_gate.begin(monotonic_microseconds(),7,8);
    report_move_discarded_before_send(first);
    const auto second=g_move_commit_gate.begin(monotonic_microseconds(),7,9);
    report_move_discarded_before_send(first);
    REQUIRE(g_move_commit_gate.pending());
    REQUIRE(g_move_commit_gate.complete(second).matched);
    REQUIRE(g_output_gate.output_enabled());
    const auto third=g_move_commit_gate.begin(monotonic_microseconds(),7,10);
    REQUIRE(g_output_gate.set_user_requested(false));
    REQUIRE(!control_observation_requested(7));
    REQUIRE(control_observation_generation_active(7));
    report_move_discarded_before_send(third);
    REQUIRE(!g_output_gate.user_requested() && !g_output_gate.output_enabled());
    REQUIRE(!g_move_commit_gate.pending());
}
void already_expired_control_is_suppressed_without_resetting_healthy_output() {
    const auto before=seed();
    const auto discarded=g_pre_send_discards.load();
    auto model=target;
    const auto observed=monotonic_microseconds()-51000;
    // The old result-ready clock would call this49ms old, but processing starts
    // after51ms. Core must see the actual age and avoid making a stale plan.
    // No Java binding is installed: a transport call would fail.
    publish_makcu_move_for_detections(model,9,7,true,observed,observed+49000,{},99,{});
    REQUIRE(g_pre_send_discards.load()==discarded);
    const auto& trace=g_control_core.control_trace();
    const auto& sample=trace.samples[(trace.count-1U)%trace.samples.size()];
    REQUIRE(sample.now_us-sample.observed_at_us>=51000U);
    REQUIRE(sample.delta_x==0 && sample.delta_y==0);
    REQUIRE(sample.reason==static_cast<unsigned>(ControlSuppressionReason::response_guard));
    REQUIRE(g_output_gate.output_enabled() && g_output_gate.user_requested());
    REQUIRE(!g_move_commit_gate.pending());
    const auto after=g_control_core.process(target,{10,observed+20000,observed+69000});
    std::cerr<<"pre-JNI continuity before="<<before.lock_id<<" after="<<after.lock_id<<" target="<<after.has_target
        <<" reason="<<static_cast<unsigned>(after.suppression_reason)<<'\n';
    REQUIRE(after.has_target && after.lock_id==before.lock_id);
}
void newer_pixels_cannot_renew_an_expired_source_decision() {
    const auto before=seed();
    const auto discarded=g_pre_send_discards.load();
    auto model=target;
    const auto now=monotonic_microseconds();
    const auto observed=now-21000U;
    // B is fresh enough for the servo. The immutable source-A deadline has
    // nevertheless elapsed, so this must not reach the unbound JNI endpoint.
    bool consumed=false;
    publish_makcu_move_for_detections(model,9,7,true,observed,now-1000U,{},100,{},now-1U,&consumed);
    REQUIRE(consumed);
    REQUIRE(g_pre_send_discards.load()==discarded+1U);
    REQUIRE(g_output_gate.output_enabled() && g_output_gate.user_requested());
    REQUIRE(!g_move_commit_gate.pending());
    const auto& trace=g_control_core.control_trace();
    const auto processed=trace.samples[(trace.count-1U)%trace.samples.size()].now_us;
    const auto after=g_control_core.process(target,{10,now-500U,processed+1U});
    REQUIRE(after.has_target && after.lock_id==before.lock_id);
}
void rejected_generation_and_duplicate_observations_have_no_consumption_receipt() {
    seed();
    auto model=target;
    const auto now=monotonic_microseconds();
    const auto observed=now-21000U;
    bool consumed=true;
    publish_makcu_move_for_detections(model,9,8,true,observed,now-1000U,{},101,{},now-1U,&consumed);
    REQUIRE(!consumed);
    publish_makcu_move_for_detections(model,9,7,true,observed,now-1000U,{},101,{},now-1U,&consumed);
    REQUIRE(consumed);
    publish_makcu_move_for_detections(model,9,7,true,observed,now-1000U,{},101,{},now-1U,&consumed);
    REQUIRE(!consumed);
    REQUIRE(g_output_gate.output_enabled());
}
}
int main(){
    try {
        processing_clock_includes_time_since_result_ready();
        old_failure_path_reproduces_session_and_identity_loss();
        unsent_expiry_retains_session_target_and_immediate_next_ticket();
        stale_duplicate_and_user_disabled_cancellations_stay_isolated();
        already_expired_control_is_suppressed_without_resetting_healthy_output();
        newer_pixels_cannot_renew_an_expired_source_decision();
        rejected_generation_and_duplicate_observations_have_no_consumption_receipt();
        std::cout<<"PASS actual bridge: old false-ACK reproducer,32 unsent drops retain identity/output, next ticket immediate, stale/user-stop isolation, pre-JNI deadline; real failures still close\n";
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
