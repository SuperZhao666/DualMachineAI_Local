#include "vfdual/desktop_video_agent.hpp"
#include "vfdual/host_display_catalog.hpp"
#include "vfdual/streamer_metrics_csv.hpp"
#include "vfdual/udp_socket.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <thread>
#include <vector>

// Runs the production capture/encode path with a loopback discard receiver.
// It neither connects to the phone nor emits mouse input. Timings are buffered
// in memory so writing a complete per-frame CSV cannot stall the measured path.
int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: VisionForgeHostProfile seconds output.csv\n";
        return 2;
    }
    const int seconds = std::atoi(argv[1]);
    if (seconds < 1 || seconds > 600) return 2;
    if (std::getenv("VF_PROFILE_GPU_HIGH")) {
        // Own process only. D3DKMT class HIGH (4), never REALTIME.
        using SetGpuClass = LONG (WINAPI*)(HANDLE, int);
        const auto gdi = LoadLibraryW(L"gdi32.dll");
        const auto set_class = gdi ? reinterpret_cast<SetGpuClass>(GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass")) : nullptr;
        const LONG result = set_class ? set_class(GetCurrentProcess(), 4) : -1;
        std::cerr << "gpu_process_high_status=" << result << '\n';
        if (gdi) FreeLibrary(gdi);
    }
    vfdual::UdpSocket sink;
    if (!sink.open() || !sink.bind_to("127.0.0.1", 0)) return 3;
    std::atomic_bool running{true};
    std::jthread drain([&] {
        std::array<std::byte, 65536> packet{};
        while (running.load()) static_cast<void>(sink.receive(packet, 50));
    });
    const auto finish = [&] { running.store(false); drain.join(); };
    const auto outputs = vfdual::enumerate_host_display_outputs();
    const auto* display = vfdual::select_preferred_display_output(outputs);
    if (!display) { finish(); return 4; }
    const auto geometry = vfdual::choose_centered_ai_capture_geometry(*display);
    vfdual::DesktopVideoAgentConfig config{};
    if (const char* preference = std::getenv("VF_PROFILE_ENCODER")) {
        if (std::string_view(preference) == "software") config.encoder_preference = vfdual::H264EncoderPreference::software;
        else if (std::string_view(preference) == "direct") config.encoder_preference = vfdual::H264EncoderPreference::nvenc;
    }
    config.adapter_index = display->adapter_index;
    config.output_index = display->output_index;
    config.capture_timeout_ms = 16;
    config.capture_region = {geometry.local_roi_x, geometry.local_roi_y,
                            geometry.local_roi_width, geometry.local_roi_height};
    config.encoder = {geometry.encoder_width, geometry.encoder_height,
                      display->refresh_hz, display->refresh_hz};
    config.local_host = "127.0.0.1";
    vfdual::UdpSocket source_reservation;
    if (!source_reservation.open() || !source_reservation.bind_to("127.0.0.1", 0)) {
        finish(); return 3;
    }
    config.local_port = source_reservation.local_port();
    source_reservation.close();
    config.phone_host = "127.0.0.1";
    config.phone_port = sink.local_port();
    vfdual::DesktopVideoAgent agent;
    if (!agent.initialize(config)) {
        std::cerr << "initialize_failed status=" << agent.encoder_diagnostics().last_failure_status << '\n';
        finish(); return 5;
    }
    const auto diagnostics = agent.encoder_diagnostics();
    std::vector<vfdual::DesktopVideoStepMetrics> records;
    records.reserve(static_cast<std::size_t>(seconds) * 1000U);
    std::uint64_t published{}, fresh{}, failures{};
    const auto collect = [&](const vfdual::DesktopVideoStepMetrics& step) {
        records.push_back(step);
        if (step.status == vfdual::DesktopVideoStepStatus::frame_published) {
            ++published;
            if (!step.repeated_content) ++fresh;
        } else if (step.status == vfdual::DesktopVideoStepStatus::encode_failed ||
                   step.status == vfdual::DesktopVideoStepStatus::publish_failed ||
                   step.status == vfdual::DesktopVideoStepStatus::capture_failed ||
                   step.status == vfdual::DesktopVideoStepStatus::capture_access_lost ||
                   step.status == vfdual::DesktopVideoStepStatus::bridge_failed) {
            ++failures;
        }
    };
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(seconds);
    std::map<DWORD, std::uint64_t> foreground_time_us;
    while (std::chrono::steady_clock::now() < deadline && !failures) {
        DWORD foreground_pid{};
        GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
        const auto sample_started = std::chrono::steady_clock::now();
        collect(agent.publish_next());
        vfdual::DesktopVideoStepMetrics completed;
        while (agent.try_pop_completed(completed)) collect(completed);
        foreground_time_us[foreground_pid] += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-sample_started).count());
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    agent.reset();
    finish();
    vfdual::StreamerMetricsCsv csv;
    if (!csv.open(argv[2])) return 6;
    for (const auto& record : records) csv.append(record);
    csv.close();
    std::cout << "seconds=" << elapsed << " published=" << published
              << " fresh=" << fresh << " fresh_fps=" << fresh / elapsed
              << " failures=" << failures << " backend=" << static_cast<int>(diagnostics.backend)
              << " same_adapter=" << diagnostics.same_adapter
              << " candidate=" << static_cast<int>(diagnostics.selected_candidate)
              << " failed_candidates=" << diagnostics.failed_candidate_mask << '\n';
    for (const auto& [pid, microseconds] : foreground_time_us)
        std::cout << "foreground_pid=" << pid << " seconds=" << microseconds / 1e6 << '\n';
    return failures || !published ? 7 : 0;
}
