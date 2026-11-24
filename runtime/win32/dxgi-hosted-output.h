// Included in dxgi.cpp's private namespace.

bool HostedOutputEnabled() {
    WCHAR value[2];
    const DWORD previous = GetLastError();
    const bool enabled = GetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_DXGI_OUTPUT", value, ARRAYSIZE(value)) == 1 &&
        value[0] == L'1';
    SetLastError(previous);
    return enabled;
}

bool ReadHostedOutput(DXGI_OUTPUT_DESC& description, DXGI_MODE_DESC& mode) {
    const HMODULE windows = GetModuleHandleW(L"XboxWin32Window.dll");
    if (!windows) return false;
    const auto monitorFromPoint = reinterpret_cast<decltype(&MonitorFromPoint)>(
        reinterpret_cast<void*>(GetProcAddress(windows, "MonitorFromPoint")));
    const auto monitorInfo = reinterpret_cast<decltype(&GetMonitorInfoW)>(
        reinterpret_cast<void*>(GetProcAddress(windows, "GetMonitorInfoW")));
    const auto displaySettings = reinterpret_cast<decltype(&EnumDisplaySettingsW)>(
        reinterpret_cast<void*>(GetProcAddress(windows, "EnumDisplaySettingsW")));
    if (!monitorFromPoint || !monitorInfo || !displaySettings) return false;
    const HMONITOR monitor = monitorFromPoint({}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info;
    info.cbSize = sizeof(info);
    if (!monitor || !monitorInfo(monitor, reinterpret_cast<MONITORINFO*>(&info))) return false;
    DEVMODEW settings;
    settings.dmSize = sizeof(settings);
    settings.dmDriverExtra = 0;
    if (!displaySettings(info.szDevice, ENUM_CURRENT_SETTINGS, &settings) || !settings.dmPelsWidth ||
            !settings.dmPelsHeight || settings.dmDisplayFrequency <= 1) return false;
    static_assert(ARRAYSIZE(description.DeviceName) == ARRAYSIZE(info.szDevice));
    for (UINT index = 0; index < ARRAYSIZE(description.DeviceName); ++index)
        description.DeviceName[index] = info.szDevice[index];
    description.DesktopCoordinates = info.rcMonitor;
    description.AttachedToDesktop = TRUE;
    description.Rotation = DXGI_MODE_ROTATION_IDENTITY;
    description.Monitor = monitor;
    mode.Width = settings.dmPelsWidth;
    mode.Height = settings.dmPelsHeight;
    mode.RefreshRate = {settings.dmDisplayFrequency, 1};
    mode.Format = DXGI_FORMAT_UNKNOWN;
    mode.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
    mode.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    return true;
}

class HostedOutput final : public IDXGIOutput {
    volatile LONG references_ = 1;
    ComPtr<IDXGIAdapter> parent_;
    const DXGI_OUTPUT_DESC description_;
    const DXGI_MODE_DESC mode_;

    static bool SupportedFormat(DXGI_FORMAT format) {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    DXGI_MODE_DESC Mode(DXGI_FORMAT format) const {
        auto mode = mode_;
        mode.Format = format;
        return mode;
    }

public:
    HostedOutput(IDXGIAdapter* parent, const DXGI_OUTPUT_DESC& description, const DXGI_MODE_DESC& mode)
        : parent_(parent), description_(description), mode_(mode) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!SameInterface(iid, __uuidof(IUnknown)) && !SameInterface(iid, __uuidof(IDXGIObject)) &&
                !SameInterface(iid, __uuidof(IDXGIOutput))) return E_NOINTERFACE;
        *out = static_cast<IDXGIOutput*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&references_);
        if (!count) {
            this->~HostedOutput();
            HeapFree(GetProcessHeap(), 0, this);
        }
        return count;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return DXGI_ERROR_NOT_FOUND; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID iid, void** out) override { return parent_->QueryInterface(iid, out); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC* out) override {
        if (!out) return DXGI_ERROR_INVALID_CALL;
        *out = description_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayModeList(DXGI_FORMAT format, UINT flags, UINT* count,
            DXGI_MODE_DESC* modes) override {
        if (!count || (flags & ~(DXGI_ENUM_MODES_INTERLACED | DXGI_ENUM_MODES_SCALING)))
            return DXGI_ERROR_INVALID_CALL;
        const UINT available = SupportedFormat(format) ? 1 : 0;
        const UINT capacity = modes ? *count : 0;
        *count = available;
        if (modes && available) {
            if (capacity < available) return DXGI_ERROR_MORE_DATA;
            *modes = Mode(format);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC* desired, DXGI_MODE_DESC* out,
            IUnknown* device) override {
        if (!desired || !out || ((!desired->Width) != (!desired->Height)) ||
                (desired->RefreshRate.Numerator && !desired->RefreshRate.Denominator) ||
                (!device && desired->Format == DXGI_FORMAT_UNKNOWN)) return DXGI_ERROR_INVALID_CALL;
        if (device || !SupportedFormat(desired->Format)) return DXGI_ERROR_UNSUPPORTED;
        *out = Mode(desired->Format);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE WaitForVBlank() override { return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE; }
    HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown*, BOOL) override { return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE; }
    void STDMETHODCALLTYPE ReleaseOwnership() override {}
    HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES*) override {
        return DXGI_ERROR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL*) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL*) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface*) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface*) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*) override {
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
};

constexpr GUID hostedNativeAdapterId = {0x3d1ce800, 0x40a1, 0x46d7, {0xb0, 0x56, 0x92, 0x39, 0x47, 0xbb, 0xbe, 0x86}};

struct IHostedNativeAdapter : IUnknown {
    virtual IDXGIAdapter* STDMETHODCALLTYPE NativeAdapter() = 0;
};

class HostedAdapter final : public IDXGIAdapter1, public IHostedNativeAdapter {
    volatile LONG references_ = 1;
    ComPtr<IDXGIAdapter1> native_;
    ComPtr<IDXGIFactory> parent_;

public:
    HostedAdapter(IDXGIAdapter1* native, IDXGIFactory* parent) : native_(native), parent_(parent) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (SameInterface(iid, hostedNativeAdapterId)) {
            *out = static_cast<IHostedNativeAdapter*>(this);
        } else if (SameInterface(iid, __uuidof(IUnknown)) || SameInterface(iid, __uuidof(IDXGIObject)) ||
                SameInterface(iid, __uuidof(IDXGIAdapter)) || SameInterface(iid, __uuidof(IDXGIAdapter1))) {
            *out = static_cast<IDXGIAdapter1*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&references_);
        if (!count) {
            this->~HostedAdapter();
            HeapFree(GetProcessHeap(), 0, this);
        }
        return count;
    }
    IDXGIAdapter* STDMETHODCALLTYPE NativeAdapter() override { return native_.Get(); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID key, UINT size, const void* data) override {
        return native_->SetPrivateData(key, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID key, const IUnknown* value) override {
        return native_->SetPrivateDataInterface(key, value);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID key, UINT* size, void* data) override {
        return native_->GetPrivateData(key, size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID iid, void** out) override { return parent_->QueryInterface(iid, out); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* out) override { return native_->GetDesc(out); }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* out) override { return native_->GetDesc1(out); }
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID iid, LARGE_INTEGER* version) override {
        return native_->CheckInterfaceSupport(iid, version);
    }
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT index, IDXGIOutput** out) override {
        const HRESULT result = native_->EnumOutputs(index, out);
        if (!out || index || result != DXGI_ERROR_NOT_FOUND) return result;
        const DWORD previous = GetLastError();
        DXGI_OUTPUT_DESC description;
        DXGI_MODE_DESC mode;
        const bool hasHostedOutput = ReadHostedOutput(description, mode);
        SetLastError(previous);
        if (!hasHostedOutput) return result;
        void* storage = HeapAlloc(GetProcessHeap(), 0, sizeof(HostedOutput));
        if (!storage) return E_OUTOFMEMORY;
        *out = new (storage) HostedOutput(this, description, mode);
        static volatile LONG events = 0;
        if (InterlockedIncrement(&events) <= 8) Trace("hosted-output-fallback", result);
        SetLastError(previous);
        return S_OK;
    }
};

void MaybeWrapHostedAdapter(IDXGIFactory* parent, IDXGIAdapter** adapter) {
    ComPtr<IDXGIAdapter1> native;
    DXGI_ADAPTER_DESC1 description;
    if (FAILED((*adapter)->QueryInterface(IID_PPV_ARGS(&native))) || FAILED(native->GetDesc1(&description)) ||
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return;
    ComPtr<IDXGIOutput> output;
    const HRESULT result = native->EnumOutputs(0, &output);
    static volatile LONG events = 0;
    if (InterlockedIncrement(&events) <= 8) Trace("hosted-output-native", result);
    if (result != DXGI_ERROR_NOT_FOUND) return;
    DXGI_OUTPUT_DESC hosted;
    DXGI_MODE_DESC mode;
    if (!ReadHostedOutput(hosted, mode)) return;
    void* storage = HeapAlloc(GetProcessHeap(), 0, sizeof(HostedAdapter));
    if (!storage) return;
    (*adapter)->Release();
    *adapter = new (storage) HostedAdapter(native.Get(), parent);
}
