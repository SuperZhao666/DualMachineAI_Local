#include "QnnExecutionProfile.hpp"
#include <array>
#include <cassert>
#include <cstdio>
using vfdual_android::QnnExecutionProfile;
namespace {
struct Fake {
  static inline unsigned created{},freed{},data_calls{},mode{};
  static inline std::uint64_t accelerator=7000;
  static inline std::array<QnnProfile_EventId_t,300> roots{};
  static inline std::array<QnnProfile_EventId_t,6> children{2,3,4,5,6,7};
  static Qnn_ErrorHandle_t create(Qnn_BackendHandle_t,QnnProfile_Level_t level,Qnn_ProfileHandle_t* out){
    assert(level==QNN_PROFILE_LEVEL_BASIC);++created;*out=reinterpret_cast<void*>(1);return QNN_SUCCESS;
  }
  static Qnn_ErrorHandle_t free(Qnn_ProfileHandle_t handle){assert(handle==reinterpret_cast<void*>(1));++freed;return QNN_SUCCESS;}
  static Qnn_ErrorHandle_t config(Qnn_ProfileHandle_t,const QnnProfile_Config_t** values){
    assert(values[0]->option==QNN_PROFILE_CONFIG_OPTION_MAX_EVENTS&&values[0]->numMaxEvents==256);
    return QNN_PROFILE_ERROR_UNSUPPORTED;
  }
  static Qnn_ErrorHandle_t events(Qnn_ProfileHandle_t,const QnnProfile_EventId_t** ids,std::uint32_t* n){
    if(mode==4)return QNN_PROFILE_ERROR_INVALID_HANDLE;
    if(mode==3){for(unsigned i=0;i<roots.size();++i)roots[i]=i+1;*ids=roots.data();*n=roots.size();}
    else {roots[0]=1;*ids=roots.data();*n=1;}
    return QNN_SUCCESS;
  }
  static Qnn_ErrorHandle_t sub(QnnProfile_EventId_t id,const QnnProfile_EventId_t** ids,std::uint32_t* n){
    *ids=nullptr;*n=0;
    if(id==1){*ids=children.data();*n=children.size();}
    if(id==7 && mode==1){*ids=roots.data();*n=1;} // Cyclic metadata must not loop forever.
    return QNN_SUCCESS;
  }
  static Qnn_ErrorHandle_t data(QnnProfile_EventId_t id,QnnProfile_EventData_t* out){
    ++data_calls;*out=QNN_PROFILE_EVENT_DATA_INIT;out->unit=QNN_PROFILE_EVENTUNIT_MICROSEC;
    switch(id){
      case 1:out->type=400;out->value=11000;break;
      case 2:out->type=3001;out->value=10900;if(mode==2)out->unit=QNN_PROFILE_EVENTUNIT_CYCLES;break;
      case 3:out->type=3002;out->value=8000;break;
      case 4:out->type=3004;out->value=accelerator;break;
      case 5:out->type=3012;out->value=accelerator-100;break;
      case 6:out->type=3003;out->unit=QNN_PROFILE_EVENTUNIT_CYCLES;out->value=8'000'000;break;
      case 7:out->type=mode==5?3004:3009;out->unit=mode==5?QNN_PROFILE_EVENTUNIT_MICROSEC:QNN_PROFILE_EVENTUNIT_COUNT;out->value=0;break;
      default:return QNN_PROFILE_ERROR_INVALID_HANDLE;
    }
    return QNN_SUCCESS;
  }
  static QNN_INTERFACE_VER_TYPE api(){
    QNN_INTERFACE_VER_TYPE a{};a.profileCreate=create;a.profileFree=free;a.profileSetConfig=config;
    a.profileGetEvents=events;a.profileGetSubEvents=sub;a.profileGetEventData=data;return a;
  }
  static void reset(){created=freed=data_calls=mode=0;accelerator=7000;}
};
void next_sample(QnnExecutionProfile& p){
  for(unsigned i=1;i<QnnExecutionProfile::kSampleEvery;++i)assert(!p.next());assert(p.next());
}
void sparse_nested_metrics_and_lifetime(){
  Fake::reset();const auto api=Fake::api();QnnExecutionProfile p;p.initialize(api,reinterpret_cast<void*>(2));
  next_sample(p);p.collect(api,100000,11100);assert(p.samples()==1);
  const auto& first=p.last();assert(first.values[1]==10900&&first.values[3]==7000&&first.values[4]==6900);
  assert(first.values[12]==8'000'000&&first.ambiguous==0&&!first.truncated);
  // Reusing the backend's per-execution handle must not sum prior executions.
  Fake::accelerator=6000;next_sample(p);p.collect(api,200000,10100);
  assert(p.last().values[3]==6000&&p.samples()==2&&Fake::created==1);
  assert(p.report().find("qnn_profile_nested_times=true")!=std::string::npos);
  p.release(api);p.release(api);assert(Fake::freed==1);
}
void bounded_bad_metadata_and_explicit_missing_values(){
  for(unsigned mode=1;mode<=5;++mode){
    Fake::reset();Fake::mode=mode;auto api=Fake::api();QnnExecutionProfile p;p.initialize(api,reinterpret_cast<void*>(2));
    next_sample(p);p.collect(api,100000,11000);
    assert(Fake::data_calls<=256);
    if(mode==1)assert(p.last().events==7&&!p.last().truncated);
    if(mode==2)assert(p.last().unit_errors==1&&(p.last().present&(1U<<1))==0);
    if(mode==3)assert(p.last().truncated);
    if(mode==4)assert(p.samples()==0&&p.report().find("qnn_profile_query_failures=1")!=std::string::npos);
    if(mode==5)assert(p.last().ambiguous&(1U<<3));
    p.release(api);assert(Fake::freed==1);
  }
}
void optional_backend_and_execution_rejection(){
  Fake::reset();auto api=Fake::api();QnnExecutionProfile p;
  auto missing=api;missing.profileGetEvents=nullptr;p.initialize(missing,reinterpret_cast<void*>(2));
  for(unsigned i=0;i<256;++i)assert(!p.next());assert(Fake::created==0);
  p.initialize(api,reinterpret_cast<void*>(2));next_sample(p);p.reject_execution(api);
  p.collect(api,100000,11000);assert(p.samples()==0&&Fake::freed==1);
  for(unsigned i=0;i<256;++i)assert(!p.next());
  assert(p.report().find("execution_rejected")!=std::string::npos);
}
}
int main(){sparse_nested_metrics_and_lifetime();bounded_bad_metadata_and_explicit_missing_values();optional_backend_and_execution_rejection();std::puts("PASS bounded sparse QNN profiling, nested units, reuse and optional failures");}
