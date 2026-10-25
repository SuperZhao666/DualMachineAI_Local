#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {
struct Stamp { unsigned long long unix_us{}, qpc_us{}; };
struct Sample { unsigned long long unix_us, elapsed_us, qpc_us; unsigned device; LONG dx, dy; USHORT flags; };
struct Failure { const char* stage{}; DWORD error{}; };
struct DesktopContext {
  const char* thread_name{"unknown"}; const char* input_name{"unknown"};
  const char* window_station{"unknown"}; bool match_known{}, matches{};
};
struct Health {
  unsigned long long wm_input{}, read_calls{}, read_failures{}, invalid_sizes{};
  unsigned long long mouse_reports{}, mouse_nonzero{}, mouse_zero{}, mouse_absolute{}, other_types{};
  UINT returned_min{~UINT{0}}, returned_max{}, reported_size_max{};
  DWORD last_read_error{}; bool registered{}, sink_start{}, sink_end{}, timer_fired{}, limit_reached{}, loop_error{};
  UINT sink_count_start{}, sink_count_end{}; unsigned mouse_count_start{}, mouse_count_end{};
  bool mouse_count_start_known{}, mouse_count_end_known{}, session_known{}; DWORD session{};
  int exit_code{}; std::array<Failure, 16> failures{}; unsigned failure_count{}, lost_failures{};
} health;
std::vector<Sample> samples;
std::map<HANDLE, unsigned> observed_devices;
std::chrono::steady_clock::time_point started;
unsigned long long qpc_frequency{};
constexpr std::size_t limit = 250'000;

void fail(const char* stage, DWORD error, int code = 9) {
  if (!health.exit_code) health.exit_code = code;
  if (health.failure_count < health.failures.size()) health.failures[health.failure_count++] = {stage, error};
  else ++health.lost_failures;
}
Stamp stamp() {
  FILETIME filetime{}; GetSystemTimePreciseAsFileTime(&filetime);
  LARGE_INTEGER counter{};
  if (!QueryPerformanceCounter(&counter)) { fail("qpc_read", GetLastError()); return {}; }
  const auto ticks = static_cast<unsigned long long>(counter.QuadPart);
  ULARGE_INTEGER time{}; time.LowPart = filetime.dwLowDateTime; time.HighPart = filetime.dwHighDateTime;
  return {(time.QuadPart - 116444736000000000ULL) / 10,
          (ticks / qpc_frequency) * 1'000'000ULL + (ticks % qpc_frequency) * 1'000'000ULL / qpc_frequency};
}
bool object_name(HANDLE handle, std::wstring& name, const char* stage) {
  DWORD bytes{};
  SetLastError(ERROR_SUCCESS);
  const BOOL queried = GetUserObjectInformationW(handle, UOI_NAME, nullptr, 0, &bytes);
  const DWORD query_error = GetLastError();
  if ((!queried && query_error != ERROR_INSUFFICIENT_BUFFER) || !bytes || bytes > 4'096 || bytes % sizeof(wchar_t)) {
    fail(stage, query_error); return false;
  }
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1U, L'\0');
  if (!GetUserObjectInformationW(handle, UOI_NAME, buffer.data(), bytes, &bytes)) {
    fail(stage, GetLastError()); return false;
  }
  name.assign(buffer.data()); return true;
}
DesktopContext desktop_context() {
  DesktopContext context;
  std::wstring thread, input, station;
  const auto thread_desktop = GetThreadDesktop(GetCurrentThreadId());
  const bool thread_ok = thread_desktop && object_name(thread_desktop, thread, "thread_desktop_name");
  if (!thread_desktop) fail("get_thread_desktop", GetLastError());
  if (thread_ok) context.thread_name = _wcsicmp(thread.c_str(), L"Default") == 0 ? "Default" : "other";
  const auto input_desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  bool input_ok{};
  if (!input_desktop) fail("open_input_desktop", GetLastError());
  else {
    input_ok = object_name(input_desktop, input, "input_desktop_name");
    if (input_ok) context.input_name = _wcsicmp(input.c_str(), L"Default") == 0 ? "Default" : "other";
    if (!CloseDesktop(input_desktop)) fail("close_input_desktop", GetLastError());
  }
  const auto window_station = GetProcessWindowStation();
  if (!window_station) fail("get_window_station", GetLastError());
  else if (object_name(window_station, station, "window_station_name"))
    context.window_station = _wcsicmp(station.c_str(), L"WinSta0") == 0 ? "WinSta0" : "other";
  context.match_known = thread_ok && input_ok;
  context.matches = context.match_known && _wcsicmp(thread.c_str(), input.c_str()) == 0;
  return context;
}
bool is_input_desktop(const DesktopContext& context) {
  return context.match_known && context.matches &&
      std::string_view(context.window_station) == "WinSta0";
}
bool count_system_mice(unsigned& result) {
  UINT count{};
  if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == UINT(-1)) {
    fail("mouse_list_size", GetLastError()); return false;
  }
  if (count > 65'536U) { fail("mouse_list_bound", ERROR_INVALID_DATA); return false; }
  std::vector<RAWINPUTDEVICELIST> devices(count);
  if (count) {
    const UINT read = GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (read == UINT(-1)) { fail("mouse_list_read", GetLastError()); return false; }
    if (read > devices.size()) { fail("mouse_list_return_bound", ERROR_INVALID_DATA); return false; }
    result = static_cast<unsigned>(std::count_if(devices.begin(), devices.begin() + read,
        [](const auto& device) { return device.dwType == RIM_TYPEMOUSE; }));
  }
  return true;
}
bool verify_sink(HWND window, UINT& mouse_registrations) {
  UINT count{};
  if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == UINT(-1)) {
    fail("sink_size", GetLastError()); return false;
  }
  if (count > 65'536U) { fail("sink_count_bound", ERROR_INVALID_DATA); return false; }
  std::vector<RAWINPUTDEVICE> registrations(count);
  const UINT read = count ? GetRegisteredRawInputDevices(registrations.data(), &count, sizeof(RAWINPUTDEVICE)) : 0;
  if (read == UINT(-1)) { fail("sink_read", GetLastError()); return false; }
  if (read > registrations.size()) { fail("sink_return_bound", ERROR_INVALID_DATA); return false; }
  bool matched{};
  for (UINT i = 0; i < read; ++i) {
    const auto& device = registrations[i];
    if (device.usUsagePage != 1 || device.usUsage != 2) continue;
    ++mouse_registrations;
    matched |= device.hwndTarget == window && (device.dwFlags & RIDEV_INPUTSINK) != 0
        && (device.dwFlags & RIDEV_REMOVE) == 0;
  }
  if (!matched) fail("mouse_sink_not_registered", ERROR_INVALID_DATA);
  return matched;
}
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_INPUT) {
    ++health.wm_input;
    const auto arrival = stamp();
    RAWINPUT input{}; UINT bytes = sizeof(input);
    ++health.read_calls;
    const UINT returned = GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT,
                                          &input, &bytes, sizeof(RAWINPUTHEADER));
    const DWORD read_error = returned == UINT(-1) ? GetLastError() : ERROR_SUCCESS;
    health.reported_size_max = std::max(health.reported_size_max, bytes);
    if (returned == UINT(-1)) {
      ++health.read_failures; health.last_read_error = read_error; fail("raw_read", read_error);
    } else {
      health.returned_min = std::min(health.returned_min, returned);
      health.returned_max = std::max(health.returned_max, returned);
      if (returned < sizeof(RAWINPUTHEADER) || returned > sizeof(input)
          || input.header.dwSize < sizeof(RAWINPUTHEADER) || input.header.dwSize > returned
          || (input.header.dwType == RIM_TYPEMOUSE && returned < offsetof(RAWINPUT, data) + sizeof(RAWMOUSE))) {
        ++health.invalid_sizes; fail("raw_size", ERROR_INVALID_DATA);
      } else if (input.header.dwType != RIM_TYPEMOUSE) ++health.other_types;
      else {
        ++health.mouse_reports;
        if (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) ++health.mouse_absolute;
        if (!input.data.mouse.lLastX && !input.data.mouse.lLastY) ++health.mouse_zero;
        else {
          ++health.mouse_nonzero;
          auto found = observed_devices.find(input.header.hDevice);
          if (found == observed_devices.end())
            found = observed_devices.emplace(input.header.hDevice, unsigned(observed_devices.size() + 1)).first;
          const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - started).count();
          samples.push_back({arrival.unix_us, static_cast<unsigned long long>(elapsed), arrival.qpc_us,
              found->second, input.data.mouse.lLastX, input.data.mouse.lLastY, input.data.mouse.usFlags});
          if (samples.size() >= limit) { health.limit_reached = true; fail("sample_limit", 0, 10); PostQuitMessage(0); }
        }
      }
    }
    return DefWindowProcW(window, message, wparam, lparam);
  }
  if (message == WM_TIMER && wparam == 1) { health.timer_fired = true; PostQuitMessage(0); return 0; }
  return DefWindowProcW(window, message, wparam, lparam);
}
void print_context(const DesktopContext& context) {
  std::cout << "{\"thread_desktop\":\"" << context.thread_name << "\",\"input_desktop\":\"" << context.input_name
            << "\",\"window_station\":\"" << context.window_station << "\",\"desktop_match_known\":"
            << context.match_known << ",\"desktop_matches_input\":" << context.matches << '}';
}
}
int wmain(int argc, wchar_t** argv) {
  if (argc != 3) { std::fputs("Usage: VisionForgeRawMouseProbe OUTPUT.csv SECONDS\n", stderr); return 2; }
  const int seconds = _wtoi(argv[2]);
  if (seconds < 1 || seconds > 120) return 2;
  LARGE_INTEGER frequency{};
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return 3;
  qpc_frequency = static_cast<unsigned long long>(frequency.QuadPart);
  samples.reserve(limit);
  health.session_known = ProcessIdToSessionId(GetCurrentProcessId(), &health.session) != FALSE;
  if (!health.session_known) fail("process_session", GetLastError());
  const auto context_start = desktop_context();
  if (!is_input_desktop(context_start) && !health.exit_code)
    fail("inactive_input_desktop_start", ERROR_INVALID_STATE, 13);
  WNDCLASSW type{}; type.lpfnWndProc = window_proc; type.hInstance = GetModuleHandleW(nullptr);
  type.lpszClassName = L"VFReadOnlyMouseProbe";
  if (!RegisterClassW(&type)) fail("register_class", GetLastError(), 3);
  HWND window = health.exit_code ? nullptr : CreateWindowExW(0, type.lpszClassName, L"", 0,
      0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, nullptr);
  if (!window && !health.exit_code) fail("create_window", GetLastError(), 4);
  RAWINPUTDEVICE mouse{1, 2, RIDEV_INPUTSINK, window};
  if (window) {
    health.registered = RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) != FALSE;
    if (!health.registered) fail("register_mouse", GetLastError(), 5);
  }
  if (health.registered) health.sink_start = verify_sink(window, health.sink_count_start);
  health.mouse_count_start_known = count_system_mice(health.mouse_count_start);
  Stamp begin{}, end{};
  if (health.registered && health.sink_start && !health.exit_code) {
    started = std::chrono::steady_clock::now(); begin = stamp();
    if (!SetTimer(window, 1, UINT(seconds * 1000), nullptr)) fail("set_timer", GetLastError(), 6);
    else {
      MSG message{}; int result{};
      while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
      if (result == -1) { health.loop_error = true; fail("get_message", GetLastError(), 11); }
      if (!health.timer_fired && !health.limit_reached && !health.loop_error) fail("unexpected_quit", 0, 12);
      if (!KillTimer(window, 1)) fail("kill_timer", GetLastError());
    }
    end = stamp();
  }
  if (health.registered) health.sink_end = verify_sink(window, health.sink_count_end);
  health.mouse_count_end_known = count_system_mice(health.mouse_count_end);
  const auto context_end = desktop_context();
  if (!is_input_desktop(context_end) && !health.exit_code)
    fail("inactive_input_desktop_end", ERROR_INVALID_STATE, 13);
  if (health.registered) {
    mouse.dwFlags = RIDEV_REMOVE; mouse.hwndTarget = nullptr;
    if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) fail("unregister_mouse", GetLastError());
  }
  if (window && !DestroyWindow(window)) fail("destroy_window", GetLastError());
  std::ofstream output(argv[1]);
  if (!output) fail("csv_open", 0, 7);
  else {
    // Keep the seven-column contract. Times denote this observer's WM_INPUT
    // handler observation, not a USB hardware timestamp or game consumption.
    output << "unix_us,elapsed_us,device_id,dx,dy,flags,qpc_us\n";
    for (const auto& sample : samples)
      output << sample.unix_us << ',' << sample.elapsed_us << ',' << sample.device << ',' << sample.dx
             << ',' << sample.dy << ',' << sample.flags << ',' << sample.qpc_us << '\n';
    output.close(); if (!output) fail("csv_write", 0, 8);
  }
  const auto elapsed = begin.qpc_us && end.qpc_us >= begin.qpc_us ? end.qpc_us - begin.qpc_us : 0;
  const bool observation_valid = !health.exit_code && health.timer_fired &&
      health.sink_start && health.sink_end && is_input_desktop(context_start) &&
      is_input_desktop(context_end) && begin.qpc_us && elapsed;
  std::cout << std::boolalpha << "{\"schema\":2,\"samples\":" << samples.size() << ",\"devices\":" << observed_devices.size()
      << ",\"requested_seconds\":" << seconds << ",\"scope\":\"read_only_mouse_handler_arrivals_no_keyboard_or_device_identity\""
      << ",\"zero_observed_does_not_mean_no_system_hid\":true,\"observation_context_valid\":" << observation_valid
      << ",\"wm_input_messages\":" << health.wm_input
      << ",\"raw_read_calls\":" << health.read_calls << ",\"raw_read_failures\":" << health.read_failures
      << ",\"raw_read_last_error\":" << health.last_read_error << ",\"raw_invalid_sizes\":" << health.invalid_sizes
      << ",\"raw_returned_bytes_min\":" << (health.returned_min == ~UINT{0} ? 0 : health.returned_min)
      << ",\"raw_returned_bytes_max\":" << health.returned_max << ",\"raw_reported_size_max\":" << health.reported_size_max
      << ",\"mouse_reports\":" << health.mouse_reports << ",\"mouse_nonzero_reports\":" << health.mouse_nonzero
      << ",\"mouse_zero_delta_reports\":" << health.mouse_zero << ",\"mouse_absolute_reports\":" << health.mouse_absolute
      << ",\"other_raw_types\":" << health.other_types << ",\"registration_succeeded\":" << health.registered
      << ",\"mouse_sink_verified_start\":" << health.sink_start << ",\"mouse_sink_verified_end\":" << health.sink_end
      << ",\"mouse_registration_count_start\":" << health.sink_count_start << ",\"mouse_registration_count_end\":" << health.sink_count_end
      << ",\"system_mouse_count_start_known\":" << health.mouse_count_start_known << ",\"system_mouse_count_start\":" << health.mouse_count_start
      << ",\"system_mouse_count_end_known\":" << health.mouse_count_end_known << ",\"system_mouse_count_end\":" << health.mouse_count_end
      << ",\"session_id_known\":" << health.session_known << ",\"session_id\":" << health.session
      << ",\"desktop_start\":"; print_context(context_start); std::cout << ",\"desktop_end\":"; print_context(context_end);
  std::cout << ",\"capture_start_unix_us\":" << begin.unix_us << ",\"capture_end_unix_us\":" << end.unix_us
      << ",\"capture_start_qpc_us\":" << begin.qpc_us << ",\"capture_end_qpc_us\":" << end.qpc_us
      << ",\"actual_elapsed_us\":" << elapsed << ",\"timer_fired\":" << health.timer_fired
      << ",\"sample_limit_reached\":" << health.limit_reached << ",\"message_loop_error\":" << health.loop_error
      << ",\"exit_code\":" << health.exit_code << ",\"diagnostic_failures_lost\":" << health.lost_failures << ",\"diagnostic_failures\":[";
  for (unsigned i = 0; i < health.failure_count; ++i) {
    if (i) std::cout << ',';
    std::cout << "{\"stage\":\"" << health.failures[i].stage << "\",\"win32_error\":" << health.failures[i].error << '}';
  }
  std::cout << "]}\n";
  return health.exit_code;
}
