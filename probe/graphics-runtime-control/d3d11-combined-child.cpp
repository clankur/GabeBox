#include <d3d11.h>
#include <wrl/client.h>

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
    return PROBE_EXPECTED_CHILD_EXIT;
}

extern "C" void WINAPI ChildEntry() {
    ExitProcess(TryCombinedCall());
}
