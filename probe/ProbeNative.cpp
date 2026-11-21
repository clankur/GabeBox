#include <windows.h>
#include <winstring.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/XboxProbe.Native.h>
#include <mutex>
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
