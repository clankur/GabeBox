#include <windows.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>

#include <chrono>
#include <string>
#include <thread>

#include "child-process.h"

using namespace winrt;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::UI::Core;
using namespace std::chrono_literals;

static std::wstring Hex(DWORD value) {
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring text = L"0x00000000";
    for (unsigned i = 0; i < 8; ++i) {
        text[9 - i] = digits[value & 0xF];
        value >>= 4;
    }
    return text;
}

static std::wstring ChildResult() {
    wchar_t executable[MAX_PATH];
    DWORD chars = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (chars == 0 || chars == MAX_PATH)
        return L"status=path-error\r\nwin32_error=" + Hex(GetLastError()) + L"\r\n";
    std::wstring path(executable, chars);
    auto split = path.find_last_of(L"\\/");
    if (split == std::wstring::npos) return L"status=path-error\r\n";
    path.replace(split + 1, std::wstring::npos, L"child.exe");

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

static IAsyncAction RunDiagnostic() {
    co_await resume_background();
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    std::wstring header = L"probe=packaged-child-process\r\nrun_id=" +
        Hex(now.dwHighDateTime) + Hex(now.dwLowDateTime) + Hex(GetCurrentProcessId()) + L"\r\n";
    auto folder = ApplicationData::Current().LocalFolder();
    auto report = co_await folder.CreateFileAsync(L"probe-report.txt", CreationCollisionOption::ReplaceExisting);
    co_await FileIO::WriteTextAsync(report, hstring(header + L"status=starting\r\n"));
    co_await FileIO::WriteTextAsync(report, hstring(header + ChildResult()));
}

struct ProbeView : implements<ProbeView, IFrameworkViewSource, IFrameworkView> {
    IFrameworkView CreateView() { return *this; }

    void Initialize(CoreApplicationView const& view) {
        m_view = view;
        m_activated = view.Activated([this](CoreApplicationView const&, IActivatedEventArgs const&) {
            m_window.Activate();
            if (!m_diagnostic) m_diagnostic = RunDiagnostic();
        });
    }

    void SetWindow(CoreWindow const& window) { m_window = window; }
    void Load(hstring const&) {}

    void Run() {
        auto dispatcher = m_window.Dispatcher();
        auto deadline = std::chrono::steady_clock::now() + 10s;
        while ((!m_diagnostic || m_diagnostic.Status() == AsyncStatus::Started) &&
               std::chrono::steady_clock::now() < deadline) {
            dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            std::this_thread::sleep_for(20ms);
        }
        if (m_diagnostic && m_diagnostic.Status() != AsyncStatus::Started) m_diagnostic.GetResults();
        CoreApplication::Exit();
    }

    void Uninitialize() {
        if (m_view) m_view.Activated(m_activated);
    }

private:
    CoreApplicationView m_view{nullptr};
    CoreWindow m_window{nullptr};
    IAsyncAction m_diagnostic{nullptr};
    event_token m_activated{};
};

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        CoreApplication::Run(make<ProbeView>());
        return 0;
    } catch (hresult_error const& error) {
        return error.code();
    }
}
