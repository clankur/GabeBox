#include "dxgi-frame.h"


#include <utility>

extern "C" __declspec(dllimport) HANDLE WINAPI CreateFile2FromAppW(
    LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);

namespace {
void AppendProbeTrace(const char* text, DWORD size) {
    WCHAR path[512];
    const DWORD length = GetEnvironmentVariableW(L"XBOX_WIN32_LOADER_TRACE", path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) return;
    const HANDLE file = CreateFile2FromAppW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text, size, &written, nullptr);
    CloseHandle(file);
}

// Count draws issued on the immediate context so traces can compare issued calls with executed work.
volatile LONG64 issuedDrawIndexed, issuedDraw;
using DrawIndexedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
DrawIndexedFn nativeDrawIndexed;
DrawFn nativeDraw;
void STDMETHODCALLTYPE CountDrawIndexed(ID3D11DeviceContext* context, UINT count, UINT start, INT base) {
    InterlockedIncrement64(&issuedDrawIndexed);
    nativeDrawIndexed(context, count, start, base);
}
void STDMETHODCALLTYPE CountDraw(ID3D11DeviceContext* context, UINT count, UINT start) {
    InterlockedIncrement64(&issuedDraw);
    nativeDraw(context, count, start);
}
void HookDrawCounts(ID3D11DeviceContext* context) {
    static volatile LONG hooked;
    if (InterlockedCompareExchange(&hooked, 1, 0)) return;
    void** table = *reinterpret_cast<void***>(context);
    DWORD protection = 0;
    // ID3D11DeviceContext vtable: DrawIndexed is slot 12 and Draw slot 13.
    if (!VirtualProtect(&table[12], 2 * sizeof(void*), PAGE_READWRITE, &protection)) return;
    nativeDrawIndexed = reinterpret_cast<DrawIndexedFn>(table[12]);
    nativeDraw = reinterpret_cast<DrawFn>(table[13]);
    table[12] = reinterpret_cast<void*>(&CountDrawIndexed);
    table[13] = reinterpret_cast<void*>(&CountDraw);
    VirtualProtect(&table[12], 2 * sizeof(void*), protection, &protection);
}

// Save a quarter-size BMP beside the loader trace so a run can be checked by eye within seconds.
void WriteProbeImage(const void* data, UINT pitch, UINT frameWidth, UINT frameHeight, DXGI_FORMAT format, UINT64 count, UINT buffer) {
    WCHAR path[512];
    DWORD length = GetEnvironmentVariableW(L"XBOX_WIN32_LOADER_TRACE", path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path) - 40) return;
    while (length && path[length - 1] != L'\\') --length;
    const WCHAR prefix[] = L"frame-probe-";
    for (const WCHAR* p = prefix; *p;) path[length++] = *p++;
    WCHAR digits[24]; int used = 0;
    UINT64 value = count;
    do { digits[used++] = static_cast<WCHAR>(L'0' + value % 10); value /= 10; } while (value);
    while (used) path[length++] = digits[--used];
    path[length++] = L'-';
    path[length++] = static_cast<WCHAR>(L'0' + buffer % 10);
    for (const WCHAR* p = L".bmp"; *p;) path[length++] = *p++;
    path[length] = 0;
    const UINT width = frameWidth / 4, height = frameHeight / 4, stride = (width * 3 + 3) & ~3u;
    const DWORD image = stride * height;
    BYTE header[54]{};
    auto put32 = [&header](unsigned at, DWORD v) { for (int i = 0; i < 4; ++i) header[at + i] = static_cast<BYTE>(v >> (8 * i)); };
    header[0] = 'B'; header[1] = 'M';
    put32(2, 54 + image); put32(10, 54); put32(14, 40); put32(18, width); put32(22, height);
    header[26] = 1; header[28] = 24; put32(34, image);
    BYTE* pixels = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, image));
    if (!pixels) return;
    const bool rgba = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    for (UINT y = 0; y < height; ++y) {
        const BYTE* line = static_cast<const BYTE*>(data) + static_cast<SIZE_T>(pitch) * (y * 4);
        BYTE* out = pixels + static_cast<SIZE_T>(stride) * (height - 1 - y);
        for (UINT x = 0; x < width; ++x) {
            const BYTE* pixel = line + 16 * x;
            out[3 * x] = rgba ? pixel[2] : pixel[0];
            out[3 * x + 1] = pixel[1];
            out[3 * x + 2] = rgba ? pixel[0] : pixel[2];
        }
    }
    const HANDLE file = CreateFile2FromAppW(path, GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, header, sizeof(header), &written, nullptr);
        WriteFile(file, pixels, image, &written, nullptr);
        CloseHandle(file);
    }
    HeapFree(GetProcessHeap(), 0, pixels);
}

// Trace the non-black share, mean and lit region of one mapped 8-bit RGBA/BGRA frame.
void ReportProbe(const void* data, UINT pitch, UINT width, UINT height, DXGI_FORMAT format, UINT64 count, UINT buffer) {
    UINT64 nonblack = 0, total = 0;
    UINT64 sum[3]{};
    UINT left = width, top = height, right = 0, bottom = 0, peak = 0;
    for (UINT row = 0; row < height; ++row) {
        const auto* line = static_cast<const BYTE*>(data) + static_cast<SIZE_T>(pitch) * row;
        for (UINT column = 0; column < width; ++column) {
            const BYTE* pixel = line + 4 * column;
            ++total;
            sum[0] += pixel[0]; sum[1] += pixel[1]; sum[2] += pixel[2];
            const UINT brightest = pixel[0] > pixel[1] ? (pixel[0] > pixel[2] ? pixel[0] : pixel[2]) : (pixel[1] > pixel[2] ? pixel[1] : pixel[2]);
            if (brightest > peak) peak = brightest;
            if (brightest <= 8) continue;
            ++nonblack;
            if (column < left) left = column;
            if (column > right) right = column;
            if (row < top) top = row;
            if (row > bottom) bottom = row;
        }
    }
    if (count <= 600) WriteProbeImage(data, pitch, width, height, format, count, buffer);
    char text[160], *out = text;
    auto put = [&out](const char* value) { while (*value) *out++ = *value++; };
    auto number = [&out](UINT64 value) {
        char digits[24]; int length = 0;
        do { digits[length++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
        while (length) *out++ = digits[--length];
    };
    put("dxgi frame-probe frame="); number(count); put(" buffer="); number(buffer);
    put(" nonblack="); number(nonblack); put("/"); number(total);
    put(" mean="); number(sum[0] / total); put(","); number(sum[1] / total); put(","); number(sum[2] / total);
    put(" peak="); number(peak);
    if (nonblack) {
        put(" box="); number(left); put(","); number(top); put("-"); number(right); put(","); number(bottom);
    }
    put(" format="); number(format);
    put("\r\n");
    AppendProbeTrace(text, static_cast<DWORD>(out - text));
}
}

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
        if (slot.queued12 > slot.submitted) {
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

HRESULT DxgiFramePublisher::Publish(IDXGISwapChain1* chain, UINT backBuffer) {
    failureStage_ = nullptr;
    if (status_ == E_PENDING) {
        status_ = OpenMailboxes();
        if (FAILED(status_)) return FailedAt("gpu-frame-mailbox-error", status_);
    }
    if (status_ != S_OK) return status_;
    if (InterlockedCompareExchange(&base_->quit, 0, 1) == 1) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    return queue_ ? Publish12(chain, backBuffer) : Publish11(chain);
}

HRESULT DxgiFramePublisher::FailedAt(const char* stage, HRESULT error) {
    failureStage_ = stage;
    return error;
}

HRESULT DxgiFramePublisher::Publish11(IDXGISwapChain1* chain) {
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
    const auto index = static_cast<unsigned>((next_ - 1) % SDL_GPU_SLOTS);
    auto& slot = slots_[index];
    const UINT64 ready = slot.ready->GetCompletedValue();
    const UINT64 done = slot.done->GetCompletedValue();
    if (ready == UINT64_MAX) return FailedAt("gpu-frame-producer-fence-error", DXGI_ERROR_DEVICE_REMOVED);
    if (done == UINT64_MAX) return FailedAt("gpu-frame-consumer-fence-error", DXGI_ERROR_DEVICE_REMOVED);
    if (ready < slot.submitted || done < slot.submitted) return S_FALSE;
    context_->CopyResource(slot.texture.Get(), source.Get());
    ProbeFrame(chain, desc);
    error = context_->Signal(slot.ready.Get(), next_);
    if (FAILED(error)) return FailedAt("gpu-frame-signal-error", error);
    context_->Flush();
    PublishSequence(index);
    return S_OK;
}


// Scan the constant buffers still bound from the frame's last draw for NaN or infinite floats.
// A camera with a broken transform or projection fails every frustum test, while post passes still run.
void DxgiFramePublisher::ProbeConstantBuffers(UINT64 count) {
    ComPtr<ID3D11Device> device;
    context_->GetDevice(&device);
    if (!device) return;
    for (int stage = 0; stage < 2; ++stage) {
        ID3D11Buffer* buffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{};
        if (stage == 0) context_->VSGetConstantBuffers(0, ARRAYSIZE(buffers), buffers);
        else context_->PSGetConstantBuffers(0, ARRAYSIZE(buffers), buffers);
        for (UINT slot = 0; slot < ARRAYSIZE(buffers); ++slot) {
            ComPtr<ID3D11Buffer> buffer;
            buffer.Attach(buffers[slot]);
            if (!buffer) continue;
            D3D11_BUFFER_DESC desc{};
            buffer->GetDesc(&desc);
            if (!desc.ByteWidth || desc.ByteWidth > 65536) continue;
            D3D11_BUFFER_DESC staging{};
            staging.ByteWidth = desc.ByteWidth;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Buffer> copy;
            if (FAILED(device->CreateBuffer(&staging, nullptr, &copy))) continue;
            context_->CopyResource(copy.Get(), buffer.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context_->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped))) continue;
            const UINT* words = static_cast<const UINT*>(mapped.pData);
            UINT nan = 0, inf = 0, first = 0xffffffffu;
            for (UINT i = 0; i < desc.ByteWidth / 4; ++i) {
                if ((words[i] & 0x7f800000u) != 0x7f800000u) continue;
                if (words[i] & 0x007fffffu) ++nan; else ++inf;
                if (first == 0xffffffffu) first = i;
            }
            context_->Unmap(copy.Get(), 0);
            char text[160], *out = text;
            auto put = [&out](const char* value) { while (*value) *out++ = *value++; };
            auto number = [&out](UINT64 value) {
                char digits[24]; int length = 0;
                do { digits[length++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
                while (length) *out++ = digits[--length];
            };
            put("dxgi constants frame="); number(count); put(stage ? " stage=ps" : " stage=vs");
            put(" slot="); number(slot); put(" bytes="); number(desc.ByteWidth);
            put(" nan="); number(nan); put(" inf="); number(inf);
            if (first != 0xffffffffu) { put(" first-float="); number(first); }
            put("\r\n");
            AppendProbeTrace(text, static_cast<DWORD>(out - text));
        }
    }
}

// Scan a few published frames to tell an all-black game image apart from a transport fault.
void DxgiFramePublisher::ProbeFrame(IDXGISwapChain1* chain, const D3D11_TEXTURE2D_DESC& desc) {
    static const UINT64 samples[] = {60, 600, 1800, 3600, 7200};
    const UINT64 count = ++published_;
    bool sample = false, before = false;
    for (UINT64 at : samples) {
        sample |= count == at;
        before |= count + 1 == at;
    }
    // Pipeline statistics for the single frame ending at each sample: whether the game issued GPU work at all.
    if (sample && statsActive_) {
        context_->End(stats_.Get());
        statsActive_ = false;
        D3D11_QUERY_DATA_PIPELINE_STATISTICS data{};
        HRESULT ready = S_FALSE;
        for (int attempt = 0; attempt < 200 && ready == S_FALSE; ++attempt) {
            ready = context_->GetData(stats_.Get(), &data, sizeof(data), 0);
            if (ready == S_FALSE) Sleep(1);
        }
        if (ready == S_OK) {
            char text[200], *out = text;
            auto put = [&out](const char* value) { while (*value) *out++ = *value++; };
            auto number = [&out](UINT64 value) {
                char digits[24]; int length = 0;
                do { digits[length++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
                while (length) *out++ = digits[--length];
            };
            put("dxgi frame-stats frame="); number(count);
            put(" vertices="); number(data.IAVertices);
            put(" vs="); number(data.VSInvocations);
            put(" primitives="); number(data.CPrimitives);
            put(" ps="); number(data.PSInvocations);
            put(" cs="); number(data.CSInvocations);
            put(" issued-indexed="); number(static_cast<UINT64>(InterlockedCompareExchange64(&issuedDrawIndexed, 0, 0)));
            put(" issued-draw="); number(static_cast<UINT64>(InterlockedCompareExchange64(&issuedDraw, 0, 0)));
            put("\r\n");
            AppendProbeTrace(text, static_cast<DWORD>(out - text));
        }
        ProbeConstantBuffers(count);
    }
    if (before) {
        if (!stats_) {
            ComPtr<ID3D11Device> device;
            context_->GetDevice(&device);
            // Engines size their camera targets from these formats; report what the device can render to.
            static const DXGI_FORMAT formats[] = {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
            for (DXGI_FORMAT format : formats) {
                UINT support = 0;
                const HRESULT checked = device ? device->CheckFormatSupport(format, &support) : E_POINTER;
                char text[120], *out = text;
                auto put = [&out](const char* value) { while (*value) *out++ = *value++; };
                auto number = [&out](UINT64 value) {
                    char digits[24]; int length = 0;
                    do { digits[length++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
                    while (length) *out++ = digits[--length];
                };
                put("dxgi format-support format="); number(format);
                put(" result="); number(static_cast<UINT>(checked));
                put(" render-target="); number((support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0);
                put(" depth-stencil="); number((support & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL) != 0);
                put(" texture2d="); number((support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0);
                put(" blendable="); number((support & D3D11_FORMAT_SUPPORT_BLENDABLE) != 0);
                put("\r\n");
                AppendProbeTrace(text, static_cast<DWORD>(out - text));
            }
            const D3D11_QUERY_DESC query{D3D11_QUERY_PIPELINE_STATISTICS, 0};
            if (device) device->CreateQuery(&query, &stats_);
        }
        if (stats_) {
            HookDrawCounts(context_.Get());
            InterlockedExchange64(&issuedDrawIndexed, 0);
            InterlockedExchange64(&issuedDraw, 0);
            context_->Begin(stats_.Get());
            statsActive_ = true;
        }
    }
    if (!sample || (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
            desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) return;
    // Scan every buffer: a renderer that writes a buffer other than 0 shows up as a lit buffer here.
    DXGI_SWAP_CHAIN_DESC1 chainDesc{};
    const UINT buffers = SUCCEEDED(chain->GetDesc1(&chainDesc)) && chainDesc.BufferCount <= 8 ? chainDesc.BufferCount : 1;
    for (UINT buffer = 0; buffer < buffers; ++buffer) {
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(chain->GetBuffer(buffer, IID_PPV_ARGS(&texture)))) ProbeBuffer(texture.Get(), desc, count, buffer);
    }
}

void DxgiFramePublisher::ProbeBuffer(ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& desc, UINT64 count, UINT buffer) {
    const DWORD previous = GetLastError();
    if (!probe_) {
        ComPtr<ID3D11Device> device;
        source->GetDevice(&device);
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        if (!device || FAILED(device->CreateTexture2D(&staging, nullptr, &probe_))) {
            SetLastError(previous);
            return;
        }
    }
    context_->CopyResource(probe_.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(probe_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        SetLastError(previous);
        return;
    }
    ReportProbe(mapped.pData, mapped.RowPitch, desc.Width, desc.Height, desc.Format, count, buffer);
    context_->Unmap(probe_.Get(), 0);
    SetLastError(previous);
}

HRESULT DxgiFramePublisher::Publish12(IDXGISwapChain1* chain, UINT backBuffer) {
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
    const auto index = static_cast<unsigned>((next_ - 1) % SDL_GPU_SLOTS);
    auto& slot = slots_[index];
    const UINT64 ready = slot.ready12->GetCompletedValue();
    const UINT64 done = slot.done12->GetCompletedValue();
    if (ready == UINT64_MAX) return FailedAt("gpu-frame-producer-fence-error", DXGI_ERROR_DEVICE_REMOVED);
    if (done == UINT64_MAX) return FailedAt("gpu-frame-consumer-fence-error", DXGI_ERROR_DEVICE_REMOVED);
    if (ready < slot.submitted || done < slot.submitted) return S_FALSE;

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

    slot.source12 = std::move(source);
    ID3D12CommandList* commands[] = {slot.commands.Get()};
    slot.queued12 = next_;
    queue_->ExecuteCommandLists(ARRAYSIZE(commands), commands);
    error = queue_->Signal(slot.ready12.Get(), next_);
    if (FAILED(error)) return FailedAt("gpu-frame-signal-error", status_ = error);
    Probe12(slot.source12.Get(), desc);
    PublishSequence(index);
    return S_OK;
}

// D3D12 counterpart of the D3D11 probe: read the presented back buffer through a readback buffer.
void DxgiFramePublisher::Probe12(ID3D12Resource* source, const D3D12_RESOURCE_DESC& desc) {
    static const UINT64 samples[] = {60, 600, 1800, 3600, 7200};
    const UINT64 count = ++published_;
    bool sample = false;
    for (UINT64 at : samples) sample |= count == at;
    if (!sample || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
             desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) return;
    const DWORD previous = GetLastError();
    ComPtr<ID3D12Device> device;
    if (FAILED(source->GetDevice(IID_PPV_ARGS(&device)))) { SetLastError(previous); return; }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    if (!probe12_) {
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&probe12_))) ||
                FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&probeAllocator_))) ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, probeAllocator_.Get(), nullptr,
                    IID_PPV_ARGS(&probeCommands_))) ||
                FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&probeFence_)))) {
            probe12_.Reset();
            SetLastError(previous);
            return;
        }
        probeCommands_->Close();
    }
    if (FAILED(probeAllocator_->Reset()) || FAILED(probeCommands_->Reset(probeAllocator_.Get(), nullptr))) {
        SetLastError(previous);
        return;
    }
    const D3D12_RESOURCE_BARRIER before = Transition(source, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const D3D12_RESOURCE_BARRIER after = Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = probe12_.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = source;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = 0;
    probeCommands_->ResourceBarrier(1, &before);
    probeCommands_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    probeCommands_->ResourceBarrier(1, &after);
    if (FAILED(probeCommands_->Close())) { SetLastError(previous); return; }
    ID3D12CommandList* lists[] = {probeCommands_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    const UINT64 value = ++probeFenceValue_;
    const HANDLE done = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (done && SUCCEEDED(queue_->Signal(probeFence_.Get(), value)) &&
            SUCCEEDED(probeFence_->SetEventOnCompletion(value, done)) && WaitForSingleObject(done, 2000) == WAIT_OBJECT_0) {
        void* data = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
        if (SUCCEEDED(probe12_->Map(0, &range, &data))) {
            ReportProbe(static_cast<const BYTE*>(data) + footprint.Offset, footprint.Footprint.RowPitch,
                static_cast<UINT>(desc.Width), desc.Height, desc.Format, count, 0);
            const D3D12_RANGE none{0, 0};
            probe12_->Unmap(0, &none);
        }
    }
    if (done) CloseHandle(done);
    SetLastError(previous);
}

void DxgiFramePublisher::PublishSequence(unsigned index) {
    auto& slot = slots_[index];
    slot.submitted = next_++;
    InterlockedExchange64(&gpu_->sequence[index], static_cast<LONG64>(slot.submitted));
    InterlockedExchange(&gpu_->gpu_active, 1);
    InterlockedExchange(&base_->frame_sequence, 1);
}
