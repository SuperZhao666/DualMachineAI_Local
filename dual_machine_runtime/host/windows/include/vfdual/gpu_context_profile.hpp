#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cwchar>

namespace vfdual {

// Only this bounded inference-copy context receives scheduling priority. Never
// raise a game, the whole process, or a full-desktop rendering context. Class
// 16 is the lowest yielding soft-realtime level, not hard-realtime (30).
inline void configure_inference_capture_scheduling(ID3D11Device* device,
                                                   UINT width, UINT height,
                                                   UINT maximum_edge = 1024) noexcept {
    // The production detector copies a 640-square physical desktop region
    // directly to its 640-square video/model input. The 1024 capacity remains
    // available to bounded non-production capture/profiling callers.
    if (!device || maximum_edge > 1024 || width == 0 || height == 0 ||
        width > maximum_edge || height > maximum_edge) return;
    wchar_t override_value[16]{};
    if (GetEnvironmentVariableW(L"VF_HOST_GPU_SCHEDULING", override_value, 16) != 0 &&
        std::wcscmp(override_value, L"normal") == 0) return;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    HRESULT status = device->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(status)) status = dxgi->GetAdapter(&adapter);
    if (SUCCEEDED(status)) status = adapter->GetDesc(&description);
    if (FAILED(status) || description.VendorId != 0x10DEU) return;
    status = dxgi->SetGPUThreadPriority(0x40000010);
    // Unsupported/denied requests leave Windows' normal scheduling intact.
    std::fprintf(stderr, "inference_capture_latency_scheduling_status=0x%lx extent=%ux%u\n",
                 static_cast<unsigned long>(status), width, height);
}

// Opt-in experiment for the capture profiler. No system scheduling settings
// or other applications are changed; the setting belongs to this D3D device.
inline void apply_gpu_context_profile(ID3D11Device* device) noexcept {
    wchar_t scheduling[16]{};
    if (device && GetEnvironmentVariableW(L"VF_HOST_GPU_SCHEDULING", scheduling, 16) != 0 &&
        std::wcscmp(scheduling, L"latency") == 0) {
        // Windows 10+: lowest soft-realtime class. This small capture/convert
        // context periodically yields; never request non-yielding hard realtime.
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        HRESULT status = device->QueryInterface(IID_PPV_ARGS(&dxgi));
        if (SUCCEEDED(status)) status = dxgi->SetGPUThreadPriority(0x40000010);
        std::fprintf(stderr, "gpu_latency_scheduling_status=0x%lx\n", static_cast<unsigned long>(status));
        return;
    }
    wchar_t value[16]{};
    if (!device || GetEnvironmentVariableW(L"VF_HOST_GPU_PRIORITY", value, 16) == 0) return;
    wchar_t* end{};
    const long requested = std::wcstol(value, &end, 10);
    if (*end != L'\0' || requested < -7 || requested > 7) return;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    HRESULT status = device->QueryInterface(IID_PPV_ARGS(&dxgi));
    int applied{};
    if (SUCCEEDED(status)) status = dxgi->SetGPUThreadPriority(static_cast<int>(requested));
    if (SUCCEEDED(status)) status = dxgi->GetGPUThreadPriority(&applied);
    std::fprintf(stderr, "gpu_context_priority=%d status=0x%lx\n", applied, static_cast<unsigned long>(status));
}

}  // namespace vfdual
