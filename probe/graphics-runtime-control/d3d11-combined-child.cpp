#include <d3d11.h>
#include <wrl/client.h>
#ifdef PROBE_D3D11_NONBLOCKING_PRESENT
#include <d3d11_4.h>
#endif

#include "../../runtime/shared/child-process.h"

using Microsoft::WRL::ComPtr;

static LRESULT CALLBACK OnMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

#ifdef PROBE_D3D11_HOSTED_OUTPUT
static bool SameLuid(LUID left, LUID right) {
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

static unsigned int SelectOutput(ComPtr<IDXGIAdapter>& adapter, LUID& luid) {
    using CreateFactory = HRESULT (WINAPI *)(REFIID, void**);
    const HMODULE dxgi = LoadLibraryA("dxgi.dll");
    const auto createFactory = dxgi ? reinterpret_cast<CreateFactory>(
        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1"))) : nullptr;
    HMODULE implementation = nullptr;
    if (!createFactory || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(createFactory), &implementation) ||
            implementation != GetModuleHandleW(L"XboxWin32Dxgi.dll")) return 0x75100000u;
    ComPtr<IDXGIFactory1> factory1;
    ComPtr<IDXGIFactory> factory;
    if (FAILED(createFactory(IID_PPV_ARGS(&factory1))) || !factory1 || FAILED(factory1.As(&factory)) ||
            !factory) return 0x75110000u;
    ComPtr<IDXGIAdapter1> nativeAdapter;
    DXGI_ADAPTER_DESC1 nativeDescription;
    if (FAILED(factory1->EnumAdapters1(0, &nativeAdapter)) || !nativeAdapter ||
            FAILED(nativeAdapter->GetDesc1(&nativeDescription)) || (nativeDescription.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        return 0x75120000u;
    ComPtr<IDXGIOutput> nativeOutput;
    const HRESULT nativeResult = nativeAdapter->EnumOutputs(0, &nativeOutput);
    if (FAILED(nativeResult) && nativeResult != DXGI_ERROR_NOT_FOUND) return static_cast<unsigned int>(nativeResult);
    DXGI_ADAPTER_DESC adapterDescription;
    if (FAILED(factory->EnumAdapters(0, &adapter)) || !adapter || FAILED(adapter->GetDesc(&adapterDescription)) ||
            !SameLuid(adapterDescription.AdapterLuid, nativeDescription.AdapterLuid)) return 0x75130000u;
    luid = adapterDescription.AdapterLuid;

    ComPtr<IDXGIOutput> output;
    const HRESULT result = adapter->EnumOutputs(0, &output);
    if (FAILED(result)) return static_cast<unsigned int>(result);
    DXGI_OUTPUT_DESC description;
    if (!output || FAILED(output->GetDesc(&description))) return 0x75140000u;
    if (SUCCEEDED(nativeResult)) {
        DXGI_OUTPUT_DESC expected;
        if (!nativeOutput || FAILED(nativeOutput->GetDesc(&expected)) ||
                description.Monitor != expected.Monitor ||
                CompareStringOrdinal(description.DeviceName, -1, expected.DeviceName, -1, FALSE) != CSTR_EQUAL) {
            return 0x75150000u;
        }
        return 0;
    }

    MONITORINFOEXW info;
    info.cbSize = sizeof(info);
    const auto monitor = MonitorFromPoint({}, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info)) || description.Monitor != monitor ||
            !description.AttachedToDesktop || description.Rotation != DXGI_MODE_ROTATION_IDENTITY ||
            description.DesktopCoordinates.right != info.rcMonitor.right ||
            description.DesktopCoordinates.bottom != info.rcMonitor.bottom ||
            CompareStringOrdinal(description.DeviceName, -1, info.szDevice, -1, FALSE) != CSTR_EQUAL) return 0x75160000u;
    DEVMODEW settings;
    settings.dmSize = sizeof(settings);
    settings.dmDriverExtra = 0;
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &settings)) return 0x75170000u;

    DXGI_MODE_DESC requested{};
    requested.Width = settings.dmPelsWidth;
    requested.Height = settings.dmPelsHeight;
    requested.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_MODE_DESC matched{};
    if (FAILED(output->FindClosestMatchingMode(&requested, &matched, nullptr)) || matched.Width != requested.Width ||
            matched.Height != requested.Height || matched.Format != requested.Format ||
            matched.RefreshRate.Numerator != settings.dmDisplayFrequency || matched.RefreshRate.Denominator != 1) {
        return 0x75180000u;
    }
    UINT count = 0;
    if (FAILED(output->GetDisplayModeList(requested.Format, 0, &count, nullptr)) || count != 1) return 0x75190000u;
    count = 0;
    if (output->GetDisplayModeList(requested.Format, 0, &count, &matched) != DXGI_ERROR_MORE_DATA || count != 1 ||
            FAILED(output->GetDisplayModeList(requested.Format, 0, &count, &matched)) || count != 1 ||
            matched.Width != requested.Width || matched.Height != requested.Height) return 0x751a0000u;
    ComPtr<IDXGIAdapter> parent;
    ComPtr<IDXGIOutput> extra;
    if (FAILED(output->GetParent(IID_PPV_ARGS(&parent))) || parent.Get() != adapter.Get() ||
            adapter->EnumOutputs(1, &extra) != nativeAdapter->EnumOutputs(1, nativeOutput.ReleaseAndGetAddressOf()) ||
            output->WaitForVBlank() != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) return 0x751b0000u;
    return 0;
}
#endif

#ifdef PROBE_D3D11_NONBLOCKING_PRESENT
struct ResizeRequest {
    IDXGISwapChain* chain;
    HRESULT result = E_PENDING;
    volatile LONG started = 0;
    volatile LONG completed = 0;
};

static void CALLBACK ResizeOnWorker(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) {
    auto& request = *static_cast<ResizeRequest*>(context);
    InterlockedExchange(&request.started, 1);
    request.result = request.chain->ResizeBuffers(0, 80, 80, DXGI_FORMAT_UNKNOWN, 0);
    InterlockedExchange(&request.completed, 1);
}

static void CALLBACK EndStalledControl(PTP_CALLBACK_INSTANCE, void*, PTP_TIMER) {
    TerminateProcess(GetCurrentProcess(), 0x752d0000u);
}

static unsigned int CheckBusyPresents(ResizeRequest& request, IDXGISwapChain1* chain1, PTP_WORK work) {
    constexpr UINT flags = DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT;
    const DXGI_PRESENT_PARAMETERS parameters{};
    const HRESULT idle = request.chain->Present(0, flags);
    if (FAILED(idle)) return 0x75230000u | (static_cast<UINT>(idle) & 0xffffu);
    const HRESULT idle1 = chain1->Present1(0, flags, &parameters);
    if (FAILED(idle1)) return 0x75240000u | (static_cast<UINT>(idle1) & 0xffffu);

    // The caller owns the device lock. The worker acquires the hosted lock
    // and then waits below it in native ResizeBuffers.
    SubmitThreadpoolWork(work);
    const auto deadline = GetTickCount64() + 1000;
    while (GetTickCount64() < deadline) {
        if (InterlockedCompareExchange(&request.completed, 0, 0)) return 0x75250000u;
        if (InterlockedCompareExchange(&request.started, 0, 0)) {
            const HRESULT result = request.chain->Present(0, flags);
            if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
                if (request.chain->Present(0, DXGI_PRESENT_DO_NOT_WAIT) != DXGI_ERROR_WAS_STILL_DRAWING)
                    return 0x752c0000u;
                if (chain1->Present1(0, flags, &parameters) != DXGI_ERROR_WAS_STILL_DRAWING ||
                        InterlockedCompareExchange(&request.completed, 0, 0)) return 0x75260000u;
                return 0;
            }
            if (FAILED(result)) return 0x75270000u | (static_cast<UINT>(result) & 0xffffu);
        }
        SleepEx(1, FALSE);
    }
    return 0x75280000u;
}

static unsigned int CheckResizeRecovery(const ResizeRequest& request, IDXGISwapChain1* chain) {
    if (request.result != S_OK) return 0x75290000u | (static_cast<UINT>(request.result) & 0xffffu);
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(chain->GetDesc(&description)) || description.BufferDesc.Width != 80 ||
            description.BufferDesc.Height != 80) return 0x752a0000u;
    const DXGI_PRESENT_PARAMETERS parameters{};
    const HRESULT result = chain->Present1(0, DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_WAIT, &parameters);
    return FAILED(result) ? 0x752b0000u | (static_cast<UINT>(result) & 0xffffu) : 0;
}

static unsigned int VerifyNonblockingPresent(IDXGISwapChain* chain, ID3D11DeviceContext* context) {
    ComPtr<IDXGISwapChain1> chain1;
    ComPtr<ID3D11Multithread> multithread;
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain1))) || !chain1 ||
            FAILED(context->QueryInterface(IID_PPV_ARGS(&multithread))) || !multithread) return 0x75200000u;
    ResizeRequest request{chain};
    const PTP_WORK work = CreateThreadpoolWork(ResizeOnWorker, &request, nullptr);
    if (!work) return 0x75220000u | (GetLastError() & 0xffffu);
    const BOOL previous = multithread->SetMultithreadProtected(TRUE);
    if (!multithread->GetMultithreadProtected()) {
        multithread->SetMultithreadProtected(previous);
        CloseThreadpoolWork(work);
        return 0x75210000u;
    }
    const PTP_TIMER watchdog = CreateThreadpoolTimer(EndStalledControl, nullptr, nullptr);
    if (!watchdog) {
        const auto error = GetLastError();
        multithread->SetMultithreadProtected(previous);
        CloseThreadpoolWork(work);
        return 0x752e0000u | (error & 0xffffu);
    }
    constexpr ULONGLONG due = static_cast<ULONGLONG>(-50'000'000LL);
    FILETIME timeout{static_cast<DWORD>(due), static_cast<DWORD>(due >> 32)};
    SetThreadpoolTimer(watchdog, &timeout, 0, 0);
    multithread->Enter();
    auto error = CheckBusyPresents(request, chain1.Get(), work);
    multithread->Leave();
    WaitForThreadpoolWorkCallbacks(work, FALSE);
    CloseThreadpoolWork(work);
    multithread->SetMultithreadProtected(previous);
    if (!error) error = CheckResizeRecovery(request, chain1.Get());
    SetThreadpoolTimer(watchdog, nullptr, 0, 0);
    WaitForThreadpoolTimerCallbacks(watchdog, TRUE);
    CloseThreadpoolTimer(watchdog);
    return error;
}
#endif

static unsigned int TryCombinedCall() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const WNDCLASSW windowClass{0, OnMessage, 0, 0, instance, nullptr, nullptr, nullptr, nullptr,
        L"D3D11CombinedControl"};
    if (!RegisterClassW(&windowClass)) return 0x73700000u | (GetLastError() & 0xffffu);
    const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"D3D11 combined control", WS_POPUP,
        0, 0, 64, 64, nullptr, nullptr, instance, nullptr);
    if (!window) return 0x73710000u | (GetLastError() & 0xffffu);
    const HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    if (!d3d11) return 0x73720000u | (GetLastError() & 0xffffu);
#ifdef PROBE_D3D11_HOSTED
    if (!SetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_DXGI", L"1"))
        return 0x73790000u | (GetLastError() & 0xffffu);
#endif
    const auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
        reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")));
    if (!create) return 0x73730000u | (GetLastError() & 0xffffu);
#ifdef PROBE_D3D11_HOSTED
    HMODULE implementation = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(create), &implementation) ||
            implementation != GetModuleHandleW(L"XboxWin32Dxgi.dll")) return 0x737a0000u;
#endif

    ComPtr<IDXGIAdapter> adapter;
    D3D_DRIVER_TYPE driver = D3D_DRIVER_TYPE_HARDWARE;
#ifdef PROBE_D3D11_HOSTED_OUTPUT
    if (!SetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_DXGI_OUTPUT", L"1"))
        return 0x751c0000u | (GetLastError() & 0xffffu);
    LUID selected{};
    if (const auto error = SelectOutput(adapter, selected)) return error;
    driver = D3D_DRIVER_TYPE_UNKNOWN;
#endif
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = 64;
    description.BufferDesc.Height = 64;
    description.BufferDesc.RefreshRate.Denominator = 1;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 1;
    description.OutputWindow = window;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    ComPtr<IDXGISwapChain> chain;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT result = create(adapter.Get(), driver, nullptr, 0, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, &description, &chain, &device, nullptr, &context);
    if (result == E_INVALIDARG) {
        result = create(adapter.Get(), driver, nullptr, 0, levels + 1, ARRAYSIZE(levels) - 1,
            D3D11_SDK_VERSION, &description, chain.ReleaseAndGetAddressOf(), device.ReleaseAndGetAddressOf(),
            nullptr, context.ReleaseAndGetAddressOf());
    }
    // Preserve the native HRESULT so the console can distinguish a bad HWND
    // from an unavailable adapter or feature level.
    if (FAILED(result)) return static_cast<unsigned int>(result);
    if (!chain || !device || !context || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_10_0) return 0x73740000u;
#ifdef PROBE_D3D11_HOSTED_OUTPUT
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> createdAdapter;
    DXGI_ADAPTER_DESC createdDescription;
    if (FAILED(device.As(&dxgiDevice)) || !dxgiDevice || FAILED(dxgiDevice->GetAdapter(&createdAdapter)) || !createdAdapter ||
            FAILED(createdAdapter->GetDesc(&createdDescription)) ||
            !SameLuid(selected, createdDescription.AdapterLuid)) return 0x751d0000u;
#endif
    ComPtr<IDXGIFactory> factory;
    if (FAILED(chain->GetParent(IID_PPV_ARGS(&factory)))) return 0x73750000u;
    if (FAILED(factory->MakeWindowAssociation(window, DXGI_MWA_NO_WINDOW_CHANGES))) return 0x73760000u;
    ComPtr<ID3D11Texture2D> buffer;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return 0x73770000u;
    D3D11_TEXTURE2D_DESC bufferDescription{};
    buffer->GetDesc(&bufferDescription);
    if (bufferDescription.Width != 64 || bufferDescription.Height != 64 ||
            bufferDescription.Format != description.BufferDesc.Format) return 0x73780000u;
#ifdef PROBE_D3D11_HOSTED
    HWND associated = nullptr;
    if (FAILED(factory->GetWindowAssociation(&associated)) || associated != window) return 0x737b0000u;
    auto invalid = description;
    invalid.SampleDesc.Count = 0;
    auto failedChain = reinterpret_cast<IDXGISwapChain*>(1);
    auto failedDevice = reinterpret_cast<ID3D11Device*>(1);
    auto failedContext = reinterpret_cast<ID3D11DeviceContext*>(1);
    D3D_FEATURE_LEVEL failedLevel = D3D_FEATURE_LEVEL_11_0;
    const HRESULT invalidResult = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels + 1,
        ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION, &invalid, &failedChain, &failedDevice,
        &failedLevel, &failedContext);
    if (invalidResult != DXGI_ERROR_INVALID_CALL) return 0x737c0000u | (static_cast<UINT>(invalidResult) & 0xffffu);
    if (failedChain || failedDevice || failedContext || failedLevel) return 0x737d0000u;
#endif
#ifdef PROBE_D3D11_NONBLOCKING_PRESENT
    buffer.Reset();
    if (const auto error = VerifyNonblockingPresent(chain.Get(), context.Get())) return error;
#endif
    return PROBE_EXPECTED_CHILD_EXIT;
}

extern "C" void WINAPI ChildEntry() {
    ExitProcess(TryCombinedCall());
}
