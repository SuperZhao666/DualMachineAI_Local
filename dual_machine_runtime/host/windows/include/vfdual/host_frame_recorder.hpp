#pragma once
#include "vfdual/desktop_video_agent.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace vfdual {
// One capture producer, one diagnostics consumer. A full queue drops a trace
// record explicitly; it never blocks capture/encoding on disk or formatting.
class HostFrameRecorder final {
 public:
  static constexpr std::size_t capacity=2048;
  using Row=std::array<std::int64_t,29>;
  explicit HostFrameRecorder(std::function<void(std::string)> sink)
      :sink_(std::move(sink)),worker_([this]{run();}){}
  ~HostFrameRecorder(){running_=false;condition_.notify_one();if(worker_.joinable())worker_.join();}
  HostFrameRecorder(const HostFrameRecorder&)=delete;
  HostFrameRecorder& operator=(const HostFrameRecorder&)=delete;
  void record(const DesktopVideoStepMetrics& s,std::uint32_t epoch,std::uint64_t at,std::uint32_t foreground_pid) noexcept {
    const auto id=++sequence_;
    const auto head=head_.load(std::memory_order_relaxed),next=(head+1)%(capacity+1);
    if(next==tail_.load(std::memory_order_acquire)){++dropped_;return;}
    rows_[head]={static_cast<std::int64_t>(id),epoch,static_cast<std::int64_t>(at),
      static_cast<std::int64_t>(s.source_sequence),s.frame_id,static_cast<std::int64_t>(s.status),
      static_cast<std::int64_t>(s.capture_present_us),static_cast<std::int64_t>(s.capture_us),
      static_cast<std::int64_t>(s.bridge_us),static_cast<std::int64_t>(s.encode_us),
      static_cast<std::int64_t>(s.publish_us),static_cast<std::int64_t>(s.capture_to_publish_us),
      static_cast<std::int64_t>(s.readback_wait_us),static_cast<std::int64_t>(s.readback_copy_us),
      static_cast<std::int64_t>(s.upload_us),static_cast<std::int64_t>(s.nvenc_wait_us),
      s.access_unit_bytes,s.datagrams_sent,s.repeated_content,s.keyframe,s.native_status,s.asynchronous_pipeline,
      static_cast<std::int64_t>(s.mf_stages_us[0]),static_cast<std::int64_t>(s.mf_stages_us[1]),
      static_cast<std::int64_t>(s.mf_stages_us[2]),static_cast<std::int64_t>(s.mf_stages_us[3]),
      static_cast<std::int64_t>(s.mf_stages_us[4]),static_cast<std::int64_t>(s.mf_stages_us[5]),foreground_pid};
    head_.store(next,std::memory_order_release);
  }
  std::uint64_t dropped() const noexcept{return dropped_.load();}
 private:
  bool pop(Row& row) noexcept {
    const auto tail=tail_.load(std::memory_order_relaxed);
    if(tail==head_.load(std::memory_order_acquire))return false;
    row=rows_[tail];tail_.store((tail+1)%(capacity+1),std::memory_order_release);return true;
  }
  void run() noexcept {
    do {
      std::ostringstream values;Row row{};std::size_t count{};
      while(count<capacity && pop(row)){
        if(count++)values<<';';for(std::size_t j=0;j<row.size();++j){if(j)values<<',';values<<row[j];}
      }
      if(count){
        try{sink_("host_frame_clock=steady_us host_frame_trace_lost="+std::to_string(dropped_.load())
          +" host_frame_sink_failures="+std::to_string(sink_failures_)+" host_frame_trace_v3=["+values.str()+"]");}
        catch(...){++sink_failures_;}
      }
      if(!running_ && tail_.load()==head_.load())break;
      std::unique_lock lock(wait_mutex_);
      condition_.wait_for(lock,std::chrono::milliseconds(250),[this]{return !running_.load();});
    }while(true);
  }
  std::function<void(std::string)> sink_;
  std::array<Row,capacity+1> rows_{};
  std::atomic_size_t head_{},tail_{};
  std::atomic_uint64_t dropped_{};
  std::uint64_t sequence_{},sink_failures_{};
  std::atomic_bool running_{true};
  std::mutex wait_mutex_;std::condition_variable condition_;std::thread worker_;
};
}
