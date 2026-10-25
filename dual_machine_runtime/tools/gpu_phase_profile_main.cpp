#include "vfdual/dxgi_desktop_capture.hpp"
#include "vfdual/host_display_catalog.hpp"
#include "vfdual/nvenc_h264_encoder.hpp"
#include "vfdual/d3d11_frame_bridge.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string_view>
#include <vector>

// Diagnostic only: isolates texture readiness from NVENC completion. No input
// injection, phone connection, game setting, or system priority changes.
int main(int argc, char** argv) {
    if (argc != 4) return 2; // seconds, capture|encode|static|direct-readback|scaled-readback, output.csv
    const int seconds = std::atoi(argv[1]);
    const std::string_view mode(argv[2]);
    if (seconds < 1 || seconds > 60 ||
        (mode != "capture" && mode != "encode" && mode != "static" &&
         mode != "direct-readback" && mode != "scaled-readback")) return 2;
    const bool readback = mode == "direct-readback" || mode == "scaled-readback";
    const bool scaled = mode == "scaled-readback";
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const auto now = [&] {
        LARGE_INTEGER value{}; QueryPerformanceCounter(&value);
        return static_cast<std::uint64_t>(value.QuadPart * 1'000'000 / frequency.QuadPart);
    };
    const auto outputs = vfdual::enumerate_host_display_outputs();
    const auto* display = vfdual::select_preferred_display_output(outputs);
    if (!display) return 3;
    const auto geometry = vfdual::choose_centered_ai_capture_geometry(*display);
    vfdual::DxgiDesktopCapture capture;
    if (capture.initialize(display->adapter_index, display->output_index,
            {geometry.local_roi_x, geometry.local_roi_y,
             geometry.local_roi_width, geometry.local_roi_height}) !=
            vfdual::CaptureStatus::frame_ready) return 4;
    auto* device = capture.native_device();
    auto* context = capture.native_context();
    // Probe the lowest soft-realtime context class only when explicitly asked.
    // Lifetime is restricted to this short diagnostic process.
    if (std::getenv("VF_PROFILE_GPU_SOFT_REALTIME")) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
            const auto status = dxgi->SetGPUThreadPriority(0x40000010);
            int actual{}; dxgi->GetGPUThreadPriority(&actual);
            std::cerr << "gpu_soft_realtime_status=" << status << " actual=" << actual << '\n';
        }
    }
    Microsoft::WRL::ComPtr<ID3D11Query> query;
    D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT, 0};
    if (FAILED(device->CreateQuery(&query_desc, &query))) return 5;
    vfdual::NvencH264Encoder encoder;
    if (mode != "capture" && !readback && !encoder.initialize(device,
            {geometry.encoder_width, geometry.encoder_height,
             display->refresh_hz, display->refresh_hz})) return 6;
    vfdual::D3D11FrameBridge bridge;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (readback) {
        if (scaled && !bridge.initialize(device,context,geometry.encoder_width,
                geometry.encoder_height,display->refresh_hz,vfdual::FrameEncoderDevicePreference::capture_source)) return 11;
        D3D11_TEXTURE2D_DESC description{};
        description.Width=scaled?geometry.encoder_width:geometry.local_roi_width;
        description.Height=scaled?geometry.encoder_height:geometry.local_roi_height;
        description.MipLevels=1;description.ArraySize=1;description.SampleDesc.Count=1;
        description.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        description.Usage=D3D11_USAGE_STAGING;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&description,nullptr,&staging)))return 12;
    }
    struct Row { std::uint64_t capture_us, ready_us, encode_us, age_us, map_us, submit_us, wait_us; DWORD foreground; };
    std::vector<Row> rows; rows.reserve(seconds * 1500);
    std::map<DWORD, std::uint64_t> foreground_time;
    vfdual::CapturedDesktopFrame frame;
    const auto start = now();
    while (now() - start < seconds * 1'000'000ULL) {
        DWORD foreground{}; GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
        const auto before = now();
        if (mode != "static" || !frame.texture) {
            const auto status = capture.acquire_next(frame, 16);
            if (status == vfdual::CaptureStatus::timeout ||
                status == vfdual::CaptureStatus::pointer_only_skipped ||
                status == vfdual::CaptureStatus::outside_region_skipped) continue;
            if (status != vfdual::CaptureStatus::frame_ready) return 7;
        }
        const auto acquired = now();
        if (readback) {
            auto* texture=frame.texture;
            if(scaled)texture=bridge.bridge(texture).texture;
            if(!texture)return 13;
            context->CopyResource(staging.Get(),texture);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if(FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))return 14;
            context->Unmap(staging.Get(),0);
        } else if (mode != "static" || rows.empty()) {
            context->End(query.Get());
            context->Flush();
            HRESULT status{};
            while ((status = context->GetData(query.Get(), nullptr, 0,
                              D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE &&
                   now() - acquired < 2'000'000) Sleep(0);
            if (status != S_OK) return 8;
        }
        const auto ready = now();
        vfdual::NvencEncodeResult encoded{};
        if (mode != "capture" && !readback) {
            encoded = encoder.encode(frame.texture);
            if (!encoded.success) return 9;
        }
        const auto done = now();
        const auto presented = frame.last_present_qpc * 1'000'000 / frequency.QuadPart;
        rows.push_back({acquired - before, ready - acquired, done - ready,
                       done >= presented ? done - presented : 0,
                       encoded.map_resource_us, encoded.encode_picture_us,
                       encoded.lock_bitstream_us, foreground});
        foreground_time[foreground] += done - before;
    }
    const auto elapsed = (now() - start) / 1e6;
    std::ofstream out(argv[3]);
    out << "capture_us,texture_ready_wait_us,encode_us,age_us,map_us,submit_us,wait_us,foreground_pid\n";
    for (const auto& r : rows) out << r.capture_us << ',' << r.ready_us << ',' << r.encode_us << ',' << r.age_us << ',' << r.map_us << ',' << r.submit_us << ',' << r.wait_us << ',' << r.foreground << '\n';
    std::cout << "mode=" << mode << " seconds=" << elapsed << " samples=" << rows.size()
              << " fps=" << rows.size() / elapsed << '\n';
    for (const auto& [pid, us] : foreground_time)
        std::cout << "foreground_pid=" << pid << " seconds=" << us / 1e6 << '\n';
    return out.good() ? 0 : 10;
}
