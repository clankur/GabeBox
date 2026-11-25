#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "wasapi-activation.h"
extern "C" {
#include "loader-trace-output.h"
}

namespace {
using Microsoft::WRL::ComPtr;

constexpr DWORD activationTimeout = 3500;
constexpr WCHAR endpointId[] = L"XboxWin32:DefaultRender";
constexpr WCHAR outputName[] = L"Default Xbox audio output";
constexpr PROPERTYKEY friendlyName =
    {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

void RecordRenderActivation(HRESULT result) {
    static volatile LONG remaining = 16;
    if (InterlockedCompareExchange(&remaining, 0, 0) <= 0 || InterlockedDecrement(&remaining) < 0) return;
    char line[] = "asset mmdevice-default-render-activation result=0x00000000\r\n";
    const char hex[] = "0123456789ABCDEF";
    const DWORD code = static_cast<DWORD>(result);
    for (DWORD digit = 0; digit < 8; ++digit)
        line[sizeof(line) - 11 + digit] = hex[(code >> (28 - digit * 4)) & 15];
    AppendTrace(line, sizeof(line) - 1);
}

template<class T, class... Args>
T *Allocate(Args... args) {
    void *storage = HeapAlloc(GetProcessHeap(), 0, sizeof(T));
    return storage ? new (storage) T(args...) : nullptr;
}

class References {
    volatile LONG count_ = 1;
public:
    ULONG Add() { return InterlockedIncrement(&count_); }

    template<class T>
    ULONG Drop(T *object) {
        const LONG count = InterlockedDecrement(&count_);
        if (!count) {
            object->~T();
            HeapFree(GetProcessHeap(), 0, object);
        }
        return count;
    }
};

template<class Interface>
HRESULT QuerySingle(Interface *object, REFIID iid, void **out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!InlineIsEqualGUID(iid, __uuidof(IUnknown)) && !InlineIsEqualGUID(iid, __uuidof(Interface)))
        return E_NOINTERFACE;
    *out = object;
    object->AddRef();
    return S_OK;
}

template<SIZE_T count>
HRESULT CopyString(const WCHAR (&source)[count], LPWSTR *out) {
    if (!out) return E_POINTER;
    *out = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(source)));
    if (!*out) return E_OUTOFMEMORY;
    for (SIZE_T i = 0; i < count; ++i) (*out)[i] = source[i];
    return S_OK;
}

class PropertyStore final : public IPropertyStore {
    References references_;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        return QuerySingle<IPropertyStore>(this, iid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return references_.Add(); }
    ULONG STDMETHODCALLTYPE Release() override { return references_.Drop(this); }
    HRESULT STDMETHODCALLTYPE GetCount(DWORD *count) override {
        if (!count) return E_POINTER;
        *count = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAt(DWORD index, PROPERTYKEY *key) override {
        if (!key) return E_POINTER;
        if (index) return E_INVALIDARG;
        *key = friendlyName;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT *value) override {
        if (!value) return E_POINTER;
        *value = {};
        if (key.pid != friendlyName.pid || !InlineIsEqualGUID(key.fmtid, friendlyName.fmtid))
            return S_OK; // Windows property stores use VT_EMPTY for an absent key.
        const HRESULT copied = CopyString(outputName, &value->pwszVal);
        if (SUCCEEDED(copied)) value->vt = VT_LPWSTR;
        return copied;
    }
    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Commit() override { return STG_E_ACCESSDENIED; }
};

class Endpoint final : public IMMDevice, public IMMEndpoint {
    References references_;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (InlineIsEqualGUID(iid, __uuidof(IMMEndpoint))) {
            if (!out) return E_POINTER;
            *out = static_cast<IMMEndpoint *>(this);
            AddRef();
            return S_OK;
        }
        return QuerySingle<IMMDevice>(this, iid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return references_.Add(); }
    ULONG STDMETHODCALLTYPE Release() override { return references_.Drop(this); }
    HRESULT STDMETHODCALLTYPE Activate(REFIID iid, DWORD context, PROPVARIANT *params, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (params || !(context & CLSCTX_INPROC_SERVER)) return E_INVALIDARG;
        if (!InlineIsEqualGUID(iid, __uuidof(IAudioClient))) return E_NOINTERFACE;
        ComPtr<IAudioClient> client;
        HRESULT result = xbox_wasapi::OpenDefaultRenderClient(activationTimeout, client.GetAddressOf());
        if (SUCCEEDED(result) && !client) result = E_UNEXPECTED;
        RecordRenderActivation(result);
        if (FAILED(result)) return result;
        *out = client.Detach();
        return result;
    }
    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD access, IPropertyStore **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (access != STGM_READ) return STG_E_ACCESSDENIED;
        *out = Allocate<PropertyStore>();
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR *out) override { return CopyString(endpointId, out); }
    HRESULT STDMETHODCALLTYPE GetState(DWORD *state) override {
        if (!state) return E_POINTER;
        *state = DEVICE_STATE_ACTIVE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataFlow(EDataFlow *flow) override {
        if (!flow) return E_POINTER;
        *flow = eRender;
        return S_OK;
    }
};

class Endpoints final : public IMMDeviceCollection {
    References references_;
    ComPtr<IMMDevice> endpoint_;
public:
    explicit Endpoints(IMMDevice *endpoint) : endpoint_(endpoint) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        return QuerySingle<IMMDeviceCollection>(this, iid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return references_.Add(); }
    ULONG STDMETHODCALLTYPE Release() override { return references_.Drop(this); }
    HRESULT STDMETHODCALLTYPE GetCount(UINT *count) override {
        if (!count) return E_POINTER;
        *count = endpoint_ ? 1 : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Item(UINT index, IMMDevice **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (index || !endpoint_) return E_INVALIDARG;
        return endpoint_.CopyTo(out);
    }
};

class Enumerator final : public IMMDeviceEnumerator {
    References references_;
    ComPtr<IMMDevice> endpoint_;
public:
    explicit Enumerator(IMMDevice *endpoint) : endpoint_(endpoint) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        return QuerySingle<IMMDeviceEnumerator>(this, iid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return references_.Add(); }
    ULONG STDMETHODCALLTYPE Release() override { return references_.Drop(this); }
    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow flow, DWORD states, IMMDeviceCollection **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if ((flow != eRender && flow != eCapture && flow != eAll) || (states & ~DEVICE_STATEMASK_ALL))
            return E_INVALIDARG;
        *out = Allocate<Endpoints>(flow != eCapture && (states & DEVICE_STATE_ACTIVE) ? endpoint_.Get() : nullptr);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow flow, ERole role, IMMDevice **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if ((flow != eCapture && flow != eRender) || (role != eConsole && role != eMultimedia && role != eCommunications))
            return E_INVALIDARG;
        return flow == eRender ? endpoint_.CopyTo(out) : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR id, IMMDevice **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!id) return E_POINTER;
        if (CompareStringOrdinal(id, -1, endpointId, -1, TRUE) != CSTR_EQUAL)
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        return endpoint_.CopyTo(out);
    }
    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient *client) override {
        return client ? E_NOTIMPL : E_POINTER;
    }
    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient *client) override {
        return client ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) : E_POINTER;
    }
};
} // namespace

extern "C" HRESULT WINAPI XboxCreateDefaultAudioEnumerator(REFIID iid, void **out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!InlineIsEqualGUID(iid, __uuidof(IMMDeviceEnumerator)) && !InlineIsEqualGUID(iid, __uuidof(IUnknown)))
        return E_NOINTERFACE;

    ComPtr<IMMDevice> endpoint;
    endpoint.Attach(Allocate<Endpoint>());
    if (!endpoint) return E_OUTOFMEMORY;
    *out = Allocate<Enumerator>(endpoint.Get());
    return *out ? S_OK : E_OUTOFMEMORY;
}
