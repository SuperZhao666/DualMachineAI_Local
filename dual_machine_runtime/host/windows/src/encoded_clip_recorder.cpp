#include "vfdual/encoded_clip_recorder.hpp"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace vfdual {
struct EncodedClipRecorder::State {
    struct Row {
        std::uint32_t id;
        std::uint64_t published, captured, offset;
        std::size_t bytes;
        bool repeated;
    };
    static constexpr std::size_t budget = 64U * 1024U * 1024U;
    static constexpr std::size_t maximum_frames = 24'000;
    std::vector<std::byte> data;
    std::vector<Row> rows;
    std::filesystem::path base;
    std::thread writer;
    std::atomic<bool> complete{false};
    std::size_t used{};
    std::uint64_t first_us{}, foreground_since{}, unix_start{};
    DWORD game_pid{};
    const char* reason{"stopped"};

    void finish(const char* why) noexcept {
        if (complete.load(std::memory_order_acquire)) return;
        reason = why;
        complete.store(true, std::memory_order_release);
        complete.notify_one();
    }
    ~State() { finish("publisher_closed"); if (writer.joinable()) writer.join(); }

    void write() noexcept {
        complete.wait(false, std::memory_order_acquire);
        try {
            if (rows.empty()) return;
            auto path = base; path += L".h264";
            std::ofstream bitstream(path, std::ios::binary);
            bitstream.write(reinterpret_cast<const char*>(data.data()), used);
            bitstream.close();
            if (!bitstream) return;
            path = base; path += L".csv";
            std::ofstream index(path);
            index << "frame_id,published_us,captured_us,offset,bytes,repeated\n";
            for (const auto& r : rows) index << r.id << ',' << r.published << ',' << r.captured
                << ',' << r.offset << ',' << r.bytes << ',' << r.repeated << '\n';
            index.close();
            if (!index) return;
            path = base; path += L".done.json";
            std::ofstream metadata(path);
            metadata << "{\"frames\":" << rows.size() << ",\"bytes\":" << used
                << ",\"first_publish_us\":" << first_us << ",\"first_publish_unix_us\":" << unix_start
                << ",\"game_pid\":" << game_pid << ",\"reason\":\"" << reason
                << "\",\"scope\":\"existing encoded game crop, up to 60 seconds and 64 MiB\"}";
        } catch (...) { /* diagnostics may never interrupt streaming */ }
    }
};

EncodedClipRecorder::EncodedClipRecorder() noexcept {
    try {
        wchar_t folder[32768]{}, pid_text[32]{};
        const auto size = GetEnvironmentVariableW(L"VF_DIAGNOSTIC_CLIP_DIR", folder, 32768);
        if (!size || size >= 32768 || !GetEnvironmentVariableW(L"VF_DIAGNOSTIC_GAME_PID", pid_text, 32)) return;
        const auto pid = wcstoul(pid_text, nullptr, 10);
        if (!pid) return;
        std::filesystem::path directory(folder);
        if (!directory.is_absolute()) return;
        std::filesystem::create_directories(directory);
        auto state = std::make_unique<State>();
        state->game_pid = pid;
        state->data.resize(State::budget);
        state->rows.reserve(State::maximum_frames);
        const auto unix_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        state->base = directory / (L"game-crop-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(unix_us));
        auto* address = state.get();
        state->writer = std::thread([address] { address->write(); });
        state_ = std::move(state);
    } catch (...) { state_.reset(); }
}

EncodedClipRecorder::~EncodedClipRecorder() = default;

void EncodedClipRecorder::append(std::uint32_t frame_id, std::span<const std::byte> au,
    std::uint64_t published_us, std::uint64_t captured_us, bool repeated) noexcept {
    if (!state_ || state_->complete.load(std::memory_order_acquire)) return;
    auto& s = *state_;
    DWORD foreground_pid{};
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
    if (foreground_pid != s.game_pid) {
        s.foreground_since = 0;
        if (s.first_us) s.finish("game_left_foreground");
        return;
    }
    if (!s.foreground_since) s.foreground_since = published_us;
    if (published_us - s.foreground_since < 250'000U) return;
    if (!s.first_us) {
        bool sps{}, pps{}, idr{};
        for (std::size_t i = 0; i + 3 < au.size(); ++i) {
            if (au[i] == std::byte{0} && au[i+1] == std::byte{0} && au[i+2] == std::byte{1}) {
                const auto type = std::to_integer<unsigned>(au[i+3]) & 31U;
                sps |= type == 7; pps |= type == 8; idr |= type == 5;
            }
        }
        if (!(sps && pps && idr)) return;
        s.first_us = published_us;
        s.unix_start = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    if (published_us < s.first_us || published_us - s.first_us >= 60'000'000U) {
        s.finish("duration_limit"); return;
    }
    if (au.size() > s.data.size() - s.used || s.rows.size() == State::maximum_frames) {
        s.finish("memory_limit"); return;
    }
    std::memcpy(s.data.data() + s.used, au.data(), au.size());
    s.rows.push_back({frame_id, published_us, captured_us, s.used, au.size(), repeated});
    s.used += au.size();
}
}
