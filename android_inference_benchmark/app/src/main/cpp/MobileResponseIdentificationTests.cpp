#include "MobileResponseEstimator.hpp"
#include "MobilePendingMotion.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
using vfdual_android::MobileResponseEstimator;
using vfdual_android::MobilePendingMotion;
using State=MobileResponseEstimator::LearningState;
void check(bool ok,const char* expression,int line) {
  if(!ok) { std::fprintf(stderr,"line %d failed: %s\n",line,expression); std::abort(); }
}
#define CHECK(x) check(bool(x),#x,__LINE__)
struct Event { int x{},y{}; std::uint64_t at{}; };
struct Simulation {
  MobileResponseEstimator estimator;
  std::vector<Event> events;
  std::uint64_t now{1'000'000U};
  std::uint32_t random{0x125784U};
  void step(std::uint64_t dt,std::uint64_t delay,float rx,float ry,unsigned period=0,
            bool valid=true,bool consistent=true,float manual_x=0.F,float manual_y=0.F) {
    const auto before=now; now+=dt;
    random^=random<<13U;random^=random>>17U;random^=random<<5U;
    static constexpr std::array pattern{-8,-1,6,-10,3,7,2,-4};
    const int x=period?pattern[events.size()%period]:int(random%19U)-9;
    const int y=period?0:int((random>>9U)%17U)-8;
    events.push_back({x,y,now}); estimator.accept(x,y,now);
    float dx{},dy{};
    for(const auto& event:events) if(event.at+delay>before && event.at+delay<=now) {
      dx-=event.x*rx;dy-=event.y*ry;
    }
    estimator.observe({consistent?before:before-1U,dx+manual_x*float(dt)/1e6F,
      dy+manual_y*float(dt)/1e6F,24,30,valid},now);
  }
};

void periodic_delay_is_not_unique() {
  for(unsigned period:{4U,6U,8U}) for(auto true_delay:{72'000ULL,96'000ULL})
    for(bool restore:{false,true}) {
      Simulation simulation;
      if(restore) CHECK(simulation.estimator.restore(1,1,true_delay,.99F,.99F));
      for(unsigned i{};i<400U;++i) simulation.step(8'000U,true_delay,1,1,period);
      const auto& estimator=simulation.estimator;
      const auto range=estimator.delay_range();
      CHECK(estimator.x().trusted && estimator.x().quality>.99F);
      CHECK(restore || !estimator.y().trusted);
      CHECK(!range.identifiable && range.candidates>1U);
      CHECK(range.minimum_us<=true_delay && range.maximum_us>=true_delay);
      CHECK(estimator.learning_state()==State::ambiguous_delay);
      if(restore) CHECK(estimator.delay_us()==true_delay);
      CHECK(estimator.delay_us()+8'000U>=true_delay);
      MobilePendingMotion pending;
      CHECK(pending.accept(20,0,10'000'000));
      const auto p=pending.predict(10'008'000,estimator.delay_us(),range.minimum_us,range.maximum_us);
      CHECK(p.maximum_x==20 && p.reports==1);
    }
}

void nonperiodic_and_sparse_learning() {
  unsigned cases{};
  // The report's boundary matrix is a validation grid, not tuning candidates.
  for(auto dt:{4'000ULL,7'143ULL,8'000ULL,8'333ULL,10'000ULL,16'667ULL,18'000ULL,18'001ULL,
               20'000ULL,25'000ULL,33'333ULL,50'000ULL})
    for(auto delay:{8'000ULL,16'000ULL,52'000ULL,96'000ULL}) {
      Simulation simulation;
      for(unsigned i{};i<320U;++i) simulation.step(dt,delay,.4F,1.4F);
      const auto& e=simulation.estimator;const auto range=e.delay_range();
      if(!e.x().trusted || !e.y().trusted || range.minimum_us>delay || range.maximum_us<delay)
        std::fprintf(stderr,"sparse dt=%llu delay=%llu nominal=%llu range=%llu..%llu candidates=%u state=%u rx=%f ry=%f\n",
          dt,delay,e.delay_us(),range.minimum_us,range.maximum_us,range.candidates,unsigned(e.learning_state()),e.x().response,e.y().response);
      CHECK(e.x().trusted && e.y().trusted && e.updates()>0);
      CHECK(std::abs(e.x().response-.4F)<.03F && std::abs(e.y().response-1.4F)<.05F);
      CHECK(range.minimum_us<=delay && range.maximum_us>=delay);
      CHECK(e.learning_state()==State::identified || e.learning_state()==State::ambiguous_delay);
      if(dt<=8'000U) CHECK(range.identifiable && e.delay_updates()>0U);
      ++cases;
    }
  std::printf("PASS real-time cadence response matrix: %u cases\n",cases);
  Simulation changed;
  CHECK(changed.estimator.restore(.4F,1.4F,72'000,.99F,.99F));
  for(unsigned i{};i<400U;++i) changed.step(8'000U,24'000U,.4F,1.4F);
  CHECK(changed.estimator.delay_range().identifiable);
  CHECK(std::llabs(std::int64_t(changed.estimator.delay_us())-24'000)<=8'000);
  CHECK(changed.estimator.x().response==.4F && changed.estimator.y().response==1.4F);
  Simulation irregular;
  constexpr std::array<std::uint64_t,4> cadence{8'000,20'000,33'333,50'000};
  for(unsigned i{};i<400U;++i) irregular.step(cadence[i%cadence.size()],52'000,.4F,1.4F,0,true,true,20.F,-15.F);
  CHECK(irregular.estimator.x().trusted && irregular.estimator.y().trusted);
  CHECK(std::abs(irregular.estimator.x().response-.4F)<.03F &&
        std::abs(irregular.estimator.y().response-1.4F)<.05F);
  CHECK(irregular.estimator.delay_range().minimum_us<=52'000 && irregular.estimator.delay_range().maximum_us>=52'000);
  Simulation intermittent;
  for(unsigned i{};i<400U;++i) intermittent.step(20'000,52'000,.4F,1.4F,0,i%11U!=0U);
  CHECK(intermittent.estimator.x().trusted && intermittent.estimator.y().trusted);
}

void invalid_evidence_does_not_force_learning() {
  for(int counts:{0,3}) {
    MobileResponseEstimator e;
    for(unsigned i{};i<240U;++i) {
      const auto at=1'000'000ULL+i*20'000U;
      e.accept(counts,counts,at);
      e.observe({at-20'000U,-1.2F,-1.2F,24,30,i>0},at);
    }
    CHECK(!e.x().trusted && !e.y().trusted && e.updates()==0);
    CHECK(!e.delay_range().identifiable && e.delay_us()!=0U);
  }
  Simulation invalid;
  for(unsigned i{};i<80U;++i) invalid.step(20'000,52'000,.4F,1.4F,0,false);
  CHECK(invalid.estimator.learning_state()==State::invalid_background);
  CHECK(invalid.estimator.updates()==0U);
  Simulation gap;
  for(unsigned i{};i<16U;++i) gap.step(20'000,52'000,.4F,1.4F);
  gap.step(80'000,52'000,.4F,1.4F);
  CHECK(gap.estimator.learning_state()==State::discontinuous_time);
  CHECK(gap.estimator.updates()==0);
  gap.estimator.observe({gap.now-40'000U,0,0,24,30,true},gap.now-20'000U);
  CHECK(gap.estimator.learning_state()==State::discontinuous_time);
  for(unsigned i{};i<320U;++i) gap.step(20'000,52'000,.4F,1.4F);
  CHECK(gap.estimator.x().trusted && gap.estimator.y().trusted);

  Simulation restored;
  CHECK(restored.estimator.restore(.7F,.7F,72'000,.99F,.99F));
  for(unsigned i{};i<320U;++i) restored.step(8'000,24'000,1.4F,1.4F);
  CHECK(restored.estimator.x().response==.7F && restored.estimator.y().response==.7F);
  CHECK(restored.estimator.delay_us()==72'000U);
  CHECK(restored.estimator.learning_state()==State::restored_scale_disagreement);
}

void pending_interval_is_risk_not_nominal_phase() {
  MobilePendingMotion ledger;
  CHECK(ledger.accept(-64,7,100'000));
  CHECK(ledger.accept(64,-7,120'000));
  const auto range=ledger.predict(150'000,80'000,8'000,80'000);
  CHECK(range.x==0 && range.y==0); // Nominal remains the actual nominal model.
  CHECK(range.minimum_x==0 && range.maximum_x==64);
  CHECK(range.minimum_y==-7 && range.maximum_y==0 && range.reports==2);
  CHECK(MobilePendingMotion::bound_axis(128,range.minimum_x,range.maximum_x,128)==64);
  CHECK(MobilePendingMotion::bound_axis(-128,range.minimum_x,range.maximum_x,128)==-128);
  // Exhaustive invariants, not control-parameter tuning.
  for(int a:{-192,-64,0,80,192}) for(int b:{-192,-64,0,80,192})
    for(int requested=-128;requested<=128;++requested) {
      const auto bounded=MobilePendingMotion::bound_axis(requested,float(a),float(b),128);
      CHECK(std::abs(bounded)<=std::abs(requested));
      CHECK(requested==0?bounded==0:bounded*requested>=0);
    }
  MobilePendingMotion oracle;
  const std::array<Event,6> reports{{{12,-7,100'000},{-20,3,110'000},{9,11,120'000},
    {42,-5,130'000},{-15,8,140'000},{-4,-9,150'000}}};
  for(const auto& event:reports) CHECK(oracle.accept(event.x,event.y,event.at));
  const auto result=oracle.predict(170'000,40'000,8'000,96'000);
  float xmin=1e9F,xmax=-1e9F,ymin=1e9F,ymax=-1e9F;
  for(unsigned delay=8'000;delay<=96'000;++delay) {
    float x{},y{};
    for(const auto& e:reports) if(170'000-e.at<delay) {x+=e.x;y+=e.y;}
    xmin=std::min(xmin,x);xmax=std::max(xmax,x);ymin=std::min(ymin,y);ymax=std::max(ymax,y);
  }
  CHECK(result.minimum_x==xmin && result.maximum_x==xmax && result.minimum_y==ymin && result.maximum_y==ymax);
  CHECK(MobilePendingMotion::compensate(3,100)==0 && MobilePendingMotion::compensate(-3,-100)==0);
}

void stationary_is_not_an_ambiguous_zero_proposal() {
  MobileResponseEstimator response;
  CHECK(response.restore(1,1,40'000,.99F,.99F));
  const std::array<MobileResponseEstimator::TargetObservation,4> points{{
    {208,208,100'000},{208,208,108'000},{208,208,116'000},{208,208,124'000}}};
  const auto fixed=response.disturbance_velocity(points,0,0,true);
  CHECK(fixed.x_valid && fixed.y_valid && fixed.x_stationary && fixed.y_stationary);
  response.accept(20,0,70'000);
  const auto ambiguous=response.disturbance_velocity(points,0,0,true);
  CHECK(ambiguous.x_valid && ambiguous.x==0.F && !ambiguous.x_stationary);
  CHECK(ambiguous.y_valid && ambiguous.y==0.F && ambiguous.y_stationary);
}
}
int main() {
  periodic_delay_is_not_unique(); nonperiodic_and_sparse_learning();
  invalid_evidence_does_not_force_learning(); pending_interval_is_risk_not_nominal_phase();
  stationary_is_not_an_ambiguous_zero_proposal();
  std::puts("PASS identifiable delay, periodic ambiguity, sparse time windows, and pending risk envelope");
}
