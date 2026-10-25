#include "MobileTargetTracker.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace vfdual_android;
struct Counts { unsigned long long correct{}, wrong_owner{}, rebuilt{}, observations{}, cases{}, rescued{}; };
int main(int argc,char** argv) {
  if(argc==2 && std::string(argv[1])=="--timing") {
    for(unsigned count:{2U,4U,8U,16U}) {
      MobileTargetTrackerConfig c;c.control_smoothing_alpha=1;c.lost_track_duration_us=500'000;
      MobileTargetTracker tracker(c);std::vector<YoloDetection> before,moved;
      for(unsigned i=0;i<count;++i) {
        const float x=100.F+(i%4)*48.F,y=100.F+(i/4)*60.F;
        before.push_back({x-10,y-20,x+10,y+20,.9F,0});
        moved.push_back({x-42,y-20,x-22,y+20,.9F,0});
      }
      (void)tracker.update(before,1,100'000);
      std::vector<double> samples;samples.reserve(1000);unsigned accepted{};
      for(unsigned i=0;i<1050;++i) {
        const auto started=std::chrono::steady_clock::now();
        const auto shift=tracker.resolve_association_frame_shift(moved,2,108'000);
        const auto us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-started).count();
        accepted+=shift.measured;if(i>=50)samples.push_back(us);
      }
      std::sort(samples.begin(),samples.end());
      std::cout<<"count="<<count<<" p50_us="<<samples[500]<<" p95_us="<<samples[950]
               <<" p99_us="<<samples[990]<<" accepted="<<accepted<<'\n';
      if(accepted!=1050)return 6;
    }
    return 0;
  }
  if(argc!=3) {std::cerr<<"input reference CSV, output JSON required\n";return 2;}
  std::ifstream input(argv[1]);if(!input)return 3;
  std::string line;std::getline(input,line);
  std::map<unsigned,std::vector<YoloDetection>> frames;
  while(std::getline(input,line)) {
    std::istringstream in(line);std::vector<std::string> fields;std::string field;
    while(std::getline(in,field,','))fields.push_back(field);
    if(fields.size()!=8)return 4;
    if(std::stoi(fields[2])<0)continue;
    YoloDetection d{std::stof(fields[4]),std::stof(fields[5]),std::stof(fields[6]),std::stof(fields[7]),std::stof(fields[3]),unsigned(std::stoi(fields[2]))};
    if(d.confidence<.2F || d.x2-d.x1<2 || d.y2-d.y1<2)continue;
    frames[std::stoul(fields[0])].push_back(d);
  }
  Counts baseline{},candidate{};unsigned improvements{},regressions{},correct_regressions{};
  double resolve_us{}; unsigned long long resolves{};
  std::ofstream details(std::string(argv[2])+".cases.csv");
  details<<"source_frame,shift_x,shift_y,perturbation,missing_index,baseline_wrong,candidate_wrong,baseline_correct,candidate_correct,consensus_used\n";
  for(const auto& [frame,initial]:frames) {
    if(initial.size()<2 || initial.size()>16)continue;
    for(float dx:{-64.F,-48.F,-32.F,-24.F,-16.F,0.F,16.F,24.F,32.F,48.F,64.F})
      for(float dy:{-12.F,0.F,12.F}) for(float noise:{0.F,2.F,8.F,16.F})
      for(int missing:{-1,0,1}) {
      std::array<Counts,2> local{};
      for(unsigned mode=0;mode<2;++mode) {
        MobileTargetTrackerConfig c;c.high_confidence_threshold=c.new_track_confidence_threshold=.4F;
        c.low_confidence_threshold=.2F;c.control_smoothing_alpha=1;c.lost_track_duration_us=500'000;
        MobileTargetTracker tracker(c);const auto before=tracker.update(initial,1,100'000);
        std::vector<YoloDetection> moved;std::vector<std::uint64_t> owners;
        std::uint64_t maximum_old{};for(const auto& d:before)maximum_old=std::max(maximum_old,d.track_id);
        // Reversed detector order and bounded independent motion are part of
        // the stress. Source geometry is real; this translation is synthetic.
        for(std::size_t j=initial.size();j-->0;) {
          if(int(j)==missing) continue;
          auto d=initial[j];const float nx=noise*(j%2?1.F:-1.F),ny=noise*(j%3?1.F:-1.F);
          d.x1+=dx+nx;d.x2+=dx+nx;d.y1+=dy+ny;d.y2+=dy+ny;
          const float visible=std::max(0.F,std::min(416.F,d.x2)-std::max(0.F,d.x1))*
              std::max(0.F,std::min(416.F,d.y2)-std::max(0.F,d.y1));
          if(visible<(d.x2-d.x1)*(d.y2-d.y1)*.25F)continue;
          moved.push_back(d);owners.push_back(before[j].track_id);
        }
        auto& count=local[mode];count.cases=1;
        if(mode) {
          const auto started=std::chrono::steady_clock::now();
          const auto shift=tracker.resolve_association_frame_shift(moved,2,108'000);
          resolve_us+=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-started).count();
          ++resolves;
          if(shift.measured){tracker.apply_visible_frame_shift(shift.shift_x,shift.shift_y);++count.rescued;}
        }
        const auto after=tracker.update(moved,2,108'000);
        for(std::size_t j=0;j<after.size();++j) {
          if(!owners[j])continue;
          ++count.observations;
          if(after[j].track_id==owners[j])++count.correct;
          else if(after[j].track_id && after[j].track_id<=maximum_old)++count.wrong_owner;
          else ++count.rebuilt;
        }
        auto& total=mode?candidate:baseline;
        total.correct+=count.correct;total.wrong_owner+=count.wrong_owner;total.rebuilt+=count.rebuilt;
        total.observations+=count.observations;total.cases+=count.cases;total.rescued+=count.rescued;
      }
      if(local[1].wrong_owner<local[0].wrong_owner)++improvements;
      if(local[1].wrong_owner>local[0].wrong_owner)++regressions;
      if(local[1].correct<local[0].correct)++correct_regressions;
      details<<frame<<','<<dx<<','<<dy<<','<<noise<<','<<missing<<','<<local[0].wrong_owner<<','<<local[1].wrong_owner<<','<<local[0].correct<<','<<local[1].correct<<','<<local[1].rescued<<'\n';
    }
  }
  std::ofstream out(argv[2]);
  out<<"{\"scope\":\"Synthetic camera shifts using recorded FP32 detection geometry; known source indices establish correspondence, not real-game identity or closed-loop accuracy\",\"baseline\":{";
  const auto emit=[&](const Counts& c){out<<"\"cases\":"<<c.cases<<",\"observations\":"<<c.observations<<",\"correct\":"<<c.correct<<",\"wrong_owner\":"<<c.wrong_owner<<",\"rebuilt\":"<<c.rebuilt<<",\"consensus_used\":"<<c.rescued;};
  emit(baseline);out<<"},\"candidate\":{";emit(candidate);
  out<<"},\"improved_cases\":"<<improvements<<",\"wrong_owner_regressed_cases\":"<<regressions
     <<",\"correct_identity_regressed_cases\":"<<correct_regressions
     <<",\"mean_resolve_us\":"<<(resolves?resolve_us/resolves:0)<<"}\n";
  std::cout<<"cases="<<candidate.cases<<" wrong_owner_before="<<baseline.wrong_owner<<" after="<<candidate.wrong_owner<<" regressed_cases="<<regressions<<'\n';
  return out && details && candidate.cases ? 0:5;
}
