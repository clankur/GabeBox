#include "../child-process.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#ifdef PROBE_DXGI_EXCHANGE
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
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

#ifdef PROBE_DXGI_WINDOW
static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

struct DesktopChainRequest {
    IDXGIFactory2* factory;
    IUnknown* device;
    HWND window;
    DXGI_SWAP_CHAIN_DESC1 description;
    IDXGISwapChain1** chain;
    HRESULT result;
};

static void CALLBACK CreateDesktopChainOnWorker(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) {
    auto& request = *static_cast<DesktopChainRequest*>(context);
    auto result = request.factory->MakeWindowAssociation(request.window, DXGI_MWA_NO_ALT_ENTER);
    HWND associated = nullptr;
    if (SUCCEEDED(result)) result = request.factory->GetWindowAssociation(&associated);
    if (SUCCEEDED(result) && associated != request.window) result = E_FAIL;
    if (SUCCEEDED(result)) result = request.factory->CreateSwapChainForHwnd(
        request.device, request.window, &request.description, nullptr, nullptr, request.chain);
    request.result = result;
}

static HRESULT CreateDesktopChain(IUnknown* device, IDXGISwapChain1** out, DXGI_SWAP_CHAIN_DESC1 description) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const WNDCLASSW windowClass{0, WindowProc, 0, 0, instance, nullptr, nullptr, nullptr, nullptr, L"DxgiWindowControl"};
    if (!RegisterClassW(&windowClass)) return HRESULT_FROM_WIN32(GetLastError());
    const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"DXGI control", 0,
        0, 0, 64, 64, nullptr, nullptr, instance, nullptr);
    if (!window) return HRESULT_FROM_WIN32(GetLastError());

    ComPtr<IDXGIFactory1> base;
    ComPtr<IDXGIFactory2> factory;
#ifdef PROBE_DXGI_D3D12
    HRESULT error = CreateDXGIFactory2(0, IID_PPV_ARGS(&base));
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
#ifndef PROBE_DXGI_D3D12
    description.BufferCount = 1;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
#endif
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

    HRESULT Open() {
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
        for (auto suffix = L".gpu"; *suffix;) *end++ = *suffix++;
        *end = 0;
        return Create(path, gpuFile, gpu, SDL_GPU_MAGIC);
    }

    HRESULT Receive(IDXGIAdapter* adapter, ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D** texture,
            UINT size = 64, UINT64 sequence = 1, ID3D11Fence** done = nullptr) {
        const auto slot = static_cast<unsigned>((sequence - 1) % SDL_GPU_SLOTS);
        if (SdlHostLoad(&base->frame_sequence) != 1 || SdlHostLoad(&base->frame_state) != SDL_HOST_EMPTY ||
                SdlHostLoad(&gpu->generation) <= 0 || !SdlHostLoad(&gpu->gpu_active) || gpu->flip_y ||
                gpu->width != size || gpu->height != size ||
                InterlockedCompareExchange64(&gpu->sequence[slot], 0, 0) != static_cast<LONG64>(sequence))
            return E_FAIL;
        ComPtr<ID3D12Device> lookup;
        ComPtr<ID3D11Device5> modern;
        ComPtr<ID3D11DeviceContext4> queue;
        ComPtr<ID3D11Fence> ready;
        HRESULT code = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&lookup));
        if (SUCCEEDED(code)) code = device->QueryInterface(IID_PPV_ARGS(&modern));
        if (SUCCEEDED(code)) code = context->QueryInterface(IID_PPV_ARGS(&queue));
        HANDLE handle = nullptr;
        if (SUCCEEDED(code)) code = lookup->OpenSharedHandleByName(gpu->names[slot][0], GENERIC_ALL, &handle);
        if (SUCCEEDED(code)) { code = modern->OpenSharedResource1(handle, IID_PPV_ARGS(texture)); CloseHandle(handle); }
        if (SUCCEEDED(code)) code = lookup->OpenSharedHandleByName(gpu->names[slot][1], GENERIC_ALL, &handle);
        if (SUCCEEDED(code)) { code = modern->OpenSharedFence(handle, IID_PPV_ARGS(&ready)); CloseHandle(handle); }
        if (SUCCEEDED(code) && done) {
            code = lookup->OpenSharedHandleByName(gpu->names[slot][2], GENERIC_ALL, &handle);
            if (SUCCEEDED(code)) { code = modern->OpenSharedFence(handle, IID_PPV_ARGS(done)); CloseHandle(handle); }
        }
        return FAILED(code) ? code : queue->Wait(ready.Get(), sequence);
    }
};
#endif

#ifdef PROBE_DXGI_D3D12
static HRESULT RenderFrame(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* commands,
        ID3D12CommandAllocator* allocator, D3D12_CPU_DESCRIPTOR_HANDLE target, ID3D12Resource* buffer, bool cyan) {
    HRESULT error = allocator->Reset();
    if (SUCCEEDED(error)) error = commands->Reset(allocator, nullptr);
    if (FAILED(error)) return error;
    device->CreateRenderTargetView(buffer, nullptr, target);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {buffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET};
    commands->ResourceBarrier(1, &barrier);
    const float color[4]{cyan ? 0.0f : 1.0f, cyan ? 1.0f : 0.0f, 1.0f, 1.0f};
    commands->ClearRenderTargetView(target, color, 0, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commands->ResourceBarrier(1, &barrier);
    error = commands->Close();
    if (SUCCEEDED(error)) {
        ID3D12CommandList* lists[] = {commands};
        queue->ExecuteCommandLists(ARRAYSIZE(lists), lists);
    }
    return error;
}

static HRESULT CheckSharedPixel(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
        ID3D11Fence* done, UINT64 sequence, bool cyan) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> readback;
    HRESULT error = device->CreateTexture2D(&desc, nullptr, &readback);
    if (FAILED(error)) return error;
    context->CopyResource(readback.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE pixels{};
    error = context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &pixels);
    if (FAILED(error)) return error;
    const auto* pixel = static_cast<const unsigned char*>(pixels.pData);
    const bool correct = pixel[0] == 255 && pixel[1] == (cyan ? 255 : 0) &&
        pixel[2] == (cyan ? 0 : 255) && pixel[3] == 255;
    context->Unmap(readback.Get(), 0);
    if (!correct) return E_FAIL;
    ComPtr<ID3D11DeviceContext4> modern;
    error = context->QueryInterface(IID_PPV_ARGS(&modern));
    if (SUCCEEDED(error)) error = modern->Signal(done, sequence);
    if (FAILED(error)) return error;
    modern->Flush();
    const auto deadline = GetTickCount64() + 1000;
    for (;;) {
        const auto completed = done->GetCompletedValue();
        if (completed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
        if (completed >= sequence) return S_OK;
        if (GetTickCount64() >= deadline) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        SleepEx(1, FALSE);
    }
}

static unsigned int VerifyFrame() {
    ComPtr<ID3D11Device> consumer;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT error = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &consumer, nullptr, &context);
    if (FAILED(error)) return Failure(1, error);
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    error = consumer.As(&dxgi);
    if (SUCCEEDED(error)) error = dxgi->GetAdapter(&adapter);
    if (FAILED(error)) return Failure(2, error);
    ComPtr<ID3D12Device> device;
    error = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(error)) return Failure(11, error);
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;
    error = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(error)) error = device->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(error)) error = device->CreateCommandList(0, queueDesc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&commands));
    if (SUCCEEDED(error)) error = commands->Close();
    if (SUCCEEDED(error)) error = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap));
    if (FAILED(error)) return Failure(12, error);
    const auto target = heap->GetCPUDescriptorHandleForHeapStart();

    ExchangeControl exchange;
    error = exchange.Open();
    if (FAILED(error)) return Failure(8, error);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    ComPtr<IDXGISwapChain1> base;
    ComPtr<IDXGISwapChain3> chain;
    error = CreateDesktopChain(queue.Get(), &base, desc);
    if (SUCCEEDED(error)) error = base.As(&chain);
    if (FAILED(error)) return Failure(3, error);
    UINT size = 64;
    constexpr UINT noBuffer = ~0u;
    UINT previousBuffer = noBuffer;
    const UINT64 resizeAfter = SDL_GPU_SLOTS + 1;
    for (UINT64 sequence = 1; sequence <= resizeAfter + 2; ++sequence) {
        if (sequence == resizeAfter + 1) {
            size = 80;
            const UINT nodeMasks[] = {1, 1};
            IUnknown* queues[] = {queue.Get(), queue.Get()};
            error = chain->ResizeBuffers1(2, size, size, desc.Format, 0, nodeMasks, queues);
            if (FAILED(error)) return Failure(15, error);
            previousBuffer = noBuffer;
        }
        const UINT currentBuffer = chain->GetCurrentBackBufferIndex();
        if (currentBuffer >= desc.BufferCount || currentBuffer == previousBuffer) return Failure(13, E_FAIL);
        previousBuffer = currentBuffer;
        ComPtr<ID3D12Resource> buffer;
        error = chain->GetBuffer(currentBuffer, IID_PPV_ARGS(&buffer));
        const bool cyan = (sequence & 1) != 0;
        if (SUCCEEDED(error)) error = RenderFrame(device.Get(), queue.Get(), commands.Get(), allocator.Get(),
            target, buffer.Get(), cyan);
        if (FAILED(error)) return Failure(13, error);
        error = chain->Present(0, 0);
        if (FAILED(error)) return Failure(9, error);
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11Fence> done;
        error = exchange.Receive(adapter.Get(), consumer.Get(), context.Get(), &texture, size, sequence, &done);
        if (FAILED(error)) return Failure(10, error);
        error = CheckSharedPixel(consumer.Get(), context.Get(), texture.Get(), done.Get(), sequence, cyan);
        if (FAILED(error)) return Failure(14, error);
    }
    return PROBE_EXPECTED_CHILD_EXIT;
}
#else
static unsigned int VerifyFrame() {
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
    ExitProcess(VerifyFrame());
}
