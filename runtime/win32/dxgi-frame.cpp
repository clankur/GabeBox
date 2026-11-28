#include "dxgi-frame.h"

#include <utility>

extern "C" __declspec(dllimport) HANDLE WINAPI CreateFile2FromAppW(
    LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);

using Microsoft::WRL::ComPtr;

namespace {
volatile LONG generationCounter;

template<typename Mailbox> HRESULT MapMailbox(const WCHAR* path, Mailbox** out) {
    HANDLE file = CreateFile2FromAppW(path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    LARGE_INTEGER size{};
    const bool valid = GetFileSizeEx(file, &size) && size.QuadPart == sizeof(Mailbox);
    HANDLE mapping = valid ? CreateFileMappingFromApp(file, nullptr, PAGE_READWRITE, sizeof(Mailbox), nullptr) : nullptr;
    HRESULT error = mapping ? S_OK : (valid ? HRESULT_FROM_WIN32(GetLastError()) : E_INVALIDARG);
    CloseHandle(file);
    if (FAILED(error)) return error;
    *out = static_cast<Mailbox*>(MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, sizeof(Mailbox)));
    if (!*out) error = HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(mapping);
    return error;
}

void ShareName(WCHAR (&name)[64], const GUID& token, unsigned slot, unsigned resource) {
    const WCHAR prefix[] = L"xbox-dxgi-";
    unsigned index = 0;
    for (; prefix[index]; ++index) name[index] = prefix[index];
    const auto* bytes = reinterpret_cast<const BYTE*>(&token);
    for (unsigned i = 0; i < sizeof(token); ++i) {
        name[index++] = L"0123456789abcdef"[bytes[i] >> 4];
        name[index++] = L"0123456789abcdef"[bytes[i] & 15];
    }
    name[index++] = '-';
    name[index++] = static_cast<WCHAR>('0' + slot);
    name[index++] = '-';
    name[index++] = static_cast<WCHAR>('0' + resource);
    name[index] = 0;
}

bool SupportedFrame(UINT64 width, UINT height, UINT samples, UINT layers, UINT levels, DXGI_FORMAT format) {
    return width && height && width <= SDL_HOST_MAX_WIDTH && height <= SDL_HOST_MAX_HEIGHT &&
        samples == 1 && layers == 1 && levels == 1 &&
        (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM);
}

// Times the host's release of each committed frame. The host signals a slot's done fence right after the draw
// on its display's composition callback, so these times follow the display's refresh at a small offset.
struct HostRelease {
    SRWLOCK lock = SRWLOCK_INIT;
    XboxHostVsync clock{};
    HANDLE events[SDL_GPU_SLOTS]{};
    UINT64 frequency = 0;
    volatile LONG state = 0; // 0 not started, 1 starting, 2 running, 3 unavailable
} hostRelease;

constexpr unsigned hostLogSize = 8192;
UINT64 hostLog[hostLogSize];
volatile LONG hostLogCount = -1; // -1 off; counts up to hostLogSize

void HostLogAdd(UINT64 qpc, unsigned kind) {
    if (InterlockedCompareExchange(&hostLogCount, 0, 0) < 0) return;
    const LONG slot = InterlockedIncrement(&hostLogCount) - 1;
    if (slot >= 0 && static_cast<unsigned>(slot) < hostLogSize) hostLog[slot] = qpc << 1 | kind;
}

DWORD WINAPI WatchHostRelease(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;) {
        const DWORD which = WaitForMultipleObjects(SDL_GPU_SLOTS, hostRelease.events, FALSE, INFINITE);
        if (which >= WAIT_OBJECT_0 + SDL_GPU_SLOTS) return 0;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        AcquireSRWLockExclusive(&hostRelease.lock);
        XboxHostVsyncObserve(&hostRelease.clock, static_cast<UINT64>(now.QuadPart));
        ReleaseSRWLockExclusive(&hostRelease.lock);
        HostLogAdd(static_cast<UINT64>(now.QuadPart), 1);
    }
}

bool StartHostRelease() {
    const LONG state = InterlockedCompareExchange(&hostRelease.state, 1, 0);
    if (state) return state == 2;
    const DWORD error = GetLastError();
    LARGE_INTEGER frequency{};
    bool ready = QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0;
    for (auto& event : hostRelease.events)
        if (ready && !(event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS))) ready = false;
    if (ready) {
        hostRelease.frequency = static_cast<UINT64>(frequency.QuadPart);
        XboxHostVsyncInit(&hostRelease.clock, hostRelease.frequency);
        HANDLE thread = CreateThread(nullptr, 0, WatchHostRelease, nullptr, 0, nullptr);
        ready = thread != nullptr;
        if (thread) CloseHandle(thread);
    }
    InterlockedExchange(&hostRelease.state, ready ? 2 : 3);
    SetLastError(error);
    return ready;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
        D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return barrier;
}
}

void DxgiFramePublisher::Slot::Close() {
    for (auto& handle : handles) {
        if (handle) CloseHandle(handle);
        handle = nullptr;
    }
    texture.Reset();
    ready.Reset();
    done.Reset();
    commands.Reset();
    allocator.Reset();
    source12.Reset();
    texture12.Reset();
    ready12.Reset();
    done12.Reset();
    queued12 = 0;
    copied = 0;
    submitted = 0;
}

DxgiFramePublisher::~DxgiFramePublisher() {
    const auto error = Reset();
    if (FAILED(error)) {
        // If the queue does not drain, keep the pending objects alive until
        // process teardown. A command list holds no application references.
        for (auto& slot : slots_) {
            if (!slot.queued12 || slot.ready12->GetCompletedValue() >= slot.queued12) continue;
            slot.commands.Detach();
            slot.allocator.Detach();
            slot.source12.Detach();
            slot.texture12.Detach();
            slot.ready12.Detach();
        }
        queue_.Detach();
        ClearSlots();
    }
    if (gpu_) UnmapViewOfFile(gpu_);
    if (base_) UnmapViewOfFile(base_);
}

HRESULT DxgiFramePublisher::Reset() {
    const auto deadline = GetTickCount64() + 2000;
    for (auto& slot : slots_) {
        if (!slot.queued12 || slot.ready12->GetCompletedValue() >= slot.queued12) continue;
        if (slot.queued12 > slot.copied) {
            const auto error = queue_->Signal(slot.ready12.Get(), slot.queued12);
            if (FAILED(error)) {
                // A removed device retires its outstanding queues.
                if (slot.ready12->GetCompletedValue() == UINT64_MAX) continue;
                return error;
            }
        }
        while (slot.ready12->GetCompletedValue() < slot.queued12) {
            if (GetTickCount64() >= deadline) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            SleepEx(1, FALSE);
        }
    }
    ClearSlots();
    return S_OK;
}

void DxgiFramePublisher::ClearSlots() {
    if (generation_) {
        InterlockedExchange(&gpu_->gpu_active, 0);
        InterlockedCompareExchange(&gpu_->accepted_generation, 0, generation_);
        InterlockedExchange(&gpu_->generation, 0);
        generation_ = 0;
    }
    // Finish submitting copies before the swap chain can replace its buffers.
    if (context_) context_->Flush();
    for (auto& slot : slots_) slot.Close();
    context_.Reset();
    status_ = E_PENDING;
}

HRESULT DxgiFramePublisher::OpenMailboxes() {
    if (gpu_) return S_OK;
    WCHAR path[1024];
    const DWORD length = GetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", path, ARRAYSIZE(path));
    if (!length) return S_FALSE;
    const WCHAR extension[] = L".gpu";
    if (length + ARRAYSIZE(extension) > ARRAYSIZE(path)) return E_INVALIDARG;
    HRESULT error = MapMailbox(path, &base_);
    if (SUCCEEDED(error) && SdlHostLoad(&base_->magic) != SDL_HOST_MAGIC) error = E_INVALIDARG;
    if (SUCCEEDED(error)) {
        for (unsigned i = 0; i < ARRAYSIZE(extension); ++i) path[length + i] = extension[i];
        error = MapMailbox(path, &gpu_);
    }
    if (SUCCEEDED(error) && SdlHostLoad(&gpu_->magic) != SDL_GPU_MAGIC) error = E_INVALIDARG;
    if (FAILED(error)) {
        if (gpu_) { UnmapViewOfFile(gpu_); gpu_ = nullptr; }
        if (base_) { UnmapViewOfFile(base_); base_ = nullptr; }
    }
    return error;
}

HRESULT DxgiFramePublisher::Connect(ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& desc) {
    if (!SupportedFrame(desc.Width, desc.Height, desc.SampleDesc.Count, desc.ArraySize, desc.MipLevels, desc.Format))
        return DXGI_ERROR_UNSUPPORTED;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device5> modern;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter1> adapterInfo;
    DXGI_ADAPTER_DESC1 info;
    source->GetDevice(&device);
    device->GetImmediateContext(&immediate);
    HRESULT error = device.As(&modern);
    if (SUCCEEDED(error)) error = immediate.As(&context_);
    if (SUCCEEDED(error)) error = device.As(&dxgi);
    if (SUCCEEDED(error)) error = dxgi->GetAdapter(&adapter);
    if (SUCCEEDED(error)) error = adapter.As(&adapterInfo);
    if (SUCCEEDED(error)) error = adapterInfo->GetDesc1(&info);
    if (FAILED(error)) return error;
    if (info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return DXGI_ERROR_UNSUPPORTED;

    ComPtr<ID3D12Device> share;
    error = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&share));
    if (FAILED(error)) return error;
    return CreateSlots(share.Get(), modern.Get(), info.AdapterLuid, desc.Width, desc.Height, desc.Format);
}

HRESULT DxgiFramePublisher::Connect(ID3D12Resource* source, const D3D12_RESOURCE_DESC& desc) {
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            !SupportedFrame(desc.Width, desc.Height, desc.SampleDesc.Count, desc.DepthOrArraySize, desc.MipLevels, desc.Format))
        return DXGI_ERROR_UNSUPPORTED;
    ComPtr<ID3D12Device> device;
    const auto error = source->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(error)) return error;
    return CreateSlots(device.Get(), nullptr, device->GetAdapterLuid(), static_cast<UINT>(desc.Width),
        desc.Height, desc.Format);
}

HRESULT DxgiFramePublisher::CreateSlots(ID3D12Device* share, ID3D11Device5* d3d11, LUID adapter,
        UINT width, UINT height, DXGI_FORMAT format) {
    GUID token{};
    HRESULT error = CoCreateGuid(&token);
    if (FAILED(error)) return error;
    if (InterlockedCompareExchange(&gpu_->generation, 1, 0) != 0) return E_ACCESSDENIED;
    generation_ = InterlockedAdd(&generationCounter, 2);
    gpu_->width = width_ = width;
    gpu_->height = height_ = height;
    gpu_->dxgi_format = format_ = format;
    gpu_->adapter_luid = (static_cast<UINT64>(static_cast<UINT32>(adapter.HighPart)) << 32) | adapter.LowPart;
    gpu_->flip_y = 0;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = width;
    resource.Height = height;
    resource.DepthOrArraySize = resource.MipLevels = 1;
    resource.Format = format;
    resource.SampleDesc.Count = 1;
    resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (unsigned i = 0; i < SDL_GPU_SLOTS; ++i) {
        auto& slot = slots_[i];
        ComPtr<ID3D12Resource> texture;
        ComPtr<ID3D12Fence> ready, done;
        error = share->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &resource, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&texture));
        if (SUCCEEDED(error)) error = share->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&ready));
        if (SUCCEEDED(error)) error = share->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&done));
        ID3D12DeviceChild* objects[] = {texture.Get(), ready.Get(), done.Get()};
        for (unsigned j = 0; SUCCEEDED(error) && j < ARRAYSIZE(objects); ++j) {
            ShareName(gpu_->names[i][j], token, i, j);
            error = share->CreateSharedHandle(objects[j], nullptr, GENERIC_ALL, gpu_->names[i][j], &slot.handles[j]);
        }
        if (SUCCEEDED(error) && d3d11) {
            error = d3d11->OpenSharedResource1(slot.handles[0], IID_PPV_ARGS(&slot.texture));
            if (SUCCEEDED(error)) error = d3d11->OpenSharedFence(slot.handles[1], IID_PPV_ARGS(&slot.ready));
            if (SUCCEEDED(error)) error = d3d11->OpenSharedFence(slot.handles[2], IID_PPV_ARGS(&slot.done));
        } else if (SUCCEEDED(error)) {
            slot.texture12 = std::move(texture);
            slot.ready12 = std::move(ready);
            slot.done12 = std::move(done);
            error = share->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));
            if (SUCCEEDED(error)) error = share->CreateCommandList(queue_->GetDesc().NodeMask,
                D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.commands));
            if (SUCCEEDED(error)) error = slot.commands->Close();
        }
        if (FAILED(error)) { ClearSlots(); return error; }
        InterlockedExchange64(&gpu_->sequence[i], 0);
        gpu_->cursor[i].shape = 0;
    }
    InterlockedExchange(&gpu_->generation, generation_);
    return S_OK;
}

HRESULT DxgiFramePublisher::Prepare(IDXGISwapChain1* chain, UINT backBuffer, unsigned& index) {
    failureStage_ = nullptr;
    if (status_ == E_PENDING) {
        status_ = OpenMailboxes();
        if (FAILED(status_)) return FailedAt("gpu-frame-mailbox-error", status_);
    }
    if (status_ != S_OK) return status_;
    if (InterlockedCompareExchange(&base_->quit, 0, 1) == 1) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    return queue_ ? Prepare12(chain, backBuffer, index) : Prepare11(chain, index);
}

HRESULT DxgiFramePublisher::FailedAt(const char* stage, HRESULT error) {
    failureStage_ = stage;
    return error;
}

HRESULT DxgiFramePublisher::Prepare11(IDXGISwapChain1* chain, unsigned& index) {
    ComPtr<ID3D11Texture2D> source;
    HRESULT error = chain->GetBuffer(0, IID_PPV_ARGS(&source));
    if (FAILED(error)) return FailedAt("gpu-frame-backbuffer-error", error);
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (generation_ && (desc.Width != width_ || desc.Height != height_ || desc.Format != format_)) ClearSlots();
    if (!generation_) {
        status_ = Connect(source.Get(), desc);
        if (FAILED(status_)) return FailedAt("gpu-frame-share-setup-error", status_);
    }
    index = static_cast<unsigned>((next_ - 1) % SDL_GPU_SLOTS);
    auto& slot = slots_[index];
    const UINT64 ready = slot.ready->GetCompletedValue();
    const UINT64 done = slot.done->GetCompletedValue();
    if (ready == UINT64_MAX) return FailedAt("gpu-frame-producer-fence-error", DXGI_ERROR_DEVICE_REMOVED);
    if (done == UINT64_MAX) return FailedAt("gpu-frame-consumer-fence-error", DXGI_ERROR_DEVICE_REMOVED);
    if (ready < slot.copied || done < slot.submitted) return S_FALSE;
    InterlockedExchange64(&gpu_->sequence[index], 0);
    context_->CopyResource(slot.texture.Get(), source.Get());
    error = context_->Signal(slot.ready.Get(), next_);
    if (FAILED(error)) return FailedAt("gpu-frame-signal-error", error);
    context_->Flush();
    slot.copied = next_++;
    return S_OK;
}

HRESULT DxgiFramePublisher::Prepare12(IDXGISwapChain1* chain, UINT backBuffer, unsigned& index) {
    ComPtr<ID3D12Resource> source;
    HRESULT error = chain->GetBuffer(backBuffer, IID_PPV_ARGS(&source));
    if (FAILED(error)) return FailedAt("gpu-frame-backbuffer-error", error);
    const auto desc = source->GetDesc();
    if (generation_ && (desc.Width != width_ || desc.Height != height_ || desc.Format != format_))
        return FailedAt("gpu-frame-format-change-error", DXGI_ERROR_INVALID_CALL);
    if (!generation_) {
        status_ = Connect(source.Get(), desc);
        if (FAILED(status_)) return FailedAt("gpu-frame-share-setup-error", status_);
    }
    index = static_cast<unsigned>((next_ - 1) % SDL_GPU_SLOTS);
    auto& slot = slots_[index];
    const UINT64 ready = slot.ready12->GetCompletedValue();
    const UINT64 done = slot.done12->GetCompletedValue();
    if (ready == UINT64_MAX) return FailedAt("gpu-frame-producer-fence-error", DXGI_ERROR_DEVICE_REMOVED);
    if (done == UINT64_MAX) return FailedAt("gpu-frame-consumer-fence-error", DXGI_ERROR_DEVICE_REMOVED);
    if (ready < slot.copied || done < slot.submitted) return S_FALSE;

    error = slot.allocator->Reset();
    if (SUCCEEDED(error)) error = slot.commands->Reset(slot.allocator.Get(), nullptr);
    if (FAILED(error)) return FailedAt("gpu-frame-command-reset-error", status_ = error);
    auto* texture = slot.texture12.Get();
    const D3D12_RESOURCE_BARRIER before[] = {
        Transition(source.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    const D3D12_RESOURCE_BARRIER after[] = {
        Transition(source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT),
        Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
    };
    slot.commands->ResourceBarrier(ARRAYSIZE(before), before);
    slot.commands->CopyResource(texture, source.Get());
    slot.commands->ResourceBarrier(ARRAYSIZE(after), after);
    error = slot.commands->Close();
    if (FAILED(error)) return FailedAt("gpu-frame-command-close-error", status_ = error);

    InterlockedExchange64(&gpu_->sequence[index], 0);
    slot.source12 = std::move(source);
    ID3D12CommandList* commands[] = {slot.commands.Get()};
    slot.queued12 = next_;
    queue_->ExecuteCommandLists(ARRAYSIZE(commands), commands);
    error = queue_->Signal(slot.ready12.Get(), next_);
    if (FAILED(error)) return FailedAt("gpu-frame-signal-error", status_ = error);
    slot.copied = next_++;
    return S_OK;
}

void DxgiFramePublisher::Commit(unsigned index) {
    auto& slot = slots_[index];
    // A rejected Present still owns its producer fence until the copy retires.
    // Only a committed sequence acquires a consumer fence obligation.
    slot.submitted = slot.copied;
    InterlockedExchange64(&gpu_->sequence[index], static_cast<LONG64>(slot.submitted));
    InterlockedExchange(&gpu_->gpu_active, 1);
    InterlockedExchange(&base_->frame_sequence, 1);
    if (InterlockedCompareExchange(&hostLogCount, 0, 0) >= 0) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        HostLogAdd(static_cast<UINT64>(now.QuadPart), 0);
    }
    if (StartHostRelease()) {
        const DWORD error = GetLastError();
        if (slot.done12) slot.done12->SetEventOnCompletion(slot.submitted, hostRelease.events[index]);
        else if (slot.done) slot.done->SetEventOnCompletion(slot.submitted, hostRelease.events[index]);
        SetLastError(error);
    }
}

void DxgiFramePublisher::HostLogStart() {
    InterlockedCompareExchange(&hostLogCount, 0, -1);
}

const UINT64* DxgiFramePublisher::HostLogFull(unsigned& count) {
    const LONG value = InterlockedCompareExchange(&hostLogCount, 0, 0);
    if (value < static_cast<LONG>(hostLogSize)) return nullptr;
    count = hostLogSize;
    return hostLog;
}

bool DxgiFramePublisher::HostVsync(UINT64 now, XboxHostVsync& out) {
    if (InterlockedCompareExchange(&hostRelease.state, 0, 0) != 2) return false;
    AcquireSRWLockShared(&hostRelease.lock);
    out = hostRelease.clock;
    ReleaseSRWLockShared(&hostRelease.lock);
    return XboxHostVsyncLocked(&out, now, hostRelease.frequency) != 0;
}
