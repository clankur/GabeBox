#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <appmodel.h>
#include <new>
#include "dxgi-frame.h"
#include "../shared/sdl-swap-clock.h"

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

bool VirtualClientRect(HWND window, RECT& area) {
    using Query = BOOL (WINAPI *)(HWND, RECT*);
    auto windows = GetModuleHandleW(L"XboxWin32Window.dll");
    auto query = windows ? reinterpret_cast<Query>(
        reinterpret_cast<void*>(GetProcAddress(windows, "GetClientRect"))) : nullptr;
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

#include "dxgi-hosted-output.h"

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
    UINT attempts_ = 0, submitted_ = 0, busy_ = 0, rejected_ = 0;
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

    void Record(HRESULT capture, HRESULT present, bool active, UINT interval, UINT flags) {
        if (!aggregate_) return;
        const auto now = Timestamp();
        if (!now) return;
        if (!start_) start_ = now;
        if (last_ && now - last_ > maxGap_) maxGap_ = now - last_;
        last_ = now;
        ++attempts_;
        if (capture == S_OK) ++(present == S_OK ? submitted_ : rejected_);
        if (capture == S_FALSE && active) ++busy_;
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
        char line[270], *out = line;
        Field(out, "dxgi cadence end-qpc=", last_);
        Field(out, " elapsed-ms=", (last_ - start_) * 1000 / frequency_);
        Field(out, " attempts=", attempts_);
        Field(out, " submitted=", submitted_);
        Field(out, " slot-busy=", busy_);
        Field(out, " present-rejected=", rejected_);
        Field(out, " max-gap-ms=", maxGap_ * 1000 / frequency_);
        Field(out, " immediate=", immediate_);
        Field(out, " sync-one=", syncOne_);
        *out++ = '\r';
        *out++ = '\n';
        AppendTrace(line, static_cast<DWORD>(out - line));
        start_ = last_;
        maxGap_ = 0;
        attempts_ = submitted_ = busy_ = rejected_ = 0;
        immediate_ = syncOne_ = 0;
    }
};

class HostedSwapChain final : public IDXGISwapChain4 {
    IDXGISwapChain1* native_;
    ComPtr<IDXGISwapChain2> native2_;
    ComPtr<IDXGISwapChain3> native3_;
    ComPtr<IDXGISwapChain4> native4_;
    ComPtr<ID3D12CommandQueue> queue_;
    IDXGIFactory2* factory_;
    HWND window_;
    volatile LONG references_ = 1;
    SRWLOCK lock_ = SRWLOCK_INIT;
    DxgiFramePublisher publisher_;
    ProducerCadenceTrace cadence_;
    XboxSdlSwapClock clock_{};
    UINT64 counterFrequency_ = 0;
    bool paceBeforePublish_ = false, use5994_ = false, tracedPublishTiming_ = false;
    bool tracedFrame_ = false, tracedPresent_ = false, tracedError_ = false, tracedRejection_ = false;

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

    HRESULT Capture(UINT flags, unsigned& index) {
        if (flags & DXGI_PRESENT_TEST) return S_FALSE;
        const HRESULT code = publisher_.Prepare(native_, queue_ ? native3_->GetCurrentBackBufferIndex() : 0, index);
        if (code == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            using RequestClose = void (WINAPI *)(HWND);
            auto windows = GetModuleHandleW(L"XboxWin32Window.dll");
            auto close = windows ? reinterpret_cast<RequestClose>(
                reinterpret_cast<void*>(GetProcAddress(windows, "XboxRequestWindowClose"))) : nullptr;
            if (close) close(window_);
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
        return code;
    }

    UINT PreparePresent(UINT interval, UINT flags, ProducerCadenceTrace::PresentTiming& timing,
            HRESULT& capture, unsigned& index) {
        timing.pacerFirst = paceBeforePublish_ && interval == 1 && flags == 0 && publisher_.IsActive();
        if (timing.pacerFirst) {
            const UINT nativeInterval = NativeInterval(interval, flags);
            if (timing.entered) timing.split = cadence_.Timestamp();
            capture = Capture(flags, index);
            if (nativeInterval == 0 && !tracedPublishTiming_) {
                Trace("pacing-before-publish", S_OK);
                tracedPublishTiming_ = true;
            }
            return nativeInterval;
        }
        capture = Capture(flags, index);
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
        if (flags & DXGI_PRESENT_DO_NOT_WAIT) {
            if (!TryAcquireSRWLockExclusive(&lock_)) return DXGI_ERROR_WAS_STILL_DRAWING;
        } else {
            AcquireSRWLockExclusive(&lock_);
        }
        if (timing.entered) timing.locked = cadence_.Timestamp();
        HRESULT capture;
        unsigned index;
        const UINT nativeInterval = PreparePresent(interval, flags, timing, capture, index);
        if (timing.entered) timing.prepared = cadence_.Timestamp();
        const HRESULT result = present(nativeInterval);
        if (timing.entered) timing.returned = cadence_.Timestamp();
        if (capture == S_OK) {
            if (result == S_OK) {
                publisher_.Commit(index);
                if (!tracedFrame_) {
                    Trace("gpu-frame-submitted", capture);
                    tracedFrame_ = true;
                }
            } else if (!tracedRejection_) {
                Trace("gpu-frame-present-rejected", result);
                tracedRejection_ = true;
            }
        }
        if (!(flags & DXGI_PRESENT_TEST)) cadence_.Record(capture, result, publisher_.IsActive(), interval, flags);
        TracePresent(interval, nativeInterval, flags, result);
        if (measure) cadence_.RecordPresent(timing, publisher_.IsActive());
        ReleaseSRWLockExclusive(&lock_);
        return result;
    }

    template <class NativeResize>
    HRESULT ResizeHosted(NativeResize resize) {
        AcquireSRWLockExclusive(&lock_);
        // D3D12 command slots retain source buffers. D3D11 flushes its copies
        // on submission and can keep the current exchange until DXGI succeeds.
        HRESULT result = queue_ ? publisher_.Reset() : S_OK;
        if (SUCCEEDED(result)) {
            if (queue_) clock_.interval = 0;
            result = resize();
            if (!queue_) {
                const DWORD error = GetLastError();
                if (result == S_OK) {
                    publisher_.Reset();
                    clock_.interval = 0;
                }
                static volatile LONG events = 0;
                if (InterlockedIncrement(&events) <= 12) Trace("d3d11-resize-buffers", result);
                SetLastError(error);
            }
        }
        ReleaseSRWLockExclusive(&lock_);
        return result;
    }

public:
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
            cadence_.Enable();
        }
        SetLastError(error);
        factory_->AddRef();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
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
        return ResizeHosted([&] {
            return native_->ResizeBuffers(count, width, height, format, flags & ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
        });
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
        return ResizeHosted([&] {
            return native3_->ResizeBuffers1(count, width, height, format,
                flags & ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH, nodeMasks, queues);
        });
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
        if (!device || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 ||
                area.right <= area.left || area.bottom <= area.top) return DXGI_ERROR_INVALID_CALL;
        if (!desc.Width) desc.Width = area.right - area.left;
        if (!desc.Height) desc.Height = area.bottom - area.top;
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

    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT index, IDXGIAdapter** out) override {
        const HRESULT result = native_->EnumAdapters(index, out);
        if (!index && SUCCEEDED(result) && out && *out && HostedOutputEnabled()) {
            const DWORD previous = GetLastError();
            MaybeWrapHostedAdapter(this, out);
            SetLastError(previous);
        }
        return result;
    }
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
        return native_->CreateSwapChainForComposition(device, desc, restrictTo, out);
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

enum class FactoryEntry { Legacy, Version1, Version2 };

HRESULT WrapFactory(ComPtr<IDXGIFactory2>& native, ComPtr<Factory>& wrapped) {
    if (!native) return E_NOINTERFACE;
    void* storage = HeapAlloc(GetProcessHeap(), 0, sizeof(Factory));
    if (!storage) return E_OUTOFMEMORY;
    wrapped.Attach(new (storage) Factory(native.Detach()));
    return S_OK;
}

HRESULT CreateFactory(FactoryEntry entry, UINT flags, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    ComPtr<IDXGIFactory2> native;
    HRESULT code;
    if (entry == FactoryEntry::Legacy) {
        using CreateLegacy = HRESULT (WINAPI *)(REFIID, void**);
        const auto module = GetModuleHandleW(L"dxgi.dll");
        const auto create = module ? reinterpret_cast<CreateLegacy>(
            reinterpret_cast<void*>(GetProcAddress(module, "CreateDXGIFactory"))) : nullptr;
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
    ComPtr<Factory> adapter;
    code = WrapFactory(native, adapter);
    if (SUCCEEDED(code)) code = adapter->QueryInterface(iid, out);
    Trace("factory", code);
    return code;
}

HRESULT DeviceFactory(ID3D11Device* device, ComPtr<IDXGIFactory2>& factory) {
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT code = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(code)) return code;
    if (!dxgiDevice) return E_NOINTERFACE;
    ComPtr<IDXGIAdapter> adapter;
    code = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(code)) return code;
    if (!adapter) return E_NOINTERFACE;
    ComPtr<IDXGIFactory> parent;
    code = adapter->GetParent(IID_PPV_ARGS(&parent));
    if (FAILED(code)) return code;
    return parent ? parent.As(&factory) : E_NOINTERFACE;
}

HRESULT CreateHostedD3D11Chain(PFN_D3D11_CREATE_DEVICE create, IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver,
        HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdk,
        const DXGI_SWAP_CHAIN_DESC& description, IDXGISwapChain** chainOut, ID3D11Device** deviceOut,
        D3D_FEATURE_LEVEL* levelOut, ID3D11DeviceContext** contextOut) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL selected{};
    HRESULT code = create(adapter, driver, software, flags, levels, levelCount, sdk, device.GetAddressOf(),
        levelOut ? &selected : nullptr, contextOut ? context.GetAddressOf() : nullptr);
    Trace(adapter ? "d3d11-device-explicit" : "d3d11-device-default", code);
    if (FAILED(code)) return code;
    if (!device || (contextOut && !context)) return E_FAIL;

    // Use the factory that actually owns the native device.
    ComPtr<IDXGIFactory2> nativeFactory;
    code = DeviceFactory(device.Get(), nativeFactory);
    if (FAILED(code)) {
        Trace("d3d11-device-factory", code);
        return code;
    }
    ComPtr<Factory> factory;
    code = WrapFactory(nativeFactory, factory);
    if (FAILED(code)) return code;
    auto mutableDescription = description;
    ComPtr<IDXGISwapChain> chain;
    code = factory->CreateSwapChain(device.Get(), &mutableDescription, &chain);
    Trace("d3d11-hosted-swap-chain", code);
    if (FAILED(code)) return code;
    if (!chain) return E_FAIL;

    *chainOut = chain.Detach();
    if (deviceOut) *deviceOut = device.Detach();
    if (levelOut) *levelOut = selected;
    if (contextOut) *contextOut = context.Detach();
    return S_OK;
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

extern "C" HRESULT WINAPI XboxD3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver,
        HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdk,
        const DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** chainOut, ID3D11Device** deviceOut,
        D3D_FEATURE_LEVEL* levelOut, ID3D11DeviceContext** contextOut) {
    const DWORD previous = GetLastError();
    ComPtr<IHostedNativeAdapter> hostedAdapter;
    if (adapter && SUCCEEDED(adapter->QueryInterface(hostedNativeAdapterId,
            reinterpret_cast<void**>(hostedAdapter.GetAddressOf())))) {
        adapter = hostedAdapter->NativeAdapter();
        hostedAdapter.Reset();
    }
    const HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    RECT area{};
    const bool hosted = description && chainOut && description->Windowed &&
        VirtualClientRect(description->OutputWindow, area);
    if (!hosted && d3d11) {
        const auto combined = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
            reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")));
        if (combined) {
            SetLastError(previous);
            return combined(adapter, driver, software, flags, levels, levelCount, sdk, description,
                chainOut, deviceOut, levelOut, contextOut);
        }
    }
    const auto create = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
        reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice"))) : nullptr;
    SetLastError(previous);
    if (!hosted && create && !chainOut) {
        return create(adapter, driver, software, flags, levels, levelCount, sdk, deviceOut, levelOut, contextOut);
    }
    if (chainOut) *chainOut = nullptr;
    if (deviceOut) *deviceOut = nullptr;
    if (levelOut) *levelOut = {};
    if (contextOut) *contextOut = nullptr;
    if (!d3d11) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    if (!create || !hosted) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    const HRESULT code = CreateHostedD3D11Chain(create, adapter, driver, software, flags, levels, levelCount,
        sdk, *description, chainOut, deviceOut, levelOut, contextOut);
    SetLastError(previous);
    return code;
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
