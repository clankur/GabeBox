#include <windows.h>
#include <winstring.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.h>
#include <winrt/XboxProbe.Native.h>
#include <array>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#include "child-process.h"
#include "pipe-protocol.h"

static std::wstring Hex(DWORD value) {
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring text = L"0x00000000";
    for (unsigned i = 0; i < 8; ++i) {
        text[9 - i] = digits[value & 0xF];
        value >>= 4;
    }
    return text;
}

static std::wstring ChildResult(std::wstring const& path) {
    HMODULE module = LoadPackagedLibrary(PROBE_CHILD_MODULE, 0);
    if (!module) return L"status=child-module-load-error\r\nwin32_error=" + Hex(GetLastError()) + L"\r\n";
    auto run = reinterpret_cast<RunPackagedChildFn>(GetProcAddress(module, PROBE_CHILD_EXPORT));
    if (!run) {
        auto error = GetLastError();
        FreeLibrary(module);
        return L"status=child-module-export-error\r\nwin32_error=" + Hex(error) + L"\r\n";
    }
    ChildProcessResult result;
    run(reinterpret_cast<const unsigned short *>(path.c_str()), &result);
    FreeLibrary(module);

    const bool matches = result.created && result.waitResult == WAIT_OBJECT_0 &&
        result.exitCode == PROBE_EXPECTED_CHILD_EXIT;
    return L"child_path=" + path + L"\r\ncreate_process=" + (result.created ? L"success" : L"failure") +
        L"\r\nwin32_error=" + Hex(result.error) + L"\r\nwait_result=" + Hex(result.waitResult) +
        L"\r\nexpected_exit_code=" + Hex(PROBE_EXPECTED_CHILD_EXIT) +
        L"\r\nchild_exit_code=" + Hex(result.exitCode) +
        L"\r\nexit_code_matches=" + (matches ? L"yes\r\n" : L"no\r\n");
}

static std::wstring PipeFailure(const wchar_t *stage, DWORD error) {
    return std::wstring(L"ipc=failed\r\nipc_stage=") + stage + L"\r\nipc_error=" + Hex(error) + L"\r\n";
}

static DWORD RemainingTime(ULONGLONG deadline) {
    ULONGLONG now = GetTickCount64();
    return now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
}

static DWORD CompletePipeIo(HANDLE pipe, OVERLAPPED& operation, DWORD& bytes,
                            BOOL started, ULONGLONG deadline) {
    if (started) return ERROR_SUCCESS;
    DWORD error = GetLastError();
    if (error != ERROR_IO_PENDING) return error;
    DWORD wait = WaitForSingleObject(operation.hEvent, RemainingTime(deadline));
    if (wait == WAIT_OBJECT_0) {
        return GetOverlappedResult(pipe, &operation, &bytes, FALSE) ? ERROR_SUCCESS : GetLastError();
    }
    error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    CancelIoEx(pipe, &operation);
    GetOverlappedResult(pipe, &operation, &bytes, TRUE);
    return error;
}

static std::wstring PipeResult(std::wstring const& child) {
    ULONGLONG tick = GetTickCount64();
    std::wstring suffix = Hex(GetCurrentProcessId()).substr(2) + Hex(DWORD(tick >> 32)).substr(2) +
                          Hex(DWORD(tick)).substr(2);
    std::wstring name = PROBE_PIPE_PREFIX + suffix;
    HANDLE rawPipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
        FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_REJECT_REMOTE_CLIENTS,
        1, 256, 256, 0, nullptr);
    if (rawPipe == INVALID_HANDLE_VALUE) return PipeFailure(L"create-pipe", GetLastError());
    winrt::handle pipe(rawPipe);
    winrt::handle connectEvent(CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS));
    if (!connectEvent) return PipeFailure(L"create-event", GetLastError());

    std::wstring arguments = L"\"" + child + L"\" " + suffix;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(child.c_str(), arguments.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &processInfo)) {
        return PipeFailure(L"create-child", GetLastError());
    }
    winrt::handle process(processInfo.hProcess);
    winrt::handle thread(processInfo.hThread);
    ULONGLONG deadline = GetTickCount64() + 5000;
    OVERLAPPED connect{};
    connect.hEvent = connectEvent.get();
    DWORD bytes = 0;
    DWORD error = CompletePipeIo(pipe.get(), connect, bytes, ConnectNamedPipe(pipe.get(), &connect), deadline);
    if (error != ERROR_SUCCESS && error != ERROR_PIPE_CONNECTED) {
        DWORD exit = 0;
        std::wstring result = PipeFailure(L"connect", error);
        if (GetExitCodeProcess(process.get(), &exit) && exit != STILL_ACTIVE)
            result += L"ipc_child_exit=" + Hex(exit) + L"\r\n";
        return result;
    }

    winrt::handle readEvent(CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS));
    if (!readEvent) return PipeFailure(L"create-event", GetLastError());
    winrt::handle writeEvent(CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS));
    if (!writeEvent) return PipeFailure(L"create-event", GetLastError());

    OVERLAPPED read{};
    read.hEvent = readEvent.get();
    DWORD request = 0;
    error = CompletePipeIo(pipe.get(), read, bytes, ReadFile(pipe.get(), &request, sizeof(request), &bytes, &read), deadline);
    if (error != ERROR_SUCCESS) return PipeFailure(L"read-request", error);
    if (bytes != sizeof(request) || request != (processInfo.dwProcessId ^ PROBE_PIPE_HELLO))
        return PipeFailure(L"invalid-request", ERROR_INVALID_DATA);

    OVERLAPPED write{};
    write.hEvent = writeEvent.get();
    DWORD reply = request ^ PROBE_PIPE_REPLY;
    error = CompletePipeIo(pipe.get(), write, bytes, WriteFile(pipe.get(), &reply, sizeof(reply), &bytes, &write), deadline);
    if (error != ERROR_SUCCESS) return PipeFailure(L"write-reply", error);
    if (bytes != sizeof(reply)) return PipeFailure(L"short-reply", ERROR_WRITE_FAULT);

    DWORD wait = WaitForSingleObject(process.get(), RemainingTime(deadline));
    if (wait != WAIT_OBJECT_0) return PipeFailure(L"wait-for-child", wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    DWORD exit = 0;
    if (!GetExitCodeProcess(process.get(), &exit)) return PipeFailure(L"read-child-exit", GetLastError());
    return std::wstring(L"ipc=") + (exit == PROBE_PIPE_EXIT ? L"success\r\n" : L"failed\r\n") +
        L"ipc_request=" + Hex(request) + L"\r\nipc_reply=" + Hex(reply) + L"\r\nipc_child_exit=" + Hex(exit) + L"\r\n";
}

static std::wstring InspectSteamImports(winrt::hstring const& inventory) {
    std::wstring result;
    std::istringstream rows(winrt::to_string(inventory));
    for (std::string row; std::getline(rows, row);) {
        std::istringstream entries(row);
        std::string library;
        if (!(entries >> library)) continue;
        auto name = winrt::to_hstring(library);
        // The package inventory contains DLL basenames. Resolve system modules only.
        if (library.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-.") != std::string::npos ||
                !library.ends_with(".dll")) continue;

        HMODULE module = LoadLibraryExW(name.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            result += std::wstring(name) + L"_system_load_error=" + Hex(GetLastError()) + L"\r\n";
            continue;
        }
        unsigned seen = 0;
        unsigned missing = 0;
        for (std::string symbol; entries >> symbol;) {
            auto nameOrOrdinal = symbol.starts_with('#') ?
                MAKEINTRESOURCEA(std::stoul(symbol.substr(1))) : symbol.c_str();
            ++seen;
            if (!GetProcAddress(module, nameOrOrdinal)) {
                ++missing;
                result += L"missing_import=" + std::wstring(name) + L"!" +
                          std::wstring(winrt::to_hstring(symbol)) + L"\r\n";
            }
        }
        FreeLibrary(module);
        result += std::wstring(name) + L"_imports_checked=" + std::to_wstring(seen) + L"\r\n";
        result += std::wstring(name) + L"_imports_missing=" + std::to_wstring(missing) + L"\r\n";
    }
    return result;
}

static std::wstring SteamCoreResult(std::wstring const& packagePath, winrt::hstring const& inventory) {
    std::wstring result;
    HMODULE control = LoadPackagedLibrary(PROBE_CHILD_MODULE, 0);
    if (control) {
        bool resolved = GetProcAddress(control, PROBE_CHILD_EXPORT) != nullptr;
        DWORD error = resolved ? ERROR_SUCCESS : GetLastError();
        FreeLibrary(control);
        result += std::wstring(L"control_load=") + (resolved ? L"success" : L"export-error") +
                  L"\r\ncontrol_error=" + Hex(error) + L"\r\n";
    } else {
        result += L"control_load=load-error\r\ncontrol_error=" + Hex(GetLastError()) + L"\r\n";
    }

    for (const auto *file : {L"steamclient64.dll", L"tier0_s64.dll", L"vstdlib_s64.dll"}) {
        DWORD attributes = GetFileAttributesW((packagePath + file).c_str());
        DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        result += std::wstring(file) + L"_file_error=" + Hex(error) + L"\r\n";
    }

    HMODULE steam = LoadPackagedLibrary(L"steamclient64.dll", 0);
    if (steam) {
        bool resolved = GetProcAddress(steam, "CreateInterface") != nullptr;
        DWORD error = resolved ? ERROR_SUCCESS : GetLastError();
        FreeLibrary(steam);
        result += std::wstring(L"steam_core=loaded\r\ncreate_interface=") + (resolved ? L"present" : L"missing") +
                  L"\r\nsteam_error=" + Hex(error) + L"\r\n";
    } else {
        result += L"steam_core=load-error\r\nsteam_error=" + Hex(GetLastError()) + L"\r\n";
    }

    for (const auto *name : {L"tier0_s64.dll", L"vstdlib_s64.dll",
                             L"keyboard-ansi.dll", L"keyboard-wide.dll"}) {
        HMODULE dependency = LoadPackagedLibrary(name, 0);
        DWORD error = dependency ? ERROR_SUCCESS : GetLastError();
        if (dependency) FreeLibrary(dependency);
        result += std::wstring(name) + L"_load_error=" + Hex(error) + L"\r\n";
    }
    return result + InspectSteamImports(inventory) + L"status=steam-core-complete\r\n";
}

using SteamFactory = void *(__cdecl *)(const char *, int *);

static winrt::Windows::Foundation::IAsyncOperation<bool> IsPinnedSteamImage() {
    using winrt::Windows::Security::Cryptography::CryptographicBuffer;
    using winrt::Windows::Security::Cryptography::Core::HashAlgorithmProvider;
    using winrt::Windows::Storage::FileIO;
    auto package = winrt::Windows::ApplicationModel::Package::Current();
    auto image = co_await package.InstalledLocation().GetFileAsync(L"steamclient64.dll");
    auto bytes = co_await FileIO::ReadBufferAsync(image);
    auto hash = HashAlgorithmProvider::OpenAlgorithm(L"SHA256").HashData(bytes);
    auto expected = CryptographicBuffer::DecodeFromHexString(
        L"5237b35064b7e1cf26754959643341fdf36c55ffefea54dcf20de6d62b46d822");
    co_return CryptographicBuffer::Compare(hash, expected);
}

// These RVAs describe the audited Valve build 1794536281 after its one-byte import adaptation.
static std::wstring ProbeSignedOutSteamUser(HMODULE image, SteamFactory factory, std::int32_t user, std::int32_t pipe) {
    using GetUser = void *(__cdecl *)(void *, std::int32_t, std::int32_t);
    using Query = bool (__cdecl *)(void *);
    using ListApps = std::int32_t (__cdecl *)(void *, std::uint32_t *, std::int32_t, bool);
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    auto table = [](void *object) {
        return object ? *static_cast<std::uintptr_t const **>(object) : nullptr;
    };
    int code = -1;
    auto engine = factory("CLIENTENGINE_INTERFACE_VERSION005", &code);
    auto engineMethods = table(engine);
    if (code != 0 || reinterpret_cast<std::uintptr_t>(engineMethods) != base + 0x138f580 ||
            engineMethods[8] != base + 0x9aa4b0) {
        return L"steam_session_abi=engine-mismatch\r\n";
    }

    auto clientUser = reinterpret_cast<GetUser>(engineMethods[8])(engine, user, pipe);
    auto userMethods = table(clientUser);
    if (reinterpret_cast<std::uintptr_t>(userMethods) != base + 0x1357d28 ||
            userMethods[1] != base + 0x8772a0 || userMethods[4] != base + 0x75e0e0 ||
            userMethods[6] != base + 0x7513f0 || userMethods[56] != base + 0x8a0450 ||
            userMethods[183] != base + 0x7ba890) {
        return L"steam_session_abi=user-mismatch\r\n";
    }

    bool loggedOn = reinterpret_cast<Query>(userMethods[4])(clientUser);
    bool connected = reinterpret_cast<Query>(userMethods[6])(clientUser);
    std::wstring report = L"steam_session_abi=verified\r\n";
    report += std::wstring(L"steam_client_connected=") + (connected ? L"yes\r\n" : L"no\r\n");
    if (!loggedOn) {
        std::array<std::uint32_t, 64> apps{};
        auto count = reinterpret_cast<ListApps>(userMethods[183])(
            clientUser, apps.data(), static_cast<std::int32_t>(apps.size()), true);
        report += L"steam_signed_out_subscriptions=" + std::to_wstring(count) + L"\r\n";
    }
    return report;
}

static winrt::Windows::Foundation::IAsyncAction SteamRuntime(
        winrt::Windows::Storage::StorageFile file, winrt::hstring prefix) {
    using winrt::Windows::Storage::FileIO;
    using Factory = void *(__cdecl *)(const char *, int *);
    using CreatePipe = std::int32_t (__cdecl *)();
    using ReleasePipe = bool (__cdecl *)(std::int32_t);
    using CreateGlobalUser = std::int32_t (__cdecl *)(std::int32_t *);
    using ReleaseUser = void (__cdecl *)(std::int32_t, std::int32_t);
    using LoggedOn = bool (__cdecl *)(std::int32_t, std::int32_t);
    constexpr auto complete = L"status=steam-core-complete\r\n";

    std::wstring report(prefix);
    co_await FileIO::WriteTextAsync(file, winrt::hstring(report + L"status=steam-factory-starting\r\n"));
    std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> module(
        LoadPackagedLibrary(L"steamclient64.dll", 0), FreeLibrary);
    if (!module) {
        report += L"steam_factory=reload-error\r\nsteam_factory_error=" + Hex(GetLastError()) + L"\r\n";
        co_await FileIO::WriteTextAsync(file, winrt::hstring(report + complete));
        co_return;
    }

    auto factory = reinterpret_cast<Factory>(GetProcAddress(module.get(), "CreateInterface"));
    if (!factory) {
        report += L"steam_factory=export-error\r\nsteam_factory_error=" + Hex(GetLastError()) + L"\r\n";
        co_await FileIO::WriteTextAsync(file, winrt::hstring(report + complete));
        co_return;
    }
    int code = -1;
    bool ready = factory("SteamClient023", &code) != nullptr && code == 0;
    report += std::wstring(L"steam_interface=SteamClient023\r\nsteam_factory=") +
        (ready ? L"success" : L"failed") + L"\r\nsteam_factory_code=" + std::to_wstring(code) + L"\r\n";
    if (!ready) {
        co_await FileIO::WriteTextAsync(file, winrt::hstring(report + complete));
        co_return;
    }

    auto createPipe = reinterpret_cast<CreatePipe>(GetProcAddress(module.get(), "Steam_CreateSteamPipe"));
    auto releasePipe = reinterpret_cast<ReleasePipe>(GetProcAddress(module.get(), "Steam_BReleaseSteamPipe"));
    if (!createPipe || !releasePipe) {
        report += L"steam_pipe=export-error\r\n";
        co_await FileIO::WriteTextAsync(file, winrt::hstring(report + complete));
        co_return;
    }
    co_await FileIO::WriteTextAsync(file, winrt::hstring(report + L"status=steam-pipe-starting\r\n"));

    std::int32_t pipe = createPipe();
    if (pipe > 0) {
        bool released = releasePipe(pipe);
        report += std::wstring(L"steam_pipe=") + (released ? L"open-and-release\r\n" : L"release-failed\r\n");
    } else {
        report += L"steam_pipe=create-failed\r\n";
    }
    report += L"steam_pipe_handle=" + std::to_wstring(pipe) + L"\r\n";
    if (pipe != 0) {
        co_await FileIO::WriteTextAsync(file, winrt::hstring(report + complete));
        co_return;
    }

    auto createUser = reinterpret_cast<CreateGlobalUser>(GetProcAddress(module.get(), "Steam_CreateGlobalUser"));
    auto releaseUser = reinterpret_cast<ReleaseUser>(GetProcAddress(module.get(), "Steam_ReleaseUser"));
    auto loggedOn = reinterpret_cast<LoggedOn>(GetProcAddress(module.get(), "Steam_BLoggedOn"));
    if (!createUser || !releaseUser || !loggedOn) {
        report += L"steam_bootstrap=export-error\r\n";
        co_await FileIO::WriteTextAsync(file, winrt::hstring(report + complete));
        co_return;
    }
    co_await FileIO::WriteTextAsync(file, winrt::hstring(report + L"status=steam-bootstrap-starting\r\n"));

    std::int32_t bootstrapPipe = 0;
    std::int32_t user = createUser(&bootstrapPipe);
    report += L"steam_bootstrap_user_handle=" + std::to_wstring(user) +
              L"\r\nsteam_bootstrap_pipe_handle=" + std::to_wstring(bootstrapPipe) + L"\r\n";
    if (user > 0 && bootstrapPipe > 0) {
        report += std::wstring(L"steam_bootstrap=success\r\nsteam_logged_on=") +
                  (loggedOn(user, bootstrapPipe) ? L"yes\r\n" : L"no\r\n");
        bool pinned = false;
        try {
            pinned = co_await IsPinnedSteamImage();
        } catch (winrt::hresult_error const& error) {
            report += L"steam_session_hash_error=" + Hex(static_cast<DWORD>(error.code())) + L"\r\n";
        }
        if (pinned) {
            co_await FileIO::WriteTextAsync(file, winrt::hstring(report + L"status=steam-session-abi-starting\r\n"));
            report += ProbeSignedOutSteamUser(module.get(), factory, user, bootstrapPipe);
        } else {
            report += L"steam_session_abi=image-mismatch\r\n";
        }
        releaseUser(bootstrapPipe, user);
    } else {
        report += L"steam_bootstrap=failed\r\n";
    }
    if (bootstrapPipe > 0) {
        report += std::wstring(L"steam_bootstrap_pipe_released=") +
                  (releasePipe(bootstrapPipe) ? L"yes\r\n" : L"no\r\n");
    }
    co_await FileIO::WriteTextAsync(file, winrt::hstring(report + complete));
}

struct ProbeFactory : winrt::implements<ProbeFactory, winrt::Windows::Foundation::IActivationFactory,
                                        winrt::XboxProbe::Native::IProbeRunnerStatics> {
    winrt::Windows::Foundation::IInspectable ActivateInstance() {
        throw winrt::hresult_not_implemented();
    }

    winrt::hstring Run() {
        auto folder = winrt::Windows::ApplicationModel::Package::Current().InstalledLocation();
        std::wstring path = std::wstring(folder.Path()) + L"\\";
        return winrt::hstring(ChildResult(path + L"child.exe") + PipeResult(path + L"pipe-child.exe"));
    }

    winrt::hstring RunSteamCore(winrt::hstring const& inventory) {
        auto package = winrt::Windows::ApplicationModel::Package::Current();
        return winrt::hstring(SteamCoreResult(std::wstring(package.InstalledLocation().Path()) + L"\\", inventory));
    }

    winrt::Windows::Foundation::IAsyncAction RunSteamRuntime(
            winrt::Windows::Storage::StorageFile const& file, winrt::hstring const& prefix) {
        return SteamRuntime(file, prefix);
    }
};

extern "C" HRESULT WINAPI ProbeGetActivationFactory(HSTRING classId, void **factory) noexcept try {
    *factory = nullptr;
    UINT32 length;
    const wchar_t *name = WindowsGetStringRawBuffer(classId, &length);
    if (std::wstring_view(name, length) != L"XboxProbe.Native.ProbeRunner") return CLASS_E_CLASSNOTAVAILABLE;
    *factory = winrt::detach_abi(winrt::make<ProbeFactory>());
    return S_OK;
} catch (...) {
    return winrt::to_hresult();
}

extern "C" HRESULT WINAPI ProbeCanUnloadNow() noexcept {
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}
