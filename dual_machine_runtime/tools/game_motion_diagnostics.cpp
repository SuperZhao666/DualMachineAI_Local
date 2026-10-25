// Deterministic counterfactuals, NOT a recreation of Overwatch's private engine.
// World-space translation and perspective projection are independent of the
// production controller. No process/game access or input injection occurs.
#include "MobileControlCore.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <stdexcept>
#include <vector>

using namespace vfdual_android;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
constexpr double pi = 3.14159265358979323846;
constexpr std::uint64_t start = 200'000, step = 7'143, age = 23'000;
struct V3 { double x{}, y{}, z{}; };
struct V2 { double x{}, y{}; };
struct Move { std::uint64_t issued{}, due{}; int x{}, y{}; };
struct Scene { const char* name; bool strafe{}, turn{}, recoil{}, parallax{}, missing_background{}, zoom{}; };
constexpr Scene scenes[]{
    {"stationary"},
    {"target_and_player_strafe", true},
    {"strafe_and_manual_view", true, true},
    {"strafe_view_and_recoil", true, true, true},
    {"near_target_far_background", true, true, false, true},
    {"intermittent_background", true, true, true, true, true},
    {"unbound_zoom_change", true, false, false, false, false, true}
};
double time(std::uint64_t at) { return (double(at) - double(start)) / 1e6; }
double focal(double horizontal_fov) { return 2560.0 / (2.0 * std::tan(horizontal_fov*pi/360.0)); }
V2 project(V3 point, V3 eye, V2 view, double f) {
    point.x -= eye.x; point.y -= eye.y; point.z -= eye.z;
    const double x = std::cos(view.x)*point.x - std::sin(view.x)*point.z;
    const double z = std::sin(view.x)*point.x + std::cos(view.x)*point.z;
    const double y = std::cos(view.y)*point.y - std::sin(view.y)*z;
    const double depth = std::sin(view.y)*point.y + std::cos(view.y)*z;
    return {208 + f*x/depth, 208 + f*y/depth};
}
V3 eye(const Scene& s, double t) {
    return {s.strafe ? .32*std::sin(7*t) : 0, 0,
            s.parallax ? .9*std::sin(3*t) : 0};
}
V3 target(const Scene& s, double t) {
    return {s.strafe ? .35+.46*(2/pi)*std::asin(std::sin(5*t)) : .55,
            s.strafe ? .10*std::sin(5.3*t) : 0,
            s.parallax ? 7.0+.5*std::sin(2*t) : 12.0};
}
V2 manual(const Scene& s, double t) {
    return {s.turn ? .045*std::sin(7*t) : 0,
        (s.turn ? .015*std::sin(9*t) : 0) +
        (s.recoil ? .009*std::fmod(std::max(0.0,t),.65)/.65 : 0)};
}
V2 camera(const Scene& s, std::uint64_t at, const std::vector<Move>& moves) {
    V2 result=manual(s,time(at));
    const double radians_per_count=.665/focal(103);
    for (const auto& move:moves) if (at>=move.due) {
        result.x += move.x*radians_per_count;
        result.y += move.y*radians_per_count;
    }
    return result;
}
double fov(const Scene& s, double t) { return s.zoom && t>2 ? 55 : 103; }
YoloDetection detection(const Scene& s, std::uint64_t at, const std::vector<Move>& moves) {
    V3 p=target(s,time(at));
    const auto e=eye(s,time(at)); const auto c=camera(s,at,moves);
    const double f=focal(fov(s,time(at)));
    V2 lo{1e9,1e9}, hi{-1e9,-1e9};
    for (double dx:{-.30,.30}) for (double dy:{-.324,1.476}) {
        auto q=project({p.x+dx,p.y+dy,p.z},e,c,f);
        lo.x=std::min(lo.x,q.x);lo.y=std::min(lo.y,q.y);
        hi.x=std::max(hi.x,q.x);hi.y=std::max(hi.y,q.y);
    }
    return {float(lo.x),float(lo.y),float(hi.x),float(hi.y),.95F,0};
}
MobileControlConfig config() {
    MobileControlConfig c;
    c.selected_class_id=c.body_class_id=0;
    c.selected_class_uses_geometric_head=true;
    c.head_body_pairing_enabled=c.selected_class_uses_head_box=false;
    c.reject_border_touching=false;
    c.body_fallback_min_confidence=0;
    c.body_fallback_max_center_distance_pixels=416;
    c.motion.response_gain=.6F;
    c.motion.maximum_prediction_pixels=8;
    apply_mobile_model_control_tuning(vfdual::kOverwatch2,c);
    c.observation_servo_enabled=true;
    c.lost_target_hold_duration_us=350'000;
    c.tracker.lost_track_duration_us=500'000;
    return c;
}
bool run(const Scene& scene, std::uint64_t delay, std::uint64_t receipt) {
    MobileControlCore core(config());
    const auto modeled_delay=std::clamp<std::uint64_t>(delay>receipt?delay-receipt:0,8'000,100'000);
    require(core.restore_response_calibration(.665F,.665F,modeled_delay,.95F,.95F), "calibration restore failed");
    std::vector<Move> moves;
    std::vector<double> errors, normalized;
    std::uint64_t previous{}, last_due{}, callback_at{}, moves_count{}, blocked{}, active{}, inside{}, switches{}, lock{};
    Move awaiting{};
    bool pending{};
    for (unsigned i=0;i<840;++i) {
        const auto observed=start+i*step, now=observed+age;
        if (pending && now>=callback_at) {
            require(core.record_accepted_move(awaiting.x,awaiting.y,callback_at), "move receipt rejected");
            pending=false;
        }
        const auto truth=detection(scene,observed,moves);
        auto box=truth;
        const float noise=float(int(i*13%17)-8)*.1F;
        box.x1+=noise;box.x2+=noise;box.y1-=noise;box.y2-=noise;
        box.x1=std::max(0.F,box.x1);box.y1=std::max(0.F,box.y1);
        box.x2=std::min(416.F,box.x2);box.y2=std::min(416.F,box.y2);
        std::array<YoloDetection,1> boxes{box};
        std::span<const YoloDetection> detections = box.x2>box.x1 && box.y2>box.y1
            ? std::span<const YoloDetection>(boxes) : std::span<const YoloDetection>{};
        ControlFrameContext frame{std::uint64_t(i+1),observed,now};
        if (previous && !(scene.missing_background && i%3==0)) {
            // A fixed distant wall has different parallax from the near body.
            const V3 wall{0,0,40};
            const auto before=project(wall,eye(scene,time(previous)),camera(scene,previous,moves),focal(fov(scene,time(previous))));
            const auto after=project(wall,eye(scene,time(observed)),camera(scene,observed,moves),focal(fov(scene,time(observed))));
            frame.background={previous,float(after.x-before.x),float(after.y-before.y),24,32,true};
        }
        MobileControlOutput output;
        if (pending) { (void)core.observe_tracking_only(detections,frame); ++blocked; }
        else output=core.process(detections,frame);
        if (output.has_target && !output.held) ++active;
        if (output.has_target) {
            if (lock && lock!=output.lock_id) ++switches;
            lock=output.lock_id;
        }
        if (!pending && output.has_move()) {
            const auto issued=now+500;
            const auto jitter=std::int64_t(double(delay)*.25*std::sin(time(now)*17));
            last_due=std::max(last_due+1'000,issued+std::uint64_t(std::int64_t(delay)+jitter));
            awaiting={issued,last_due,output.delta_x,output.delta_y};
            moves.push_back(awaiting); ++moves_count;
            callback_at=issued+receipt; pending=true;
        }
        previous=observed;
        if (i<140) continue;
        const auto b=detection(scene,now,moves);
        const double x=(b.x1+b.x2)/2, y=b.y1+.18*(b.y2-b.y1);
        const double error=std::hypot(x-208,y-208);
        errors.push_back(error); normalized.push_back(error/std::max(1.F,b.x2-b.x1));
        if (208>=b.x1 && 208<=b.x2 && 208>=b.y1 && 208<=b.y2) ++inside;
    }
    double sum{}; for (auto e:errors) sum+=e*e;
    std::sort(errors.begin(),errors.end());std::sort(normalized.begin(),normalized.end());
    const double rms=std::sqrt(sum/errors.size()), p95=errors[errors.size()*95/100];
    const double body_percent=100.0*inside/errors.size(), np95=normalized[normalized.size()*95/100];
    const bool passed=body_percent>=95 && np95<=.25 && switches==0;
    std::printf("{\"scene\":\"%s\",\"physical_delay_us\":%llu,\"receipt_delay_us\":%llu,\"image_age_us\":%llu,\"rms_px\":%.4f,\"p95_px\":%.4f,\"p95_body_widths\":%.4f,\"inside_body_percent\":%.3f,\"move_hz\":%.3f,\"receipt_blocked_frames\":%llu,\"active_frames\":%llu,\"switches\":%llu,\"synthetic_acceptance\":%s}\n",
        scene.name,delay,receipt,age,rms,p95,np95,body_percent,moves_count/(840.*step/1e6),blocked,active,switches,passed?"true":"false");
    return passed;
}
}
int main(int argc,char** argv) {
    // Geometry contracts: crop coordinates retain the full viewport focal length.
    const auto q=project({1,0,10},{},{},focal(103));
    require(std::abs((q.x-208)-focal(103)*.1)<1e-9, "projection geometry");
    require(std::abs(std::atan((q.x-208)/focal(103))-std::atan(.1))<1e-9, "angular geometry");
    require(focal(55)>focal(103)*2, "zoom geometry");
    if (argc>1 && std::string_view(argv[1])=="--geometry-only") return 0;
    std::setvbuf(stdout,nullptr,_IONBF,0);
    bool passed=true;
    for (const auto& scene:scenes) {
        for (auto delay:{4'000U,32'000U,60'000U}) passed=run(scene,delay,0)&&passed;
        passed=run(scene,60'000,16'000)&&passed;
    }
    return argc>1 && std::string_view(argv[1])=="--verify" && !passed ? 2:0;
}
