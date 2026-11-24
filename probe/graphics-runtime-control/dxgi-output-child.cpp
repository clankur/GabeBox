#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "../../runtime/shared/child-process.h"

using Microsoft::WRL::ComPtr;
using MonitorInfo = BOOL (WINAPI *)(HMONITOR, LPMONITORINFO);
using DisplaySettings = BOOL (WINAPI *)(LPCSTR, DWORD, DEVMODEA*);

static unsigned Failure(unsigned stage, HRESULT error) {
    return 0x75000000u | (stage << 16) | (static_cast<unsigned>(error) & 0xffffu);
}

static HRESULT ReadCurrentMode(MonitorInfo monitorInfo, DisplaySettings displaySettings,
        HMONITOR monitor, DEVMODEA& mode, unsigned& stage) {
    stage = 5;
    MONITORINFOEXA info{};
    info.cbSize = sizeof(info);
    SetLastError(ERROR_SUCCESS);
    if (!monitorInfo(monitor, &info)) {
        const DWORD error = GetLastError();
        return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
    }
    stage = 6;
    mode.dmSize = sizeof(mode);
    SetLastError(ERROR_SUCCESS);
    if (!displaySettings(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
        const DWORD error = GetLastError();
        return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
    }
    return mode.dmPelsWidth && mode.dmPelsHeight ? S_OK : E_UNEXPECTED;
}

#ifdef PROBE_DXGI_OUTPUT_HOSTED
static unsigned CheckHostedMode() {
    if (!SetEnvironmentVariableW(L"XBOX_SDL_RENDER_WIDTH", L"1280") ||
            !SetEnvironmentVariableW(L"XBOX_SDL_RENDER_HEIGHT", L"720"))
        return Failure(10, HRESULT_FROM_WIN32(GetLastError()));
    HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    DEVMODEA ansi{};
    unsigned stage = 0;
    HRESULT result = ReadCurrentMode(GetMonitorInfoA, EnumDisplaySettingsA, monitor, ansi, stage);
    if (FAILED(result)) return Failure(stage + 8, result);
    DEVMODEW wide{};
    wide.dmSize = sizeof(wide);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &wide) ||
            ansi.dmPelsWidth != 1280 || ansi.dmPelsHeight != 720 || ansi.dmDisplayFrequency != 60 ||
            ansi.dmPelsWidth != wide.dmPelsWidth || ansi.dmPelsHeight != wide.dmPelsHeight ||
            ansi.dmFields != wide.dmFields || ansi.dmDisplayFrequency != wide.dmDisplayFrequency)
        return Failure(11, E_UNEXPECTED);
    struct {
        DEVMODEA mode;
        DWORD guard;
    } guarded{};
    guarded.mode.dmSize = sizeof(guarded.mode);
    guarded.mode.dmDriverExtra = sizeof(guarded.guard);
    guarded.guard = 0x10203040;
    if (!EnumDisplaySettingsA("\\\\.\\display1", ENUM_CURRENT_SETTINGS, &guarded.mode) ||
            guarded.guard != 0x10203040 || guarded.mode.dmDriverExtra || guarded.mode.dmPelsWidth != 1280 ||
            EnumDisplaySettingsA("unrelated-display", ENUM_CURRENT_SETTINGS, &guarded.mode) ||
            GetLastError() != ERROR_INVALID_PARAMETER)
        return Failure(12, E_UNEXPECTED);
    if (!SetEnvironmentVariableW(L"XBOX_SDL_RENDER_WIDTH", nullptr) ||
            !SetEnvironmentVariableW(L"XBOX_SDL_RENDER_HEIGHT", nullptr))
        return Failure(10, HRESULT_FROM_WIN32(GetLastError()));
    return 0;
}
#endif

static unsigned RunOutputProbe() {
#ifdef PROBE_DXGI_OUTPUT_HOSTED
    if (const unsigned result = CheckHostedMode()) return result;
    // The window shim uses this component to verify a native HMONITOR.
    if (!LoadPackagedLibrary(L"XboxWin32Dxgi.dll", 0)) return Failure(15, HRESULT_FROM_WIN32(GetLastError()));
    const MonitorInfo monitorInfo = GetMonitorInfoA;
    const DisplaySettings displaySettings = EnumDisplaySettingsA;
#endif
    ComPtr<IDXGIFactory1> factory1;
    ComPtr<IDXGIFactory> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory1));
    if (SUCCEEDED(result)) result = factory1 ? factory1.As(&factory) : E_UNEXPECTED;
    if (FAILED(result) || !factory) return Failure(1, FAILED(result) ? result : E_UNEXPECTED);
    ComPtr<IDXGIAdapter> adapter;
    result = factory->EnumAdapters(0, &adapter);
    if (FAILED(result) || !adapter) return Failure(2, FAILED(result) ? result : E_UNEXPECTED);
    ComPtr<IDXGIOutput> output;
    result = adapter->EnumOutputs(0, &output);
    if (FAILED(result)) return static_cast<unsigned>(result);
    DXGI_OUTPUT_DESC description{};
    result = output ? output->GetDesc(&description) : E_UNEXPECTED;
    if (FAILED(result)) return Failure(3, result);
#ifndef PROBE_DXGI_OUTPUT_HOSTED
    const HMODULE user32 = LoadLibraryExW(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!user32) return Failure(4, HRESULT_FROM_WIN32(GetLastError()));
    const auto monitorInfo = reinterpret_cast<MonitorInfo>(
        reinterpret_cast<void*>(GetProcAddress(user32, "GetMonitorInfoA")));
    const auto displaySettings = reinterpret_cast<DisplaySettings>(
        reinterpret_cast<void*>(GetProcAddress(user32, "EnumDisplaySettingsA")));
    if (!monitorInfo || !displaySettings) return Failure(4, HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
#endif
    DEVMODEA mode{};
    unsigned stage = 0;
    result = ReadCurrentMode(monitorInfo, displaySettings, description.Monitor, mode, stage);
    if (FAILED(result)) return Failure(stage, result);
    DXGI_MODE_DESC requested{}, matched{};
    requested.Width = mode.dmPelsWidth;
    requested.Height = mode.dmPelsHeight;
    if (mode.dmDisplayFrequency >= 2) requested.RefreshRate = {mode.dmDisplayFrequency, 1};
    requested.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    result = output->FindClosestMatchingMode(&requested, &matched, nullptr);
    if (FAILED(result)) return Failure(7, result);
    if (!matched.RefreshRate.Denominator) return Failure(8, E_UNEXPECTED);
    return PROBE_EXPECTED_CHILD_EXIT;
}

extern "C" void WINAPI ChildEntry() {
    ExitProcess(RunOutputProbe());
}
