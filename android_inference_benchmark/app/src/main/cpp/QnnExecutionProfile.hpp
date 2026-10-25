#pragma once
#include "QnnInterface.h"
#include "HTP/QnnHtpProfile.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>

namespace vfdual_android {
// Low-rate, bounded BASIC profiling. These are nested measurements: host RPC,
// DSP RPC and accelerator time must not be summed as independent pipeline work.
// QnnGraph_execute resets a reused per-call profile handle (QnnGraph.h).
class QnnExecutionProfile final {
 public:
  static constexpr std::uint64_t kSampleEvery=128U;
  struct Field {std::uint32_t type,unit;const char* name;};
  static constexpr std::array<Field,14> fields{{
    {QNN_PROFILE_EVENTTYPE_EXECUTE,QNN_PROFILE_EVENTUNIT_MICROSEC,"execute_us"},
    {QNN_HTP_PROFILE_EVENTTYPE_GRAPH_EXECUTE_HOST_RPC_TIME_MICROSEC,QNN_PROFILE_EVENTUNIT_MICROSEC,"host_rpc_us"},
    {QNN_HTP_PROFILE_EVENTTYPE_GRAPH_EXECUTE_HTP_RPC_TIME_MICROSEC,QNN_PROFILE_EVENTUNIT_MICROSEC,"htp_rpc_us"},
    {QNN_HTP_PROFILE_EVENTTYPE_GRAPH_EXECUTE_ACCEL_TIME_MICROSEC,QNN_PROFILE_EVENTUNIT_MICROSEC,"accelerator_inclusive_us"},
    {QNN_HTP_PROFILE_EVENTTYPE_GRAPH_EXECUTE_ACCEL_EXCL_WAIT_TIME_MICROSEC,QNN_PROFILE_EVENTUNIT_MICROSEC,"accelerator_excl_wait_us"},
    {QNN_PROFILE_EVENTTYPE_EXECUTE_QUEUE_WAIT,QNN_PROFILE_EVENTUNIT_MICROSEC,"queue_wait_us"},
    {QNN_PROFILE_EVENTTYPE_EXECUTE_PREPROCESS,QNN_PROFILE_EVENTUNIT_MICROSEC,"preprocess_us"},
    {QNN_PROFILE_EVENTTYPE_EXECUTE_POSTPROCESS,QNN_PROFILE_EVENTUNIT_MICROSEC,"postprocess_us"},
    {QNN_HTP_PROFILE_EVENTTYPE_GRAPH_EXECUTE_VTCM_ACQUIRE_TIME,QNN_PROFILE_EVENTUNIT_MICROSEC,"vtcm_wait_us"},
    {QNN_HTP_PROFILE_EVENTTYPE_GRAPH_EXECUTE_RESOURCE_POWER_UP_TIME,QNN_PROFILE_EVENTUNIT_MICROSEC,"power_up_us"},
    {QNN_HTP_PROFILE_EVENTTYPE_GRAPH_EXECUTE_YIELD_INSTANCE_WAIT_TIME,QNN_PROFILE_EVENTUNIT_MICROSEC,"yield_wait_us"},
    {QNN_HTP_PROFILE_EVENTTYPE_GRAPH_EXECUTE_YIELD_COUNT,QNN_PROFILE_EVENTUNIT_COUNT,"yield_count"},
    {QNN_HTP_PROFILE_EVENTTYPE_GRAPH_EXECUTE_ACCEL_TIME_CYCLE,QNN_PROFILE_EVENTUNIT_CYCLES,"accelerator_cycles"},
    {QNN_PROFILE_EVENTTYPE_EXECUTE_DEVICE,QNN_PROFILE_EVENTUNIT_MICROSEC,"device_us"}
  }};
  struct Sample {
    std::uint64_t execution{},started_us{},wall_us{},read_us{};
    std::array<std::uint64_t,fields.size()> values{};
    std::uint32_t present{},ambiguous{},events{},query_errors{},unit_errors{};
    bool truncated{};
  };

  void initialize(const QNN_INTERFACE_VER_TYPE& api,Qnn_BackendHandle_t backend) noexcept {
    release(api);executions_=samples_=query_failures_=0;last_={};history_.fill({});
    if(!api.profileCreate||!api.profileFree||!api.profileGetEvents||!api.profileGetSubEvents||!api.profileGetEventData){state_="unsupported_api";return;}
    Qnn_ProfileHandle_t handle{};
    const auto status=api.profileCreate(backend,QNN_PROFILE_LEVEL_BASIC,&handle);
    if(status!=QNN_SUCCESS||!handle){state_="create_failed";return;}
    handle_=handle;state_="enabled";
    if(api.profileSetConfig){
      QnnProfile_Config_t limit=QNN_PROFILE_CONFIG_INIT;
      limit.option=QNN_PROFILE_CONFIG_OPTION_MAX_EVENTS;limit.numMaxEvents=kMaximumEvents;
      const QnnProfile_Config_t* options[]{&limit,nullptr};
      // This optional limit is not an inference requirement. The local reader
      // is always bounded, even on a backend that rejects MAX_EVENTS.
      event_limit_supported_=api.profileSetConfig(handle_,options)==QNN_SUCCESS;
    }
  }
  void release(const QNN_INTERFACE_VER_TYPE& api) noexcept {
    if(handle_ && api.profileFree)(void)api.profileFree(handle_);
    handle_=nullptr;state_="disabled";event_limit_supported_=false;
  }
  [[nodiscard]] Qnn_ProfileHandle_t next() noexcept {
    ++executions_;return handle_ && executions_%kSampleEvery==0U?handle_:nullptr;
  }
  void reject_execution(const QNN_INTERFACE_VER_TYPE& api) noexcept {
    release(api);state_="execution_rejected";
  }
  void collect(const QNN_INTERFACE_VER_TYPE& api,std::uint64_t started_us,std::uint64_t wall_us) noexcept {
    if(!handle_)return;
    const auto began=std::chrono::steady_clock::now();
    Sample sample{};sample.execution=executions_;sample.started_us=started_us;sample.wall_us=wall_us;
    const QnnProfile_EventId_t* roots{};std::uint32_t count{};
    if(api.profileGetEvents(handle_,&roots,&count)!=QNN_SUCCESS || (count && !roots)){
      ++query_failures_;return;
    }
    std::array<QnnProfile_EventId_t,kMaximumEvents> pending{},visited{};
    std::size_t queued{},read{},seen{};
    const auto enqueue=[&](const QnnProfile_EventId_t* ids,std::uint32_t size){
      if(size && !ids){++sample.query_errors;return;}
      const auto take=std::min<std::size_t>(size,kMaximumEvents-queued);
      for(std::size_t i=0;i<take;++i)pending[queued++]=ids[i];
      sample.truncated|=take<size;
    };
    enqueue(roots,count);
    while(read<queued){
      const auto id=pending[read++];
      if(std::find(visited.begin(),visited.begin()+seen,id)!=visited.begin()+seen)continue;
      visited[seen++]=id;
      QnnProfile_EventData_t event=QNN_PROFILE_EVENT_DATA_INIT;
      if(api.profileGetEventData(id,&event)==QNN_SUCCESS){
        ++sample.events;
        for(std::size_t i=0;i<fields.size();++i)if(event.type==fields[i].type){
          const auto bit=1U<<i;
          if(event.unit!=fields[i].unit){++sample.unit_errors;sample.ambiguous|=bit;continue;}
          if(sample.present&bit)sample.ambiguous|=bit;
          else {sample.values[i]=event.value;sample.present|=bit;}
        }
      } else ++sample.query_errors;
      const QnnProfile_EventId_t* children{};std::uint32_t child_count{};
      if(api.profileGetSubEvents(id,&children,&child_count)==QNN_SUCCESS)enqueue(children,child_count);
      else ++sample.query_errors;
    }
    sample.read_us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-began).count();
    last_=sample;history_[samples_++%history_.size()]=sample;
  }
  [[nodiscard]] const Sample& last() const noexcept {return last_;}
  [[nodiscard]] std::uint64_t samples() const noexcept {return samples_;}
  [[nodiscard]] std::string report() const {
    std::ostringstream out;
    out<<"qnn_profile_state="<<state_<<" qnn_profile_every="<<kSampleEvery
       <<" qnn_profile_samples="<<samples_<<" qnn_profile_query_failures="<<query_failures_
       <<" qnn_profile_event_limit="<<event_limit_supported_
       <<" qnn_profile_nested_times=true qnn_profile_last=["<<last_.execution<<','<<last_.started_us
       <<','<<last_.wall_us<<','<<last_.read_us<<','<<last_.events<<','<<last_.present<<','<<last_.ambiguous;
    for(std::size_t i=0;i<fields.size();++i){out<<',';if(valid(last_,i))out<<last_.values[i];else out<<-1;}
    out<<"]";
    for(std::size_t i=0;i<fields.size();++i){
      std::array<std::uint64_t,64> values{};std::size_t count{};
      for(std::size_t j=0;j<std::min<std::uint64_t>(samples_,history_.size());++j)
        if(valid(history_[j],i))values[count++]=history_[j].values[i];
      if(!count)continue;
      std::sort(values.begin(),values.begin()+count);
      out<<" qnn_profile_"<<fields[i].name<<"{n="<<count<<" p50="<<values[count/2]
         <<" p95="<<values[(count-1)*95/100]<<"}";
    }
    return out.str();
  }
 private:
  static constexpr std::size_t kMaximumEvents=256U;
  static bool valid(const Sample& sample,std::size_t i) noexcept {
    return (sample.present&(1U<<i)) && !(sample.ambiguous&(1U<<i)) && !sample.truncated;
  }
  Qnn_ProfileHandle_t handle_{};
  const char* state_{"disabled"};
  bool event_limit_supported_{};
  std::uint64_t executions_{},samples_{},query_failures_{};
  Sample last_{};
  std::array<Sample,64> history_{};
};
}
