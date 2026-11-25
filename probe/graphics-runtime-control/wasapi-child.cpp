#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include "../../runtime/shared/child-process.h"
#ifdef PROBE_WASAPI_ASYNC
#include "../../runtime/win32/wasapi-activation.h"
#endif

using Microsoft::WRL::ComPtr;

static unsigned Failure(unsigned operation, HRESULT error) {
    return 0x74000000u | (operation << 16) | (static_cast<unsigned>(error) & 0xffffu);
}

#ifndef PROBE_WASAPI_ASYNC
class EndpointNotifications final : public IMMNotificationClient {
    volatile LONG references_ = 1;

public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!InlineIsEqualGUID(iid, __uuidof(IUnknown)) &&
                !InlineIsEqualGUID(iid, __uuidof(IMMNotificationClient))) return E_NOINTERFACE;
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&references_); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

#ifdef PROBE_WASAPI_HOSTED
static HRESULT CheckHostedDiscovery(IMMDeviceEnumerator *enumerator, IMMDeviceCollection *endpoints,
                                    IMMDevice *selected, UINT count) {
    if (count != 1) return E_UNEXPECTED;
    ComPtr<IMMDevice> item;
    HRESULT result = endpoints->Item(0, &item);
    if (FAILED(result) || !item) return FAILED(result) ? result : E_UNEXPECTED;
    LPWSTR first = nullptr;
    LPWSTR second = nullptr;
    result = item->GetId(&first);
    if (SUCCEEDED(result)) result = selected->GetId(&second);
    if (SUCCEEDED(result) && (!first || !second || CompareStringOrdinal(first, -1, second, -1, FALSE) != CSTR_EQUAL))
        result = E_UNEXPECTED;
    CoTaskMemFree(first);
    CoTaskMemFree(second);
    if (FAILED(result)) return result;

    ComPtr<IUnknown> identity;
    ComPtr<IUnknown> endpointIdentity;
    ComPtr<IMMEndpoint> direction;
    result = selected->QueryInterface(IID_PPV_ARGS(&identity));
    if (SUCCEEDED(result)) result = item->QueryInterface(IID_PPV_ARGS(&direction));
    if (SUCCEEDED(result)) result = direction ? direction.As(&endpointIdentity) : E_UNEXPECTED;
    if (FAILED(result) || !identity || identity.Get() != endpointIdentity.Get())
        return FAILED(result) ? result : E_UNEXPECTED;

    ComPtr<IPropertyStore> properties;
    result = item->OpenPropertyStore(STGM_READ, &properties);
    if (FAILED(result) || !properties) return FAILED(result) ? result : E_UNEXPECTED;
    DWORD propertyCount = 0;
    result = properties->GetCount(&propertyCount);
    if (FAILED(result) || propertyCount != 1) return FAILED(result) ? result : E_UNEXPECTED;
    constexpr PROPERTYKEY friendly =
        {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    PROPERTYKEY firstKey{};
    result = properties->GetAt(0, &firstKey);
    if (FAILED(result) || firstKey.pid != friendly.pid || !InlineIsEqualGUID(firstKey.fmtid, friendly.fmtid))
        return FAILED(result) ? result : E_UNEXPECTED;
    PROPVARIANT name{};
    result = properties->GetValue(friendly, &name);
    const bool named = name.vt == VT_LPWSTR && name.pwszVal && name.pwszVal[0];
    PropVariantClear(&name);
    if (FAILED(result) || !named) return FAILED(result) ? result : E_UNEXPECTED;
    constexpr PROPERTYKEY deviceFormat =
        {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};
    PROPVARIANT format{};
    result = properties->GetValue(deviceFormat, &format);
    const bool missingFormat = format.vt == VT_EMPTY && !format.blob.cbSize && !format.blob.pBlobData;
    PropVariantClear(&format);
    if (FAILED(result) || !missingFormat) return FAILED(result) ? result : E_UNEXPECTED;

    const HRESULT notFound = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    ComPtr<IMMDevice> missing;
    if (endpoints->Item(1, &missing) != E_INVALIDARG || missing ||
            enumerator->GetDevice(L"invalid-device", &missing) != notFound || missing ||
            enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &missing) != notFound || missing)
        return E_UNEXPECTED;
    ComPtr<IMMDeviceCollection> capture;
    result = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &capture);
    if (SUCCEEDED(result)) result = capture ? capture->GetCount(&count) : E_UNEXPECTED;
    if (FAILED(result)) return result;
    return count == 0 ? S_OK : E_UNEXPECTED;
}
#endif

static unsigned OpenDesktopRenderClient(IAudioClient **out) {
    EndpointNotifications notifications;
    ComPtr<IMMDeviceEnumerator> enumerator;
#ifdef PROBE_WASAPI_HOSTED
    const HMODULE adapter = LoadPackagedLibrary(L"XboxWin32DefaultAudio.dll", 0);
    if (!adapter) return Failure(26, HRESULT_FROM_WIN32(GetLastError()));
    using CreateEnumerator = HRESULT (WINAPI *)(REFIID, void **);
    const auto factory = reinterpret_cast<CreateEnumerator>(
        reinterpret_cast<void *>(GetProcAddress(adapter, "XboxCreateDefaultAudioEnumerator")));
    if (!factory) return Failure(26, HRESULT_FROM_WIN32(GetLastError()));
    HRESULT result = factory(__uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(enumerator.GetAddressOf()));
#else
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&enumerator));
#endif
    if (FAILED(result)) return Failure(2, result);
    if (!enumerator) return Failure(2, E_UNEXPECTED);
    ComPtr<IMMDeviceCollection> endpoints;
    result = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints);
    if (FAILED(result)) return Failure(3, result);
    UINT count = 0;
    result = endpoints ? endpoints->GetCount(&count) : E_UNEXPECTED;
    if (FAILED(result) || !count) return Failure(3, FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    ComPtr<IMMDevice> selected;
    result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &selected);
    if (FAILED(result) || !selected) return Failure(4, FAILED(result) ? result : E_UNEXPECTED);
#ifdef PROBE_WASAPI_HOSTED
    result = CheckHostedDiscovery(enumerator.Get(), endpoints.Get(), selected.Get(), count);
    if (FAILED(result)) return Failure(27, result);
    result = enumerator->RegisterEndpointNotificationCallback(&notifications);
    if (result != E_NOTIMPL) return Failure(5, FAILED(result) ? result : E_UNEXPECTED);
    result = enumerator->UnregisterEndpointNotificationCallback(&notifications);
    if (result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return Failure(6, FAILED(result) ? result : E_UNEXPECTED);
#else
    result = enumerator->RegisterEndpointNotificationCallback(&notifications);
    if (FAILED(result)) return Failure(5, result);
    const HRESULT removed = enumerator->UnregisterEndpointNotificationCallback(&notifications);
    // Keep the stack-owned callback alive until process exit if native detach fails.
    if (FAILED(removed)) ExitProcess(Failure(6, removed));
#endif

    LPWSTR id = nullptr;
    result = selected->GetId(&id);
    if (FAILED(result) || !id) {
        CoTaskMemFree(id);
        return Failure(7, FAILED(result) ? result : E_UNEXPECTED);
    }
    ComPtr<IMMDevice> render;
    result = enumerator->GetDevice(id, &render);
    CoTaskMemFree(id);
    if (FAILED(result) || !render) return Failure(8, FAILED(result) ? result : E_UNEXPECTED);
    ComPtr<IMMEndpoint> endpoint;
    result = render->QueryInterface(IID_PPV_ARGS(&endpoint));
    if (FAILED(result) || !endpoint) return Failure(25, FAILED(result) ? result : E_UNEXPECTED);
    EDataFlow direction = eAll;
    result = endpoint->GetDataFlow(&direction);
    if (FAILED(result) || direction != eRender) return Failure(25, FAILED(result) ? result : E_UNEXPECTED);
    result = render->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(out));
    return FAILED(result) || !*out ? Failure(9, FAILED(result) ? result : E_UNEXPECTED) : 0;
}
#endif

static HRESULT PrepareClient(IAudioClient *client, WAVEFORMATEX *mix) {
    constexpr DWORD sampleRate = 44100;
    constexpr UINT32 requestedFrames = 512;
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (mix->nSamplesPerSec != sampleRate) {
        flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        mix->nSamplesPerSec = sampleRate;
        mix->nAvgBytesPerSec = sampleRate * mix->nBlockAlign;
    }
    ComPtr<IAudioClient2> properties;
    if (SUCCEEDED(client->QueryInterface(IID_PPV_ARGS(&properties))) && properties) {
        const AudioClientProperties settings{sizeof(settings), FALSE, AudioCategory_Other, AUDCLNT_STREAMOPTIONS_NONE};
        properties->SetClientProperties(&settings);
    }
    ComPtr<IAudioClient3> modern;
    HRESULT result = client->QueryInterface(IID_PPV_ARGS(&modern));
    if (SUCCEEDED(result) && modern) {
        UINT32 period, fundamental, minimum, maximum;
        result = modern->GetSharedModeEnginePeriod(mix, &period, &fundamental, &minimum, &maximum);
        if (SUCCEEDED(result) && fundamental && minimum && minimum <= maximum) {
            const UINT64 rounded = (requestedFrames + (UINT64)fundamental / 2) / fundamental * fundamental;
            const UINT32 frames = rounded < minimum ? minimum : (rounded > maximum ? maximum : (UINT32)rounded);
            result = modern->InitializeSharedAudioStream(flags, frames, mix, nullptr);
            if (SUCCEEDED(result)) return result;
        }
    }
    return client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, mix, nullptr);
}

static HRESULT SubmitSilence(IAudioRenderClient *render, UINT32 frames) {
    BYTE *buffer = nullptr;
    const HRESULT result = render->GetBuffer(frames, &buffer);
    if (FAILED(result)) return result;
    return render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
}

static unsigned ObserveRenderEvents(IAudioClient *client, IAudioRenderClient *render, HANDLE event, UINT32 frames) {
    unsigned drained = 0;
    for (unsigned wake = 0; wake < 4; ++wake) {
        const DWORD wait = WaitForSingleObject(event, 1000);
        if (wait != WAIT_OBJECT_0) return Failure(18, HRESULT_FROM_WIN32(
            wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError()));
        UINT32 padding = 0;
        HRESULT result = client->GetCurrentPadding(&padding);
        if (FAILED(result)) return Failure(19, result);
        if (padding > frames) return Failure(19, E_UNEXPECTED);
        if (padding == frames) continue;
        result = SubmitSilence(render, frames - padding);
        if (FAILED(result)) return Failure(20, result);
        if (++drained == 2) return 0;
    }
    return Failure(19, HRESULT_FROM_WIN32(ERROR_NO_DATA));
}

static unsigned ExerciseRender(IAudioClient *client, HANDLE event) {
    WAVEFORMATEX *mix = nullptr;
    HRESULT result = client->GetMixFormat(&mix);
    if (FAILED(result) || !mix || !mix->nBlockAlign || !mix->nChannels || !mix->nSamplesPerSec) {
        CoTaskMemFree(mix);
        return Failure(11, FAILED(result) ? result : E_UNEXPECTED);
    }
    REFERENCE_TIME period = 0;
    result = client->GetDevicePeriod(&period, nullptr);
    if (FAILED(result) || period <= 0) {
        CoTaskMemFree(mix);
        return Failure(12, FAILED(result) ? result : E_UNEXPECTED);
    }
    result = PrepareClient(client, mix);
    CoTaskMemFree(mix);
    if (FAILED(result)) return Failure(13, result);
    result = client->SetEventHandle(event);
    if (FAILED(result)) return Failure(14, result);
    UINT32 frames = 0;
    result = client->GetBufferSize(&frames);
    if (FAILED(result) || !frames) return Failure(15, FAILED(result) ? result : E_UNEXPECTED);
    ComPtr<IAudioRenderClient> render;
    result = client->GetService(IID_PPV_ARGS(&render));
    if (FAILED(result) || !render) return Failure(16, FAILED(result) ? result : E_UNEXPECTED);
    result = SubmitSilence(render.Get(), frames);
    if (FAILED(result)) return Failure(17, result);
    result = client->Start();
    if (FAILED(result)) return Failure(21, result);
    const unsigned observed = ObserveRenderEvents(client, render.Get(), event, frames);
    result = client->Stop();
    if (observed) return observed;
    if (FAILED(result)) return Failure(22, result);
    result = client->Reset();
    return FAILED(result) ? Failure(23, result) : PROBE_EXPECTED_CHILD_EXIT;
}

static unsigned TryAudio() {
    ComPtr<IAudioClient> client;
#ifdef PROBE_WASAPI_ASYNC
    const HRESULT opened = xbox_wasapi::OpenDefaultRenderClient(3500, client.GetAddressOf());
    if (FAILED(opened)) return Failure(24, opened);
#else
    const unsigned opened = OpenDesktopRenderClient(client.GetAddressOf());
    if (opened) return opened;
#endif
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready) return Failure(10, HRESULT_FROM_WIN32(GetLastError()));
    const unsigned outcome = ExerciseRender(client.Get(), ready);
    client.Reset();
    CloseHandle(ready);
    return outcome;
}

extern "C" void WINAPI ChildEntry() {
#ifdef PROBE_WASAPI_ASYNC
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#else
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (initialized == RPC_E_CHANGED_MODE) initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    if (FAILED(initialized)) ExitProcess(Failure(1, initialized));
    const unsigned result = TryAudio();
    CoUninitialize();
    ExitProcess(result);
}
