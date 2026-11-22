#include "dxgi-frame.h"

#include <d3d12.h>

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
}

void DxgiFramePublisher::Slot::Close() {
    for (auto& handle : handles) {
        if (handle) CloseHandle(handle);
        handle = nullptr;
    }
    texture.Reset();
    ready.Reset();
    done.Reset();
    submitted = 0;
}

DxgiFramePublisher::~DxgiFramePublisher() {
    Reset();
    if (gpu_) UnmapViewOfFile(gpu_);
    if (base_) UnmapViewOfFile(base_);
}

void DxgiFramePublisher::Reset() {
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
    if (!desc.Width || !desc.Height || desc.Width > SDL_HOST_MAX_WIDTH || desc.Height > SDL_HOST_MAX_HEIGHT ||
            desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.MipLevels != 1 ||
            (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM))
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
    GUID token{};
    if (SUCCEEDED(error)) error = CoCreateGuid(&token);
    if (FAILED(error)) return error;
    if (InterlockedCompareExchange(&gpu_->generation, 1, 0) != 0) return E_ACCESSDENIED;
    generation_ = InterlockedAdd(&generationCounter, 2);
    gpu_->width = width_ = desc.Width;
    gpu_->height = height_ = desc.Height;
    gpu_->dxgi_format = format_ = desc.Format;
    gpu_->adapter_luid = (static_cast<UINT64>(static_cast<UINT32>(info.AdapterLuid.HighPart)) << 32) | info.AdapterLuid.LowPart;
    gpu_->flip_y = 0;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = desc.Width;
    resource.Height = desc.Height;
    resource.DepthOrArraySize = resource.MipLevels = 1;
    resource.Format = desc.Format;
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
        if (SUCCEEDED(error)) error = modern->OpenSharedResource1(slot.handles[0], IID_PPV_ARGS(&slot.texture));
        if (SUCCEEDED(error)) error = modern->OpenSharedFence(slot.handles[1], IID_PPV_ARGS(&slot.ready));
        if (SUCCEEDED(error)) error = modern->OpenSharedFence(slot.handles[2], IID_PPV_ARGS(&slot.done));
        if (FAILED(error)) { Reset(); return error; }
        InterlockedExchange64(&gpu_->sequence[i], 0);
        gpu_->cursor[i].shape = 0;
    }
    InterlockedExchange(&gpu_->generation, generation_);
    return S_OK;
}

HRESULT DxgiFramePublisher::Publish(IDXGISwapChain1* chain) {
    if (status_ == E_PENDING) status_ = OpenMailboxes();
    if (status_ != S_OK) return status_;
    if (InterlockedCompareExchange(&base_->quit, 0, 1) == 1) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    ComPtr<ID3D11Texture2D> source;
    HRESULT error = chain->GetBuffer(0, IID_PPV_ARGS(&source));
    if (FAILED(error)) return error;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (generation_ && (desc.Width != width_ || desc.Height != height_ || desc.Format != format_)) Reset();
    if (!generation_) {
        status_ = Connect(source.Get(), desc);
        if (FAILED(status_)) return status_;
    }
    const auto index = static_cast<unsigned>((next_ - 1) % SDL_GPU_SLOTS);
    auto& slot = slots_[index];
    const UINT64 ready = slot.ready->GetCompletedValue();
    const UINT64 done = slot.done->GetCompletedValue();
    if (ready == UINT64_MAX || done == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
    if (ready < slot.submitted || done < slot.submitted) return S_FALSE;
    context_->CopyResource(slot.texture.Get(), source.Get());
    error = context_->Signal(slot.ready.Get(), next_);
    if (FAILED(error)) return error;
    context_->Flush();
    slot.submitted = next_++;
    InterlockedExchange64(&gpu_->sequence[index], static_cast<LONG64>(slot.submitted));
    InterlockedExchange(&gpu_->gpu_active, 1);
    InterlockedExchange(&base_->frame_sequence, 1);
    return S_OK;
}
