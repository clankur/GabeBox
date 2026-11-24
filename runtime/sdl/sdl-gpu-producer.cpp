// The pinned Mesa D3D12 EGL driver exposes its graphics queue and GL renderbuffer.
// Copy the completed GL framebuffer on that queue into two same-package textures.
#include <windows.h>
#include <d3d12.h>
#include <new>
#include <type_traits>

#include "SDL_stdinc.h"
#include "SDL_loadso.h"
#include "../../sdl-startup-trace.h"
#include "sdl-gpu-protocol.h"
#include "mesa-glinterop.h"

namespace {
struct MesaDevice { uint64_t luid; ID3D12Device *device; ID3D12CommandQueue *queue; };
struct MesaResource { ID3D12Resource *resource; uint64_t offset; };
typedef void (APIENTRY *GlFlush)();
template<class T> void release(T *&v) { if (v) { v->Release(); v = nullptr; } }

struct PreserveLastError {
    DWORD value = GetLastError();
    ~PreserveLastError() { SetLastError(value); }
};

struct Slot {
    ID3D12Resource *texture = nullptr, *source = nullptr;
    ID3D12Fence *ready = nullptr, *done = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    HANDLE named[3] = {};
    UINT64 submitted = 0;
    void close() {
        for (HANDLE handle : named) if (handle) CloseHandle(handle);
        release(list); release(allocator); release(source); release(texture); release(ready); release(done);
    }
};
struct Producer {
    SdlGpuMailbox *box = nullptr;
    void *display = nullptr, *context = nullptr;
    decltype(&MesaGLInteropEGLExportObject) export_object = nullptr;
    decltype(&MesaGLInteropEGLFlushObjects) flush_objects = nullptr;
    GlFlush gl_flush = nullptr;
    MesaDevice mesa = {};
    Slot slots[SDL_GPU_SLOTS];
    UINT64 next = 1;
    LONG generation = 0;
    bool failed = false;
    bool traceStartup = false, tracedCopy = false;
    unsigned startupSwaps = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    int width = 0, height = 0;
    void close() {
        if (generation) {
            InterlockedExchange(&box->gpu_active, 0);
            InterlockedExchange(&box->accepted_generation, 0);
            InterlockedExchange(&box->generation, 0);
        }
        // SetEventOnCompletion(NULL) synchronously drains this producer's own
        // command lists without waiting for the other process to release a slot.
        for (auto &s : slots)
            if (s.submitted && s.ready->GetCompletedValue() < s.submitted)
                s.ready->SetEventOnCompletion(s.submitted, nullptr);
        for (auto &s : slots) s.close();
        if (box) UnmapViewOfFile(box);
        release(mesa.queue); release(mesa.device);
    }
};
// SDL's Windows DLL deliberately omits the C++ runtime. Keep heap ownership
// in SDL, including array cleanup, without compiler-generated deleting destructors.
static_assert(std::is_trivially_destructible<Producer>::value, "Producer must only own explicitly released state");
static volatile LONG generation_counter;

bool fail(Producer *p, const char *stage, HRESULT code) {
    char message[128];
    SDL_snprintf(message, sizeof(message), "sdl_gpu_failure=%s:0x%08x\n", stage, (unsigned)code);
    XboxSdlTrace(message);
    p->failed = true;
    if (p->generation) {
        InterlockedExchange(&p->box->gpu_active, 0);
        InterlockedExchange(&p->box->generation, -p->generation);
    }
    return false;
}
SdlGpuMailbox *open_mailbox(int *retry) {
    WCHAR path[1024];
    DWORD n = GetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", path, ARRAYSIZE(path));
    if (!n || n + 5 > ARRAYSIZE(path)) return nullptr;
    SDL_memcpy(path + n, L".gpu", 5 * sizeof(WCHAR));
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD reason = GetLastError();
        *retry = reason == ERROR_FILE_NOT_FOUND || reason == ERROR_PATH_NOT_FOUND || reason == ERROR_SHARING_VIOLATION;
        return nullptr;
    }
    LARGE_INTEGER size = {};
    bool sized = GetFileSizeEx(file, &size) != 0;
    *retry = sized && !size.QuadPart; // presenter has just created the file
    HANDLE mapping = sized && size.QuadPart == sizeof(SdlGpuMailbox)
        ? CreateFileMappingFromApp(file, nullptr, PAGE_READWRITE, 0, nullptr) : nullptr;
    CloseHandle(file);
    if (!mapping) return nullptr;
    auto *box = static_cast<SdlGpuMailbox *>(MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, sizeof(SdlGpuMailbox)));
    CloseHandle(mapping);
    if (box) {
        LONG magic = InterlockedCompareExchange(&box->magic, 0, 0);
        if (magic != SDL_GPU_MAGIC) {
            *retry = magic == 0;
            UnmapViewOfFile(box);
            return nullptr;
        }
    }
    return box;
}
mesa_glinterop_export_in object(unsigned color) {
    mesa_glinterop_export_in in = {};
    in.version = 2;
    in.target = 0x8D41; // GL_RENDERBUFFER
    in.obj = color;
    in.access = MESA_GLINTEROP_ACCESS_READ_ONLY;
    return in;
}
ID3D12Resource *export_color(Producer *p, unsigned color) {
    auto in = object(color);
    MesaResource resource = {};
    in.out_driver_data = &resource;
    in.out_driver_data_size = sizeof(resource);
    mesa_glinterop_export_out out = {};
    out.version = 1;
    int result = p->export_object(p->display, p->context, &in, &out);
    // Another driver may export an NT handle in lieu of D3D12 driver data.
    if (out.win32_handle && out.win32_handle != INVALID_HANDLE_VALUE) CloseHandle(out.win32_handle);
    return !result && out.out_driver_data_written == sizeof(resource) && !resource.offset ? resource.resource : nullptr;
}
DXGI_FORMAT exchange_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
HRESULT create_slot(Producer *p, Slot &s, D3D12_RESOURCE_DESC desc, unsigned i, const WCHAR *guid) {
    auto *device = p->mesa.device;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                  nullptr, IID_PPV_ARGS(&s.texture));
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.ready));
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.done));
    if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator));
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator,
                                                       nullptr, IID_PPV_ARGS(&s.list));
    if (SUCCEEDED(hr)) hr = s.list->Close();
    ID3D12DeviceChild *shared[] = {s.texture, s.ready, s.done};
    for (unsigned j = 0; SUCCEEDED(hr) && j < 3; ++j) {
        auto &name = p->box->names[i][j];
        SDL_wcslcpy(name, L"xbox-sdl-", ARRAYSIZE(name));
        SDL_wcslcat(name, guid, ARRAYSIZE(name));
        WCHAR suffix[] = { L'-', (WCHAR)(L'0' + i), L'-', (WCHAR)(L'0' + j), 0 };
        SDL_wcslcat(name, suffix, ARRAYSIZE(name));
        hr = device->CreateSharedHandle(shared[j], nullptr, GENERIC_ALL, name, &s.named[j]);
    }
    return hr;
}
bool initialize(Producer *p, void *egl, void *display, void *context, void *flush, unsigned color, int w, int h, int *retry) {
    if (!egl || !display || !context || !flush) return fail(p, "context", E_NOINTERFACE);
    auto query = reinterpret_cast<decltype(&MesaGLInteropEGLQueryDeviceInfo)>(SDL_LoadFunction(egl, "MesaGLInteropEGLQueryDeviceInfo"));
    p->export_object = reinterpret_cast<decltype(p->export_object)>(SDL_LoadFunction(egl, "MesaGLInteropEGLExportObject"));
    p->flush_objects = reinterpret_cast<decltype(p->flush_objects)>(SDL_LoadFunction(egl, "MesaGLInteropEGLFlushObjects"));
    if (!query || !p->export_object || !p->flush_objects) return fail(p, "egl-exports", E_NOINTERFACE);
    p->display = display; p->context = context; p->gl_flush = reinterpret_cast<GlFlush>(flush);
    MesaDevice driver = {};
    mesa_glinterop_device_info info = {};
    info.version = 4; info.driver_data_size = sizeof(driver); info.driver_data = &driver;
    if (query(display, context, &info) || info.driver_data_size != sizeof(driver) || !driver.device || !driver.queue)
        return fail(p, "mesa-device", E_NOINTERFACE);
    p->mesa = driver; p->mesa.device->AddRef(); p->mesa.queue->AddRef();
    auto *source = export_color(p, color);
    if (!source) return fail(p, "mesa-renderbuffer", E_NOINTERFACE);
    D3D12_RESOURCE_DESC desc = source->GetDesc();
    desc.Format = exchange_format(desc.Format);
    if (desc.Format == DXGI_FORMAT_UNKNOWN || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
        desc.Width != (UINT64)w || desc.Height != (UINT)h) return fail(p, "texture-layout", E_INVALIDARG);
    // Keep optional interop independent of extra static imports on older Xbox builds.
    using CreateGuid = HRESULT (WINAPI *)(GUID *);
    auto ole = GetModuleHandleW(L"ole32.dll");
    auto create_guid = ole ? reinterpret_cast<CreateGuid>(GetProcAddress(ole, "CoCreateGuid")) : nullptr;
    GUID token;
    if (!create_guid || FAILED(create_guid(&token))) return fail(p, "name", E_NOINTERFACE);
    WCHAR guid[2 * sizeof(token) + 1] = {};
    const WCHAR hex[] = L"0123456789abcdef";
    auto bytes = reinterpret_cast<const unsigned char *>(&token);
    for (unsigned i = 0; i < sizeof(token); ++i) {
        guid[2 * i] = hex[bytes[i] >> 4];
        guid[2 * i + 1] = hex[bytes[i] & 15];
    }
    if (InterlockedCompareExchange(&p->box->generation, 1, 0) != 0) {
        *retry = 1;
        return false;
    }
    p->generation = InterlockedAdd(&generation_counter, 2);
    InterlockedExchange(&p->box->accepted_generation, 0);
    p->box->adapter_luid = p->mesa.luid;
    p->box->width = p->width = w; p->box->height = p->height = h;
    p->box->dxgi_format = p->format = desc.Format;
    p->box->flip_y = 1;
    // Build a normal committed exchange texture independent of Mesa allocation details.
    desc.Alignment = 0; desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    for (unsigned i = 0; i < SDL_GPU_SLOTS; ++i) {
        InterlockedExchange64(&p->box->sequence[i], 0);
        p->box->cursor[i].shape = 0;
        HRESULT hr = create_slot(p, p->slots[i], desc, i, guid);
        if (FAILED(hr)) return fail(p, "share-slot", hr);
    }
    InterlockedExchange(&p->box->generation, p->generation);
    XboxSdlTrace("sdl_gpu_interop=published\n");
    return true;
}
void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = r;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

bool producerReady(Producer *p, bool hostBound, Uint32 shape, int w, int h, const void *rgba) {
    const bool followUp = p->traceStartup && p->startupSwaps == 1;
    if (p->traceStartup && p->startupSwaps < 2) ++p->startupSwaps;
    if (!hostBound && !followUp) return false;

    const bool accepted = InterlockedCompareExchange(&p->box->accepted_generation, 0, 0) == p->generation;
    const bool heartbeat = (accepted || followUp) &&
        (DWORD)(GetTickCount() - (DWORD)InterlockedCompareExchange(&p->box->heartbeat_ms, 0, 0)) <= 1000;
    const bool cursor = (heartbeat || followUp) &&
        (!shape || (rgba && w >= 1 && h >= 1 && w <= SDL_GPU_CURSOR_SIZE && h <= SDL_GPU_CURSOR_SIZE &&
                   InterlockedCompareExchange(&p->box->supports_cursor, 0, 0)));
    if (followUp) {
        const PreserveLastError preserve;
        char line[128];
        SDL_snprintf(line, sizeof(line), "sdl_gpu_followup=host_bound=%d accepted=%d heartbeat_fresh=%d cursor_supported=%d\n",
                     hostBound, accepted, heartbeat, cursor);
        XboxSdlTrace(line);
    }
    if (!hostBound) return false;
    if (accepted && heartbeat && cursor) return true;
    InterlockedExchange(&p->box->gpu_active, 0);
    return false;
}
} // namespace

extern "C" void XboxSdlDestroyGpu(void *state) {
    if (!state) return;
    const PreserveLastError preserve;
    auto *p = static_cast<Producer *>(state);
    p->close();
    p->~Producer();
    SDL_free(p);
}

extern "C" void *XboxSdlCreateGpu(void *egl, void *display, void *context, void *flush, unsigned color, int width, int height, int *retry) {
    const PreserveLastError preserve;
    *retry = 0;
    auto *box = open_mailbox(retry);
    if (!box) return nullptr;
    // Contention can outlast a frame. Retry without exporting a GL renderbuffer
    // or touching the generation still held by the other context.
    if (InterlockedCompareExchange(&box->generation, 0, 0) != 0) {
        *retry = 1;
        UnmapViewOfFile(box);
        return nullptr;
    }
    void *storage = SDL_malloc(sizeof(Producer));
    if (!storage) { UnmapViewOfFile(box); return nullptr; }
    auto *p = new(storage) Producer;
    p->box = box;
    p->traceStartup = GetEnvironmentVariableW(L"XBOX_SDL_TRACE_FILE", nullptr, 0) > 1;
    if (!initialize(p, egl, display, context, flush, color, width, height, retry)) { XboxSdlDestroyGpu(p); return nullptr; }
    return p;
}
extern "C" int XboxSdlGpuFrame(void *state, int host_bound, unsigned color, Uint32 shape, int x, int y,
                              int w, int h, int pitch, const void *rgba) {
    auto *p = static_cast<Producer *>(state);
    if (!p || p->failed || !producerReady(p, host_bound != 0, shape, w, h, rgba)) return 0;
    unsigned i = (unsigned)((p->next - 1) % SDL_GPU_SLOTS);
    Slot &s = p->slots[i];
    UINT64 ready = s.ready->GetCompletedValue(), done = s.done->GetCompletedValue();
    if (ready == UINT64_MAX || done == UINT64_MAX) return fail(p, "device-removed", DXGI_ERROR_DEVICE_REMOVED);
    if (ready < s.submitted || done < s.submitted)
        return InterlockedCompareExchange(&p->box->gpu_active, 0, 0) != 0;
    release(s.source);
    auto in = object(color);
    // Pinned Mesa transitions to COMMON, glFlush submits it on the exported
    // queue. ExportObject also waits for Mesa's GL dispatch thread to finish.
    const bool traceCopy = p->traceStartup && !p->tracedCopy;
    if (traceCopy) {
        p->tracedCopy = true;
        XboxSdlTrace("sdl_gpu_mesa_flush=begin\n");
    }
    int rc = p->flush_objects(p->display, p->context, 1, &in, nullptr);
    if (traceCopy) XboxSdlTrace("sdl_gpu_mesa_flush=returned\n");
    if (rc) return fail(p, "mesa-flush", rc);
    if (traceCopy) XboxSdlTrace("sdl_gpu_gl_flush=begin\n");
    p->gl_flush();
    if (traceCopy) {
        XboxSdlTrace("sdl_gpu_gl_flush=returned\n");
        XboxSdlTrace("sdl_gpu_mesa_export=begin\n");
    }
    auto *source = export_color(p, color);
    if (traceCopy) XboxSdlTrace("sdl_gpu_mesa_export=returned\n");
    if (!source) return fail(p, "mesa-export", E_NOINTERFACE);
    auto source_desc = source->GetDesc();
    if (source_desc.Width != (UINT64)p->width || source_desc.Height != (UINT)p->height ||
        source_desc.SampleDesc.Count != 1 || exchange_format(source_desc.Format) != p->format)
        return fail(p, "source-changed", E_INVALIDARG);
    source->AddRef(); s.source = source;
    HRESULT hr = s.allocator->Reset();
    if (SUCCEEDED(hr)) hr = s.list->Reset(s.allocator, nullptr);
    if (FAILED(hr)) return fail(p, "reset", hr);
    transition(s.list, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(s.list, s.texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    s.list->CopyResource(s.texture, source);
    transition(s.list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    transition(s.list, s.texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    hr = s.list->Close();
    if (FAILED(hr)) return fail(p, "close-list", hr);
    ID3D12CommandList *lists[] = {s.list};
    p->mesa.queue->ExecuteCommandLists(1, lists);
    hr = p->mesa.queue->Signal(s.ready, p->next);
    if (FAILED(hr)) return fail(p, "signal", hr);
    s.submitted = p->next++;
    auto &cursor = p->box->cursor[i];
    if (shape && cursor.shape != shape) {
        auto *bytes = static_cast<const Uint8 *>(rgba);
        for (int row = 0; row < h; ++row)
            SDL_memcpy(cursor.rgba + row * w * 4, bytes + row * pitch, w * 4);
    }
    cursor.shape = shape;
    cursor.x = x; cursor.y = y;
    cursor.width = w; cursor.height = h;
    InterlockedExchange64(&p->box->sequence[i], (LONG64)s.submitted);
    InterlockedExchange(&p->box->gpu_active, 1);
    if (s.submitted == 1) XboxSdlTrace("sdl_gpu_first_copy=submitted\n");
    return 1;
}
