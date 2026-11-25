#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <activation.h>
#include <appmodel.h>
#include <strsafe.h>
#include <winstring.h>

namespace {
constexpr WCHAR className[] = L"BigPicture.Native.Launcher";
// ABI of the two statics in BigPicture.Native.idl.
constexpr GUID launcherStaticsId{0xd40b9765, 0xf8ae, 0x4494, {0x8a,0x46,0xd9,0x70,0x87,0x01,0xe0,0x5c}};
struct ILauncherStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Start(UINT32 *pid) = 0;
    virtual HRESULT STDMETHODCALLTYPE PollExitCode(INT64 *code) = 0;
};

class Launcher final : public IActivationFactory, public ILauncherStatics {
    HANDLE process = nullptr;
    DWORD pid = 0;
    INT64 lastExit = -1;
    SRWLOCK lock = SRWLOCK_INIT;

    struct Guard {
        SRWLOCK &lock;
        explicit Guard(SRWLOCK &value) : lock(value) { AcquireSRWLockExclusive(&lock); }
        ~Guard() { ReleaseSRWLockExclusive(&lock); }
    };

public:
    HRESULT STDMETHODCALLTYPE Start(UINT32 *result) override {
        if (!result) return E_POINTER;
        Guard guard(lock);
        // Calling again in the same host (e.g. reactivation) uses the initial launch.
        if (pid) { *result = pid; return S_OK; }
        WCHAR directory[MAX_PATH], exe[MAX_PATH], command[MAX_PATH + 16];
        UINT32 length = ARRAYSIZE(directory);
        LONG error = GetCurrentPackagePath(&length, directory);
        if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
        HRESULT hr = StringCchCatW(directory, ARRAYSIZE(directory), L"\\ValveClient");
        if (SUCCEEDED(hr)) hr = StringCchCopyW(exe, ARRAYSIZE(exe), directory);
        if (SUCCEEDED(hr)) hr = StringCchCatW(exe, ARRAYSIZE(exe), L"\\steam.exe");
        if (SUCCEEDED(hr)) hr = StringCchCopyW(command, ARRAYSIZE(command), L"\"");
        if (SUCCEEDED(hr)) hr = StringCchCatW(command, ARRAYSIZE(command), exe);
        if (SUCCEEDED(hr)) hr = StringCchCatW(command, ARRAYSIZE(command), L"\" -gamepadui");
        if (FAILED(hr)) return hr;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info;
        if (!CreateProcessW(exe, command, nullptr, nullptr, FALSE, 0, nullptr, directory, &startup, &info))
            return HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(info.hThread);
        process = info.hProcess;
        *result = pid = info.dwProcessId;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE PollExitCode(INT64 *code) override {
        if (!code) return E_POINTER;
        Guard guard(lock);
        if (!pid) return E_UNEXPECTED;
        if (process) {
            DWORD wait = WaitForSingleObjectEx(process, 0, FALSE);
            if (wait == WAIT_FAILED) return HRESULT_FROM_WIN32(GetLastError());
            if (wait == WAIT_OBJECT_0) {
                DWORD exitCode;
                if (!GetExitCodeProcess(process, &exitCode)) return HRESULT_FROM_WIN32(GetLastError());
                CloseHandle(process);
                process = nullptr;
                lastExit = exitCode;
            }
        }
        *code = lastExit; // -1 while running, otherwise the unsigned 32-bit exit code.
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (IsEqualIID(iid, launcherStaticsId))
            *result = static_cast<ILauncherStatics *>(this);
        else if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IInspectable) ||
                 IsEqualIID(iid, IID_IActivationFactory) || IsEqualIID(iid, IID_IAgileObject))
            *result = static_cast<IActivationFactory *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    // The singleton and its process observation live for the host's lifetime.
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *count, IID **iids) override {
        if (!count || !iids) return E_POINTER;
        *count = 0;
        *iids = static_cast<IID *>(CoTaskMemAlloc(2 * sizeof(IID)));
        if (!*iids) return E_OUTOFMEMORY;
        (*iids)[0] = IID_IActivationFactory;
        (*iids)[1] = launcherStaticsId;
        *count = 2;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override {
        return WindowsCreateString(className, ARRAYSIZE(className) - 1, name);
    }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *level) override {
        if (!level) return E_POINTER;
        *level = BaseTrust;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable **instance) override {
        if (!instance) return E_POINTER;
        *instance = nullptr;
        return E_NOTIMPL; // Static-only runtime class.
    }
};
constinit Launcher factory;
}

extern "C" HRESULT WINAPI DllGetActivationFactory(HSTRING requested, IActivationFactory **result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    UINT32 length;
    auto name = WindowsGetStringRawBuffer(requested, &length);
    if (length != ARRAYSIZE(className) - 1) return CLASS_E_CLASSNOTAVAILABLE;
    for (UINT32 index = 0; index < length; ++index)
        if (name[index] != className[index]) return CLASS_E_CLASSNOTAVAILABLE;
    *result = static_cast<IActivationFactory *>(&factory);
    factory.AddRef();
    return S_OK;
}

extern "C" HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }
