#include <windows.h>
#include <fileapifromapp.h>
#include <winstring.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
#include <winrt/Windows.Security.Cryptography.DataProtection.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.Diagnostics.h>
#include <winrt/Windows.System.h>
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
#include "SteamSdlSurface.h"
#include "SteamCloudRestore.h"

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

struct PipeError {
    const wchar_t *stage;
    DWORD code;
};

struct PipeSession {
    winrt::handle pipe;
    winrt::handle process;
    std::wstring suffix;
    DWORD pid = 0;
    DWORD helperToken = 0;
};

static std::mutex lifetimeMutex;
static PipeSession lifetimeSession;

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

static winrt::handle PipeEvent() {
    winrt::handle event(CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS));
    if (!event) throw PipeError{L"create-event", GetLastError()};
    return event;
}

static void OpenPipe(PipeSession& session, std::wstring const& child, bool lifetime) {
    ULONGLONG tick = GetTickCount64();
    session.suffix = Hex(GetCurrentProcessId()).substr(2) + Hex(DWORD(tick >> 32)).substr(2) +
                     Hex(DWORD(tick)).substr(2);
    std::wstring name = PROBE_PIPE_PREFIX + session.suffix;
    HANDLE rawPipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
        FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_REJECT_REMOTE_CLIENTS,
        1, 256, 256, 0, nullptr);
    if (rawPipe == INVALID_HANDLE_VALUE) throw PipeError{L"create-pipe", GetLastError()};
    session.pipe.attach(rawPipe);
    auto event = PipeEvent();

    std::wstring arguments = L"\"" + child + L"\" " + (lifetime ? L"L" : L"") + session.suffix;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(child.c_str(), arguments.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &processInfo)) {
        throw PipeError{L"create-child", GetLastError()};
    }
    session.process.attach(processInfo.hProcess);
    session.pid = processInfo.dwProcessId;
    winrt::handle thread(processInfo.hThread);

    OVERLAPPED connect{};
    connect.hEvent = event.get();
    DWORD bytes = 0;
    auto deadline = GetTickCount64() + 5000;
    DWORD error = CompletePipeIo(session.pipe.get(), connect, bytes,
                                 ConnectNamedPipe(session.pipe.get(), &connect), deadline);
    if (error != ERROR_SUCCESS && error != ERROR_PIPE_CONNECTED) throw PipeError{L"connect", error};
}

template <typename Message>
static void TransferPipe(PipeSession const& session, Message& message, bool write, ULONGLONG deadline) {
    auto event = PipeEvent();
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD bytes = 0;
    auto pipe = session.pipe.get();
    BOOL started = write ? WriteFile(pipe, &message, sizeof(message), &bytes, &operation)
                         : ReadFile(pipe, &message, sizeof(message), &bytes, &operation);
    DWORD error = CompletePipeIo(pipe, operation, bytes, started, deadline);
    if (error != ERROR_SUCCESS || bytes != sizeof(message)) {
        throw PipeError{write ? L"write-reply" : L"read-request",
                        error != ERROR_SUCCESS ? error : ERROR_INVALID_DATA};
    }
}

static DWORD ChildExit(PipeSession const& session, ULONGLONG deadline) {
    DWORD wait = WaitForSingleObject(session.process.get(), RemainingTime(deadline));
    if (wait != WAIT_OBJECT_0)
        throw PipeError{L"wait-for-child", wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError()};
    DWORD exit = 0;
    if (!GetExitCodeProcess(session.process.get(), &exit)) throw PipeError{L"read-child-exit", GetLastError()};
    return exit;
}

static std::wstring PipeFailure(const wchar_t *test, PipeSession const& session, PipeError error) {
    std::wstring label = test;
    std::wstring report = label + L"=failed\r\n" + label + L"_stage=" + error.stage +
                          L"\r\n" + label + L"_error=" + Hex(error.code) + L"\r\n";
    DWORD exit = 0;
    if (session.process && GetExitCodeProcess(session.process.get(), &exit) && exit != STILL_ACTIVE)
        report += label + L"_child_exit=" + Hex(exit) + L"\r\n";
    return report;
}

static std::wstring PipeResult(std::wstring const& child) {
    PipeSession session;
    try {
        OpenPipe(session, child, false);
        auto deadline = GetTickCount64() + 5000;
        DWORD request = 0;
        TransferPipe(session, request, false, deadline);
        if (request != (session.pid ^ PROBE_PIPE_HELLO)) throw PipeError{L"invalid-request", ERROR_INVALID_DATA};
        DWORD reply = request ^ PROBE_PIPE_REPLY;
        TransferPipe(session, reply, true, deadline);
        DWORD exit = ChildExit(session, deadline);
        return std::wstring(L"ipc=") + (exit == PROBE_PIPE_EXIT ? L"success\r\n" : L"failed\r\n") +
            L"ipc_request=" + Hex(request) + L"\r\nipc_reply=" + Hex(reply) +
            L"\r\nipc_child_exit=" + Hex(exit) + L"\r\n";
    } catch (PipeError error) {
        return PipeFailure(L"ipc", session, error);
    }
}

static std::wstring LifetimeIdentity(PipeSession const& session) {
    return L"parent_pid=" + Hex(GetCurrentProcessId()) + L"\r\nchannel_id=" + session.suffix +
        L"\r\nhelper_pid=" + Hex(session.pid) + L"\r\nhelper_token=" + Hex(session.helperToken) + L"\r\n";
}

static std::wstring StartLifetimePipe(std::wstring const& child) {
    std::lock_guard lock(lifetimeMutex);
    lifetimeSession = {};
    try {
        OpenPipe(lifetimeSession, child, true);
        ProbeLifetimeHello hello{};
        TransferPipe(lifetimeSession, hello, false, GetTickCount64() + 5000);
        if (hello.pid != (lifetimeSession.pid ^ PROBE_PIPE_HELLO))
            throw PipeError{L"invalid-request", ERROR_INVALID_DATA};
        lifetimeSession.helperToken = hello.token;
        return L"lifetime=ready\r\n" + LifetimeIdentity(lifetimeSession);
    } catch (PipeError error) {
        auto report = PipeFailure(L"lifetime", lifetimeSession, error);
        lifetimeSession = {};
        return report;
    }
}

static std::wstring FinishLifetimePipe() {
    std::lock_guard lock(lifetimeMutex);
    if (!lifetimeSession.pipe) return L"lifetime=failed\r\nlifetime_stage=no-open-connection\r\n";
    PipeSession session = std::move(lifetimeSession);
    try {
        ULONGLONG tick = GetTickCount64();
        DWORD challenge = DWORD(tick);
        TransferPipe(session, challenge, true, tick + 5000);
        DWORD exit = ChildExit(session, tick + 5000);
        DWORD expected = PROBE_LIFETIME_EXIT(challenge, session.helperToken);
        return std::wstring(L"lifetime=") + (exit == expected ? L"complete\r\n" : L"failed\r\n") +
            LifetimeIdentity(session) + L"return_challenge=" + Hex(challenge) +
            L"\r\nlifetime_child_exit=" + Hex(exit) + L"\r\nlifetime_expected_exit=" + Hex(expected) + L"\r\n";
    } catch (PipeError error) {
        return LifetimeIdentity(session) + PipeFailure(L"lifetime", session, error);
    }
}

static std::wstring InstalledPath() {
    auto folder = winrt::Windows::ApplicationModel::Package::Current().InstalledLocation();
    return std::wstring(folder.Path()) + L"\\";
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
    std::uint64_t downloadStarted = 0;
    std::uint64_t lastTransition = 0;
    std::uint32_t lastInstallState = 0;
    enum class DownloadRetry { NotAttempted, Accepted, Declined };
    DownloadRetry downloadRetry = DownloadRetry::NotAttempted;
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

#include "SteamCloudTransfer.h"

static void SteamSignOut() {
    std::lock_guard lock(steamSessionMutex);
    if (!liveSteamSession) return;
    delete liveSteamSession;
    liveSteamSession = nullptr;
    --winrt::get_module_lock();
}

static constexpr auto SteamCacheFile = L"steam-session.dpapi";
static constexpr auto SteamCacheOptIn = L"steamSessionRemembered";
static constexpr std::wstring_view SteamCacheSchema = L"steam-session-v1\n";
static constexpr std::uint32_t SteamCacheMaximum = 65536;

static bool SteamSessionIsCurrent(std::uint32_t expiry) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return expiry != 0 && static_cast<std::int64_t>(expiry) > now;
}

struct WipeSteamBuffer {
    std::uint8_t *data;
    std::uint32_t length;
    ~WipeSteamBuffer() { SecureZeroMemory(data, length); }
};

static winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Storage::Streams::IBuffer>
ProtectSteamSession(winrt::hstring account, winrt::hstring approval, winrt::hstring accountId,
                    std::uint32_t expiry) {
    using winrt::Windows::Security::Cryptography::DataProtection::DataProtectionProvider;
    using winrt::Windows::Storage::Streams::Buffer;
    using winrt::Windows::Storage::Streams::IBuffer;
    if (!SteamSessionIsCurrent(expiry) ||
            std::wstring_view(account).find(L'\n') != std::wstring_view::npos ||
            std::wstring_view(approval).find(L'\n') != std::wstring_view::npos) {
        co_return IBuffer{nullptr};
    }
    try {
        std::wstring metadata(SteamCacheSchema);
        metadata += std::to_wstring(expiry) + L"\n" + std::wstring(accountId) + L"\n" +
            std::wstring(account) + L"\n";
        const auto metadataBytes = metadata.size() * sizeof(wchar_t);
        const auto approvalBytes = approval.size() * sizeof(wchar_t);
        const auto length = static_cast<std::uint32_t>(metadataBytes + approvalBytes);
        Buffer clear(length);
        clear.Length(length);
        const WipeSteamBuffer wipe{clear.data(), length};
        std::memcpy(wipe.data, metadata.data(), metadataBytes);
        std::memcpy(wipe.data + metadataBytes, approval.data(), approvalBytes);
        approval.clear();
        co_return co_await DataProtectionProvider(L"LOCAL=user").ProtectAsync(clear);
    } catch (winrt::hresult_error const&) {
        co_return IBuffer{nullptr};
    }
}

static winrt::Windows::Foundation::IAsyncOperation<bool> ForgetSteamSession() {
    using namespace winrt::Windows::Storage;
    try {
        auto values = ApplicationData::Current().LocalSettings().Values();
        if (values.HasKey(SteamCacheOptIn)) values.Remove(SteamCacheOptIn);
    } catch (winrt::hresult_error const&) {}
    try {
        auto cached = co_await ApplicationData::Current().LocalCacheFolder().TryGetItemAsync(SteamCacheFile);
        if (cached) co_await cached.DeleteAsync(StorageDeleteOption::PermanentDelete);
        co_return true;
    } catch (winrt::hresult_error const&) {
        co_return false;
    }
}

static winrt::Windows::Foundation::IAsyncOperation<bool>
SaveSteamSession(winrt::Windows::Storage::Streams::IBuffer sealed) {
    using namespace winrt::Windows::Storage;
    if (!sealed || !sealed.Length() || sealed.Length() > SteamCacheMaximum) co_return false;
    try {
        auto data = ApplicationData::Current();
        auto values = data.LocalSettings().Values();
        if (values.HasKey(SteamCacheOptIn)) values.Remove(SteamCacheOptIn);
        auto cache = co_await data.LocalCacheFolder().CreateFileAsync(
            SteamCacheFile, CreationCollisionOption::ReplaceExisting);
        co_await FileIO::WriteBufferAsync(cache, sealed);
        values.Insert(SteamCacheOptIn, winrt::box_value(true));
        co_return true;
    } catch (winrt::hresult_error const&) {}
    co_await ForgetSteamSession();
    co_return false;
}

static winrt::Windows::Foundation::IAsyncOperation<bool> SteamSignOutAndForget() {
    const bool cleared = co_await ForgetSteamSession();
    SteamSignOut();
    co_return cleared;
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
        winrt::hstring approval, winrt::hstring accountId, bool remember, std::uint32_t expiry,
        bool restoring = false) {
    using winrt::Windows::Storage::FileIO;
    using CreateUser = std::int32_t (__cdecl *)(std::int32_t *);
    using SetToken = void (__cdecl *)(void *, const char *, const char *);
    using LogOn = std::int32_t (__cdecl *)(void *, std::uint64_t);
    const auto complete = restoring ? L"status=steam-session-fallback\r\n" : L"status=steam-core-complete\r\n";
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

    winrt::Windows::Storage::Streams::IBuffer sealed{nullptr};
    if (remember) sealed = co_await ProtectSteamSession(account, approval, accountId, expiry);
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
    ULONGLONG deadline = GetTickCount64() + (restoring ? 20000 : 60000);
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
    size_t namedCount = 0;
    for (size_t i = 0; i < apps.size(); ++i) {
        if (names[i].empty()) continue;
        ++namedCount;
        if (appType(clientApps, apps[i]) != gameType) continue;
        JsonObject entry;
        entry.SetNamedValue(L"id", JsonValue::CreateNumberValue(apps[i]));
        entry.SetNamedValue(L"name", JsonValue::CreateStringValue(names[i]));
        entry.SetNamedValue(L"game", JsonValue::CreateBooleanValue(true));
        handles.games.push_back(apps[i]);
        library.Append(entry);
    }
    report += L"steam_library_named=" + std::to_wstring(namedCount) + L"\r\n";
    report += L"steam_library_filter=games-only\r\n";
    report += L"steam_library_visible=" + std::to_wstring(library.Size()) + L"\r\n";
    if (!namedCount) {
        report += L"steam_library_metadata=missing\r\n";
        co_await save(complete);
        co_return winrt::hstring{};
    }

    auto payload = library.Stringify();
    if (remember) {
        const bool saved = co_await SaveSteamSession(sealed);
        report += saved ? L"steam_session_cache=saved\r\n" : L"steam_session_cache=unavailable\r\n";
    }
    if (restoring) report += L"steam_session_cache=restored\r\n";
    co_await save(L"status=steam-library-rendering\r\n");
    std::lock_guard lock(steamSessionMutex);
    ++winrt::get_module_lock();
    liveSteamSession = session.release();
    co_return payload;
}

static winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> ResumeSteamSignIn(
        winrt::Windows::Storage::StorageFile reportFile, winrt::hstring prefix) {
    using winrt::Windows::Security::Cryptography::DataProtection::DataProtectionProvider;
    using namespace winrt::Windows::Storage;
    using namespace winrt::Windows::Storage::Streams;
    bool optedIn = false;
    try {
        auto data = ApplicationData::Current();
        auto values = data.LocalSettings().Values();
        optedIn = values.HasKey(SteamCacheOptIn) &&
            winrt::unbox_value_or<bool>(values.Lookup(SteamCacheOptIn), false);
        if (!optedIn) co_return winrt::hstring{};
        auto file = co_await data.LocalCacheFolder().GetFileAsync(SteamCacheFile);
        auto stream = co_await file.OpenReadAsync();
        const auto size = stream.Size();
        if (size == 0 || size > SteamCacheMaximum) throw winrt::hresult_invalid_argument();
        const auto length = static_cast<std::uint32_t>(size);
        auto encrypted = co_await stream.ReadAsync(Buffer(length), length, InputStreamOptions::None);
        stream.Close();
        if (encrypted.Length() != length) throw winrt::hresult_invalid_argument();
        winrt::hstring accountId, account, approval;
        {
            auto clear = co_await DataProtectionProvider().UnprotectAsync(encrypted);
            const WipeSteamBuffer wipe{clear.data(), clear.Length()};
            if (wipe.length % sizeof(wchar_t) != 0) throw winrt::hresult_invalid_argument();
            std::wstring_view fields(reinterpret_cast<const wchar_t *>(wipe.data), wipe.length / sizeof(wchar_t));
            if (!fields.starts_with(SteamCacheSchema)) throw winrt::hresult_invalid_argument();
            fields.remove_prefix(SteamCacheSchema.size());
            auto nextField = [&]() {
                const auto newline = fields.find(L'\n');
                if (newline == fields.npos) throw winrt::hresult_invalid_argument();
                const auto field = fields.substr(0, newline);
                fields.remove_prefix(newline + 1);
                return field;
            };
            const auto expiryText = winrt::to_string(nextField());
            std::uint32_t expiry = 0;
            const auto parsed = std::from_chars(expiryText.data(), expiryText.data() + expiryText.size(), expiry);
            if (parsed.ec != std::errc{} || parsed.ptr != expiryText.data() + expiryText.size() ||
                    !SteamSessionIsCurrent(expiry)) {
                throw winrt::hresult_invalid_argument();
            }
            accountId = winrt::hstring(nextField());
            account = winrt::hstring(nextField());
            approval = winrt::hstring(fields);
        }
        auto payload = co_await SteamSignIn(reportFile, prefix, std::move(account), std::move(approval),
            std::move(accountId), false, 0, true);
        if (!payload.empty()) co_return payload;
    } catch (winrt::hresult_error const&) {}
    if (optedIn) co_await ForgetSteamSession();
    co_return winrt::hstring(L"fallback");
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

static const wchar_t *SteamPathScope(std::wstring const& directory, std::wstring const& library,
        std::wstring const& data, std::wstring const& package) {
    if (directory.empty()) return L"unresolved";
    auto within = [&](std::wstring const& root) {
        return CompareStringOrdinal(directory.c_str(), -1, root.c_str(), -1, TRUE) == CSTR_EQUAL ||
            IsLocalSteamInstall(directory, root);
    };
    if (within(library)) return L"app-library";
    if (within(data)) return L"app-data";
    if (within(package)) return L"read-only-package";
    return L"outside-app";
}

struct SteamDestination {
    std::int32_t index;
    std::wstring_view installScope;
    std::wstring_view libraryScope;
};

static SteamDestination ReadSteamDestination(void *manager, std::uintptr_t const *methods,
        std::uint32_t app, std::wstring const& library,
        std::wstring const& data, std::wstring const& package) {
    using AppLibraryFolder = std::int32_t (__cdecl *)(void *, std::uint32_t);
    auto index = reinterpret_cast<AppLibraryFolder>(methods[82])(manager, app);
    auto directory = SteamPath(reinterpret_cast<SteamAppPath>(methods[5]), manager, app);
    auto folder = index < 0 ? std::wstring{} : SteamPath(reinterpret_cast<SteamAppPath>(methods[76]), manager, index);
    return {index, SteamPathScope(directory, library, data, package), SteamPathScope(folder, library, data, package)};
}

static std::wstring DescribeSteamDestination(void *manager, std::uintptr_t const *methods,
        std::uint32_t app, std::wstring_view checkpoint, std::wstring const& library,
        std::wstring const& data, std::wstring const& package) {
    const auto destination = ReadSteamDestination(manager, methods, app, library, data, package);
    std::wstring prefix = L"steam_launch_redistributables_" + std::wstring(checkpoint) + L"_";
    auto report = prefix + L"install_scope=" + std::wstring(destination.installScope) + L"\r\n";
    report += prefix + L"library_index=" + std::to_wstring(destination.index) + L"\r\n";
    if (destination.index >= 0) {
        report += prefix + L"library_scope=" + std::wstring(destination.libraryScope) + L"\r\n";
    }
    return report;
}

static bool RequestSteamDownloadNow(void *manager, std::uintptr_t const *methods, std::uint32_t app) {
    using SetQueuePlacement = bool (__cdecl *)(void *, std::uint32_t, std::int32_t);
    constexpr std::int32_t firstInQueue = 1;
    return reinterpret_cast<SetQueuePlacement>(methods[56])(manager, app, firstInQueue);
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
    if (!methods || methods[56] != base + 0x766730 || methods[82] != base + 0x781040) {
        co_return winrt::hstring(L"interface-error");
    }
    auto manager = session->appManager;
    auto registration = RegisterSteamLibrary(manager, methods, path);
    if (registration.index < 0) {
        co_return winrt::hstring(L"library-folder-error:" + std::to_wstring(registration.result));
    }

    constexpr std::uint32_t updatePaused = 0x200;
    const bool wasPaused = (reinterpret_cast<SteamAppValue>(methods[4])(manager, app) & updatePaused) != 0;
    auto install = reinterpret_cast<SteamInstall>(methods[0]);
    auto result = install(manager, app, registration.index, false);
    if (result != 0) co_return winrt::hstring(L"install-rejected:" + std::to_wstring(result));
    session->downloadApp = app;
    session->libraryPath = path;
    session->downloadStarted = session->lastTransition = GetTickCount64();
    session->lastInstallState = 0;
    using Retry = SteamSignInHandles::DownloadRetry;
    session->downloadRetry = Retry::NotAttempted;
    if (wasPaused) {
        // This is the explicit “Download now” action for the selected, already queued title.
        const bool accepted = RequestSteamDownloadNow(manager, methods, app);
        session->downloadRetry = accepted ? Retry::Accepted : Retry::Declined;
        if (accepted) {
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
    constexpr auto installMask = fullyInstalled | updateRequired | missingOrCorrupt | downloading | stagingOrCommitting;
    using ActiveApp = std::uint32_t (__cdecl *)(void *);
    const auto app = session->downloadApp;
    auto state = reinterpret_cast<SteamAppValue>(methods[4])(manager, app);
    auto activeApp = reinterpret_cast<ActiveApp>(methods[42])(manager);
    auto now = GetTickCount64();
    if (state != session->lastInstallState || activeApp == session->downloadApp) {
        session->lastInstallState = state;
        session->lastTransition = now;
    }

    using Retry = SteamSignInHandles::DownloadRetry;
    if ((state & installMask) != fullyInstalled && (state & updatePaused) && now - session->downloadStarted >= 3000 &&
            session->downloadRetry == Retry::NotAttempted) {
        const bool accepted = RequestSteamDownloadNow(manager, methods, app);
        session->downloadRetry = accepted ? Retry::Accepted : Retry::Declined;
        if (accepted) {
            session->lastTransition = now;
            return winrt::hstring(L"resuming:" + std::to_wstring(state));
        }
    }

    std::wstring checkpoint;
    if ((state & installMask) == fullyInstalled) {
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
    auto report = checkpoint + L":" + std::to_wstring(state);
    if (!session->downloadApp) {
        using winrt::Windows::ApplicationModel::Package;
        using winrt::Windows::Storage::ApplicationData;
        const auto destination = ReadSteamDestination(manager, methods, app, session->libraryPath,
            std::wstring(ApplicationData::Current().LocalFolder().Path()),
            std::wstring(Package::Current().InstalledLocation().Path()));
        report += L"\r\nsteam_download_install_scope=" + std::wstring(destination.installScope) + L"\r\n";
        report += L"steam_download_library_scope=" + std::wstring(destination.libraryScope) + L"\r\n";
        report += L"steam_download_library_index=" + std::to_wstring(destination.index) + L"\r\n";
        const auto retry = session->downloadRetry;
        report += std::wstring(L"steam_download_retry=") +
            (retry == Retry::Accepted ? L"accepted" : retry == Retry::Declined ? L"declined" : L"not-attempted") + L"\r\n";
    }
    return winrt::hstring(report);
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

struct SteamGameProcess {
    winrt::handle handle;
    std::wstring_view checkpoint = L"not-reported";
    std::wstring_view imageRole = L"unavailable";
    std::wstring basename;
    DWORD processId = 0;
    std::uint64_t createdAt = 0;
    DWORD error = ERROR_SUCCESS;

    void Observe(void *clientUser, std::uintptr_t const *methods, std::uint64_t gameId,
            FILETIME const& launchTime) {
        if (handle) return;
        using GameCount = std::uint32_t (__cdecl *)(void *);
        using GetGameID = std::uint64_t * (__cdecl *)(void *, std::uint64_t *, std::uint32_t);
        using GetGamePID = DWORD (__cdecl *)(void *, std::uint32_t);
        const auto count = (std::min)(reinterpret_cast<GameCount>(methods[132])(clientUser), 16u);
        for (std::uint32_t index = 0; index < count; ++index) {
            std::uint64_t runningId = 0;
            reinterpret_cast<GetGameID>(methods[133])(clientUser, &runningId, index);
            if (runningId != gameId) continue;
            const auto pid = reinterpret_cast<GetGamePID>(methods[134])(clientUser, index);
            if (!pid) continue;
            winrt::handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid)};
            if (!process) {
                error = GetLastError();
                checkpoint = L"open-error";
                continue;
            }
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!GetProcessTimes(process.get(), &created, &exited, &kernel, &user)) {
                error = GetLastError();
                checkpoint = L"times-error";
                continue;
            }
            error = ERROR_SUCCESS;
            if (CompareFileTime(&created, &launchTime) < 0) {
                checkpoint = L"pre-existing";
                continue;
            }
            handle = std::move(process);
            processId = pid;
            createdAt = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            checkpoint = L"captured";
            try {
                using winrt::Windows::System::Diagnostics::ProcessDiagnosticInfo;
                if (auto diagnostic = ProcessDiagnosticInfo::TryGetForProcessId(pid)) {
                    auto image = diagnostic.ExecutableFileName();
                    auto filename = std::wstring_view(image);
                    if (auto separator = filename.find_last_of(L"\\/"); separator != std::wstring_view::npos) {
                        filename.remove_prefix(separator + 1);
                    }
                    const auto size = static_cast<int>(filename.size());
                    if (filename.find_first_of(L"\r\n") == filename.npos) basename.assign(filename);
                    if (CompareStringOrdinal(filename.data(), size, L"javaw.exe", -1, TRUE) == CSTR_EQUAL) {
                        imageRole = L"java-windowed";
                    } else if (CompareStringOrdinal(filename.data(), size, L"java.exe", -1, TRUE) == CSTR_EQUAL) {
                        imageRole = L"java-console";
                    } else if (!filename.empty()) {
                        imageRole = L"other";
                    }
                }
            } catch (winrt::hresult_error const&) {}
            return;
        }
    }

    std::wstring Report() const {
        auto report = L"steam_launch_process=" + std::wstring(checkpoint) + L"\r\n";
        if (error) report += L"steam_launch_process_error=" + Hex(error) + L"\r\n";
        if (!handle) return report;
        report += L"steam_launch_process_image=" + std::wstring(imageRole) + L"\r\n";
        DWORD exitCode = 0;
        if (WaitForSingleObject(handle.get(), 0) == WAIT_OBJECT_0 && GetExitCodeProcess(handle.get(), &exitCode)) {
            report += L"steam_launch_process_exit=" + Hex(exitCode) + L"\r\n";
        }
        return report;
    }
};

static std::wstring DescribeProcessWait(HANDLE handle, DWORD timeout, const wchar_t *key) {
    const auto started = GetTickCount64();
    const auto wait = WaitForSingleObject(handle, timeout);
    const auto waitError = wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    std::wstring field(key);
    auto result = field + L"_ms=" + std::to_wstring(GetTickCount64() - started) + L"\r\n";
    if (wait == WAIT_TIMEOUT) return result + field + L"=running\r\n";
    if (wait != WAIT_OBJECT_0) {
        return result + field + L"=wait-error\r\n" + field + L"_error=" + Hex(waitError) + L"\r\n";
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(handle, &code)) {
        const auto error = GetLastError();
        return result + field + L"=exit-code-error\r\n" + field + L"_error=" + Hex(error) + L"\r\n";
    }
    return result + field + L"=exited\r\n" + field + L"_exit=" + Hex(code) + L"\r\n";
}

static winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> ProbeJavaVersion(std::wstring gameDirectory) {
    co_await winrt::resume_background();
    const auto java = gameDirectory + L"\\jre\\bin\\java.exe";
    auto command = L"\"" + java + L"\" -version";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(java.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, gameDirectory.c_str(), &startup, &child)) {
        co_return winrt::hstring(L"steam_java_probe=create-error\r\nsteam_java_probe_error=" +
            Hex(GetLastError()) + L"\r\n");
    }
    winrt::handle process(child.hProcess);
    winrt::handle thread(child.hThread);
    DWORD wait = WaitForSingleObject(process.get(), 10000);
    if (wait == WAIT_TIMEOUT) co_return winrt::hstring(L"steam_java_probe=timeout\r\n");
    DWORD exitCode = 0;
    if (wait != WAIT_OBJECT_0 || !GetExitCodeProcess(process.get(), &exitCode)) {
        co_return winrt::hstring(L"steam_java_probe=wait-error\r\nsteam_java_probe_error=" +
            Hex(GetLastError()) + L"\r\n");
    }
    co_return winrt::hstring(L"steam_java_probe=exited\r\nsteam_java_probe_exit=" + Hex(exitCode) + L"\r\n");
}

static winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> RunSlayConsoleDiagnostic(
        winrt::Windows::Storage::StorageFolder library, std::wstring packageRoot, std::wstring log) {
    constexpr std::uint32_t app = 646570;
    const std::uint64_t gameId = app;
    if (!co_await HasCompletedSteamManifest(library, app)) co_return winrt::hstring(L"steam_slay_console=not-installed\r\n");
    const auto game = std::wstring(library.Path()) + L"\\steamapps\\common\\SlayTheSpire";
    const auto java = game + L"\\jre\\bin\\java.exe";
    auto command = L"\"" + java + L"\" -Xmx32m -cp \"" + packageRoot +
        L"\" SlayConsoleDiagnostic \"" + log + L"\" \"" + game + L"\"";
    PROCESS_INFORMATION child{};
    {
        std::lock_guard lock(steamSessionMutex);
        auto session = liveSteamSession;
        if (!session || !session->OwnsSession() || session->downloadApp || session->launchCall) {
            co_return winrt::hstring(L"steam_slay_console=session-unavailable\r\n");
        }
        auto methods = FindSteamAppManager(*session);
        const auto base = reinterpret_cast<std::uintptr_t>(session->module);
        if (!methods || session->userMethods[182] != base + 0x75d640 || session->userMethods[67] != base + 0x75a320) {
            co_return winrt::hstring(L"steam_slay_console=interface-error\r\n");
        }
        using IsSubscribed = bool (__cdecl *)(void *, std::uint32_t);
        using GameQuery = bool (__cdecl *)(void *, const std::uint64_t *);
        auto manager = session->appManager;
        if (std::find(session->games.begin(), session->games.end(), app) == session->games.end() ||
                !reinterpret_cast<IsSubscribed>(session->userMethods[182])(session->clientUser, app) ||
                reinterpret_cast<GameQuery>(session->userMethods[67])(session->clientUser, &gameId) ||
                (reinterpret_cast<SteamAppValue>(methods[4])(manager, app) & 0x7000a6) != 4) {
            co_return winrt::hstring(L"steam_slay_console=not-ready\r\n");
        }
        auto installed = SteamPath(reinterpret_cast<SteamAppPath>(methods[5]), manager, app);
        if (CompareStringOrdinal(installed.c_str(), -1, game.c_str(), -1, TRUE) != CSTR_EQUAL) {
            co_return winrt::hstring(L"steam_slay_console=install-path-error\r\n");
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (!CreateProcessW(java.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, game.c_str(), &startup, &child)) {
            co_return winrt::hstring(L"steam_slay_console=create-error\r\nsteam_slay_console_error=" + Hex(GetLastError()) + L"\r\n");
        }
    }
    winrt::handle process(child.hProcess), thread(child.hThread);
    co_await winrt::resume_background();
    if (WaitForSingleObject(process.get(), 35000) != WAIT_OBJECT_0) {
        co_return winrt::hstring(L"steam_slay_console=helper-wait-incomplete\r\n");
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(process.get(), &code)) co_return winrt::hstring(L"steam_slay_console=helper-exit-unavailable\r\n");
    co_return winrt::hstring(L"steam_slay_console=helper-exited\r\nsteam_slay_console_helper_exit=" + Hex(code) + L"\r\n");
}

enum class GameBackend { SteamPlay, DosBox, SdlHost };
static constexpr std::uint32_t xcomDosAppId = 7760;
static constexpr std::uint32_t dwarfFortressAppId = 975370;
static constexpr std::uint32_t stardewValleyAppId = 413150;
static constexpr std::uint32_t slayTheSpireAppId = 646570;

static winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> RestoreSlaySteamCloud(
        std::uint32_t app, bool preview) {
    using namespace winrt::Windows::Data::Json;
    using namespace winrt::Windows::Storage;
    if (app != slayTheSpireAppId) co_return winrt::hstring(L"unsupported");
    if (steamLaunchActive.test_and_set()) co_return winrt::hstring(L"busy");
    struct ResetOperation {
        ~ResetOperation() { steamLaunchActive.clear(); }
    } resetOperation;
    {
        std::lock_guard lock(steamSdlMutex);
        if (steamSdlSurface && steamSdlSurface->Alive()) co_return winrt::hstring(L"busy");
    }
    try {
        auto root = ApplicationData::Current().LocalFolder();
        auto library = co_await root.GetFolderAsync(L"SteamLibrary");
        if (!co_await HasCompletedSteamManifest(library, app)) co_return winrt::hstring(L"not-installed");
        auto apps = co_await library.GetFolderAsync(L"steamapps");
        auto common = co_await apps.GetFolderAsync(L"common");
        auto game = co_await common.GetFolderAsync(L"SlayTheSpire");
        if (!SlayCloudDirectoryExists(std::wstring(game.Path()))) co_return winrt::hstring(L"not-installed");
        std::uint64_t account = 0;
        {
            std::lock_guard lock(steamSessionMutex);
            if (!liveSteamSession || !liveSteamSession->OwnsSession()) co_return winrt::hstring(L"sign-in");
            account = liveSteamSession->accountId;
        }
        auto cloud = co_await root.CreateFolderAsync(L"SteamCloud", CreationCollisionOption::OpenIfExists);
        auto title = co_await cloud.CreateFolderAsync(L"646570", CreationCollisionOption::OpenIfExists);
        auto stage = co_await title.CreateFolderAsync(L"download", CreationCollisionOption::GenerateUniqueName);
        winrt::hstring result;
        try {
            auto response = JsonObject::Parse(co_await StageSteamCloudFiles(app, stage));
            auto status = response.GetNamedString(L"status");
            if (status == L"ready") {
                bool sameAccount;
                {
                    std::lock_guard lock(steamSessionMutex);
                    sameAccount = liveSteamSession && liveSteamSession->accountId == account && liveSteamSession->OwnsSession();
                }
                auto files = response.GetNamedArray(L"files");
                if (!sameAccount) {
                    result = L"sign-in";
                } else if (preview) {
                    auto restored = co_await PreviewSlayCloudRestore(stage, files);
                    result = winrt::hstring(L"verified:" + std::to_wstring(files.Size()) +
                        L":preview-installed:" + std::to_wstring(restored));
                } else {
                    RollBackSlayCloud(std::wstring(title.Path()), std::wstring(game.Path()));
                    auto restored = co_await RestoreSlayCloudFiles(title, stage, game, files);
                    result = restored ? winrt::hstring(L"restored:" + std::to_wstring(restored)) : winrt::hstring(L"no-compatible-saves");
                }
            } else {
                auto serviceError = response.GetNamedNumber(L"steamResult", 0);
                result = preview && serviceError ? winrt::hstring(std::wstring(status) + L":" +
                    std::to_wstring(static_cast<std::int32_t>(serviceError))) : status;
            }
        } catch (winrt::hresult_error const&) {
            result = L"storage-error";
        }
        try {
            co_await stage.DeleteAsync(StorageDeleteOption::PermanentDelete);
        } catch (winrt::hresult_error const&) {
            // A downloaded staging folder is private; it never contains live saves.
        }
        co_return result;
    } catch (winrt::hresult_error const&) {
        co_return winrt::hstring(L"storage-error");
    }
}

static winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> LaunchLocalSteamGame(
        std::uint32_t app, winrt::Windows::Storage::StorageFile file, winrt::hstring prefix,
        GameBackend backend) {
    using namespace winrt::Windows::Storage;
    using IsSubscribed = bool (__cdecl *)(void *, std::uint32_t);
    using GameQuery = bool (__cdecl *)(void *, const std::uint64_t *);
    using Launch = std::uint64_t (__cdecl *)(void *, const std::uint64_t *, std::uint32_t, std::uint32_t, const char *);
    using CallResult = bool (__cdecl *)(std::int32_t, std::uint64_t, void *, std::int32_t, std::int32_t, bool *);
    using RefreshLibrary = void (__cdecl *)(void *);
    if (backend == GameBackend::DosBox && app != xcomDosAppId) {
        co_return winrt::hstring(L"steam_launch=unsupported-runtime\r\n");
    }
    const bool hostedSdl = backend == GameBackend::SdlHost;
    const bool hostedStardew = hostedSdl && app == stardewValleyAppId;
    const bool hostedSlay = hostedSdl && app == slayTheSpireAppId;
    if (hostedSdl && app != dwarfFortressAppId && !hostedStardew && !hostedSlay) {
        co_return winrt::hstring(L"steam_launch=unsupported-runtime\r\n");
    }
    if (steamLaunchActive.test_and_set()) co_return winrt::hstring(L"steam_launch=busy\r\n");
    struct ResetLaunch {
        ~ResetLaunch() { steamLaunchActive.clear(); }
    } resetLaunch;
    std::wstring report;
    auto checkpoint = [&](std::wstring_view phase) -> winrt::Windows::Foundation::IAsyncAction {
        auto text = std::wstring(prefix) + report + L"steam_launch_phase=" + std::wstring(phase) +
            L"\r\nstatus=steam-game-launch-starting\r\n";
        try {
            co_await FileIO::WriteTextAsync(file, winrt::hstring(text));
        } catch (winrt::hresult_error const&) {
            // A diagnostic write must not interrupt Steam's launch or callback processing.
        }
    };
    co_await checkpoint(L"checking-library");
    auto root = ApplicationData::Current().LocalFolder();
    auto item = co_await root.TryGetItemAsync(L"SteamLibrary");
    auto folder = item.try_as<StorageFolder>();
    if (!folder) co_return winrt::hstring(L"steam_launch=no-local-library\r\n");
    auto path = folder.Path();
    if (hostedSlay) {
        try {
            const auto cloudPath = SlayCloudChild(SlayCloudChild(root.Path(), L"SteamCloud"), L"646570");
            const auto slayPath = SlayCloudChild(path, L"steamapps\\common\\SlayTheSpire");
            RollBackSlayCloud(cloudPath, slayPath);
        } catch (winrt::hresult_error const&) {
            co_return winrt::hstring(L"steam_launch=save-recovery-error\r\n");
        }
    }

    const std::uint64_t gameId = app;  // Regular Steam AppID in CGameID's 24-bit app field.
    if (!app || app > 0x00ffffff) co_return winrt::hstring(L"steam_launch=invalid-game\r\n");
    std::uint32_t launchOption = 0;
    bool snapshotRequested = false;
    bool waitOnly = false;
    bool dummyVideo = false;
    bool sdlWorkingDirectory = false;
    bool nativeSdlView = false;
    bool sdlLateFrame = false;
    std::wstring sdlPreferencesPath;
    std::wstring diagnosticRenderer;
    if (backend != GameBackend::DosBox) {
        auto packageFolder = winrt::Windows::ApplicationModel::Package::Current().InstalledLocation();
        if (hostedSdl && !co_await packageFolder.TryGetItemAsync(winrt::hstring(
                L"steam-sdl-host-" + std::to_wstring(app) + L".txt"))) {
            co_return winrt::hstring(L"steam_launch=unsupported-runtime\r\n");
        }
        if (hostedSdl) {
            nativeSdlView = static_cast<bool>(co_await packageFolder.TryGetItemAsync(L"Fhd.Native.winmd"));
        }
        snapshotRequested = static_cast<bool>(co_await packageFolder.TryGetItemAsync(winrt::hstring(
            L"steam-startup-snapshot-" + std::to_wstring(app) + L".txt")));
        if (snapshotRequested) {
            waitOnly = static_cast<bool>(co_await packageFolder.TryGetItemAsync(winrt::hstring(
                L"steam-startup-wait-only-" + std::to_wstring(app) + L".txt")));
            dummyVideo = static_cast<bool>(co_await packageFolder.TryGetItemAsync(winrt::hstring(
                L"steam-startup-sdl-dummy-video-" + std::to_wstring(app) + L".txt")));
            sdlWorkingDirectory = static_cast<bool>(co_await packageFolder.TryGetItemAsync(winrt::hstring(
                L"steam-startup-sdl-working-directory-" + std::to_wstring(app) + L".txt")));
            sdlLateFrame = static_cast<bool>(co_await packageFolder.TryGetItemAsync(winrt::hstring(
                L"steam-startup-sdl-late-frame-" + std::to_wstring(app) + L".txt")));
            if (co_await packageFolder.TryGetItemAsync(winrt::hstring(
                    L"steam-startup-sdl-local-preferences-" + std::to_wstring(app) + L".txt"))) {
                try {
                    auto preferences = co_await root.CreateFolderAsync(L"SDLPreferences", CreationCollisionOption::OpenIfExists);
                    auto titlePreferences = co_await preferences.CreateFolderAsync(winrt::hstring(std::to_wstring(app)),
                        CreationCollisionOption::OpenIfExists);
                    sdlPreferencesPath = titlePreferences.Path().c_str();
                } catch (winrt::hresult_error const& error) {
                    report += L"steam_launch_sdl_preferences_error=" + Hex(error.code()) + L"\r\n";
                }
            }
            auto renderer = (co_await packageFolder.TryGetItemAsync(winrt::hstring(
                L"steam-startup-sdl-renderer-" + std::to_wstring(app) + L".txt"))).try_as<StorageFile>();
            if (renderer) {
                const auto choice = co_await FileIO::ReadTextAsync(renderer);
                if (choice != L"software" && choice != L"direct3d11") {
                    co_return winrt::hstring(L"steam_launch=invalid-sdl-renderer\r\n");
                }
                diagnosticRenderer = choice.c_str();
            }
        }
        if (hostedSdl && app == dwarfFortressAppId && (!snapshotRequested || !dummyVideo || diagnosticRenderer != L"software" ||
                          !sdlWorkingDirectory || sdlPreferencesPath.empty())) {
            co_return winrt::hstring(report + L"steam_sdl_host=profile-error\r\n");
        }
        if (hostedStardew && (!co_await packageFolder.TryGetItemAsync(L"StardewStartupDiagnostic.dll") ||
                              !co_await packageFolder.TryGetItemAsync(L"stardew-sdl-surfaceless.txt") ||
                              !co_await packageFolder.TryGetItemAsync(L"dxil.dll"))) {
            co_return winrt::hstring(report + L"steam_sdl_host=profile-error\r\n");
        }
        if (hostedSlay && (!co_await packageFolder.TryGetItemAsync(L"SlayConsoleDiagnostic.class") ||
                           !co_await packageFolder.TryGetItemAsync(L"slay-live-host.txt") ||
                           !co_await packageFolder.TryGetItemAsync(L"dxil.dll"))) {
            co_return winrt::hstring(report + L"steam_sdl_host=profile-error\r\n");
        }
        auto optionItem = co_await packageFolder.TryGetItemAsync(winrt::hstring(
            L"steam-launch-option-" + std::to_wstring(app) + L".txt"));
        if (optionItem) {
            auto optionFile = optionItem.try_as<StorageFile>();
            if (!optionFile) co_return winrt::hstring(L"steam_launch=invalid-launch-option\r\n");
            auto value = winrt::to_string(co_await FileIO::ReadTextAsync(optionFile));
            auto end = value.find_last_not_of("\r\n");
            value.resize(end == std::string::npos ? 0 : end + 1);
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), launchOption);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
                co_return winrt::hstring(L"steam_launch=invalid-launch-option\r\n");
            }
        }
        report += L"steam_launch_option_index=" + std::to_wstring(launchOption) + L"\r\n";
    }
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

    report += L"steam_launch_library_refreshed=yes\r\n";
    co_await checkpoint(L"checking-installed-state");
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

    constexpr std::uint32_t commonRedistributables = 228980;
    constexpr std::uint32_t updatePaused = 0x200;
    const std::wstring libraryRoot(path);
    const std::wstring dataRoot(root.Path());
    const std::wstring packageRoot(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path());
    bool waitForSharedInstall = false;
    co_await checkpoint(L"checking-prerequisites");
    {
        std::lock_guard lock(steamSessionMutex);
        auto session = liveSteamSession;
        if (!session || !session->OwnsSession()) co_return winrt::hstring(report + L"steam_launch=session-lost\r\n");
        auto manager = session->appManager;
        auto methods = SteamMethods(manager);
        const auto base = reinterpret_cast<std::uintptr_t>(session->module);
        if (methods[18] != base + 0x77fe20 || methods[56] != base + 0x766730 ||
                methods[82] != base + 0x781040) {
            co_return winrt::hstring(report + L"steam_launch=interface-error\r\n");
        }
        auto directory = SteamPath(reinterpret_cast<SteamAppPath>(methods[5]), manager, app);
        if (!IsLocalSteamInstall(directory, std::wstring(path))) {
            co_return winrt::hstring(report + L"steam_launch=install-path-error\r\n");
        }

        using GetDependencies = std::uint32_t (__cdecl *)(void *, std::uint32_t, std::uint32_t *, std::uint32_t);
        std::array<std::uint32_t, 128> dependencies{};
        auto count = reinterpret_cast<GetDependencies>(methods[18])(
            manager, app, dependencies.data(), static_cast<std::uint32_t>(dependencies.size()));
        report += L"steam_launch_dependency_count=" + std::to_wstring(count) + L"\r\n";
        if (count > dependencies.size()) co_return winrt::hstring(report + L"steam_launch=dependency-list-truncated\r\n");
        auto end = dependencies.begin() + count;
        if (std::find(dependencies.begin(), end, commonRedistributables) != end) {
            report += L"steam_launch_common_redistributables=required\r\n";
            report += L"steam_launch_redistributables_requested_index=" + std::to_wstring(libraryIndex) + L"\r\n";
            auto state = reinterpret_cast<SteamAppValue>(methods[4])(manager, commonRedistributables);
            report += L"steam_launch_redistributables_initial_state=" + std::to_wstring(state) + L"\r\n";
            report += DescribeSteamDestination(
                manager, methods, commonRedistributables, L"initial", libraryRoot, dataRoot, packageRoot);
            if ((state & installMask) != fullyInstalled) {
                // Let Steam obtain the shared dependency of the game it just verified the account owns.
                auto install = reinterpret_cast<SteamInstall>(methods[0]);
                auto code = install(manager, commonRedistributables, libraryIndex, false);
                report += L"steam_launch_redistributables_install_result=" + std::to_wstring(code) + L"\r\n";
                report += DescribeSteamDestination(
                    manager, methods, commonRedistributables, L"queued", libraryRoot, dataRoot, packageRoot);
                if (code != 0) co_return winrt::hstring(report + L"steam_launch=dependency-install-rejected\r\n");
                waitForSharedInstall = true;
            }
        }
    }

    if (waitForSharedInstall) {
        co_await checkpoint(L"waiting-for-prerequisites");
        const auto started = GetTickCount64();
        auto deadline = started + 90000;
        bool retriedPaused = false;
        for (;;) {
            {
                std::lock_guard lock(steamSessionMutex);
                auto session = liveSteamSession;
                if (!session || !session->OwnsSession()) {
                    co_return winrt::hstring(report + L"steam_launch=session-lost\r\n");
                }
                session->PumpCallbacks();
                auto manager = session->appManager;
                auto methods = SteamMethods(manager);
                auto state = reinterpret_cast<SteamAppValue>(methods[4])(manager, commonRedistributables);
                const auto now = GetTickCount64();
                if (!retriedPaused && now - started >= 3000 && (state & updatePaused)) {
                    auto accepted = RequestSteamDownloadNow(manager, methods, commonRedistributables);
                    retriedPaused = true;
                    report += std::wstring(L"steam_launch_redistributables_retry=") +
                        (accepted ? L"accepted\r\n" : L"declined\r\n");
                }
                if ((state & installMask) == fullyInstalled) {
                    report += L"steam_launch_redistributables_final_state=" + std::to_wstring(state) + L"\r\n";
                    report += DescribeSteamDestination(
                        manager, methods, commonRedistributables, L"final", libraryRoot, dataRoot, packageRoot);
                    break;
                }
                if (now >= deadline) {
                    report += L"steam_launch_redistributables_final_state=" + std::to_wstring(state) + L"\r\n";
                    report += DescribeSteamDestination(
                        manager, methods, commonRedistributables, L"final", libraryRoot, dataRoot, packageRoot);
                    co_return winrt::hstring(report + L"steam_launch=dependency-install-incomplete\r\n");
                }
            }
            co_await winrt::resume_after(std::chrono::milliseconds(500));
            co_await caller;
        }
    }

    FILETIME launchTime{};
    std::wstring gameDirectory;
    bool traceStardew = false;
    bool diagnoseManagedStardew = false;
    if (app == 413150) {
        auto package = winrt::Windows::ApplicationModel::Package::Current().InstalledLocation();
        traceStardew = static_cast<bool>(co_await package.TryGetItemAsync(L"stardew-host-trace.txt"));
        if (traceStardew && !hostedStardew) {
            diagnoseManagedStardew = static_cast<bool>(co_await package.TryGetItemAsync(L"StardewStartupDiagnostic.dll"));
        }
    }
    struct SdlTrialEnvironment {
        wchar_t const* name;
        bool created = false;
        bool Missing() const {
            wchar_t existing[1];
            SetLastError(ERROR_SUCCESS);
            return !GetEnvironmentVariableW(name, existing, 1) && GetLastError() == ERROR_ENVVAR_NOT_FOUND;
        }

        bool Set(wchar_t const* value) {
            if (!Missing()) return false;
            created = SetEnvironmentVariableW(name, value) != FALSE;
            return created;
        }
        ~SdlTrialEnvironment() {
            if (created) SetEnvironmentVariableW(name, nullptr);
        }
    } traceFile{L"COREHOST_TRACEFILE"}, traceEnabled{L"COREHOST_TRACE"},
      rendererHint{L"SDL_RENDER_DRIVER"}, videoHint{L"SDL_VIDEODRIVER"}, sdlTrace{L"XBOX_SDL_TRACE_FILE"},
      gameDirectoryHint{L"XBOX_SDL_WORKING_DIRECTORY"}, preferencesHint{L"XBOX_SDL_PREFERENCES_ROOT"},
      frameDelayHint{L"XBOX_SDL_SOFTWARE_FRAME_DELAY_MS"}, sdlHostHint{L"XBOX_SDL_HOST_MAILBOX"},
      sdlGamepadHint{L"XBOX_SDL_HOST_GAMEPAD"},
      renderWidth{L"XBOX_SDL_RENDER_WIDTH"}, renderHeight{L"XBOX_SDL_RENDER_HEIGHT"},
      managedHook{L"DOTNET_STARTUP_HOOKS"}, managedLog{L"XBOX_STEAM_MANAGED_LOG"}, managedHeap{L"DOTNET_GCHeapHardLimit"};
    std::shared_ptr<SteamSdlSurface> pendingSdlSurface;
    winrt::handle slayProcess;
    std::wstring slayTempDirectory;
    if (hostedSlay) {
        auto temp = co_await root.CreateFolderAsync(L"slay-native-temp", CreationCollisionOption::OpenIfExists);
        slayTempDirectory = temp.Path().c_str();
    }
    if (snapshotRequested) {
        co_await winrt::resume_background();
        report += L"steam_launch_probe_pid=" + std::to_wstring(GetCurrentProcessId()) + L"\r\n";
        report += DescribeProcessWait(GetCurrentProcess(), 50, L"steam_launch_snapshot_control");
        co_await checkpoint(L"tested-process-wait");
        co_await caller;
    }
    co_await checkpoint(backend == GameBackend::DosBox ? L"checking-dos-runtime" : L"submitting-play");
    if ((backend == GameBackend::DosBox || hostedStardew || hostedSlay) && !co_await HasCompletedSteamManifest(folder, app)) {
        co_return winrt::hstring(report + L"steam_launch=not-installed\r\n");
    }
    {
        std::lock_guard lock(steamSessionMutex);
        auto session = liveSteamSession;
        if (!session || !session->OwnsSession()) co_return winrt::hstring(report + L"steam_launch=session-lost\r\n");
        if (session->downloadApp || session->launchCall) co_return winrt::hstring(report + L"steam_launch=busy\r\n");
        const auto base = reinterpret_cast<std::uintptr_t>(session->module);
        auto methods = FindSteamAppManager(*session);
        if (!methods || session->userMethods[182] != base + 0x75d640 ||
                session->userMethods[67] != base + 0x75a320) {
            co_return winrt::hstring(report + L"steam_launch=interface-error\r\n");
        }
        if (std::find(session->games.begin(), session->games.end(), app) == session->games.end() ||
                !reinterpret_cast<IsSubscribed>(session->userMethods[182])(session->clientUser, app)) {
            co_return winrt::hstring(report + L"steam_launch=no-license\r\n");
        }
        auto manager = session->appManager;
        auto state = reinterpret_cast<SteamAppValue>(methods[4])(manager, app);
        if ((state & installMask) != fullyInstalled) {
            co_return winrt::hstring(report + L"steam_launch=not-installed\r\nsteam_launch_install_state=" +
                std::to_wstring(state) + L"\r\n");
        }
        gameDirectory = SteamPath(reinterpret_cast<SteamAppPath>(methods[5]), manager, app);
        if (!IsLocalSteamInstall(gameDirectory, libraryRoot)) {
            co_return winrt::hstring(report + L"steam_launch_game_install_scope=" +
                SteamPathScope(gameDirectory, libraryRoot, dataRoot, packageRoot) + L"\r\nsteam_launch=install-path-error\r\n");
        }
        if (hostedStardew && CompareStringOrdinal(gameDirectory.c_str(), -1,
                (libraryRoot + L"\\steamapps\\common\\Stardew Valley").c_str(), -1, TRUE) != CSTR_EQUAL) {
            co_return winrt::hstring(report + L"steam_launch=install-path-error\r\n");
        }
        if (hostedSlay && CompareStringOrdinal(gameDirectory.c_str(), -1,
                (libraryRoot + L"\\steamapps\\common\\SlayTheSpire").c_str(), -1, TRUE) != CSTR_EQUAL) {
            co_return winrt::hstring(report + L"steam_launch=install-path-error\r\n");
        }
        if (reinterpret_cast<GameQuery>(session->userMethods[67])(session->clientUser, &gameId)) {
            co_return winrt::hstring(report + L"steam_launch=already-running\r\n");
        }
        session->PumpCallbacks();
        if (backend == GameBackend::DosBox) {
            const auto expected = libraryRoot + L"\\steamapps\\common\\XCom UFO Defense";
            if (CompareStringOrdinal(gameDirectory.c_str(), -1, expected.c_str(), -1, TRUE) != CSTR_EQUAL) {
                co_return winrt::hstring(report + L"steam_launch=install-path-error\r\n");
            }
            co_return winrt::hstring(report + L"steam_dos_install=verified\r\n");
        }
        result = reinterpret_cast<CallResult>(GetProcAddress(session->module, "Steam_GetAPICallResult"));
        if (methods[2] != base + 0x874f20 || session->userMethods[68] != base + 0x75a4d0 ||
                session->userMethods[132] != base + 0x8794c0 || session->userMethods[133] != base + 0x7b3e30 ||
                session->userMethods[134] != base + 0x7b3fc0 ||
                reinterpret_cast<std::uintptr_t>(result) != base + 0x9abb80) {
            co_return winrt::hstring(report + L"steam_launch=interface-error\r\n");
        }
        GetSystemTimeAsFileTime(&launchTime);
        if (traceStardew) {
            const auto id = std::to_wstring((static_cast<std::uint64_t>(launchTime.dwHighDateTime) << 32) |
                launchTime.dwLowDateTime);
            const auto tracePath = dataRoot + L"\\stardew-host-" + id + L".log";
            if (traceFile.Missing() && traceEnabled.Missing() && traceFile.Set(tracePath.c_str()) && traceEnabled.Set(L"1")) {
                report += L"steam_stardew_host_run=" + id + L"\r\n";
            } else {
                report += L"steam_stardew_host_trace=environment-unavailable\r\n";
            }
            if (diagnoseManagedStardew) {
                const auto logPath = dataRoot + L"\\stardew-managed-" + id + L".log";
                const auto hookPath = packageRoot + L"\\StardewStartupDiagnostic.dll";
                if (managedLog.Missing() && managedHook.Missing() && managedLog.Set(logPath.c_str()) &&
                        managedHook.Set(hookPath.c_str())) {
                    report += L"steam_stardew_managed_run=" + id + L"\r\n";
                } else {
                    report += L"steam_stardew_managed=environment-unavailable\r\n";
                }
            }
        }
        if (hostedStardew) {
            const auto id = std::to_wstring((static_cast<std::uint64_t>(launchTime.dwHighDateTime) << 32) |
                launchTime.dwLowDateTime);
            if (!managedLog.Set((dataRoot + L"\\stardew-managed-" + id + L".log").c_str()) ||
                    !managedHook.Set((packageRoot + L"\\StardewStartupDiagnostic.dll").c_str()) ||
                    !managedHeap.Set(L"10000000")) {
                co_return winrt::hstring(report + L"steam_sdl_host=environment-error\r\n");
            }
            report += L"steam_stardew_managed_run=" + id + L"\r\n";
        }
        if (snapshotRequested) {
            const auto id = std::to_wstring((static_cast<std::uint64_t>(launchTime.dwHighDateTime) << 32) |
                launchTime.dwLowDateTime);
            const auto file = dataRoot + L"\\sdl-startup-" + id + L".txt";
            if (sdlTrace.Set(file.c_str())) report += L"steam_launch_sdl_run=" + id + L"\r\n";
        }
        if (!diagnosticRenderer.empty()) {
            const auto applied = rendererHint.Set(diagnosticRenderer.c_str());
            report += L"steam_launch_sdl_renderer=" + (applied ? diagnosticRenderer : std::wstring(L"unavailable")) + L"\r\n";
        }
        if (dummyVideo) {
            report += videoHint.Set(L"dummy") ? L"steam_launch_sdl_video=dummy\r\n" : L"steam_launch_sdl_video=unavailable\r\n";
        }
        if (sdlWorkingDirectory) {
            report += gameDirectoryHint.Set(gameDirectory.c_str()) ? L"steam_launch_sdl_cwd=requested\r\n" :
                L"steam_launch_sdl_cwd=unavailable\r\n";
        }
        if (!sdlPreferencesPath.empty()) {
            report += preferencesHint.Set(sdlPreferencesPath.c_str()) ? L"steam_launch_sdl_preferences=requested\r\n" :
                L"steam_launch_sdl_preferences=unavailable\r\n";
        }
        if (sdlLateFrame && frameDelayHint.Set(L"12000")) {
            report += L"steam_launch_sdl_frame_delay_ms=12000\r\n";
        }
        if (hostedSdl) {
            const auto mailbox = dataRoot + L"\\sdl-host-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L".bin";
            try {
                pendingSdlSurface = std::make_shared<SteamSdlSurface>();
                pendingSdlSurface->Open(mailbox);
            } catch (winrt::hresult_error const& error) {
                co_return winrt::hstring(report + L"steam_sdl_host=mailbox-error\r\nsteam_sdl_host_hresult=" +
                    Hex(error.code()) + L"\r\n");
            }
            if (nativeSdlView) {
                if (!renderWidth.Set(L"1920") || !renderHeight.Set(L"1080")) {
                    co_return winrt::hstring(report + L"steam_sdl_host=display-environment-error\r\n");
                }
                const auto separator = mailbox.find_last_of(L"\\");
                report += L"steam_sdl_mailbox=" + mailbox.substr(separator + 1) + L"\r\n";
            }
            if (!sdlHostHint.Set(mailbox.c_str()) || ((hostedStardew || hostedSlay) && !sdlGamepadHint.Set(L"1"))) {
                co_return winrt::hstring(report + L"steam_sdl_host=environment-error\r\n");
            }
        }
        if (hostedSlay) {
            const auto id = std::to_wstring((static_cast<std::uint64_t>(launchTime.dwHighDateTime) << 32) |
                launchTime.dwLowDateTime);
            const auto java = gameDirectory + L"\\jre\\bin\\java.exe";
            const auto jar = gameDirectory + L"\\desktop-1.0.jar";
            const auto cwd = packageRoot + L"\\slay-working-directory.dll";
            const auto log = dataRoot + L"\\slay-console-" + id + L".log";
            auto command = L"\"" + java + L"\" -Xms32m -Xmx256m -XX:+UseSerialGC -XX:TieredStopAtLevel=1 "
                L"-XX:ReservedCodeCacheSize=48m -Duser.dir=\"" + gameDirectory + L"\" -Djava.io.tmpdir=\"" +
                slayTempDirectory + L"\" -cp \"" + packageRoot + L";" + packageRoot + L"\\slay-lwjgl-display.jar;" +
                jar + L"\" SlayConsoleDiagnostic --launch-hosted \"" + gameDirectory + L"\" \"" + cwd + L"\" \"" +
                jar + L"\" \"" + log + L"\"";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION child{};
            if (!CreateProcessW(java.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                nullptr, gameDirectory.c_str(), &startup, &child)) {
                co_return winrt::hstring(report + L"steam_slay_console=create-error\r\nsteam_slay_console_error=" +
                    Hex(GetLastError()) + L"\r\n");
            }
            slayProcess.attach(child.hProcess);
            CloseHandle(child.hThread);
            report += L"steam_slay_console_run=" + id + L"\r\n";
        } else {
            call = reinterpret_cast<Launch>(methods[2])(manager, &gameId, launchOption, 0, "");
            if (!call) co_return winrt::hstring(report + L"steam_launch=request-rejected\r\n");
            session->launchCall = call;
        }
    }

    if (hostedSlay) {
        co_await checkpoint(L"waiting-for-sdl-host");
        const auto deadline = GetTickCount64() + 35000;
        while (WaitForSingleObject(slayProcess.get(), 0) == WAIT_TIMEOUT && GetTickCount64() < deadline) {
            if (pendingSdlSurface->HasFrame()) {
                pendingSdlSurface->Attach(std::move(slayProcess));
                std::lock_guard lock(steamSdlMutex);
                steamSdlSurface = std::move(pendingSdlSurface);
                co_return winrt::hstring(report + L"steam_sdl_host=ready\r\n");
            }
            co_await winrt::resume_after(std::chrono::milliseconds(100));
            co_await caller;
        }
        DWORD exitCode = 0;
        if (GetExitCodeProcess(slayProcess.get(), &exitCode) && exitCode != STILL_ACTIVE) {
            co_return winrt::hstring(report + L"steam_sdl_host=exited\r\nsteam_launch_process_exit=" + Hex(exitCode) + L"\r\n");
        }
        pendingSdlSurface->Quit();
        co_await winrt::resume_background();
        if (WaitForSingleObject(slayProcess.get(), 2000) == WAIT_TIMEOUT) TerminateProcess(slayProcess.get(), ERROR_TIMEOUT);
        co_return winrt::hstring(report + L"steam_sdl_host=timeout\r\n");
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
    co_await checkpoint(L"waiting-for-steam");
    const auto started = GetTickCount64();
    const auto deadline = started + (hostedSdl ? 40000 : 25000);
    using winrt::Windows::System::MemoryManager;
    std::uint64_t memoryLimit = 0, peakMemory = 0;
    try {
        memoryLimit = MemoryManager::AppMemoryUsageLimit();
    } catch (winrt::hresult_error const&) {}
    SteamGameProcess process;
    bool accepted = false;
    bool observedRunning = false;
    bool recordedRunning = false;
    bool snapshotTaken = false;
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
                                if (code == 17) report += L"steam_launch_client_reason=dependency-not-ready\r\n";
                                if (code == 29) report += L"steam_launch_client_reason=invalid-platform\r\n";
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
            if (running) process.Observe(user, session->userMethods, gameId, launchTime);
            if (memoryLimit) {
                try {
                    peakMemory = (std::max)(peakMemory, MemoryManager::AppMemoryUsage());
                } catch (winrt::hresult_error const&) {
                    memoryLimit = 0;
                }
            }
            if (pendingSdlSurface && accepted && process.handle && pendingSdlSurface->HasFrame() &&
                    WaitForSingleObject(process.handle.get(), 0) == WAIT_TIMEOUT) {
                report += process.Report();
                pendingSdlSurface->Attach(std::move(process.handle));
                {
                    std::lock_guard surfaceLock(steamSdlMutex);
                    steamSdlSurface = std::move(pendingSdlSurface);
                }
                session->launchCall = 0;
                co_return winrt::hstring(report + L"steam_sdl_host=ready\r\n");
            }
            const bool capturePending = !hostedSdl && snapshotRequested && !snapshotTaken && process.handle;
            const bool queryWindow = !hostedSdl && (!snapshotRequested || (accepted && running &&
                (snapshotTaken || (!capturePending && GetTickCount64() >= deadline))));
            auto windowReady = queryWindow && reinterpret_cast<GameQuery>(session->userMethods[68])(user, &gameId);
            if (accepted && windowReady && running) verdict = L"window-ready";
            if ((responded && !accepted) || (accepted && windowReady && running) ||
                    (!capturePending && GetTickCount64() >= deadline)) {
                if (accepted && observedRunning && verdict == std::wstring_view(L"start-unconfirmed")) {
                    verdict = running ? L"running-window-unconfirmed" : L"exited-during-startup";
                }
                report += std::wstring(L"steam_launch_running_seen=") + (observedRunning ? L"yes\r\n" : L"no\r\n");
                report += process.Report();
                if (memoryLimit) {
                    report += L"steam_launch_app_memory_peak=" + std::to_wstring(peakMemory) + L"\r\n";
                    report += L"steam_launch_app_memory_limit=" + std::to_wstring(memoryLimit) + L"\r\n";
                }
                session->launchCall = 0;
                break;
            }
        }
        if (!hostedSdl && snapshotRequested && !snapshotTaken && process.handle) {
            // Write the identity and immediate OS result before waiting or making another Steam call.
            report += L"steam_launch_snapshot_pid=" + std::to_wstring(process.processId) + L"\r\n";
            report += L"steam_launch_snapshot_created_filetime=" + std::to_wstring(process.createdAt) + L"\r\n";
            report += L"steam_launch_snapshot_executable=" +
                (process.basename.empty() ? std::wstring(L"unavailable") : process.basename) + L"\r\n";
            report += DescribeProcessWait(process.handle.get(), 0, L"steam_launch_snapshot_initial");
            co_await checkpoint(L"captured-process");
            co_await winrt::resume_background();
            if (waitOnly) {
                for (int sample = 1; sample <= 12; ++sample) {
                    const auto field = L"steam_launch_wait_sample_" + std::to_wstring(sample);
                    report += DescribeProcessWait(process.handle.get(), 500, field.c_str());
                    try {
                        report += field + L"_memory_usage=" + std::to_wstring(MemoryManager::AppMemoryUsage()) + L"\r\n";
                        report += field + L"_memory_limit=" + std::to_wstring(MemoryManager::AppMemoryUsageLimit()) + L"\r\n";
                    } catch (winrt::hresult_error const&) {}
                    co_await checkpoint(L"waiting-on-process-handle");
                    if (WaitForSingleObject(process.handle.get(), 0) == WAIT_OBJECT_0) break;
                }
                report += process.Report();
                report += L"steam_launch=process-wait-complete\r\n";
                co_await caller;
                {
                    std::lock_guard lock(steamSessionMutex);
                    if (liveSteamSession && liveSteamSession->launchCall == call) liveSteamSession->launchCall = 0;
                }
                co_return winrt::hstring(report);
            }
            report += DescribeProcessWait(process.handle.get(), 1000, L"steam_launch_snapshot_followup");
            co_await checkpoint(L"sampled-process");
            co_await caller;
            snapshotTaken = true;
            recordedRunning = true;
        } else if (observedRunning && !recordedRunning) {
            recordedRunning = true;
            co_await checkpoint(L"game-running");
        }
        co_await winrt::resume_after(std::chrono::milliseconds(GetTickCount64() - started < 3000 ? 50 : 250));
        co_await caller;
    }
    report += L"steam_launch=" + std::wstring(verdict) + L"\r\n";
    if (app == 646570 && verdict == std::wstring_view(L"exited-during-startup")) {
        try {
            auto package = winrt::Windows::ApplicationModel::Package::Current().InstalledLocation();
            DWORD code = 0;
            if (accepted && process.handle && WaitForSingleObject(process.handle.get(), 0) == WAIT_OBJECT_0 &&
                    GetExitCodeProcess(process.handle.get(), &code) && code == 1 &&
                    co_await package.TryGetItemAsync(L"SlayConsoleDiagnostic.class")) {
                const auto id = std::to_wstring((static_cast<std::uint64_t>(launchTime.dwHighDateTime) << 32) |
                    launchTime.dwLowDateTime);
                report += L"steam_slay_console_run=" + id + L"\r\n";
                co_await checkpoint(L"collecting-slay-console");
                report += (co_await RunSlayConsoleDiagnostic(folder, packageRoot,
                    dataRoot + L"\\slay-console-" + id + L".log")).c_str();
            }
            if (co_await package.TryGetItemAsync(L"steam-java-probe.txt")) {
                const auto javaReport = co_await ProbeJavaVersion(gameDirectory);
                report += javaReport.c_str();
            }
        } catch (winrt::hresult_error const& error) {
            report += L"steam_java_probe=runtime-error\r\nsteam_java_probe_error=" + Hex(error.code()) + L"\r\n";
        }
    }
    co_return winrt::hstring(report);
}

struct ProbeFactory : winrt::implements<ProbeFactory, winrt::Windows::Foundation::IActivationFactory,
                                        winrt::XboxProbe::Native::IProbeRunnerStatics> {
    winrt::Windows::Foundation::IInspectable ActivateInstance() {
        throw winrt::hresult_not_implemented();
    }

    winrt::hstring Run() {
        auto path = InstalledPath();
        return winrt::hstring(ChildResult(path + L"child.exe") + PipeResult(path + L"pipe-child.exe"));
    }

    winrt::hstring StartLifetime() {
        return winrt::hstring(StartLifetimePipe(InstalledPath() + L"pipe-child.exe"));
    }

    winrt::hstring FinishLifetime() {
        return winrt::hstring(FinishLifetimePipe());
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
            winrt::hstring const& account, winrt::hstring const& approval, winrt::hstring const& accountId,
            bool remember, std::uint32_t expiresAt) {
        return SteamSignIn(file, prefix, account, approval, accountId, remember, expiresAt);
    }

    winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> ResumeSteamSignIn(
            winrt::Windows::Storage::StorageFile const& file, winrt::hstring const& prefix) {
        return ::ResumeSteamSignIn(file, prefix);
    }

    winrt::Windows::Foundation::IAsyncOperation<bool> SignOutSteam() {
        return SteamSignOutAndForget();
    }

    winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> QueueSteamDownload(std::uint32_t app) {
        return StartSteamDownload(app);
    }

    winrt::hstring SteamDownloadStatus() {
        return ReadSteamDownload();
    }

    winrt::hstring SteamStorageInfo() {
        auto path = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
        ULARGE_INTEGER callerAvailable{}, total{}, volumeAvailable{};
        if (!GetDiskFreeSpaceExW(path.c_str(), &callerAvailable, &total, &volumeAvailable)) {
            return winrt::hstring(L"error=" + Hex(GetLastError()) + L"\r\n");
        }
        return winrt::hstring(L"caller_available=" + std::to_wstring(callerAvailable.QuadPart) +
            L"\r\nvolume_size=" + std::to_wstring(total.QuadPart) +
            L"\r\nvolume_available=" + std::to_wstring(volumeAvailable.QuadPart) + L"\r\n");
    }

    winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> AuthorizeXcomDos(
            winrt::Windows::Storage::StorageFile const& file, winrt::hstring const& prefix) {
        return AuthorizeDosGame(xcomDosAppId, file, prefix);
    }

    winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> AuthorizeDosGame(std::uint32_t app,
            winrt::Windows::Storage::StorageFile const& file, winrt::hstring const& prefix) {
        return LaunchLocalSteamGame(app, file, prefix, GameBackend::DosBox);
    }

    winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> LaunchSteamGame(std::uint32_t app,
            winrt::Windows::Storage::StorageFile const& file, winrt::hstring const& prefix) {
        return LaunchLocalSteamGame(app, file, prefix, GameBackend::SteamPlay);
    }

    winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> LaunchSteamSdlGame(std::uint32_t app,
            winrt::Windows::Storage::StorageFile const& file, winrt::hstring const& prefix) {
        return LaunchLocalSteamGame(app, file, prefix, GameBackend::SdlHost);
    }

    winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> RestoreSteamCloud(std::uint32_t app, bool preview) {
        return RestoreSlaySteamCloud(app, preview);
    }

    winrt::hstring SteamSdlRgbDeltaFrame(std::uint32_t previousSequence) {
        std::lock_guard lock(steamSdlMutex);
        return steamSdlSurface ? steamSdlSurface->RgbFrame(previousSequence) : winrt::hstring{};
    }

    winrt::Windows::Storage::Streams::IBuffer SteamSdlFrame() {
        std::lock_guard lock(steamSdlMutex);
        return steamSdlSurface ? steamSdlSurface->Frame() : nullptr;
    }

    std::uint32_t SendSteamSdlInput(std::uint32_t pointer, std::uint32_t keys) {
        std::lock_guard lock(steamSdlMutex);
        return steamSdlSurface ? steamSdlSurface->Input(pointer, keys) : 0;
    }

    std::uint32_t SendSteamSdlGamepad(std::uint32_t buttons, std::int32_t leftX, std::int32_t leftY,
            std::int32_t rightX, std::int32_t rightY, std::int32_t leftTrigger, std::int32_t rightTrigger) {
        std::lock_guard lock(steamSdlMutex);
        return steamSdlSurface ? steamSdlSurface->Gamepad(buttons, {leftX, leftY, rightX, rightY, leftTrigger, rightTrigger}) : 0;
    }

    std::uint32_t SteamSdlGamepadAck() {
        std::lock_guard lock(steamSdlMutex);
        return steamSdlSurface ? steamSdlSurface->GamepadAck() : 0;
    }

    bool SteamSdlKeyboardReady() {
        std::lock_guard lock(steamSdlMutex);
        return steamSdlSurface && steamSdlSurface->KeyboardReady();
    }

    bool SendSteamSdlKey(std::uint32_t scan, bool down) {
        std::lock_guard lock(steamSdlMutex);
        return steamSdlSurface && steamSdlSurface->Key(scan, down);
    }

    bool SendSteamSdlText(winrt::hstring const& text) {
        std::lock_guard lock(steamSdlMutex);
        return steamSdlSurface && steamSdlSurface->Text(text);
    }

    void ResetSteamSdlKeyboard() {
        std::lock_guard lock(steamSdlMutex);
        if (steamSdlSurface) steamSdlSurface->ResetKeyboard();
    }

    bool SteamSdlRunning() {
        std::lock_guard lock(steamSdlMutex);
        if (steamSdlSurface && !steamSdlSurface->Alive()) steamSdlSurface.reset();
        return steamSdlSurface != nullptr;
    }

    void QuitSteamSdl() {
        std::lock_guard lock(steamSdlMutex);
        if (steamSdlSurface) steamSdlSurface->Quit();
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
    std::lock_guard lock(lifetimeMutex);
    return winrt::get_module_lock() || lifetimeSession.pipe ? S_FALSE : S_OK;
}
