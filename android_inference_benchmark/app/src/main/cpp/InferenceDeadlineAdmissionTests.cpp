#include "InferenceDeadlineAdmission.hpp"
#include <cstdlib>
#include <iostream>

#define CHECK(value) do {if(!(value)){std::cerr<<"failure line "<<__LINE__<<": "<<#value<<'\n';std::abort();}}while(false)

using vfdual_android::InferenceDeadlineAdmission;
using O=InferenceDeadlineAdmission::Observation;
using P=InferenceDeadlineAdmission::Preparing;

static InferenceDeadlineAdmission measured(std::uint64_t backend) {
  InferenceDeadlineAdmission policy;
  for(unsigned i=0;i<InferenceDeadlineAdmission::kMinimumSamples;++i)policy.record_backend_wall(backend);
  return policy;
}

int main() {
  constexpr std::uint64_t now=1'000'000;
  const O late{now-28'000,1},fresh{now-6'000,1};
  InferenceDeadlineAdmission startup;
  CHECK(!startup.prefer_ready(late,fresh,now));
  for(unsigned i=1;i<InferenceDeadlineAdmission::kMinimumSamples;++i){
    startup.record_backend_wall(26'000);CHECK(!startup.prefer_ready(late,fresh,now));
  }
  startup.record_backend_wall(26'000);CHECK(startup.ready());
  CHECK(startup.prefer_ready(late,fresh,now));
  CHECK(startup.prefer_ready(fresh,{now-1'000,1},now));
  CHECK(startup.prefer_ready(fresh,{fresh.observed_us+1,1},now));
  CHECK(!startup.prefer_ready(late,{now-6'000,2},now));
  CHECK(!startup.prefer_ready(late,late,now));
  CHECK(!startup.prefer_ready(late,{now+1,1},now));
  CHECK(!startup.prefer_ready({now+1,1},fresh,now));
  CHECK(!startup.prefer_ready({0,1},fresh,now));

  // Known preprocessing may receive a short wait only when the newer frame
  // can plausibly finish inside the unchanged50ms horizon after that wait.
  CHECK(startup.wait_for_preparing(late,P{fresh,now-1'000},now));
  CHECK(!startup.wait_for_preparing(late,P{fresh,now-8'001},now));
  CHECK(!startup.wait_for_preparing(late,P{fresh,now+1},now));
  CHECK(!startup.wait_for_preparing(late,P{fresh,0},now));
  CHECK(!startup.wait_for_preparing(late,P{{now-25'000,1},now-1'000},now));
  CHECK(!startup.wait_for_preparing(late,P{{now-19'000,1},now-1'000},now));
  CHECK(startup.replacement_wait_budget_us(late,P{fresh,now-1'000},now)==2'000);

  // Below deadline pressure, only a measured, not-yet-overdue conversion can
  // buy a short wait. The actual budget is the predicted remaining duration.
  const O young{now-15'000,1},near_ready{now-10'000,1};
  CHECK(startup.replacement_wait_budget_us(young,P{near_ready,now-200,900},now)==700);
  CHECK(startup.replacement_wait_budget_us(young,P{near_ready,now-200,0},now)==0);
  CHECK(startup.replacement_wait_budget_us(young,P{near_ready,now-200,200},now)==0);
  CHECK(startup.replacement_wait_budget_us(young,P{near_ready,now-200,199},now)==0);
  CHECK(startup.replacement_wait_budget_us(young,P{near_ready,now-200,1'201},now)==0);
  CHECK(startup.replacement_wait_budget_us(young,P{{now-11'000,1},now-200,1'200},now)==1'000);
  CHECK(startup.replacement_wait_budget_us(young,P{{now-11'001,1},now-200,1'200},now)==0);
  CHECK(startup.replacement_wait_budget_us(young,P{{near_ready.observed_us,2},now-200,900},now)==0);
  CHECK(startup.replacement_wait_budget_us(young,P{{young.observed_us-1,1},now-200,900},now)==0);
  CHECK(startup.replacement_wait_budget_us(young,P{near_ready,now+1,900},now)==0);
  CHECK(startup.replacement_wait_budget_us(young,P{near_ready,0,900},now)==0);
  CHECK(startup.replacement_wait_budget_us(young,P{{now-100,1},now-200,900},now)==0);
  CHECK(startup.replacement_wait_budget_us(late,P{{now-10'000,1},now-8'000},now)==2'000);
  CHECK(startup.replacement_wait_budget_us(late,P{{now-10'000,1},now-8'001},now)==0);
  CHECK(!measured(26'000).wait_for_preparing(young,P{near_ready,now-200,0},now));
  CHECK(!InferenceDeadlineAdmission{}.wait_for_preparing(young,P{near_ready,now-200,900},now));
  const auto tight=measured(44'000);
  CHECK(tight.replacement_wait_budget_us({now-8'000,1},P{{now-2'500,1},now-200,700},now)==0);
  CHECK(tight.replacement_wait_budget_us({now-8'000,1},P{{now-2'499,1},now-200,700},now)==500);

  // Slow and inference-only use cases cannot turn into an always-discard loop.
  // The worker still executes its original frame if no eligible replacement
  // materializes. Slow devices can use already-ready input, but never wait.
  const auto slow=measured(60'000);
  CHECK(slow.prefer_ready(late,fresh,now));
  CHECK(!slow.wait_for_preparing(late,P{fresh,now-1'000},now));
  CHECK(slow.replacement_wait_budget_us(young,P{near_ready,now-200,900},now)==0);

  // Model one worker turn: timeout/failure executes the original, and even a
  // stream of successively newer inputs permits at most one exchange.
  const auto turn=[&](O selected,P preparing,O first_ready,O later_ready,bool ready_now,std::uint64_t finishes_in) {
    unsigned exchanges=0;
    const auto budget=startup.replacement_wait_budget_us(selected,preparing,now);
    if((ready_now || (budget && finishes_in<=budget)) && startup.prefer_ready(selected,first_ready,now)) {
      selected=first_ready;++exchanges;
    }
    (void)later_ready;
    CHECK(exchanges<=1);
    return selected;
  };
  CHECK(turn(young,P{near_ready,now-200,900},near_ready,{now-1,1},false,701).observed_us==young.observed_us);
  CHECK(turn(young,P{near_ready,now-200,900},near_ready,{now-1,1},false,700).observed_us==near_ready.observed_us);
  CHECK(turn(young,P{near_ready,now-200,900},{near_ready.observed_us,2},{now-1,1},false,700).observed_us==young.observed_us);
  CHECK(turn(young,P{near_ready,now-200,0},near_ready,{now-1,1},false,0).observed_us==young.observed_us);
  CHECK(turn(young,P{near_ready,now-200,900},near_ready,{now-1,1},true,0).observed_us==near_ready.observed_us);

  // A short scheduling outlier must not redefine all admission times. A real
  // backend transition is adopted within the bounded16-execution history.
  auto changing=measured(12'000);changing.record_backend_wall(250'000);
  CHECK(changing.backend_estimate_us()==12'000);
  changing.record_backend_wall(0);changing.record_backend_wall(1'000'001);
  CHECK(changing.backend_estimate_us()==12'000);
  for(unsigned i=0;i<16;++i)changing.record_backend_wall(26'000);
  CHECK(changing.backend_estimate_us()==26'000);
  for(unsigned i=0;i<16;++i)changing.record_backend_wall(11'000);
  CHECK(changing.backend_estimate_us()==11'000);

  // Deterministic burst/jitter fixture: preserve one execution opportunity per
  // turn, replacing late candidates only when a newer same-generation image is
  // actually ready. Missing, failed and wrong-generation producers retain the
  // original candidate rather than creating a detection gap.
  unsigned executions=0,replacements=0,actionable_before=0,actionable_after=0;
  for(unsigned i=0;i<1'000;++i){
    const auto at=now+i*33'333ULL;
    const O old{at-(i%4==0?10'000U:28'000U),3};
    const O newer{at-6'000,(i%5==0?4U:3U)};
    const bool ready=i%7!=0;
    O chosen=old;
    if(ready && startup.prefer_ready(old,newer,at)){chosen=newer;++replacements;}
    ++executions;
    actionable_before+=at-old.observed_us+26'000+3'000<50'000;
    actionable_after+=at-chosen.observed_us+26'000+3'000<50'000;
    if(!ready || newer.generation!=old.generation)CHECK(chosen.observed_us==old.observed_us);
  }
  CHECK(executions==1'000 && replacements>400);
  CHECK(actionable_after>actionable_before+400);
  CHECK(InferenceDeadlineAdmission::kMaximumReplacementWaitUs==2'000);
  CHECK(InferenceDeadlineAdmission::kMaximumOpportunisticWaitUs==1'000);
  std::cout<<"PASS deadline admission: warm latest, bounded opportunity, startup/slow/generation/clock/deadline guards, one exchange/fallback, rolling estimate, burst freshness="
      <<actionable_before<<"->"<<actionable_after<<" executions="<<executions<<'\n';
}
