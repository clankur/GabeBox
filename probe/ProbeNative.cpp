#include <windows.h>
#include <winstring.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.h>
#include <winrt/XboxProbe.Native.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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
using SteamQuery = bool (__cdecl *)(void *);
using SteamListApps = std::int32_t (__cdecl *)(void *, std::uint32_t *, std::int32_t, bool);
using SteamReadAppData = std::int32_t (__cdecl *)(void *, std::uint32_t, const char *, char *, std::int32_t);
using SteamRequestAppInfo = bool (__cdecl *)(void *, const std::uint32_t *, std::int32_t);
using SteamGetId = std::uint64_t *(__cdecl *)(void *, std::uint64_t *);
using SteamAppValue = std::uint32_t (__cdecl *)(void *, std::uint32_t);
using SteamAppPath = std::uint32_t (__cdecl *)(void *, std::uint32_t, char *, std::uint32_t);
using SteamInstall = std::int32_t (__cdecl *)(void *, std::uint32_t, std::int32_t, bool);

struct SteamCallback {
    std::int32_t user;
    std::int32_t message;
    const void *data;
    std::int32_t length;
};

using SteamGetCallback = bool (__cdecl *)(std::int32_t, SteamCallback *);
using SteamFreeCallback = void (__cdecl *)(std::int32_t);

static std::uintptr_t const *SteamMethods(void *object) {
    return object ? *static_cast<std::uintptr_t const **>(object) : nullptr;
}

static void *FindPinnedSteamInterface(HMODULE image, SteamFactory factory, std::int32_t user,
        std::int32_t pipe, unsigned getterSlot, std::uintptr_t getterRva) {
    using GetInterface = void *(__cdecl *)(void *, std::int32_t, std::int32_t);
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    int code = -1;
    auto engine = factory("CLIENTENGINE_INTERFACE_VERSION005", &code);
    auto engineMethods = SteamMethods(engine);
    if (code != 0 || reinterpret_cast<std::uintptr_t>(engineMethods) != base + 0x138f580 ||
            engineMethods[getterSlot] != base + getterRva) {
        return nullptr;
    }
    return reinterpret_cast<GetInterface>(engineMethods[getterSlot])(engine, user, pipe);
}

static std::uintptr_t const *FindPinnedSteamUser(HMODULE image, SteamFactory factory,
        std::int32_t user, std::int32_t pipe, void *&clientUser, std::wstring &report) {
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    clientUser = FindPinnedSteamInterface(image, factory, user, pipe, 8, 0x9aa4b0);
    auto userMethods = SteamMethods(clientUser);
    if (reinterpret_cast<std::uintptr_t>(userMethods) != base + 0x1357d28 ||
            userMethods[1] != base + 0x8772a0 || userMethods[4] != base + 0x75e0e0 ||
            userMethods[3] != base + 0x876fe0 || userMethods[6] != base + 0x7513f0 ||
            userMethods[10] != base + 0x7b8ea0 || userMethods[56] != base + 0x8a0450 ||
            userMethods[183] != base + 0x7ba890) {
        report += L"steam_session_abi=user-mismatch\r\n";
        return nullptr;
    }

    report += L"steam_session_abi=verified\r\n";
    return userMethods;
}

static std::wstring ProbeSignedOutSteamUser(HMODULE image, SteamFactory factory, std::int32_t user, std::int32_t pipe) {
    std::wstring report;
    void *clientUser = nullptr;
    auto userMethods = FindPinnedSteamUser(image, factory, user, pipe, clientUser, report);
    if (!userMethods) return report;
    bool loggedOn = reinterpret_cast<SteamQuery>(userMethods[4])(clientUser);
    bool connected = reinterpret_cast<SteamQuery>(userMethods[6])(clientUser);
    report += std::wstring(L"steam_client_connected=") + (connected ? L"yes\r\n" : L"no\r\n");
    if (!loggedOn) {
        std::array<std::uint32_t, 64> apps{};
        auto count = reinterpret_cast<SteamListApps>(userMethods[183])(
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

struct SteamSignInHandles {
    using ReleasePipe = bool (__cdecl *)(std::int32_t);
    using ReleaseUser = void (__cdecl *)(std::int32_t, std::int32_t);
    HMODULE module = nullptr;
    ReleasePipe releasePipe = nullptr;
    ReleaseUser releaseUser = nullptr;
    std::int32_t pipe = 0;
    std::int32_t user = 0;
    void *clientUser = nullptr;
    void (__cdecl *logOff)(void *) = nullptr;
    SteamFactory factory = nullptr;
    SteamGetCallback getCallback = nullptr;
    SteamFreeCallback freeCallback = nullptr;
    std::uintptr_t const *userMethods = nullptr;
    std::uint64_t accountId = 0;
    std::vector<std::uint32_t> games;

    void *appManager = nullptr;
    std::uint32_t downloadApp = 0;
    std::wstring libraryPath;
    std::uint64_t lastTransition = 0;
    std::uint32_t lastInstallState = 0;
    std::uint64_t launchCall = 0;

    void PumpCallbacks() const {
        for (unsigned i = 0; i < 32; ++i) {
            SteamCallback event{};
            if (!getCallback(pipe, &event)) break;
            freeCallback(pipe);
        }
    }

    bool OwnsSession() const {
        if (!reinterpret_cast<SteamQuery>(userMethods[4])(clientUser) ||
                !reinterpret_cast<SteamQuery>(userMethods[6])(clientUser)) return false;
        std::uint64_t currentId = 0;
        reinterpret_cast<SteamGetId>(userMethods[10])(clientUser, &currentId);
        return currentId == accountId;
    }

    ~SteamSignInHandles() {
        if (logOff) logOff(clientUser);
        if (pipe > 0) {
            if (user > 0) releaseUser(pipe, user);
            releasePipe(pipe);
        }
        if (module) FreeLibrary(module);
    }
};

// Only explicit sign-out releases a live session. Process exit reclaims it without calling
// Valve code under the DLL loader lock.
static std::mutex steamSessionMutex;
static SteamSignInHandles *liveSteamSession = nullptr;
static std::atomic_flag steamSignInActive{};
static std::atomic_flag steamLaunchActive{};

static void SteamSignOut() {
    std::lock_guard lock(steamSessionMutex);
    if (!liveSteamSession) return;
    delete liveSteamSession;
    liveSteamSession = nullptr;
    --winrt::get_module_lock();
}

static winrt::hstring ReadSteamAppField(SteamReadAppData read, void *apps, std::uint32_t id, const char *key) {
    std::array<char, 2048> buffer{};
    auto size = read(apps, id, key, buffer.data(), static_cast<std::int32_t>(buffer.size()));
    if (size <= 0 || size >= static_cast<std::int32_t>(buffer.size())) return {};
    buffer[size] = '\0';
    try {
        return winrt::to_hstring(std::string_view(buffer.data()));
    } catch (winrt::hresult_error const&) {
        return {};
    }
}

static winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> SteamSignIn(
        winrt::Windows::Storage::StorageFile file, winrt::hstring prefix, winrt::hstring account,
        winrt::hstring approval, winrt::hstring accountId) {
    using winrt::Windows::Storage::FileIO;
    using CreateUser = std::int32_t (__cdecl *)(std::int32_t *);
    using SetToken = void (__cdecl *)(void *, const char *, const char *);
    using LogOn = std::int32_t (__cdecl *)(void *, std::uint64_t);
    constexpr auto complete = L"status=steam-core-complete\r\n";
    std::wstring report(prefix);
    auto save = [&](const wchar_t *suffix) {
        return FileIO::WriteTextAsync(file, winrt::hstring(report + suffix));
    };
    if (steamSignInActive.test_and_set()) co_return winrt::hstring{};
    struct ResetSignIn {
        ~ResetSignIn() { steamSignInActive.clear(); }
    } resetSignIn;
    {
        std::lock_guard lock(steamSessionMutex);
        if (liveSteamSession) co_return winrt::hstring{};
    }

    auto idText = winrt::to_string(accountId);
    std::uint64_t requestedId = 0;
    auto parsed = std::from_chars(idText.data(), idText.data() + idText.size(), requestedId);
    if (parsed.ec != std::errc{} || parsed.ptr != idText.data() + idText.size() || !requestedId ||
            account.empty() || account.size() > 64 || approval.empty() || approval.size() > 8192 ||
            !(co_await IsPinnedSteamImage())) {
        report += L"steam_signin=input-or-image-error\r\n";
        co_await save(complete);
        co_return winrt::hstring{};
    }

    co_await save(L"status=steam-native-signin-starting\r\n");
    auto session = std::make_unique<SteamSignInHandles>();
    auto &handles = *session;
    auto module = handles.module = LoadPackagedLibrary(L"steamclient64.dll", 0);
    if (!module) {
        report += L"steam_signin=load-error\r\nsteam_signin_error=" + Hex(GetLastError()) + L"\r\n";
        co_await save(complete);
        co_return winrt::hstring{};
    }

    auto factory = reinterpret_cast<SteamFactory>(GetProcAddress(module, "CreateInterface"));
    auto create = reinterpret_cast<CreateUser>(GetProcAddress(module, "Steam_CreateGlobalUser"));
    handles.getCallback = reinterpret_cast<SteamGetCallback>(GetProcAddress(module, "Steam_BGetCallback"));
    handles.freeCallback = reinterpret_cast<SteamFreeCallback>(GetProcAddress(module, "Steam_FreeLastCallback"));
    handles.factory = factory;
    handles.releasePipe = reinterpret_cast<SteamSignInHandles::ReleasePipe>(GetProcAddress(module, "Steam_BReleaseSteamPipe"));
    handles.releaseUser = reinterpret_cast<SteamSignInHandles::ReleaseUser>(GetProcAddress(module, "Steam_ReleaseUser"));
    if (!factory || !create || !handles.releasePipe || !handles.releaseUser || !handles.getCallback ||
            !handles.freeCallback) {
        report += L"steam_signin=export-error\r\n";
        co_await save(complete);
        co_return winrt::hstring{};
    }

    handles.user = create(&handles.pipe);
    if (handles.user <= 0 || handles.pipe <= 0) {
        report += L"steam_signin=bootstrap-error\r\n";
        co_await save(complete);
        co_return winrt::hstring{};
    }
    void *clientUser = nullptr;
    auto methods = FindPinnedSteamUser(module, factory, handles.user, handles.pipe, clientUser, report);
    if (!methods) {
        report += L"steam_signin=interface-error\r\n";
        co_await save(complete);
        co_return winrt::hstring{};
    }
    auto loggedOn = reinterpret_cast<SteamQuery>(methods[4]);
    auto connected = reinterpret_cast<SteamQuery>(methods[6]);
    if (loggedOn(clientUser)) {
        report += L"steam_signin=existing-session\r\n";
        co_await save(complete);
        co_return winrt::hstring{};
    }

    auto token = winrt::to_string(approval);
    approval.clear();
    auto name = winrt::to_string(account);
    reinterpret_cast<SetToken>(methods[56])(clientUser, token.c_str(), name.c_str());
    SecureZeroMemory(token.data(), token.size());
    const auto started = reinterpret_cast<LogOn>(methods[1])(clientUser, requestedId);
    report += L"steam_logon_start=" + std::to_wstring(started) + L"\r\n";
    if (started != 1) {
        report += L"steam_signin=logon-rejected\r\n";
        co_await save(complete);
        co_return winrt::hstring{};
    }
    handles.clientUser = clientUser;
    handles.userMethods = methods;
    handles.accountId = requestedId;
    handles.logOff = reinterpret_cast<decltype(handles.logOff)>(methods[3]);
    co_await save(L"status=steam-awaiting-authenticated-library\r\n");

    winrt::apartment_context caller;
    std::vector<std::uint32_t> apps(65536);
    ULONGLONG deadline = GetTickCount64() + 60000;
    bool identityMatches = false;
    bool wrongAccount = false;
    std::int32_t count = 0;
    while (GetTickCount64() < deadline) {
        handles.PumpCallbacks();
        if (loggedOn(clientUser) && connected(clientUser)) {
            std::uint64_t actualId = 0;
            reinterpret_cast<SteamGetId>(methods[10])(clientUser, &actualId);
            if (actualId && actualId != requestedId) {
                wrongAccount = true;
                break;
            }
            if (actualId == requestedId) {
                identityMatches = true;
                count = reinterpret_cast<SteamListApps>(methods[183])(
                    clientUser, apps.data(), static_cast<std::int32_t>(apps.size()), true);
                if (count > 0 && count < static_cast<std::int32_t>(apps.size())) break;
            }
        }
        co_await winrt::resume_after(std::chrono::milliseconds(250));
        co_await caller;
    }

    if (wrongAccount) {
        report += L"steam_signin=account-mismatch\r\n";
    } else if (identityMatches) {
        report += L"steam_signin=authenticated\r\nsteam_library_count=" + std::to_wstring(count) + L"\r\n";
        const bool verified = count > 0 && count < static_cast<std::int32_t>(apps.size());
        report += std::wstring(L"steam_library=") + (verified ? L"verified\r\n" : L"unconfirmed\r\n");
    } else {
        report += L"steam_signin=timeout\r\n";
    }
    if (wrongAccount || !identityMatches || count <= 0 || count >= static_cast<std::int32_t>(apps.size())) {
        co_await save(complete);
        co_return winrt::hstring{};
    }

    auto clientApps = FindPinnedSteamInterface(module, factory, handles.user, handles.pipe, 17, 0x9a8290);
    const auto appsMethods = SteamMethods(clientApps);
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    if (reinterpret_cast<std::uintptr_t>(appsMethods) != base + 0x135f168 ||
            appsMethods[0] != base + 0x77fa60 || appsMethods[7] != base + 0x884200 ||
            appsMethods[10] != base + 0x781ef0) {
        report += clientApps ? L"steam_library_metadata=abi-mismatch\r\n" : L"steam_library_metadata=no-interface\r\n";
        co_await save(complete);
        co_return winrt::hstring{};
    }

    co_await save(L"status=steam-reading-library-titles\r\n");
    auto read = reinterpret_cast<SteamReadAppData>(appsMethods[0]);
    auto request = reinterpret_cast<SteamRequestAppInfo>(appsMethods[7]);
    apps.resize(count);
    std::vector<winrt::hstring> names(count);
    std::vector<std::uint32_t> missing;
    deadline = GetTickCount64() + 20000;
    for (bool requested = false;; requested = true) {
        handles.PumpCallbacks();
        missing.clear();
        for (size_t i = 0; i < apps.size(); ++i) {
            if (names[i].empty()) names[i] = ReadSteamAppField(read, clientApps, apps[i], "name");
            if (names[i].empty()) missing.push_back(apps[i]);
        }
        if (missing.empty() || GetTickCount64() >= deadline) break;
        if (!requested) {
            report += request(clientApps, missing.data(), static_cast<std::int32_t>(missing.size()))
                ? L"steam_library_metadata_request=accepted\r\n" : L"steam_library_metadata_request=not-queued\r\n";
        }
        co_await winrt::resume_after(std::chrono::milliseconds(1500));
        co_await caller;
    }

    using namespace winrt::Windows::Data::Json;
    JsonArray library;
    auto appType = reinterpret_cast<SteamAppValue>(appsMethods[10]);
    constexpr std::uint32_t gameType = 1;
    for (size_t i = 0; i < apps.size(); ++i) {
        if (names[i].empty()) continue;
        JsonObject entry;
        entry.SetNamedValue(L"id", JsonValue::CreateNumberValue(apps[i]));
        entry.SetNamedValue(L"name", JsonValue::CreateStringValue(names[i]));
        entry.SetNamedValue(L"type", JsonValue::CreateStringValue(ReadSteamAppField(read, clientApps, apps[i], "type")));
        const bool game = appType(clientApps, apps[i]) == gameType;
        entry.SetNamedValue(L"game", JsonValue::CreateBooleanValue(game));
        if (game) handles.games.push_back(apps[i]);
        library.Append(entry);
    }
    report += L"steam_library_named=" + std::to_wstring(library.Size()) + L"\r\n";
    if (!library.Size()) {
        report += L"steam_library_metadata=missing\r\n";
        co_await save(complete);
        co_return winrt::hstring{};
    }

    auto payload = library.Stringify();
    co_await save(L"status=steam-library-rendering\r\n");
    std::lock_guard lock(steamSessionMutex);
    ++winrt::get_module_lock();
    liveSteamSession = session.release();
    co_return payload;
}

static std::wstring SteamPath(SteamAppPath readPath, void *manager, std::uint32_t id) {
    std::array<char, 4096> buffer{};
    auto size = readPath(manager, id, buffer.data(), static_cast<std::uint32_t>(buffer.size()));
    if (!size || size >= buffer.size()) return {};
    buffer[size] = '\0';
    std::wstring path(winrt::to_hstring(std::string_view(buffer.data())));
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (!path.empty() && path.back() == L'\\') path.pop_back();
    return path;
}

static std::uintptr_t const *FindSteamAppManager(SteamSignInHandles &session) {
    const auto base = reinterpret_cast<std::uintptr_t>(session.module);
    auto manager = FindPinnedSteamInterface(session.module, session.factory, session.user, session.pipe,
        43, 0x9a81f0);
    auto methods = SteamMethods(manager);
    if (reinterpret_cast<std::uintptr_t>(methods) != base + 0x135f708 ||
            methods[0] != base + 0x86b7b0 || methods[4] != base + 0x780b00 ||
            methods[5] != base + 0x780950 || methods[42] != base + 0x793400 ||
            methods[75] != base + 0x7ab120 || methods[76] != base + 0x7a34b0 ||
            methods[77] != base + 0x74b280) return nullptr;
    session.appManager = manager;
    return methods;
}

struct SteamLibraryRegistration {
    std::int32_t index;
    std::int32_t result;
};

static SteamLibraryRegistration RegisterSteamLibrary(void *manager, std::uintptr_t const *methods,
        winrt::hstring const& path) {
    using LibraryCount = std::uint32_t (__cdecl *)(void *);
    using AddFolder = std::int32_t (__cdecl *)(void *, const char *);
    auto libraryCount = reinterpret_cast<LibraryCount>(methods[75]);
    auto libraryPath = reinterpret_cast<SteamAppPath>(methods[76]);
    auto findFolder = [&]() -> std::int32_t {
        auto count = libraryCount(manager);
        for (std::uint32_t i = 0; i < count && i < 1024; ++i) {
            auto directory = SteamPath(libraryPath, manager, i);
            if (CompareStringOrdinal(directory.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL) return i;
        }
        return -1;
    };
    auto index = findFolder();
    if (index >= 0) return {index, 0};
    auto addFolder = reinterpret_cast<AddFolder>(methods[77]);
    auto result = addFolder(manager, winrt::to_string(path).c_str());
    return {findFolder(), result};
}

static bool IsLocalSteamInstall(std::wstring const& directory, std::wstring const& root) {
    auto prefix = root + L"\\";
    return directory.size() > prefix.size() && CompareStringOrdinal(
        directory.c_str(), static_cast<int>(prefix.size()), prefix.c_str(),
        static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL &&
        directory.find(L"\\..\\") == std::wstring::npos && !directory.ends_with(L"\\..");
}

static winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> StartSteamDownload(std::uint32_t app) {
    using namespace winrt::Windows::Storage;
    auto root = ApplicationData::Current().LocalFolder();
    auto folder = co_await root.CreateFolderAsync(L"SteamLibrary", CreationCollisionOption::OpenIfExists);
    auto path = folder.Path();

    std::lock_guard lock(steamSessionMutex);
    auto session = liveSteamSession;
    if (!session || !session->OwnsSession()) co_return winrt::hstring(L"session-lost");
    if (session->downloadApp || steamLaunchActive.test()) co_return winrt::hstring(L"busy");
    const auto base = reinterpret_cast<std::uintptr_t>(session->module);
    using IsSubscribed = bool (__cdecl *)(void *, std::uint32_t);
    if (session->userMethods[182] != base + 0x75d640) co_return winrt::hstring(L"interface-error");
    if (std::find(session->games.begin(), session->games.end(), app) == session->games.end() ||
            !reinterpret_cast<IsSubscribed>(session->userMethods[182])(session->clientUser, app)) {
        co_return winrt::hstring(L"no-license");
    }

    auto methods = FindSteamAppManager(*session);
    if (!methods) co_return winrt::hstring(L"interface-error");
    auto manager = session->appManager;
    auto registration = RegisterSteamLibrary(manager, methods, path);
    if (registration.index < 0) {
        co_return winrt::hstring(L"library-folder-error:" + std::to_wstring(registration.result));
    }

    constexpr std::uint32_t updatePaused = 0x200;
    const bool wasPaused = (reinterpret_cast<SteamAppValue>(methods[4])(manager, app) & updatePaused) != 0;
    const auto queuePlacement = methods[56];
    if (wasPaused && queuePlacement != base + 0x766730) {
        co_return winrt::hstring(L"interface-error");
    }
    auto install = reinterpret_cast<SteamInstall>(methods[0]);
    auto result = install(manager, app, registration.index, false);
    if (result != 0) co_return winrt::hstring(L"install-rejected:" + std::to_wstring(result));
    session->downloadApp = app;
    session->libraryPath = path;
    session->lastTransition = GetTickCount64();
    session->lastInstallState = 0;
    if (wasPaused) {
        // This is the explicit “Download now” action for the selected, already queued title.
        using SetQueuePlacement = bool (__cdecl *)(void *, std::uint32_t, std::int32_t);
        constexpr std::int32_t firstInQueue = 1;
        if (reinterpret_cast<SetQueuePlacement>(queuePlacement)(manager, app, firstInQueue)) {
            co_return winrt::hstring(L"resuming");
        }
    }
    co_return winrt::hstring(L"queued");
}

static winrt::hstring ReadSteamDownload() {
    std::lock_guard lock(steamSessionMutex);
    auto session = liveSteamSession;
    if (!session || !session->downloadApp) return L"idle";
    if (!session->OwnsSession()) {
        session->downloadApp = 0;
        return L"session-lost";
    }
    session->PumpCallbacks();
    auto manager = session->appManager;
    auto methods = SteamMethods(manager);
    constexpr std::uint32_t updateRequired = 0x2;
    constexpr std::uint32_t fullyInstalled = 0x4;
    constexpr std::uint32_t updatePaused = 0x200;
    constexpr std::uint32_t missingOrCorrupt = 0xa0;
    constexpr std::uint32_t stagingOrCommitting = 0x600000;
    constexpr std::uint32_t downloading = 0x100000;
    using ActiveApp = std::uint32_t (__cdecl *)(void *);
    auto state = reinterpret_cast<SteamAppValue>(methods[4])(manager, session->downloadApp);
    auto activeApp = reinterpret_cast<ActiveApp>(methods[42])(manager);
    auto now = GetTickCount64();
    if (state != session->lastInstallState || activeApp == session->downloadApp) {
        session->lastInstallState = state;
        session->lastTransition = now;
    }

    std::wstring checkpoint;
    if ((state & (fullyInstalled | updateRequired | missingOrCorrupt | downloading | stagingOrCommitting)) ==
            fullyInstalled) {
        auto directory = SteamPath(reinterpret_cast<SteamAppPath>(methods[5]), manager, session->downloadApp);
        checkpoint = IsLocalSteamInstall(directory, session->libraryPath) ? L"installed" : L"install-path-error";
        session->downloadApp = 0;
    } else if (now - session->lastTransition >= 90000) {
        checkpoint = L"stalled";
        session->downloadApp = 0;
    } else if (state & stagingOrCommitting) {
        checkpoint = L"staging";
    } else if (activeApp == session->downloadApp || (state & downloading)) {
        checkpoint = L"downloading";
    } else if (state & updatePaused) {
        checkpoint = L"paused";
    } else {
        checkpoint = L"queued";
    }
    return winrt::hstring(checkpoint + L":" + std::to_wstring(state));
}

static winrt::Windows::Foundation::IAsyncOperation<bool> HasCompletedSteamManifest(
        winrt::Windows::Storage::StorageFolder library, std::uint32_t app) {
    using namespace winrt::Windows::Storage;
    auto appsItem = co_await library.TryGetItemAsync(L"steamapps");
    auto appsFolder = appsItem.try_as<StorageFolder>();
    if (!appsFolder) co_return false;
    auto item = co_await appsFolder.TryGetItemAsync(winrt::hstring(L"appmanifest_" + std::to_wstring(app) + L".acf"));
    auto manifest = item.try_as<StorageFile>();
    if (!manifest) co_return false;
    auto text = co_await FileIO::ReadTextAsync(manifest);
    std::wstring_view view(text);
    constexpr std::wstring_view key = L"\"StateFlags\"";
    auto field = view.find(key);
    if (field == view.npos) co_return false;
    auto value = view.find_first_not_of(L" \t", field + key.size());
    co_return value != view.npos && view.substr(value, 3) == L"\"4\"";
}

static winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> LaunchLocalSteamGame(std::uint32_t app) {
    using namespace winrt::Windows::Storage;
    using IsSubscribed = bool (__cdecl *)(void *, std::uint32_t);
    using GameQuery = bool (__cdecl *)(void *, const std::uint64_t *);
    using Launch = std::uint64_t (__cdecl *)(void *, const std::uint64_t *, std::uint32_t, std::uint32_t, const char *);
    using CallResult = bool (__cdecl *)(std::int32_t, std::uint64_t, void *, std::int32_t, std::int32_t, bool *);
    using RefreshLibrary = void (__cdecl *)(void *);
    if (steamLaunchActive.test_and_set()) co_return winrt::hstring(L"steam_launch=busy\r\n");
    struct ResetLaunch {
        ~ResetLaunch() { steamLaunchActive.clear(); }
    } resetLaunch;
    auto root = ApplicationData::Current().LocalFolder();
    auto item = co_await root.TryGetItemAsync(L"SteamLibrary");
    auto folder = item.try_as<StorageFolder>();
    if (!folder) co_return winrt::hstring(L"steam_launch=no-local-library\r\n");
    auto path = folder.Path();

    const std::uint64_t gameId = app;  // Regular Steam AppID in CGameID's 24-bit app field.
    if (!app || app > 0x00ffffff) co_return winrt::hstring(L"steam_launch=invalid-game\r\n");
    const bool installedManifest = co_await HasCompletedSteamManifest(folder, app);
    std::uint64_t call = 0;
    CallResult result = nullptr;
    std::int32_t libraryIndex = -1;
    {
        std::lock_guard lock(steamSessionMutex);
        auto session = liveSteamSession;
        if (!session || !session->OwnsSession()) co_return winrt::hstring(L"steam_launch=session-lost\r\n");
        if (session->downloadApp) co_return winrt::hstring(L"steam_launch=busy\r\n");
        auto managerMethods = FindSteamAppManager(*session);
        const auto base = reinterpret_cast<std::uintptr_t>(session->module);
        if (!managerMethods || managerMethods[83] != base + 0x880030 ||
                session->userMethods[182] != base + 0x75d640) {
            co_return winrt::hstring(L"steam_launch=interface-error\r\n");
        }
        if (std::find(session->games.begin(), session->games.end(), app) == session->games.end() ||
                !reinterpret_cast<IsSubscribed>(session->userMethods[182])(session->clientUser, app)) {
            co_return winrt::hstring(L"steam_launch=no-license\r\n");
        }
        auto library = RegisterSteamLibrary(session->appManager, managerMethods, path);
        if (library.index < 0) co_return winrt::hstring(L"steam_launch=library-folder-error\r\n");
        libraryIndex = library.index;
        reinterpret_cast<RefreshLibrary>(managerMethods[83])(session->appManager);
    }

    std::wstring report = L"steam_launch_library_refreshed=yes\r\n";
    winrt::apartment_context caller;
    auto installDeadline = GetTickCount64() + 6000;
    bool attemptedRegistration = false;
    constexpr std::uint32_t installMask = 0x7000a6;
    constexpr std::uint32_t fullyInstalled = 4;
    do {
        {
            std::lock_guard lock(steamSessionMutex);
            auto session = liveSteamSession;
            if (!session || !session->OwnsSession()) {
                co_return winrt::hstring(report + L"steam_launch=session-lost\r\n");
            }
            session->PumpCallbacks();
            auto manager = session->appManager;
            auto methods = SteamMethods(manager);
            auto state = reinterpret_cast<SteamAppValue>(methods[4])(manager, app);
            if ((state & installMask) == fullyInstalled) break;
            if (GetTickCount64() >= installDeadline) {
                if (attemptedRegistration || !installedManifest || state != 1) {
                    co_return winrt::hstring(report + L"steam_launch=not-installed\r\nsteam_launch_install_state=" +
                        std::to_wstring(state) + L"\r\n");
                }
                auto install = reinterpret_cast<SteamInstall>(methods[0]);
                auto code = install(manager, app, libraryIndex, false);
                report += L"steam_launch_reopen_result=" + std::to_wstring(code) + L"\r\n";
                if (code != 0) co_return winrt::hstring(report + L"steam_launch=not-installed\r\n");
                attemptedRegistration = true;
                installDeadline = GetTickCount64() + 30000;
            }
        }
        co_await winrt::resume_after(std::chrono::milliseconds(500));
        co_await caller;
    } while (true);

    {
        std::lock_guard lock(steamSessionMutex);
        auto session = liveSteamSession;
        if (!session || !session->OwnsSession()) co_return winrt::hstring(L"steam_launch=session-lost\r\n");
        if (session->downloadApp || session->launchCall) co_return winrt::hstring(L"steam_launch=busy\r\n");
        const auto base = reinterpret_cast<std::uintptr_t>(session->module);
        auto methods = FindSteamAppManager(*session);
        result = reinterpret_cast<CallResult>(GetProcAddress(session->module, "Steam_GetAPICallResult"));
        if (!methods || methods[2] != base + 0x874f20 || session->userMethods[182] != base + 0x75d640 ||
                session->userMethods[67] != base + 0x75a320 || session->userMethods[68] != base + 0x75a4d0 ||
                reinterpret_cast<std::uintptr_t>(result) != base + 0x9abb80) {
            co_return winrt::hstring(L"steam_launch=interface-error\r\n");
        }
        if (std::find(session->games.begin(), session->games.end(), app) == session->games.end() ||
                !reinterpret_cast<IsSubscribed>(session->userMethods[182])(session->clientUser, app)) {
            co_return winrt::hstring(L"steam_launch=no-license\r\n");
        }
        auto manager = session->appManager;
        auto state = reinterpret_cast<SteamAppValue>(methods[4])(manager, app);
        if ((state & installMask) != fullyInstalled) {
            co_return winrt::hstring(L"steam_launch=not-installed\r\nsteam_launch_install_state=" +
                std::to_wstring(state) + L"\r\n");
        }
        auto directory = SteamPath(reinterpret_cast<SteamAppPath>(methods[5]), manager, app);
        if (!IsLocalSteamInstall(directory, std::wstring(path))) {
            co_return winrt::hstring(L"steam_launch=install-path-error\r\n");
        }
        if (reinterpret_cast<GameQuery>(session->userMethods[67])(session->clientUser, &gameId)) {
            co_return winrt::hstring(L"steam_launch=already-running\r\n");
        }
        session->PumpCallbacks();
        call = reinterpret_cast<Launch>(methods[2])(manager, &gameId, 0, 0, "");
        if (!call) co_return winrt::hstring(L"steam_launch=request-rejected\r\n");
        session->launchCall = call;
    }

    struct CompletionEvent {
        std::uint64_t call;
        std::int32_t kind;
        std::uint32_t size;
    };
    constexpr std::int32_t callCompleted = 703;
    constexpr std::int32_t launchResult = 1270027;
    std::array<std::uint8_t, 524> payload{};
    auto verdict = L"timeout";
    report += L"steam_launch_submitted=yes\r\n";
    auto deadline = GetTickCount64() + 25000;
    bool accepted = false;
    bool observedRunning = false;
    for (;;) {
        {
            std::lock_guard lock(steamSessionMutex);
            auto session = liveSteamSession;
            if (!session || session->launchCall != call) co_return winrt::hstring(report + L"steam_launch=session-lost\r\n");
            bool responded = false;
            for (unsigned i = 0; i < 32; ++i) {
                SteamCallback callback{};
                if (!session->getCallback(session->pipe, &callback)) break;
                if (callback.message == callCompleted && callback.length == sizeof(CompletionEvent) && callback.data) {
                    CompletionEvent completion{};
                    std::memcpy(&completion, callback.data, sizeof(completion));
                    if (completion.call == call) {
                        responded = true;
                        bool failed = false;
                        if (completion.kind == launchResult && completion.size == payload.size() &&
                                result(session->pipe, call, payload.data(), static_cast<std::int32_t>(payload.size()),
                                    launchResult, &failed) && !failed) {
                            std::uint64_t reportedGame = 0;
                            std::int32_t code = -1;
                            std::memcpy(&reportedGame, payload.data(), sizeof(reportedGame));
                            std::memcpy(&code, payload.data() + sizeof(reportedGame), sizeof(code));
                            if (reportedGame == gameId) {
                                report += L"steam_launch_client_result=" + std::to_wstring(code) + L"\r\n";
                                accepted = code == 0;
                                verdict = accepted ? L"start-unconfirmed" : L"client-rejected";
                            }
                        }
                        if (!accepted && verdict == std::wstring_view(L"timeout")) verdict = L"result-unreadable";
                    }
                }
                session->freeCallback(session->pipe);
            }
            auto user = session->clientUser;
            auto running = reinterpret_cast<GameQuery>(session->userMethods[67])(user, &gameId);
            observedRunning = observedRunning || running;
            auto windowReady = reinterpret_cast<GameQuery>(session->userMethods[68])(user, &gameId);
            if (accepted && windowReady && running) verdict = L"window-ready";
            if ((responded && !accepted) || (accepted && windowReady && running) || GetTickCount64() >= deadline) {
                if (accepted && observedRunning && verdict == std::wstring_view(L"start-unconfirmed")) {
                    verdict = running ? L"running-window-unconfirmed" : L"exited-during-startup";
                }
                report += std::wstring(L"steam_launch_running_seen=") + (observedRunning ? L"yes\r\n" : L"no\r\n");
                session->launchCall = 0;
                break;
            }
        }
        co_await winrt::resume_after(std::chrono::milliseconds(250));
        co_await caller;
    }
    co_return winrt::hstring(report + L"steam_launch=" + verdict + L"\r\n");
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

    winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> RunSteamSignIn(
            winrt::Windows::Storage::StorageFile const& file, winrt::hstring const& prefix,
            winrt::hstring const& account, winrt::hstring const& approval, winrt::hstring const& accountId) {
        return SteamSignIn(file, prefix, account, approval, accountId);
    }

    void SignOutSteam() {
        SteamSignOut();
    }

    winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> QueueSteamDownload(std::uint32_t app) {
        return StartSteamDownload(app);
    }

    winrt::hstring SteamDownloadStatus() {
        return ReadSteamDownload();
    }

    winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> LaunchSteamGame(std::uint32_t app) {
        return LaunchLocalSteamGame(app);
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
