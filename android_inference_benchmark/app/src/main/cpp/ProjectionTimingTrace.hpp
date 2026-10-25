#pragma once

#include "DiagnosticTraceCursor.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

namespace vfdual_android {
// Fixed numeric source/control ledger. Producer only copies bounded rows;
// Native decoder owns synchronization and formats a drained snapshot outside it.
// All clocks are phone monotonic us. Scaled geometric fields are MODEL pixels.
class ProjectionTimingTrace final {
 public:
  enum class Event : std::int64_t { attempt=1, comparison=2, unscored=3 };
  enum class Outcome : std::int64_t {
    no_newer_ready=1, empty_source=2, source_expired=3, wrong_generation=4,
    projector_rejected=5, gate_rejected=6, shadow_complete=7,
    active_projected=8, chronological_source=9, old_source_skipped=10,
    not_configured=11, observation_not_consumed=12,
    compared=13, superseded_unscored=14, failed_unscored=15,
    generation_unscored=16, stopped_unscored=17, stale_unscored=18
  };
  // projection_trace_v1 fields:
  // id,mode,event,generation,Aseq,Awire,Aobs,Bseq,Bwire,Bobs,
  // currentNNPseq,currentNNPwire,currentNNPobs,controlSeq,controlWire,controlObs,
  // previousL,Adeadline,started,finished,mathWork,sourceCount,validCount,
  // gateReason,mathReason,outcome,comparisonMatched,NNPcount,minIoUe6,
  // mean18PointErrorMilli,AageAtFinish,BageAtFinish.
  using Row=std::array<std::int64_t,32>;
  // projection_target_trace_v1 fields:
  // id,event,mode,generation,Aseq,Awire,Aobs,Bseq,Bwire,Bobs,sourceIndex,
  // valid,mathRejection,dxMilli,dyMilli,qualityE6,fbErrorMilli,support,candidates,
  // sameBNNPindex,minIoUe6,error18PointMilli. -1 means unavailable.
  // For comparison events valid means one-to-one detector agreement IoU>=.50;
  // raw low-IoU best pairs keep their numeric IoU/error but are NOT matches.
  // Observation minIoU includes unmatched source boxes (0); mean18 error uses
  // only accepted matches, with matched/source/NNP counts retained explicitly.
  using TargetRow=std::array<std::int64_t,22>;
  static constexpr std::size_t capacity=512, target_capacity=4096;

  template<std::size_t Fields,std::size_t Capacity> struct Ledger {
    using Value=std::array<std::int64_t,Fields>;
    struct Snapshot {
      std::array<Value,Capacity> rows{};
      std::size_t count{};
      std::uint64_t lost{};
      std::string report(const char* key,const char* lost_key) const {
        std::ostringstream out;out<<lost_key<<'='<<lost<<' '<<key<<"=[";
        for(std::size_t i=0;i<count;++i){
          if(i)out<<';';
          for(std::size_t j=0;j<Fields;++j){if(j)out<<',';out<<rows[i][j];}
        }
        return out.str()+']';
      }
    };
    void record(Value row) noexcept {row[0]=static_cast<std::int64_t>(++total);rows[(total-1)%Capacity]=row;}
    bool has_pending() const noexcept{return total!=drained;}
    void drain(Snapshot& out) noexcept {
      const auto range=cursor.advance(total,Capacity);out.count=0;out.lost=range.lost;
      for(auto id=range.first;id<=range.last;++id)out.rows[out.count++]=rows[(id-1)%Capacity];
      drained=total;
    }
    std::array<Value,Capacity> rows{};
    std::uint64_t total{},drained{};
    DiagnosticTraceCursor cursor{};
  };
  struct Snapshot {
    Ledger<32,capacity>::Snapshot observations;
    Ledger<22,target_capacity>::Snapshot targets;
    std::string report() const {
      return "projection_clock=monotonic_us projection_geometry_space=model_pixels "
          +observations.report("projection_trace_v1","projection_trace_lost")+" "
          +targets.report("projection_target_trace_v1","projection_target_trace_lost");
    }
  };
  void record(Row row) noexcept{observations_.record(row);}
  void record_target(TargetRow row) noexcept{targets_.record(row);}
  bool has_pending() const noexcept{return observations_.has_pending()||targets_.has_pending();}
  void drain(Snapshot& out) noexcept{observations_.drain(out.observations);targets_.drain(out.targets);}
 private:
  Ledger<32,capacity> observations_;
  Ledger<22,target_capacity> targets_;
};
} // namespace vfdual_android
