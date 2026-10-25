#pragma once
#include "DiagnosticTraceCursor.hpp"
#include <array>
#include <cstdint>
#include <sstream>
#include <string>

namespace vfdual_android {
// Successful frames already have pipeline_trace_v1. This independent bounded
// ledger keeps work which disappeared before reaching that success trace.
enum class PipelineOutcome : std::uint32_t {
  generation_before_inference=1, stale_before_inference=2,
  qnn_failed=3, generation_after_inference=4, stopped_after_inference=5,
  preprocessed_superseded=6, repeated_decoded_content=7,
  decoded_discarded=8, compressed_discarded=9,
  deadline_replaced_before_inference=10
};
class PipelineOutcomeTrace {
 public:
  static constexpr std::size_t capacity=2048;
  using Row=std::array<std::uint64_t,8>;
  struct Snapshot {
    std::array<Row,capacity> rows{};std::size_t count{};std::uint64_t lost{};
    std::string report() const {
      std::ostringstream out;
      out<<"outcome_clock=monotonic_us pipeline_outcome_lost="<<lost<<" pipeline_outcome_v1=[";
      for(std::size_t i=0;i<count;++i){if(i)out<<';';for(unsigned j=0;j<8;++j){if(j)out<<',';out<<rows[i][j];}}
      return out.str()+']';
    }
  };
  // Caller serializes access; record never allocates or formats a string.
  void record(std::uint64_t sequence,std::uint64_t wire,std::uint64_t generation,
      std::uint64_t observed,std::uint64_t at,PipelineOutcome reason,std::uint64_t detail=0) noexcept {
    auto id=++total_;rows_[(id-1)%capacity]={id,sequence,wire,generation,observed,at,static_cast<std::uint64_t>(reason),detail};
  }
  void drain(Snapshot& out) noexcept {
    auto range=cursor_.advance(total_,capacity);out.count=0;out.lost=range.lost;
    for(auto i=range.first;i<=range.last;++i)out.rows[out.count++]=rows_[(i-1)%capacity];
  }
 private:
  std::array<Row,capacity> rows_{};std::uint64_t total_{};DiagnosticTraceCursor cursor_{};
};
}
