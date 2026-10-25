#include "ProjectionTimingTrace.hpp"
#include <cassert>
#include <memory>

int main(){
  using Trace=vfdual_android::ProjectionTimingTrace;
  auto trace=std::make_unique<Trace>();
  auto snapshot=std::make_unique<Trace::Snapshot>();
  assert(!trace->has_pending());
  Trace::Row row{};row[1]=1;row[2]=1;row[3]=3;row[6]=100'000'000'001LL;
  for(std::size_t i=0;i<Trace::capacity+2;++i)trace->record(row);
  Trace::TargetRow target{};target[1]=1;target[4]=47;target[19]=target[20]=target[21]=-1;
  trace->record_target(target);
  assert(trace->has_pending());trace->drain(*snapshot);assert(!trace->has_pending());
  assert(snapshot->observations.count==Trace::capacity && snapshot->observations.lost==2);
  assert(snapshot->observations.rows[0][0]==3);
  assert(snapshot->targets.count==1 && snapshot->targets.rows[0][19]==-1);
  const auto text=snapshot->report();
  assert(text.find("projection_trace_lost=2")!=std::string::npos);
  assert(text.find("projection_target_trace_lost=0")!=std::string::npos);
  assert(text.find("100000000001")!=std::string::npos); // Integer timestamps never lose digits.
  assert(text.find("projection_trace_v1=[")!=std::string::npos);
  assert(text.find("projection_target_trace_v1=[")!=std::string::npos);
  trace->drain(*snapshot);
  assert(snapshot->observations.count==0 && snapshot->targets.count==0);
}
