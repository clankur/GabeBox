#include "../child-process.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#ifdef PROBE_HOSTED_XINPUT
#include <appmodel.h>
#include <xinput.h>
#endif

#ifdef PROBE_DXGI_EXCHANGE
#include <d3d11_4.h>
#include <d3d12.h>
#include "../sdl-host-protocol.h"
#include "../sdl-gpu-protocol.h"
extern "C" __declspec(dllimport) HANDLE WINAPI CreateFile2FromAppW(
    LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
#endif

using Microsoft::WRL::ComPtr;

extern "C" __declspec(dllimport) __declspec(noreturn) void WINAPI ExitProcess(unsigned int code);

static unsigned int Failure(unsigned int stage, HRESULT error) {
    return 0x6C000000u | (stage << 16) | (static_cast<unsigned int>(error) & 0xffffu);
}

#if defined(PROBE_DXGI_WINDOW) && !defined(PROBE_HOSTED_XINPUT)
static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

struct DesktopChainRequest {
    IDXGIFactory2* factory;
    ID3D11Device* device;
    HWND window;
    DXGI_SWAP_CHAIN_DESC1 description;
    IDXGISwapChain1** out;
    HRESULT result;
};

static void CALLBACK CreateDesktopChainOnWorker(PTP_CALLBACK_INSTANCE, PVOID context, PTP_WORK) {
    auto& request = *static_cast<DesktopChainRequest*>(context);
    auto error = request.factory->MakeWindowAssociation(request.window, DXGI_MWA_NO_ALT_ENTER);
    HWND associated = nullptr;
    if (SUCCEEDED(error)) error = request.factory->GetWindowAssociation(&associated);
    if (SUCCEEDED(error) && associated != request.window) error = E_FAIL;
    if (SUCCEEDED(error)) error = request.factory->CreateSwapChainForHwnd(request.device, request.window,
        &request.description, nullptr, nullptr, request.out);
    if (SUCCEEDED(error)) error = (*request.out)->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    request.result = error;
}

static HRESULT CreateDesktopChain(ID3D11Device* device, IDXGISwapChain1** out, DXGI_SWAP_CHAIN_DESC1 description) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const WNDCLASSW windowClass{0, WindowProc, 0, 0, instance, nullptr, nullptr, nullptr, nullptr, L"DxgiWindowControl"};
    if (!RegisterClassW(&windowClass)) return HRESULT_FROM_WIN32(GetLastError());
    const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"DXGI control", 0,
        0, 0, 64, 64, nullptr, nullptr, instance, nullptr);
    if (!window) return HRESULT_FROM_WIN32(GetLastError());

    ComPtr<IDXGIFactory1> base;
    ComPtr<IDXGIFactory2> factory;
#ifdef PROBE_DXGI_DYNAMIC
    const auto system = GetModuleHandleW(L"dxgi.dll");
    const auto adapter = GetModuleHandleW(L"XboxWin32Dxgi.dll");
    if (!system || !adapter) return E_NOINTERFACE;
    if (GetProcAddress(system, "XboxMissingDxgiControlExport") || GetLastError() != ERROR_PROC_NOT_FOUND)
        return E_FAIL;
    HRESULT error;
    if (const auto export2 = GetProcAddress(system, "CreateDXGIFactory2")) {
        if (export2 != GetProcAddress(adapter, "CreateDXGIFactory2")) return E_FAIL;
        using CreateFactory2 = HRESULT (WINAPI *)(UINT, REFIID, void**);
        const auto create = reinterpret_cast<CreateFactory2>(reinterpret_cast<void*>(export2));
        error = create(0, IID_PPV_ARGS(&base));
    } else {
        const auto export1 = GetProcAddress(system, "CreateDXGIFactory1");
        if (!export1 || export1 != GetProcAddress(adapter, "CreateDXGIFactory1")) return E_FAIL;
        using CreateFactory = HRESULT (WINAPI *)(REFIID, void**);
        const auto create = reinterpret_cast<CreateFactory>(reinterpret_cast<void*>(export1));
        error = create(IID_PPV_ARGS(&base));
    }
#else
    HRESULT error = CreateDXGIFactory1(IID_PPV_ARGS(&base));
#endif
    if (SUCCEEDED(error)) error = base.As(&factory);
    ComPtr<IUnknown> baseIdentity, factoryIdentity;
    if (SUCCEEDED(error)) error = base.As(&baseIdentity);
    if (SUCCEEDED(error)) error = factory.As(&factoryIdentity);
    if (SUCCEEDED(error) && baseIdentity.Get() != factoryIdentity.Get()) return E_NOINTERFACE;
    if (FAILED(error)) return error;
    description.Width = description.Height = 0;
    description.BufferCount = 1;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    DesktopChainRequest request{factory.Get(), device, window, description, out, E_FAIL};
    const PTP_WORK work = CreateThreadpoolWork(CreateDesktopChainOnWorker, &request, nullptr);
    if (!work) return HRESULT_FROM_WIN32(GetLastError());
    SubmitThreadpoolWork(work);
    WaitForThreadpoolWorkCallbacks(work, FALSE);
    CloseThreadpoolWork(work);
    return request.result;
}
#endif

#ifdef PROBE_DXGI_EXCHANGE
struct ExchangeControl {
    WCHAR path[1024];
    HANDLE baseFile = INVALID_HANDLE_VALUE, gpuFile = INVALID_HANDLE_VALUE;
    SdlHostMailbox* base = nullptr;
    SdlGpuMailbox* gpu = nullptr;

    template<class Mailbox> static HRESULT Create(const WCHAR* path, HANDLE& file, Mailbox*& box, LONG magic) {
        CREATEFILE2_EXTENDED_PARAMETERS flags{};
        flags.dwSize = sizeof(flags);
        flags.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
        flags.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE;
        file = CreateFile2FromAppW(path, GENERIC_READ | GENERIC_WRITE | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, CREATE_NEW, &flags);
        if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        HANDLE mapping = CreateFileMappingFromApp(file, nullptr, PAGE_READWRITE, sizeof(Mailbox), nullptr);
        if (!mapping) return HRESULT_FROM_WIN32(GetLastError());
        box = static_cast<Mailbox*>(MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, sizeof(Mailbox)));
        const auto code = box ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(mapping);
        if (box) InterlockedExchange(&box->magic, magic);
        return code;
    }

    ~ExchangeControl() {
        SetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", nullptr);
        if (gpu) UnmapViewOfFile(gpu);
        if (base) UnmapViewOfFile(base);
        if (gpuFile != INVALID_HANDLE_VALUE) CloseHandle(gpuFile);
        if (baseFile != INVALID_HANDLE_VALUE) CloseHandle(baseFile);
    }

    HRESULT Open(bool sharedGraphics = true) {
        const DWORD offset = GetTempPathW(ARRAYSIZE(path), path);
        if (!offset || offset + 40 > ARRAYSIZE(path)) return E_FAIL;
        WCHAR* end = path + offset;
        for (auto prefix = L"xbox-dxgi-control-"; *prefix;) *end++ = *prefix++;
        const auto token = static_cast<UINT>(GetTickCount()) ^ GetCurrentProcessId();
        for (int shift = 28; shift >= 0; shift -= 4) *end++ = L"0123456789abcdef"[(token >> shift) & 15];
        *end = 0;
        HRESULT code = Create(path, baseFile, base, SDL_HOST_MAGIC);
        if (FAILED(code)) return code;
        if (!SetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", path)) return HRESULT_FROM_WIN32(GetLastError());
        if (!sharedGraphics) return S_OK;
        for (auto suffix = L".gpu"; *suffix;) *end++ = *suffix++;
        *end = 0;
        return Create(path, gpuFile, gpu, SDL_GPU_MAGIC);
    }

    HRESULT Receive(IDXGIAdapter* adapter, ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D** texture) {
        if (SdlHostLoad(&base->frame_sequence) != 1 || SdlHostLoad(&base->frame_state) != SDL_HOST_EMPTY ||
                SdlHostLoad(&gpu->generation) <= 0 || !SdlHostLoad(&gpu->gpu_active) || gpu->flip_y ||
                gpu->width != 64 || gpu->height != 64 || InterlockedCompareExchange64(&gpu->sequence[0], 0, 0) != 1)
            return E_FAIL;
        ComPtr<ID3D12Device> lookup;
        ComPtr<ID3D11Device5> modern;
        ComPtr<ID3D11DeviceContext4> queue;
        ComPtr<ID3D11Fence> ready;
        HRESULT code = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&lookup));
        if (SUCCEEDED(code)) code = device->QueryInterface(IID_PPV_ARGS(&modern));
        if (SUCCEEDED(code)) code = context->QueryInterface(IID_PPV_ARGS(&queue));
        HANDLE handle = nullptr;
        if (SUCCEEDED(code)) code = lookup->OpenSharedHandleByName(gpu->names[0][0], GENERIC_ALL, &handle);
        if (SUCCEEDED(code)) { code = modern->OpenSharedResource1(handle, IID_PPV_ARGS(texture)); CloseHandle(handle); }
        if (SUCCEEDED(code)) code = lookup->OpenSharedHandleByName(gpu->names[0][1], GENERIC_ALL, &handle);
        if (SUCCEEDED(code)) { code = modern->OpenSharedFence(handle, IID_PPV_ARGS(&ready)); CloseHandle(handle); }
        return FAILED(code) ? code : queue->Wait(ready.Get(), 1);
    }
};
#endif

#ifdef PROBE_HOSTED_XINPUT
static DWORD VerifyHostedXInput(SdlHostGamepad& pad) {
    const auto system = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto input = LoadPackagedLibrary(L"XboxWin32Input.dll", 0);
    if (!system || !input) return ERROR_MOD_NOT_FOUND;
    SetEnvironmentVariableW(L"XBOX_WIN32_HOST_XINPUT", nullptr);
    if (GetProcAddress(system, "XInputGetState") == GetProcAddress(input, "XInputGetState")) return ERROR_INVALID_FUNCTION;
    if (!SetEnvironmentVariableW(L"XBOX_WIN32_HOST_XINPUT", L"1")) return GetLastError();

    const auto state = GetProcAddress(system, "XInputGetState");
    const auto capabilities = GetProcAddress(system, "XInputGetCapabilities");
    const auto vibration = GetProcAddress(system, "XInputSetState");
    SetEnvironmentVariableW(L"XBOX_WIN32_HOST_XINPUT", nullptr);
    if (!state || !capabilities || !vibration || state != GetProcAddress(input, "XInputGetState") ||
            capabilities != GetProcAddress(input, "XInputGetCapabilities") ||
            vibration != GetProcAddress(input, "XInputSetState")) return ERROR_PROC_NOT_FOUND;
    const auto getState = reinterpret_cast<decltype(&XInputGetState)>(reinterpret_cast<void*>(state));
    const auto getCapabilities = reinterpret_cast<decltype(&XInputGetCapabilities)>(reinterpret_cast<void*>(capabilities));
    const auto setState = reinterpret_cast<decltype(&XInputSetState)>(reinterpret_cast<void*>(vibration));
    XINPUT_STATE sample{};
    XINPUT_CAPABILITIES caps{};
    XINPUT_VIBRATION motors{32767, 0};
    if (getState(0, &sample) != ERROR_DEVICE_NOT_CONNECTED ||
            getCapabilities(0, 0, &caps) != ERROR_DEVICE_NOT_CONNECTED) return ERROR_INVALID_DATA;

    InterlockedIncrement(&pad.serial);
    InterlockedExchange(&pad.buttons, SDL_HOST_PAD_CONNECTED | 1);
    InterlockedExchange(&pad.axes[0], 12000);
    InterlockedExchange(&pad.axes[1], -4000);
    InterlockedExchange(&pad.axes[4], -32768);
    InterlockedExchange(&pad.axes[5], 32767);
    const auto packet = InterlockedIncrement(&pad.serial);
    if (getState(0, &sample) != ERROR_SUCCESS || sample.dwPacketNumber != static_cast<DWORD>(packet) ||
            sample.Gamepad.wButtons != XINPUT_GAMEPAD_A || sample.Gamepad.sThumbLX != 12000 ||
            sample.Gamepad.sThumbLY != 4000 || sample.Gamepad.bLeftTrigger != 0 || sample.Gamepad.bRightTrigger != 255 ||
            SdlHostLoad(&pad.ack) != packet || getCapabilities(0, XINPUT_FLAG_GAMEPAD, &caps) != ERROR_SUCCESS ||
            caps.Type != XINPUT_DEVTYPE_GAMEPAD || caps.Vibration.wLeftMotorSpeed || caps.Vibration.wRightMotorSpeed ||
            setState(0, &motors) != ERROR_NOT_SUPPORTED || getState(1, &sample) != ERROR_DEVICE_NOT_CONNECTED)
        return ERROR_INVALID_DATA;

    InterlockedIncrement(&pad.serial);
    InterlockedExchange(&pad.buttons, 0);
    InterlockedIncrement(&pad.serial);
    if (getState(0, &sample) != ERROR_DEVICE_NOT_CONNECTED || setState(0, &motors) != ERROR_DEVICE_NOT_CONNECTED)
        return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}

static unsigned int VerifyControl() {
    ExchangeControl exchange;
    const HRESULT open = exchange.Open(false);
    if (FAILED(open)) return Failure(8, open);
    const DWORD input = VerifyHostedXInput(exchange.base->gamepad);
    return input ? Failure(11, HRESULT_FROM_WIN32(input)) : PROBE_EXPECTED_CHILD_EXIT;
}
#else
static unsigned int VerifyControl() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT error = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(error)) return Failure(1, error);

    ComPtr<IDXGIDevice> graphics;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    error = device.As(&graphics);
    if (SUCCEEDED(error)) error = graphics->GetAdapter(&adapter);
    if (SUCCEEDED(error)) error = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(error)) return Failure(2, error);
    ComPtr<IDXGIAdapter1> adapterInfo;
    DXGI_ADAPTER_DESC1 information;
    error = adapter.As(&adapterInfo);
    if (SUCCEEDED(error)) error = adapterInfo->GetDesc1(&information);
    if (FAILED(error)) return Failure(2, error);
    if (information.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return Failure(2, DXGI_ERROR_UNSUPPORTED);

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = 64;
    description.Height = 64;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
#ifdef PROBE_DXGI_EXCHANGE
    ExchangeControl exchange;
    error = exchange.Open();
    if (FAILED(error)) return Failure(8, error);
#endif
    ComPtr<IDXGISwapChain1> swapChain;
#ifdef PROBE_DXGI_WINDOW
    error = CreateDesktopChain(device.Get(), &swapChain, description);
#else
    error = factory->CreateSwapChainForComposition(device.Get(), &description, nullptr, &swapChain);
#endif
    if (FAILED(error)) return Failure(3, error);

    ComPtr<ID3D11Texture2D> frame;
    ComPtr<ID3D11RenderTargetView> target;
    error = swapChain->GetBuffer(0, IID_PPV_ARGS(&frame));
    if (SUCCEEDED(error)) error = device->CreateRenderTargetView(frame.Get(), nullptr, &target);
    if (FAILED(error)) return Failure(4, error);
    const float color[4]{0.0f, 1.0f, 1.0f, 1.0f};
    context->ClearRenderTargetView(target.Get(), color);

    D3D11_TEXTURE2D_DESC readable{};
    frame->GetDesc(&readable);
    if (readable.Width != 64 || readable.Height != 64) return Failure(4, E_FAIL);
#ifdef PROBE_DXGI_EXCHANGE
    error = swapChain->Present(0, 0);
    if (FAILED(error)) return Failure(9, error);
    ComPtr<ID3D11Device> consumer;
    ComPtr<ID3D11DeviceContext> consumerContext;
    error = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &consumer, nullptr, &consumerContext);
    if (SUCCEEDED(error)) error = exchange.Receive(adapter.Get(), consumer.Get(), consumerContext.Get(), frame.ReleaseAndGetAddressOf());
    if (FAILED(error)) return Failure(10, error);
    device = consumer;
    context = consumerContext;
#endif
    readable.Usage = D3D11_USAGE_STAGING;
    readable.BindFlags = 0;
    readable.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    readable.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> copy;
    error = device->CreateTexture2D(&readable, nullptr, &copy);
    if (FAILED(error)) return Failure(5, error);
    context->CopyResource(copy.Get(), frame.Get());

    D3D11_MAPPED_SUBRESOURCE pixels{};
    error = context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &pixels);
    if (FAILED(error)) return Failure(6, error);
    const auto *pixel = static_cast<const unsigned char *>(pixels.pData);
    const bool cyan = pixel[0] == 255 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255;
    context->Unmap(copy.Get(), 0);
    return cyan ? PROBE_EXPECTED_CHILD_EXIT : Failure(7, E_FAIL);
}
#endif

extern "C" void WINAPI ChildEntry() {
    ExitProcess(VerifyControl());
}
