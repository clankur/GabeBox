#pragma once

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <new>
#include <wrl/client.h>

namespace xbox_wasapi {

class AudioActivation final : public IActivateAudioInterfaceCompletionHandler {
    volatile LONG references_ = 1;
    HANDLE ready_;

public:
    explicit AudioActivation(HANDLE ready) : ready_(ready) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!InlineIsEqualGUID(iid, __uuidof(IUnknown)) &&
                !InlineIsEqualGUID(iid, __uuidof(IAgileObject)) &&
                !InlineIsEqualGUID(iid, __uuidof(IActivateAudioInterfaceCompletionHandler))) return E_NOINTERFACE;
        *out = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = InterlockedDecrement(&references_);
        if (!remaining) {
            CloseHandle(ready_);
            this->~AudioActivation();
            HeapFree(GetProcessHeap(), 0, this);
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *) override {
        SetEvent(ready_);
        return S_OK;
    }

    HRESULT Wait(DWORD timeout) {
        const DWORD wait = WaitForSingleObject(ready_, timeout);
        if (wait == WAIT_TIMEOUT) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if (wait == WAIT_FAILED) return HRESULT_FROM_WIN32(GetLastError());
        if (wait != WAIT_OBJECT_0) return E_UNEXPECTED;
        return S_OK;
    }
};

inline constexpr WCHAR defaultRender[] = L"{e6327cad-dcec-4949-ae8a-991e976a79d2}";

// Read the activation result on the calling COM thread. The agile callback
// only signals readiness and may finish after the caller's wait has expired.
inline HRESULT OpenDefaultRenderClient(DWORD timeout, IAudioClient **client) {
    if (!client) return E_POINTER;
    *client = nullptr;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready) return HRESULT_FROM_WIN32(GetLastError());
    void *storage = HeapAlloc(GetProcessHeap(), 0, sizeof(AudioActivation));
    if (!storage) {
        CloseHandle(ready);
        return E_OUTOFMEMORY;
    }
    Microsoft::WRL::ComPtr<AudioActivation> completion;
    completion.Attach(new (storage) AudioActivation(ready));
    Microsoft::WRL::ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    const HRESULT requested = ActivateAudioInterfaceAsync(
        defaultRender, __uuidof(IAudioClient), nullptr, completion.Get(), &operation);
    if (FAILED(requested)) return requested;
    if (!operation) return E_UNEXPECTED;
    const HRESULT readyResult = completion->Wait(timeout);
    if (FAILED(readyResult)) return readyResult;
    HRESULT activation = E_UNEXPECTED;
    Microsoft::WRL::ComPtr<IUnknown> result;
    const HRESULT completed = operation->GetActivateResult(&activation, &result);
    if (FAILED(completed)) return completed;
    if (FAILED(activation)) return activation;
    return result ? result->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void **>(client)) : E_UNEXPECTED;
}

} // namespace xbox_wasapi
