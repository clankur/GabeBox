#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <new>
#include "dxgi-frame.h"
#include "../sdl-swap-clock.h"

extern "C" __declspec(dllimport) HANDLE WINAPI CreateFile2FromAppW(
    LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);

namespace {

bool SameInterface(REFIID first, REFIID second) {
    if (first.Data1 != second.Data1 || first.Data2 != second.Data2 || first.Data3 != second.Data3) return false;
    for (unsigned i = 0; i < ARRAYSIZE(first.Data4); ++i)
        if (first.Data4[i] != second.Data4[i]) return false;
    return true;
}

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

class HostedSwapChain final : public IDXGISwapChain1 {
    IDXGISwapChain1* native_;
    IDXGIFactory2* factory_;
    HWND window_;
    volatile LONG references_ = 1;
    SRWLOCK lock_ = SRWLOCK_INIT;
    DxgiFramePublisher publisher_;
    XboxSdlSwapClock clock_{};
    UINT64 counterFrequency_ = 0;
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
            clock_.frequency = counterFrequency_;
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

    void Publish(UINT flags) {
        if (flags & DXGI_PRESENT_TEST) return;
        const HRESULT code = publisher_.Publish(native_);
        if (code == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            using RequestClose = void (WINAPI *)(HWND);
            auto windows = GetModuleHandleW(L"XboxWin32Window.dll");
            auto close = windows ? reinterpret_cast<RequestClose>(GetProcAddress(windows, "XboxRequestWindowClose")) : nullptr;
            if (close) close(window_);
        } else if (code == S_OK && !tracedFrame_) {
            Trace("gpu-frame-submitted", code);
            tracedFrame_ = true;
        } else if (FAILED(code) && !tracedError_) {
            Trace("gpu-frame-error", code);
            tracedError_ = true;
        }
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

public:
    HostedSwapChain(IDXGISwapChain1* native, IDXGIFactory2* factory, HWND window)
        : native_(native), factory_(factory), window_(window) {
        factory_->AddRef();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!SameInterface(iid, __uuidof(IUnknown)) && !SameInterface(iid, __uuidof(IDXGIObject)) &&
                !SameInterface(iid, __uuidof(IDXGIDeviceSubObject)) && !SameInterface(iid, __uuidof(IDXGISwapChain)) &&
                !SameInterface(iid, __uuidof(IDXGISwapChain1))) return E_NOINTERFACE;
        *out = static_cast<IDXGISwapChain1*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&references_);
        if (!count) {
            publisher_.Reset();
            native_->Release();
            factory_->Release();
            this->~HostedSwapChain();
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
        AcquireSRWLockExclusive(&lock_);
        Publish(flags);
        const UINT nativeInterval = NativeInterval(interval, flags);
        const HRESULT code = native_->Present(nativeInterval, flags);
        TracePresent(interval, nativeInterval, flags, code);
        ReleaseSRWLockExclusive(&lock_);
        return code;
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
        if (count == 1) count = 2;
        AcquireSRWLockExclusive(&lock_);
        publisher_.Reset();
        clock_.interval = 0;
        const HRESULT code = native_->ResizeBuffers(count, width, height, format, flags & ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
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
        AcquireSRWLockExclusive(&lock_);
        Publish(flags);
        const UINT nativeInterval = NativeInterval(interval, flags);
        const HRESULT code = native_->Present1(nativeInterval, flags, parameters);
        TracePresent(interval, nativeInterval, flags, code);
        ReleaseSRWLockExclusive(&lock_);
        return code;
    }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return native_->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** out) override { return native_->GetRestrictToOutput(out); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* color) override { return native_->SetBackgroundColor(color); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* out) override { return native_->GetBackgroundColor(out); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override { return native_->SetRotation(rotation); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* out) override { return native_->GetRotation(out); }
};

class Factory final : public IDXGIFactory2 {
    IDXGIFactory2* native_;
    volatile LONG references_ = 1;
    void* volatile associatedWindow_ = nullptr;

    HRESULT CreateVirtualChain(IUnknown* device, HWND window, RECT area, DXGI_SWAP_CHAIN_DESC1 desc, IDXGISwapChain1** out) {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!device || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 ||
                area.right <= area.left || area.bottom <= area.top) return DXGI_ERROR_INVALID_CALL;
        if (!desc.Width) desc.Width = area.right - area.left;
        if (!desc.Height) desc.Height = area.bottom - area.top;
        if (desc.BufferCount < 2) desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        IDXGISwapChain1* native = nullptr;
        const HRESULT code = native_->CreateSwapChainForComposition(device, &desc, nullptr, &native);
        Trace("composition-swap-chain", code);
        if (FAILED(code)) return code;
        void* storage = HeapAlloc(GetProcessHeap(), 0, sizeof(HostedSwapChain));
        if (!storage) {
            native->Release();
            return E_OUTOFMEMORY;
        }
        *out = new (storage) HostedSwapChain(native, this, window);
        return S_OK;
    }

public:
    explicit Factory(IDXGIFactory2* native) : native_(native) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!SameInterface(iid, __uuidof(IUnknown)) && !SameInterface(iid, __uuidof(IDXGIObject)) &&
                !SameInterface(iid, __uuidof(IDXGIFactory)) && !SameInterface(iid, __uuidof(IDXGIFactory1)) &&
                !SameInterface(iid, __uuidof(IDXGIFactory2))) return E_NOINTERFACE;
        *out = static_cast<IDXGIFactory2*>(this);
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
        return native_->CreateSwapChainForComposition(device, desc, restrictTo, out);
    }
};

} // namespace

extern "C" HRESULT WINAPI XboxCreateDXGIFactory1(REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    IDXGIFactory2* native = nullptr;
    HRESULT code = CreateDXGIFactory1(IID_PPV_ARGS(&native));
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
