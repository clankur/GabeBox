#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dcomp.h>

#ifndef DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED
#define DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED static_cast<HRESULT>(0x88980800L)
#endif
#include <appmodel.h>
#include <new>
#include "dxgi-frame.h"
#include "gpu-memory-budget.h"
#include "../shared/sdl-swap-clock.h"
#include "../shared/steam-handoff-control.h"

extern "C" __declspec(dllimport) HANDLE WINAPI CreateFile2FromAppW(
    LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);

namespace {

using Microsoft::WRL::ComPtr;

bool SameInterface(REFIID first, REFIID second) {
    if (first.Data1 != second.Data1 || first.Data2 != second.Data2 || first.Data3 != second.Data3) return false;
    for (unsigned i = 0; i < ARRAYSIZE(first.Data4); ++i)
        if (first.Data4[i] != second.Data4[i]) return false;
    return true;
}

// Private identity of a hosted swap chain, used by the hosted composition
// device to bind a chain to the virtual window it is shown in.
constexpr GUID hostedSwapChainId = {0x4c3b6e8a, 0x5b5f, 0x4e33, {0x9d, 0x2a, 0x7e, 0x0b, 0x1d, 0x3c, 0x9a, 0x41}};

bool VirtualWindowsActive() { return GetModuleHandleW(L"XboxWin32Window.dll") != nullptr; }

bool VirtualClientRect(HWND window, RECT& area) {
    using Query = BOOL (WINAPI *)(HWND, RECT*);
    auto windows = GetModuleHandleW(L"XboxWin32Window.dll");
    auto query = windows ? reinterpret_cast<Query>(GetProcAddress(windows, "GetClientRect")) : nullptr;
    if (!query || !window) return false;
    const DWORD error = GetLastError();
    const bool found = query(window, &area) != FALSE;
    SetLastError(error);
    return found;
}

void AppendTrace(const char* text, DWORD size) {
    const DWORD error = GetLastError();
    WCHAR path[512];
    const DWORD length = GetEnvironmentVariableW(L"XBOX_WIN32_LOADER_TRACE", path, ARRAYSIZE(path));
    if (length && length < ARRAYSIZE(path)) {
        const HANDLE file = CreateFile2FromAppW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            OPEN_ALWAYS, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, text, size, &written, nullptr);
            CloseHandle(file);
        }
    }
    SetLastError(error);
}

void Trace(const char* phase, HRESULT code) {
    char line[100], *out = line;
    for (auto prefix = "dxgi "; *prefix;) *out++ = *prefix++;
    while (*phase) *out++ = *phase++;
    for (auto label = " result=0x"; *label;) *out++ = *label++;
    for (int shift = 28; shift >= 0; shift -= 4) *out++ = "0123456789ABCDEF"[(DWORD(code) >> shift) & 15];
    *out++ = '\r';
    *out++ = '\n';
    AppendTrace(line, static_cast<DWORD>(out - line));
}

#include "dxgi-memory-budget.h"

template<size_t N>
bool HasPackageMarker(const WCHAR (&suffix)[N]) {
    WCHAR path[1024];
    UINT32 length = ARRAYSIZE(path);
    if (GetCurrentPackagePath(&length, path) != ERROR_SUCCESS || !length ||
            length - 1 + N > ARRAYSIZE(path)) return false;
    for (size_t i = 0; i < N; ++i) path[length - 1 + i] = suffix[i];
    const HANDLE marker = CreateFile2FromAppW(path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, nullptr);
    if (marker == INVALID_HANDLE_VALUE) return false;
    CloseHandle(marker);
    return true;
}

class ProducerCadenceTrace {
    static constexpr LONG maxRecords = 48;
    inline static volatile LONG records_ = 0;
    inline static volatile LONG stallRecords_ = 0;
    UINT64 frequency_ = 0, start_ = 0, last_ = 0, maxGap_ = 0;
    UINT64 previousReturn_ = 0;
    UINT attempts_ = 0, submitted_ = 0, busy_ = 0;
    UINT immediate_ = 0, syncOne_ = 0;
    bool aggregate_ = true;

    static void Field(char*& out, const char* label, UINT64 value) {
        while (*label) *out++ = *label++;
        char digits[20];
        unsigned count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        while (count) *out++ = digits[--count];
    }

public:
    struct PresentTiming {
        UINT64 entered = 0, locked = 0, split = 0, prepared = 0, returned = 0;
        bool pacerFirst = false;
    };

    void Enable() {
        LARGE_INTEGER frequency{};
        if (GetEnvironmentVariableW(L"XBOX_WIN32_LOADER_TRACE", nullptr, 0) &&
                QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
            frequency_ = static_cast<UINT64>(frequency.QuadPart);
    }

    bool Enabled() const { return frequency_ != 0; }

    UINT64 Timestamp() const {
        if (!frequency_) return 0;
        LARGE_INTEGER counter{};
        const DWORD previous = GetLastError();
        const BOOL sampled = QueryPerformanceCounter(&counter);
        SetLastError(previous);
        return sampled && counter.QuadPart > 0 ? static_cast<UINT64>(counter.QuadPart) : 0;
    }

    void RecordPresent(const PresentTiming& timing, bool active) {
        if (!timing.entered || !timing.locked || !timing.split || !timing.prepared || !timing.returned || !active) {
            previousReturn_ = 0;
            return;
        }
        const UINT64 begin = previousReturn_ && previousReturn_ < timing.entered ? previousReturn_ : timing.entered;
        previousReturn_ = timing.returned;
        if (timing.returned - begin < frequency_ / 20 || stallRecords_ >= maxRecords ||
                InterlockedIncrement(&stallRecords_) > maxRecords) return;
        // The loader's report also accepts this line at maximum QPC width.
        char line[182], *out = line;
        Field(out, "dxgi stall qpc=", begin);
        Field(out, " enter=", timing.entered);
        Field(out, " lock=", timing.locked);
        Field(out, " split=", timing.split);
        Field(out, " present=", timing.prepared);
        Field(out, " end=", timing.returned);
        Field(out, " prepace=", timing.pacerFirst);
        *out++ = '\r';
        *out++ = '\n';
        AppendTrace(line, static_cast<DWORD>(out - line));
        // Exclude diagnostic file I/O from the next call's measured interval.
        previousReturn_ = Timestamp();
    }

    void Record(HRESULT result, bool active, UINT interval, UINT flags) {
        if (!aggregate_) return;
        const auto now = Timestamp();
        if (!now) return;
        if (!start_) start_ = now;
        if (last_ && now - last_ > maxGap_) maxGap_ = now - last_;
        last_ = now;
        ++attempts_;
        if (result == S_OK) ++submitted_;
        if (result == S_FALSE && active) ++busy_;
        if (!flags && interval == 0) ++immediate_;
        if (!flags && interval == 1) ++syncOne_;
        if (now - start_ >= frequency_ * 5) Flush();
    }

    void Flush() {
        if (!attempts_ || !aggregate_) return;
        if (InterlockedIncrement(&records_) > maxRecords) {
            aggregate_ = false;
            return;
        }
        char line[240], *out = line;
        Field(out, "dxgi cadence end-qpc=", last_);
        Field(out, " elapsed-ms=", (last_ - start_) * 1000 / frequency_);
        Field(out, " attempts=", attempts_);
        Field(out, " submitted=", submitted_);
        Field(out, " slot-busy=", busy_);
        Field(out, " max-gap-ms=", maxGap_ * 1000 / frequency_);
        Field(out, " immediate=", immediate_);
        Field(out, " sync-one=", syncOne_);
        *out++ = '\r';
        *out++ = '\n';
        AppendTrace(line, static_cast<DWORD>(out - line));
        start_ = last_;
        maxGap_ = 0;
        attempts_ = submitted_ = busy_ = 0;
        immediate_ = syncOne_ = 0;
    }
};

// Diagnostic for measured packages: record when a hosted D3D12 device is
// removed relative to the frames presented and published, so a removal can be
// attributed to the game's own GPU work or to the hosted frame copy.
volatile LONG hostedPresents = 0, hostedPublishes = 0;

struct RemovalWatch {
    ID3D12Device* device;
    UINT64 start;
};

char* TraceText(char* out, const char* end, const char* text) {
    while (text && *text && out < end) *out++ = *text++;
    return out;
}

char* TraceName(char* out, const char* end, const char* narrow, const wchar_t* wide) {
    if (narrow && *narrow) return TraceText(out, end, narrow);
    if (!wide || !*wide) return TraceText(out, end, "-");
    for (; *wide && out < end; ++wide) *out++ = *wide < 0x80 && *wide > 0x20 ? static_cast<char>(*wide) : '_';
    return out;
}

char* TraceHex(char* out, const char* end, UINT64 value, int digits) {
    for (int shift = (digits - 1) * 4; shift >= 0 && out < end; shift -= 4) *out++ = "0123456789ABCDEF"[(value >> shift) & 15];
    return out;
}

// Reports D3D12 device-removed extended data (enabled by loader trace before the
// device was created): every command list whose breadcrumbs did not all
// complete, with the last completed and the first incomplete operations, and
// the allocations that match a GPU page fault.
void TraceRemovedData(ID3D12Device* device) {
    ComPtr<ID3D12DeviceRemovedExtendedData> data;
    HRESULT result = device->QueryInterface(IID_PPV_ARGS(data.GetAddressOf()));
    Trace("dred-query", result);
    if (FAILED(result)) return;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
    result = data->GetAutoBreadcrumbsOutput(&crumbs);
    Trace("dred-breadcrumbs", result);
    int reported = 0;
    for (auto* node = SUCCEEDED(result) ? crumbs.pHeadAutoBreadcrumbNode : nullptr; node && reported < 12; node = node->pNext) {
        const UINT32 done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
        if (!node->pCommandHistory || done >= node->BreadcrumbCount) continue;
        ++reported;
        char line[400], *out = line;
        const char* end = line + sizeof(line) - 4;
        out = TraceText(out, end, "dxgi dred-node queue=");
        out = TraceName(out, end, node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW);
        out = TraceText(out, end, " list=");
        out = TraceName(out, end, node->pCommandListDebugNameA, node->pCommandListDebugNameW);
        out = TraceText(out, end, " completed=0x");
        out = TraceHex(out, end, done, 4);
        out = TraceText(out, end, "/0x");
        out = TraceHex(out, end, node->BreadcrumbCount, 4);
        out = TraceText(out, end, " ops=");
        const UINT32 first = done > 6 ? done - 6 : 0;
        const UINT32 last = done + 4 < node->BreadcrumbCount ? done + 4 : node->BreadcrumbCount;
        for (UINT32 i = first; i < last; ++i) {
            if (i == done) out = TraceText(out, end, "[");
            out = TraceHex(out, end, node->pCommandHistory[i], 2);
            out = TraceText(out, end, i == done ? "] " : " ");
        }
        *out++ = '\r';
        *out++ = '\n';
        AppendTrace(line, static_cast<DWORD>(out - line));
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    result = data->GetPageFaultAllocationOutput(&fault);
    Trace("dred-page-fault", result);
    if (FAILED(result)) return;
    char line[400], *out = line;
    const char* end = line + sizeof(line) - 4;
    out = TraceText(out, end, "dxgi dred-fault va=0x");
    out = TraceHex(out, end, fault.PageFaultVA, 16);
    int listed = 0;
    for (auto* node = fault.pHeadExistingAllocationNode; node && listed < 4; node = node->pNext, ++listed) {
        out = TraceText(out, end, " existing=");
        out = TraceName(out, end, node->ObjectNameA, node->ObjectNameW);
        out = TraceText(out, end, ":0x");
        out = TraceHex(out, end, static_cast<UINT64>(node->AllocationType), 2);
    }
    listed = 0;
    for (auto* node = fault.pHeadRecentFreedAllocationNode; node && listed < 4; node = node->pNext, ++listed) {
        out = TraceText(out, end, " freed=");
        out = TraceName(out, end, node->ObjectNameA, node->ObjectNameW);
        out = TraceText(out, end, ":0x");
        out = TraceHex(out, end, static_cast<UINT64>(node->AllocationType), 2);
    }
    *out++ = '\r';
    *out++ = '\n';
    AppendTrace(line, static_cast<DWORD>(out - line));
}

DWORD WINAPI WatchDeviceRemoval(void* parameter) {
    auto* watch = static_cast<RemovalWatch*>(parameter);
    LARGE_INTEGER frequency{}, now{};
    QueryPerformanceFrequency(&frequency);
    for (int i = 0; i < 180 * 50; ++i) {
        const HRESULT reason = watch->device->GetDeviceRemovedReason();
        if (reason != S_OK) {
            QueryPerformanceCounter(&now);
            Trace("device-removed-ms", static_cast<HRESULT>((now.QuadPart - watch->start) * 1000 / frequency.QuadPart));
            Trace("device-removed-reason", reason);
            Trace("device-removed-presents", static_cast<HRESULT>(InterlockedCompareExchange(&hostedPresents, 0, 0)));
            Trace("device-removed-publishes", static_cast<HRESULT>(InterlockedCompareExchange(&hostedPublishes, 0, 0)));
            TraceRemovedData(watch->device);
            break;
        }
        Sleep(20);
    }
    watch->device->Release();
    HeapFree(GetProcessHeap(), 0, watch);
    return 0;
}

void StartRemovalWatch(ID3D12CommandQueue* queue) {
    static volatile LONG started = 0;
    if (!queue || InterlockedExchange(&started, 1)) return;
    auto* watch = static_cast<RemovalWatch*>(HeapAlloc(GetProcessHeap(), 0, sizeof(RemovalWatch)));
    if (!watch) return;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    watch->start = static_cast<UINT64>(now.QuadPart);
    watch->device = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&watch->device)))) {
        HeapFree(GetProcessHeap(), 0, watch);
        return;
    }
    const HANDLE thread = CreateThread(nullptr, 0, WatchDeviceRemoval, watch, 0, nullptr);
    if (thread) CloseHandle(thread);
    else {
        watch->device->Release();
        HeapFree(GetProcessHeap(), 0, watch);
    }
}

class HostedSwapChain final : public IDXGISwapChain4 {
    IDXGISwapChain1* native_;
    ComPtr<IDXGISwapChain2> native2_;
    ComPtr<IDXGISwapChain3> native3_;
    ComPtr<IDXGISwapChain4> native4_;
    ComPtr<ID3D12CommandQueue> queue_;
    IDXGIFactory2* factory_;
    HWND volatile window_;
    volatile LONG references_ = 1;
    SRWLOCK lock_ = SRWLOCK_INIT;
    DxgiFramePublisher publisher_;
    ProducerCadenceTrace cadence_;
    XboxSdlSwapClock clock_{};
    UINT64 counterFrequency_ = 0;
    bool paceBeforePublish_ = false, use5994_ = false, tracedPublishTiming_ = false, skipPublish_ = false;
    bool tracedFrame_ = false, tracedPresent_ = false, tracedError_ = false;

    UINT NativeInterval(UINT interval, UINT flags) {
        if (flags & DXGI_PRESENT_TEST) return interval;
        if (!publisher_.IsActive() || flags != 0 || interval > 4) {
            clock_.interval = 0;
            return interval;
        }
        if (!interval) {
            clock_.interval = 0;
            return 0;
        }
        LARGE_INTEGER counter{};
        if (!counterFrequency_) {
            if (!QueryPerformanceFrequency(&counter) || counter.QuadPart <= 0) return interval;
            counterFrequency_ = static_cast<UINT64>(counter.QuadPart);
            // The shared clock divides by 60. Scaling its numerator by
            // 1001/1000 selects a 60000/1001 Hz period; QPC still uses its
            // unmodified frequency when converting ticks to sleep time.
            clock_.frequency = counterFrequency_ + (use5994_ ? counterFrequency_ / 1000 : 0);
            if (use5994_) Trace("pacing-rate-millihz", 59940);
        }
        if (!QueryPerformanceCounter(&counter)) return interval;

        // The host owns display vsync. Keep the game's requested cadence
        // without waiting for a second flip queue on the virtual window.
        auto now = static_cast<UINT64>(counter.QuadPart);
        const auto deadline = XboxSdlNextSwap(&clock_, now, static_cast<int>(interval));
        while (now < deadline) {
            SleepEx(static_cast<DWORD>(((deadline - now) * 1000 + counterFrequency_ - 1) / counterFrequency_), FALSE);
            if (!QueryPerformanceCounter(&counter)) return interval;
            now = static_cast<UINT64>(counter.QuadPart);
        }
        return 0;
    }

    void Publish(UINT interval, UINT flags) {
        if (flags & DXGI_PRESENT_TEST) return;
        if (cadence_.Enabled()) StartRemovalWatch(queue_.Get());
        if (skipPublish_) return;
        InterlockedIncrement(&hostedPublishes);
        const HRESULT code = publisher_.Publish(native_, queue_ ? native3_->GetCurrentBackBufferIndex() : 0);
        cadence_.Record(code, publisher_.IsActive(), interval, flags);
        if (code == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            using RequestClose = void (WINAPI *)(HWND);
            auto windows = GetModuleHandleW(L"XboxWin32Window.dll");
            auto close = windows ? reinterpret_cast<RequestClose>(GetProcAddress(windows, "XboxRequestWindowClose")) : nullptr;
            const HWND window = window_;
            if (close && window) close(window);
        } else if (code == S_OK && !tracedFrame_) {
            Trace("gpu-frame-submitted", code);
            tracedFrame_ = true;
        } else if (FAILED(code) && !tracedError_) {
            tracedError_ = true;
            if (const char* stage = publisher_.FailureStage()) Trace(stage, code);
            if (code == DXGI_ERROR_DEVICE_REMOVED && queue_) {
                const DWORD error = GetLastError();
                ComPtr<ID3D12Device> device;
                const HRESULT queried = queue_->GetDevice(IID_PPV_ARGS(device.GetAddressOf()));
                if (SUCCEEDED(queried))
                    Trace("gpu-frame-device-removed-reason", device->GetDeviceRemovedReason());
                else
                    Trace("gpu-frame-device-query", queried);
                device.Reset();
                SetLastError(error);
            }
            Trace("gpu-frame-error", code);
        }
    }

    UINT PreparePresent(UINT interval, UINT flags, ProducerCadenceTrace::PresentTiming& timing) {
        timing.pacerFirst = paceBeforePublish_ && interval == 1 && flags == 0 && publisher_.IsActive();
        if (timing.pacerFirst) {
            const UINT nativeInterval = NativeInterval(interval, flags);
            if (timing.entered) timing.split = cadence_.Timestamp();
            Publish(interval, flags);
            if (nativeInterval == 0 && !tracedPublishTiming_) {
                Trace("pacing-before-publish", S_OK);
                tracedPublishTiming_ = true;
            }
            return nativeInterval;
        }
        Publish(interval, flags);
        if (timing.entered) timing.split = cadence_.Timestamp();
        return NativeInterval(interval, flags);
    }

    void TracePresent(UINT interval, UINT nativeInterval, UINT flags, HRESULT code) {
        if (!tracedPresent_ && !(flags & DXGI_PRESENT_TEST)) {
            Trace("present-interval", static_cast<HRESULT>(interval));
            Trace("native-present-interval", static_cast<HRESULT>(nativeInterval));
            Trace("present-flags", static_cast<HRESULT>(flags));
            Trace("present", code);
            tracedPresent_ = true;
        }
    }

    template <class NativePresent>
    HRESULT PresentHosted(UINT interval, UINT flags, NativePresent present) {
        ProducerCadenceTrace::PresentTiming timing;
        const bool measure = cadence_.Enabled() && !(flags & DXGI_PRESENT_TEST);
        if (measure) timing.entered = cadence_.Timestamp();
        AcquireSRWLockExclusive(&lock_);
        if (timing.entered) timing.locked = cadence_.Timestamp();
        const UINT nativeInterval = PreparePresent(interval, flags, timing);
        if (timing.entered) timing.prepared = cadence_.Timestamp();
        const HRESULT result = present(nativeInterval);
        if (timing.entered) timing.returned = cadence_.Timestamp();
        TracePresent(interval, nativeInterval, flags, result);
        if (!(flags & DXGI_PRESENT_TEST) && InterlockedIncrement(&hostedPresents) <= 10 && cadence_.Enabled())
            Trace("present-result", result);
        if (measure) cadence_.RecordPresent(timing, publisher_.IsActive());
        ReleaseSRWLockExclusive(&lock_);
        return result;
    }

public:
    // A composition chain starts without a window. DirectComposition binds it
    // when a committed target shows it; the latest committed window wins.
    void BindWindow(HWND window) { InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&window_), window); }

    HostedSwapChain(IDXGISwapChain1* native, IDXGISwapChain3* native3, ID3D12CommandQueue* queue,
            IDXGIFactory2* factory, HWND window)
        : native_(native), native3_(native3), queue_(queue), factory_(factory), window_(window) {
        const DWORD error = GetLastError();
        native_->QueryInterface(IID_PPV_ARGS(&native2_));
        native_->QueryInterface(IID_PPV_ARGS(&native4_));
        publisher_.SetQueue(queue);
        if (HasPackageMarker(L"\\fhd-measure.txt")) {
            paceBeforePublish_ = HasPackageMarker(L"\\fhd-paced-publish.txt");
            use5994_ = HasPackageMarker(L"\\fhd-refresh-5994.txt");
            // Diagnostic only: present without copying frames to the host.
            skipPublish_ = HasPackageMarker(L"\\fhd-skip-publish.txt");
            if (skipPublish_) Trace("publish-skipped", S_OK);
            cadence_.Enable();
        }
        SetLastError(error);
        factory_->AddRef();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (SameInterface(iid, hostedSwapChainId)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        if (!SameInterface(iid, __uuidof(IUnknown)) && !SameInterface(iid, __uuidof(IDXGIObject)) &&
                !SameInterface(iid, __uuidof(IDXGIDeviceSubObject)) && !SameInterface(iid, __uuidof(IDXGISwapChain)) &&
                !SameInterface(iid, __uuidof(IDXGISwapChain1)) &&
                !(native2_ && SameInterface(iid, __uuidof(IDXGISwapChain2))) &&
                !(native3_ && SameInterface(iid, __uuidof(IDXGISwapChain3))) &&
                !(native4_ && SameInterface(iid, __uuidof(IDXGISwapChain4)))) return E_NOINTERFACE;
        *out = static_cast<IDXGISwapChain4*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&references_);
        if (!count) {
            cadence_.Flush();
            auto* native = native_;
            auto* factory = factory_;
            this->~HostedSwapChain();
            native->Release();
            factory->Release();
            HeapFree(GetProcessHeap(), 0, this);
        }
        return count;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID key, UINT size, const void* data) override {
        return native_->SetPrivateData(key, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID key, const IUnknown* value) override {
        return native_->SetPrivateDataInterface(key, value);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID key, UINT* size, void* data) override {
        return native_->GetPrivateData(key, size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID iid, void** out) override { return factory_->QueryInterface(iid, out); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** out) override { return native_->GetDevice(iid, out); }

    HRESULT STDMETHODCALLTYPE Present(UINT interval, UINT flags) override {
        return PresentHosted(interval, flags, [this, flags](UINT nativeInterval) {
            return native_->Present(nativeInterval, flags);
        });
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID iid, void** out) override { return native_->GetBuffer(index, iid, out); }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* output) override {
        return native_->SetFullscreenState(fullscreen, output);
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** output) override {
        return native_->GetFullscreenState(fullscreen, output);
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* out) override {
        if (!out) return E_POINTER;
        const auto code = native_->GetDesc(out);
        if (SUCCEEDED(code)) out->OutputWindow = window_;
        return code;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override {
        RECT area{};
        if ((!width || !height) && !VirtualClientRect(window_, area)) return DXGI_ERROR_INVALID_CALL;
        if (!width) width = area.right - area.left;
        if (!height) height = area.bottom - area.top;
        if (count == 1) {
            if (queue_) return DXGI_ERROR_INVALID_CALL;
            count = 2;
        }
        AcquireSRWLockExclusive(&lock_);
        HRESULT code = publisher_.Reset();
        if (SUCCEEDED(code)) {
            clock_.interval = 0;
            code = native_->ResizeBuffers(count, width, height, format, flags & ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
        }
        ReleaseSRWLockExclusive(&lock_);
        return code;
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* mode) override { return native_->ResizeTarget(mode); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** out) override { return native_->GetContainingOutput(out); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* out) override { return native_->GetFrameStatistics(out); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* out) override { return native_->GetLastPresentCount(out); }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* out) override { return native_->GetDesc1(out); }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* out) override {
        return native_->GetFullscreenDesc(out);
    }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* out) override {
        if (!out) return E_POINTER;
        *out = window_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID iid, void** out) override { return native_->GetCoreWindow(iid, out); }
    HRESULT STDMETHODCALLTYPE Present1(UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) override {
        return PresentHosted(interval, flags, [this, flags, parameters](UINT nativeInterval) {
            return native_->Present1(nativeInterval, flags, parameters);
        });
    }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return native_->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** out) override { return native_->GetRestrictToOutput(out); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* color) override { return native_->SetBackgroundColor(color); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* out) override { return native_->GetBackgroundColor(out); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override { return native_->SetRotation(rotation); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* out) override { return native_->GetRotation(out); }
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) override { return native2_->SetSourceSize(width, height); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* width, UINT* height) override { return native2_->GetSourceSize(width, height); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override { return native2_->SetMaximumFrameLatency(latency); }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* latency) override { return native2_->GetMaximumFrameLatency(latency); }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return native2_->GetFrameLatencyWaitableObject(); }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) override {
        return native2_->SetMatrixTransform(matrix);
    }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) override {
        return native2_->GetMatrixTransform(matrix);
    }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return native3_->GetCurrentBackBufferIndex(); }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE space, UINT* flags) override {
        return native3_->CheckColorSpaceSupport(space, flags);
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE space) override { return native3_->SetColorSpace1(space); }
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void* metadata) override {
        return native4_->SetHDRMetaData(type, size, metadata);
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
            const UINT* nodeMasks, IUnknown* const* queues) override {
        if (queue_ && count == 1) return DXGI_ERROR_INVALID_CALL;
        if (queue_ && queues) {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            const auto result = native_->GetDesc1(&desc);
            if (FAILED(result)) return result;
            const UINT buffers = count ? count : desc.BufferCount;
            if (buffers > DXGI_MAX_SWAP_CHAIN_BUFFERS) return DXGI_ERROR_INVALID_CALL;
            ComPtr<IUnknown> presentQueue;
            if (FAILED(queue_.As(&presentQueue))) return E_NOINTERFACE;
            for (UINT index = 0; index < buffers; ++index) {
                ComPtr<IUnknown> bufferQueue;
                if (!queues[index] || FAILED(queues[index]->QueryInterface(IID_PPV_ARGS(&bufferQueue))))
                    return DXGI_ERROR_INVALID_CALL;
                if (bufferQueue.Get() != presentQueue.Get()) return DXGI_ERROR_UNSUPPORTED;
            }
        }
        RECT area{};
        if ((!width || !height) && !VirtualClientRect(window_, area)) return DXGI_ERROR_INVALID_CALL;
        if (!width) width = area.right - area.left;
        if (!height) height = area.bottom - area.top;
        AcquireSRWLockExclusive(&lock_);
        HRESULT code = publisher_.Reset();
        if (SUCCEEDED(code)) {
            clock_.interval = 0;
            code = native3_->ResizeBuffers1(count, width, height, format,
                flags & ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH, nodeMasks, queues);
        }
        ReleaseSRWLockExclusive(&lock_);
        return code;
    }
};

class Factory final : public IDXGIFactory6 {
    IDXGIFactory2* native_;
    volatile LONG references_ = 1;
    void* volatile associatedWindow_ = nullptr;

    template<class Interface, class Call>
    HRESULT WithNative(Call call) {
        ComPtr<Interface> version;
        const auto code = native_->QueryInterface(IID_PPV_ARGS(&version));
        return SUCCEEDED(code) ? call(version.Get()) : code;
    }

    HRESULT CreateVirtualChain(IUnknown* device, HWND window, RECT area, DXGI_SWAP_CHAIN_DESC1 desc, IDXGISwapChain1** out) {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (area.right <= area.left || area.bottom <= area.top) return DXGI_ERROR_INVALID_CALL;
        if (!desc.Width) desc.Width = area.right - area.left;
        if (!desc.Height) desc.Height = area.bottom - area.top;
        return CreateHostedChain(device, window, desc, out);
    }

    HRESULT CreateHostedChain(IUnknown* device, HWND window, DXGI_SWAP_CHAIN_DESC1 desc, IDXGISwapChain1** out) {
        if (!device || !desc.Width || !desc.Height || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0)
            return DXGI_ERROR_INVALID_CALL;
        ComPtr<ID3D12CommandQueue> queue;
        device->QueryInterface(IID_PPV_ARGS(&queue));
        if (queue && (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT || desc.BufferCount < 2))
            return DXGI_ERROR_INVALID_CALL;
        if (desc.BufferCount < 2) desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        IDXGISwapChain1* native = nullptr;
        HRESULT code = native_->CreateSwapChainForComposition(device, &desc, nullptr, &native);
        Trace("composition-swap-chain", code);
        if (FAILED(code)) return code;
        ComPtr<IDXGISwapChain3> native3;
        code = native->QueryInterface(IID_PPV_ARGS(&native3));
        if (queue) {
            Trace("d3d12-swap-chain3", code);
            if (FAILED(code)) {
                native->Release();
                return code;
            }
        }
        void* storage = HeapAlloc(GetProcessHeap(), 0, sizeof(HostedSwapChain));
        if (!storage) {
            native->Release();
            return E_OUTOFMEMORY;
        }
        *out = new (storage) HostedSwapChain(native, native3.Get(), queue.Get(), this, window);
        return S_OK;
    }

public:
    explicit Factory(IDXGIFactory2* native) : native_(native) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!SameInterface(iid, __uuidof(IUnknown)) && !SameInterface(iid, __uuidof(IDXGIObject)) &&
                !SameInterface(iid, __uuidof(IDXGIFactory)) && !SameInterface(iid, __uuidof(IDXGIFactory1)) &&
                !SameInterface(iid, __uuidof(IDXGIFactory2))) {
            if (!SameInterface(iid, __uuidof(IDXGIFactory3)) && !SameInterface(iid, __uuidof(IDXGIFactory4)) &&
                    !SameInterface(iid, __uuidof(IDXGIFactory5)) && !SameInterface(iid, __uuidof(IDXGIFactory6)))
                return E_NOINTERFACE;
            ComPtr<IUnknown> version;
            const auto code = native_->QueryInterface(iid, reinterpret_cast<void**>(version.GetAddressOf()));
            if (FAILED(code)) return code;
        }
        *out = static_cast<IDXGIFactory6*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&references_);
        if (!count) {
            native_->Release();
            this->~Factory();
            HeapFree(GetProcessHeap(), 0, this);
        }
        return count;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID key, UINT size, const void* data) override {
        return native_->SetPrivateData(key, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID key, const IUnknown* value) override {
        return native_->SetPrivateDataInterface(key, value);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID key, UINT* size, void* data) override {
        return native_->GetPrivateData(key, size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID iid, void** out) override { return native_->GetParent(iid, out); }

    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT index, IDXGIAdapter** out) override { return native_->EnumAdapters(index, out); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND window, UINT flags) override {
        RECT area{};
        if (!VirtualClientRect(window, area)) {
            const auto code = native_->MakeWindowAssociation(window, flags);
            if (SUCCEEDED(code)) InterlockedExchangePointer(&associatedWindow_, nullptr);
            return code;
        }
        if (flags & ~(DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_PRINT_SCREEN))
            return DXGI_ERROR_INVALID_CALL;
        InterlockedExchangePointer(&associatedWindow_, window);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* out) override {
        if (!out) return E_POINTER;
        const auto window = static_cast<HWND>(InterlockedCompareExchangePointer(&associatedWindow_, nullptr, nullptr));
        if (!window) return native_->GetWindowAssociation(out);
        *out = window;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) override {
        RECT area{};
        if (!desc || !VirtualClientRect(desc->OutputWindow, area)) return native_->CreateSwapChain(device, desc, out);
        if (!out) return E_POINTER;
        DXGI_SWAP_CHAIN_DESC1 composition{};
        composition.Width = desc->BufferDesc.Width;
        composition.Height = desc->BufferDesc.Height;
        composition.Format = desc->BufferDesc.Format;
        composition.SampleDesc = desc->SampleDesc;
        composition.BufferUsage = desc->BufferUsage;
        composition.BufferCount = desc->BufferCount;
        composition.Flags = desc->Flags;
        IDXGISwapChain1* chain = nullptr;
        const HRESULT code = CreateVirtualChain(device, desc->OutputWindow, area, composition, &chain);
        *out = chain;
        return code;
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE module, IDXGIAdapter** out) override {
        return native_->CreateSoftwareAdapter(module, out);
    }
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT index, IDXGIAdapter1** out) override { return native_->EnumAdapters1(index, out); }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return native_->IsCurrent(); }
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return native_->IsWindowedStereoEnabled(); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* restrictTo, IDXGISwapChain1** out) override {
        RECT area{};
        if (!VirtualClientRect(window, area))
            return native_->CreateSwapChainForHwnd(device, window, desc, fullscreen, restrictTo, out);
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!desc) return E_INVALIDARG;
        if (restrictTo || desc->Stereo) return DXGI_ERROR_UNSUPPORTED;
        return CreateVirtualChain(device, window, area, *desc, out);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* device, IUnknown* window,
            const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictTo, IDXGISwapChain1** out) override {
        return native_->CreateSwapChainForCoreWindow(device, window, desc, restrictTo, out);
    }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE resource, LUID* out) override {
        return native_->GetSharedResourceAdapterLuid(resource, out);
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND window, UINT message, DWORD* cookie) override {
        return native_->RegisterStereoStatusWindow(window, message, cookie);
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE event, DWORD* cookie) override {
        return native_->RegisterStereoStatusEvent(event, cookie);
    }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD cookie) override { native_->UnregisterStereoStatus(cookie); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND window, UINT message, DWORD* cookie) override {
        return native_->RegisterOcclusionStatusWindow(window, message, cookie);
    }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE event, DWORD* cookie) override {
        return native_->RegisterOcclusionStatusEvent(event, cookie);
    }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD cookie) override { native_->UnregisterOcclusionStatus(cookie); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc,
            IDXGIOutput* restrictTo, IDXGISwapChain1** out) override {
        // Hosted games draw into virtual windows that the console never shows,
        // so a composition chain is only visible through the hosted presenter.
        if (!VirtualWindowsActive()) return native_->CreateSwapChainForComposition(device, desc, restrictTo, out);
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!desc) return E_INVALIDARG;
        if (restrictTo || desc->Stereo) return DXGI_ERROR_UNSUPPORTED;
        const HRESULT code = CreateHostedChain(device, nullptr, *desc, out);
        Trace("hosted-composition-chain", code);
        return code;
    }
    UINT STDMETHODCALLTYPE GetCreationFlags() override {
        ComPtr<IDXGIFactory3> native3;
        return SUCCEEDED(native_->QueryInterface(IID_PPV_ARGS(&native3))) ? native3->GetCreationFlags() : 0;
    }
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID iid, void** out) override {
        return WithNative<IDXGIFactory4>([&](auto* native) { return native->EnumAdapterByLuid(luid, iid, out); });
    }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID iid, void** out) override {
        return WithNative<IDXGIFactory4>([&](auto* native) { return native->EnumWarpAdapter(iid, out); });
    }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE feature, void* data, UINT size) override {
        return WithNative<IDXGIFactory5>([&](auto* native) { return native->CheckFeatureSupport(feature, data, size); });
    }
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT index, DXGI_GPU_PREFERENCE preference,
            REFIID iid, void** out) override {
        return WithNative<IDXGIFactory6>([&](auto* native) {
            return native->EnumAdapterByGpuPreference(index, preference, iid, out);
        });
    }
};


// DirectComposition for virtual windows. A hosted game's windows exist only
// inside XboxWin32Window.dll, so the system compositor cannot target them.
// This device supports the subset that presents one swap chain per window:
// CreateTargetForHwnd, CreateVisual, IDCompositionVisual::SetContent,
// IDCompositionTarget::SetRoot and Commit. Commit binds each hosted chain to
// its window. Calls outside that subset return E_NOTIMPL, and real HWNDs are
// rejected with E_INVALIDARG instead of being silently ignored.
template<class Derived, class Interface>
class CompositionObject : public Interface {
    volatile LONG references_ = 1;

public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!SameInterface(iid, __uuidof(IUnknown)) && !SameInterface(iid, __uuidof(Interface))) return E_NOINTERFACE;
        *out = static_cast<Interface*>(this);
        this->AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&references_);
        if (!count) {
            auto* object = static_cast<Derived*>(this);
            object->~Derived();
            HeapFree(GetProcessHeap(), 0, object);
        }
        return count;
    }
};

template<class Object, class... Arguments>
Object* NewCompositionObject(Arguments... arguments) {
    void* storage = HeapAlloc(GetProcessHeap(), 0, sizeof(Object));
    return storage ? new (storage) Object(arguments...) : nullptr;
}

// Private identity of visuals created by the hosted composition device.
constexpr GUID compositionVisualId = {0x9a7c0b52, 0x3e1d, 0x4f6a, {0x8b, 0x44, 0x2d, 0x91, 0x6e, 0x5f, 0x07, 0xc3}};

class CompositionVisual final : public CompositionObject<CompositionVisual, IDCompositionVisual> {
    SRWLOCK lock_ = SRWLOCK_INIT;
    IUnknown* content_ = nullptr;

public:
    ~CompositionVisual() {
        if (content_) content_->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (out && SameInterface(iid, compositionVisualId)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        return CompositionObject::QueryInterface(iid, out);
    }

    // Returns the hosted chain shown by this visual with a reference, or null.
    HostedSwapChain* HostedContent() {
        AcquireSRWLockShared(&lock_);
        void* chain = nullptr;
        if (content_ && FAILED(content_->QueryInterface(hostedSwapChainId, &chain))) chain = nullptr;
        ReleaseSRWLockShared(&lock_);
        return static_cast<HostedSwapChain*>(chain);
    }

    HRESULT STDMETHODCALLTYPE SetContent(IUnknown* content) override {
        if (content) content->AddRef();
        AcquireSRWLockExclusive(&lock_);
        IUnknown* previous = content_;
        content_ = content;
        ReleaseSRWLockExclusive(&lock_);
        if (previous) previous->Release();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetOffsetX(float) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetOffsetX(IDCompositionAnimation*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetOffsetY(float) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetOffsetY(IDCompositionAnimation*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetTransform(const D2D_MATRIX_3X2_F&) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetTransform(IDCompositionTransform*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetTransformParent(IDCompositionVisual*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetEffect(IDCompositionEffect*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetBorderMode(DCOMPOSITION_BORDER_MODE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetClip(const D2D_RECT_F&) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetClip(IDCompositionClip*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddVisual(IDCompositionVisual*, BOOL, IDCompositionVisual*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RemoveVisual(IDCompositionVisual*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RemoveAllVisuals() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetCompositeMode(DCOMPOSITION_COMPOSITE_MODE) override { return E_NOTIMPL; }
};

class CompositionTarget final : public CompositionObject<CompositionTarget, IDCompositionTarget> {
    SRWLOCK lock_ = SRWLOCK_INIT;
    CompositionVisual* root_ = nullptr;

public:
    ~CompositionTarget() {
        if (root_) root_->Release();
    }

    const HWND window;
    CompositionTarget* next = nullptr;  // Owning device's list; guarded by the device lock.

    explicit CompositionTarget(HWND target) : window(target) {}

    // Returns the hosted chain shown at this target's root with a reference, or null.
    HostedSwapChain* HostedContent() {
        AcquireSRWLockShared(&lock_);
        HostedSwapChain* chain = root_ ? root_->HostedContent() : nullptr;
        ReleaseSRWLockShared(&lock_);
        return chain;
    }

    HRESULT STDMETHODCALLTYPE SetRoot(IDCompositionVisual* visual) override {
        // Only visuals from this adapter can be shown on a virtual window.
        void* root = nullptr;
        if (visual && FAILED(visual->QueryInterface(compositionVisualId, &root))) return E_INVALIDARG;
        AcquireSRWLockExclusive(&lock_);
        CompositionVisual* previous = root_;
        root_ = static_cast<CompositionVisual*>(root);
        ReleaseSRWLockExclusive(&lock_);
        if (previous) previous->Release();
        return S_OK;
    }
};

class CompositionDevice final : public CompositionObject<CompositionDevice, IDCompositionDevice> {
    SRWLOCK lock_ = SRWLOCK_INIT;
    CompositionTarget* targets_ = nullptr;  // One reference each.

public:
    ~CompositionDevice() {
        for (auto* target = targets_; target;) {
            auto* next = target->next;
            target->Release();
            target = next;
        }
    }

    HRESULT STDMETHODCALLTYPE Commit() override {
        // Collect bindings under the lock and apply them after releasing it.
        HostedSwapChain* chains[16]{};
        HWND windows[16]{};
        UINT count = 0;
        AcquireSRWLockShared(&lock_);
        for (auto* target = targets_; target && count < ARRAYSIZE(chains); target = target->next) {
            if (auto* chain = target->HostedContent()) {
                chains[count] = chain;
                windows[count++] = target->window;
            }
        }
        ReleaseSRWLockShared(&lock_);
        for (UINT i = 0; i < count; ++i) {
            chains[i]->BindWindow(windows[i]);
            chains[i]->Release();
        }
        static volatile LONG traced = 0;
        if (count && InterlockedExchange(&traced, 1) == 0) Trace("dcomp-commit-bound", static_cast<HRESULT>(count));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE WaitForCommitCompletion() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DCOMPOSITION_FRAME_STATISTICS*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateTargetForHwnd(HWND window, BOOL, IDCompositionTarget** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        RECT area{};
        if (!VirtualClientRect(window, area)) return E_INVALIDARG;
        AcquireSRWLockExclusive(&lock_);
        for (auto* target = targets_; target; target = target->next) {
            if (target->window == window) {
                ReleaseSRWLockExclusive(&lock_);
                return DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED;
            }
        }
        auto* target = NewCompositionObject<CompositionTarget>(window);
        if (!target) {
            ReleaseSRWLockExclusive(&lock_);
            return E_OUTOFMEMORY;
        }
        target->next = targets_;
        targets_ = target;
        target->AddRef();
        ReleaseSRWLockExclusive(&lock_);
        *out = target;
        Trace("dcomp-target", S_OK);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateVisual(IDCompositionVisual** out) override {
        if (!out) return E_POINTER;
        *out = NewCompositionObject<CompositionVisual>();
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE CreateSurface(UINT, UINT, DXGI_FORMAT, DXGI_ALPHA_MODE, IDCompositionSurface** out) override {
        return Unsupported(out);
    }
    HRESULT STDMETHODCALLTYPE CreateVirtualSurface(UINT, UINT, DXGI_FORMAT, DXGI_ALPHA_MODE,
            IDCompositionVirtualSurface** out) override { return Unsupported(out); }
    HRESULT STDMETHODCALLTYPE CreateSurfaceFromHandle(HANDLE, IUnknown** out) override { return Unsupported(out); }
    HRESULT STDMETHODCALLTYPE CreateSurfaceFromHwnd(HWND, IUnknown** out) override { return Unsupported(out); }
    HRESULT STDMETHODCALLTYPE CreateTranslateTransform(IDCompositionTranslateTransform** out) override { return Unsupported(out); }
    HRESULT STDMETHODCALLTYPE CreateScaleTransform(IDCompositionScaleTransform** out) override { return Unsupported(out); }
    HRESULT STDMETHODCALLTYPE CreateRotateTransform(IDCompositionRotateTransform** out) override { return Unsupported(out); }
    HRESULT STDMETHODCALLTYPE CreateSkewTransform(IDCompositionSkewTransform** out) override { return Unsupported(out); }
    HRESULT STDMETHODCALLTYPE CreateMatrixTransform(IDCompositionMatrixTransform** out) override { return Unsupported(out); }
    HRESULT STDMETHODCALLTYPE CreateTransformGroup(IDCompositionTransform**, UINT, IDCompositionTransform** out) override {
        return Unsupported(out);
    }
    HRESULT STDMETHODCALLTYPE CreateTranslateTransform3D(IDCompositionTranslateTransform3D** out) override { return Unsupported(out); }
    HRESULT STDMETHODCALLTYPE CreateScaleTransform3D(IDCompositionScaleTransform3D** out) override { return Unsupported(out); }
    HRESULT STDMETHODCALLTYPE CreateRotateTransform3D(IDCompositionRotateTransform3D** out) override { return Unsupported(out); }
    HRESULT STDMETHODCALLTYPE CreateMatrixTransform3D(IDCompositionMatrixTransform3D** out) override { return Unsupported(out); }
    HRESULT STDMETHODCALLTYPE CreateTransform3DGroup(IDCompositionTransform3D**, UINT, IDCompositionTransform3D** out) override {
        return Unsupported(out);
    }
    HRESULT STDMETHODCALLTYPE CreateEffectGroup(IDCompositionEffectGroup** out) override { return Unsupported(out); }
    HRESULT STDMETHODCALLTYPE CreateRectangleClip(IDCompositionRectangleClip** out) override { return Unsupported(out); }
    HRESULT STDMETHODCALLTYPE CreateAnimation(IDCompositionAnimation** out) override { return Unsupported(out); }
    HRESULT STDMETHODCALLTYPE CheckDeviceState(BOOL* valid) override {
        if (!valid) return E_POINTER;
        *valid = TRUE;
        return S_OK;
    }

private:
    template<class Out>
    static HRESULT Unsupported(Out** out) {
        if (out) *out = nullptr;
        return E_NOTIMPL;
    }
};

// A hosted Direct3D game initializes the publisher's Steamworks SDK against
// the running Steam client before its own SteamAPI_Init, as the WGL adapter
// does for OpenGL titles. Titles using the legacy import adapter are excluded.
void InitializeSteamHandoff() {
    static volatile LONG checked = 0;
    if (InterlockedExchange(&checked, 1)) return;
    const DWORD previous = GetLastError();
    WCHAR legacy[2];
    if (GetEnvironmentVariableW(L"XBOX_STEAM_HANDOFF_SDK", nullptr, 0) <= 1 ||
            (GetEnvironmentVariableW(L"XBOX_STEAM_HANDOFF_LEGACY", legacy, ARRAYSIZE(legacy)) == 1 && legacy[0] == L'1')) {
        SetLastError(previous);
        return;
    }
    using Initialize = DWORD (WINAPI *)(BOOL);
    const HMODULE module = LoadPackagedLibrary(L"XboxWin32SteamDiscovery.dll", 0);
    const auto initialize = module ? reinterpret_cast<Initialize>(GetProcAddress(module, "XboxSteamInitialize")) : nullptr;
    if (!initialize) {
        Trace("steam-handoff-unavailable", HRESULT_FROM_WIN32(GetLastError()));
    } else {
        constexpr DWORD required = STEAM_HANDOFF_SDK_INITIALIZED | STEAM_HANDOFF_SDK_MATCHED;
        const DWORD result = initialize(TRUE);
        Trace((result & required) == required ? "steam-handoff-ready" : "steam-handoff-error", static_cast<HRESULT>(result));
    }
    if (module) FreeLibrary(module);
    SetLastError(previous);
}

enum class FactoryEntry { Legacy, Version1, Version2 };

HRESULT CreateFactory(FactoryEntry entry, UINT flags, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    InitializeSteamHandoff();
    IDXGIFactory2* native = nullptr;
    HRESULT code;
    if (entry == FactoryEntry::Legacy) {
        using CreateLegacy = HRESULT (WINAPI *)(REFIID, void**);
        const auto module = GetModuleHandleW(L"dxgi.dll");
        const auto create = module ? reinterpret_cast<CreateLegacy>(GetProcAddress(module, "CreateDXGIFactory")) : nullptr;
        if (!create) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        ComPtr<IDXGIFactory> legacy;
        code = create(IID_PPV_ARGS(&legacy));
        if (SUCCEEDED(code)) code = legacy->QueryInterface(IID_PPV_ARGS(&native));
    } else if (entry == FactoryEntry::Version1) {
        code = CreateDXGIFactory1(IID_PPV_ARGS(&native));
    } else {
        code = CreateDXGIFactory2(flags, IID_PPV_ARGS(&native));
    }
    if (FAILED(code)) return code;
    void* storage = HeapAlloc(GetProcessHeap(), 0, sizeof(Factory));
    if (!storage) {
        native->Release();
        return E_OUTOFMEMORY;
    }
    auto* adapter = new (storage) Factory(native);
    code = adapter->QueryInterface(iid, out);
    adapter->Release();
    Trace("factory", code);
    return code;
}

} // namespace

extern "C" HRESULT WINAPI XboxCreateDXGIFactory(REFIID iid, void** out) {
    return CreateFactory(FactoryEntry::Legacy, 0, iid, out);
}

extern "C" HRESULT WINAPI XboxCreateDXGIFactory1(REFIID iid, void** out) {
    return CreateFactory(FactoryEntry::Version1, 0, iid, out);
}

extern "C" HRESULT WINAPI XboxCreateDXGIFactory2(UINT flags, REFIID iid, void** out) {
    return CreateFactory(FactoryEntry::Version2, flags, iid, out);
}

extern "C" BOOL WINAPI XboxGetOutputMonitorRect(HMONITOR monitor, RECT* area) {
    if (!monitor || !area) return FALSE;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return FALSE;
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(adapterIndex, &adapter))) return FALSE;
        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(outputIndex, &output))) break;
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
                *area = desc.DesktopCoordinates;
                return TRUE;
            }
        }
    }
}

extern "C" HRESULT WINAPI XboxD3DReflect(LPCVOID bytecode, SIZE_T size, REFIID iid, void** out) {
    const HRESULT code = D3DReflect(bytecode, size, iid, out);
    static volatile LONG events = 0;
    if (InterlockedIncrement(&events) <= 8) Trace(bytecode && size ? "shader-reflect" : "shader-reflect-empty", code);
    return code;
}

extern "C" HRESULT WINAPI XboxD3DGetInputSignatureBlob(LPCVOID bytecode, SIZE_T size, ID3DBlob** out) {
    const HRESULT code = D3DGetInputSignatureBlob(bytecode, size, out);
    static volatile LONG events = 0;
    if (InterlockedIncrement(&events) <= 8) Trace(bytecode && size ? "shader-input-signature" : "shader-input-signature-empty", code);
    return code;
}

extern "C" HRESULT WINAPI XboxDCompositionCreateDevice(IDXGIDevice* renderingDevice, REFIID iid, void** out) {
    // The hosted device never renders, so the optional DXGI device is unused.
    (void)renderingDevice;
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!VirtualWindowsActive()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    auto* device = NewCompositionObject<CompositionDevice>();
    if (!device) return E_OUTOFMEMORY;
    const HRESULT code = device->QueryInterface(iid, out);
    device->Release();
    Trace("dcomp-device", code);
    return code;
}

// Caps the video memory budget that Mesa's d3d12 driver reads from the native adapter. See dxgi-memory-budget.h.
extern "C" HRESULT WINAPI XboxInstallGpuMemoryBudget(void) {
    const DWORD previous = GetLastError();
    const HRESULT result = InstallMemoryBudget();
    SetLastError(previous);
    return result;
}
