// Real production Bridge/Core/configure code; no JVM or physical I/O.
#include "MakcuMoveBridge.cpp"
#include <cstdio>
#include <vector>
using namespace vfdual_android;
namespace {
int failures{};
void check(bool ok,const char* message) {if(!ok){++failures;std::fprintf(stderr,"FAIL %s\n",message);}}
MakcuControlProfile profile() {
  MakcuControlProfile p;p.model_token=std::string(vfdual::kOverwatch2.token);
  p.body_class_id=p.selected_class_id=0;p.head_class_id=vfdual::kNoModelClass;
  p.selected_class_uses_head_box=false;p.selected_class_uses_geometric_head=true;
  p.target_y_ratio=.18F;p.relative_gain=.6F;p.deadzone_pixels=.5F;
  return p;
}
void train_ambiguous_range() {
  struct Event{int x;std::uint64_t at;};std::vector<Event> events;
  static constexpr std::array pattern{-8,-1,6,-10};
  std::uint64_t at=1'000'000U;
  for(unsigned i=0;i<400;++i) {
    const auto before=at;at+=8'000U;const int x=pattern[i%pattern.size()];
    check(g_control_core.record_accepted_move(x,0,at),"C3 identification command accepted");events.push_back({x,at});
    float shift{};for(const auto& e:events)if(e.at+96'000U>before && e.at+96'000U<=at)shift-=e.x*.7F;
    (void)g_control_core.process({}, {i+1U,at,at+1'000U,{before,shift,0,24,32,true}});
  }
}
void train_identifiable_range(std::uint64_t start) {
  struct Event{int x,y;std::uint64_t at;};std::vector<Event> events;
  std::uint32_t random=0x125784U;auto at=start;
  for(unsigned i=0;i<400;++i) {
    const auto before=at;at+=8'000U;random^=random<<13U;random^=random>>17U;random^=random<<5U;
    const int x=int(random%19U)-9,y=int((random>>9U)%17U)-8;
    check(g_control_core.record_accepted_move(x,y,at),"C3 informative accepted command");events.push_back({x,y,at});
    float dx{},dy{};for(const auto& e:events)if(e.at+96'000U>before && e.at+96'000U<=at){dx-=e.x*.7F;dy-=e.y*.7F;}
    (void)g_control_core.process({}, {i+1U,at,at+1'000U,{before,dx,dy,24,32,true}});
  }
}
void runtime_reconfiguration(int cache_mode) {
  const auto p0=profile();set_control_calibration_scope(p0.model_token+"|runtime-test-"+std::to_string(cache_mode));
  check(configure_makcu_control_profile(p0),"C3 initial disabled profile");
  check(restore_control_calibration(.7F,.7F,24'000U,.99F,.99F),"C3 cold file-style restore");
  train_ambiguous_range();const auto before=control_calibration_snapshot();
  std::printf("C3 before mode=%d nominal=%.0f min=%.0f max=%.0f identifiable=%.0f candidates=%.0f\n",cache_mode,before[2],before[8],before[9],before[7],before[10]);
  check(before[5]!=0 && before[7]==0 && before[9]>before[2] && before[10]>1,"C3 real Core establishes trusted-scale ambiguous delay");
  if(cache_mode==0)g_saved_calibration={};
  g_java_bridge_ready.store(true); // Configuration precondition only; no offer is dispatched.
  for(unsigned i=0;i<(cache_mode==2?3U:1U);++i) {
    auto p=p0;p.relative_gain=i%2?.4F:.6F;p.deadzone_pixels=i%2?.25F:.5F;
    p.output_enabled=false;check(configure_makcu_control_profile(p),"C3 same-scope disabled phase");
    p.output_enabled=true;check(configure_makcu_control_profile(p),"C3 same-scope enabled phase");
    const auto after=control_calibration_snapshot();
    std::printf("C3 after mode=%d iteration=%u nominal=%.0f min=%.0f max=%.0f identifiable=%.0f candidates=%.0f\n",cache_mode,i,after[2],after[8],after[9],after[7],after[10]);
    check(after[8]<=before[8] && after[9]>=before[9] && after[7]==before[7] && after[10]>=before[10],
        "C3 runtime migration cannot collapse established uncertainty or invent identifiability");
  }
  constexpr std::uint64_t command_at=10'000'000U;
  check(g_control_core.record_accepted_move(100,0,command_at),"C3 new command after reconfiguration");
  const std::array target{YoloDetection{400,302,440,402,.95F,0}};
  const auto age=std::uint64_t((before[2]+before[9])*.5);
  check(age>std::uint64_t(before[2]) && age<std::uint64_t(before[9]),"C3 probe lies after nominal and before latest visibility");
  const auto out=g_control_core.process(target,{1,command_at+age,command_at+age+1'000U});
  const auto& trace=g_control_core.control_trace();const auto& row=trace.samples[(trace.count-1U)%trace.samples.size()];
  std::printf("C3 risk mode=%d nominal_pending=%.2f risk_reports=%u delta_x=%d\n",cache_mode,row.pending_x,row.pending_reports,out.delta_x);
  check(row.pending_reports>=1U && out.delta_x<=28,"C3 actual Core retains late-mode pending risk and budget for new commands");
  if(cache_mode==2) {
    auto p=p0;p.output_enabled=false;check(configure_makcu_control_profile(p),"C3 reconfigure before informative new observations");
    train_identifiable_range(20'000'000U);const auto identified=control_calibration_snapshot();
    std::printf("C3 informative nominal=%.0f min=%.0f max=%.0f identifiable=%.0f candidates=%.0f\n",
        identified[2],identified[8],identified[9],identified[7],identified[10]);
    check(identified[7]==1 && identified[8]<=96'000 && identified[9]>=96'000 &&
        identified[9]-identified[8]<before[9]-before[8],"C3 actual new nonperiodic evidence may identify and narrow the prior");
    p.output_enabled=true;check(configure_makcu_control_profile(p),"C3 identified same-scope reconfiguration");
    const auto migrated=control_calibration_snapshot();
    check(migrated[7]==identified[7] && migrated[8]==identified[8] && migrated[9]==identified[9] &&
        migrated[10]==identified[10],"C3 in-memory migration also preserves valid identification qualification");
    const auto saved=g_control_core.response_estimator().runtime_calibration();
    MobileResponseEstimator restored;check(restored.restore_runtime_calibration(saved),"C3 valid typed runtime restore");
    auto invalid=saved;invalid.range.minimum_us=invalid.range.maximum_us+1U;
    check(!restored.restore_runtime_calibration(invalid) && restored.delay_range().minimum_us==saved.range.minimum_us,
        "C3 malformed uncertainty is rejected atomically without narrowing a valid prior");
    invalid=saved;invalid.x.trusted=false;
    check(!restored.restore_runtime_calibration(invalid),"C3 untrusted scale cannot be upgraded by runtime migration");
  }
  set_control_calibration_scope(p0.model_token+"|other-route");
  auto p=p0;p.output_enabled=false;check(configure_makcu_control_profile(p),"C3 new route profile");
  const auto changed=control_calibration_snapshot();check(changed[5]==0,"C3 scope change isolates previous trusted calibration");
  set_control_calibration_scope(p0.model_token+"|runtime-test-"+std::to_string(cache_mode));
  check(configure_makcu_control_profile(p),"C3 returning route profile");
  check(control_calibration_snapshot()[5]==0,"C3 returning to a scope does not resurrect an invalidated older cache");
  check(restore_control_calibration(.7F,.7F,24'000U,.99F,.99F),"C3 cold validated point calibration remains supported");
  const auto cold=control_calibration_snapshot();
  check(cold[5]==1 && cold[2]==24'000 && cold[8]==24'000 && cold[9]==24'000 && cold[7]==0,
      "C3 cold file-style restore is a scoped prior rather than invented current identifiability");
}
}
int main() {
  for(int mode:{0,1,2})runtime_reconfiguration(mode);
  std::printf("round3_calibration_failures=%d\n",failures);
  return failures?1:0;
}
