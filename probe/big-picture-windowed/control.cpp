#include <windows.h>
#include <cstdio>
#include <atomic>
#include <winrt/WindowedControl.Native.h>
#include <winrt/Microsoft.Web.WebView2.Core.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation;
using namespace Windows::UI::Core;
using namespace Microsoft::Web::WebView2::Core;

namespace {
CoreWebView2Controller controller{nullptr};
std::atomic<bool> started{};
bool loaded = false, timer = false, clicked = false;

void record(char const* phase, HRESULT hr = S_OK, bool first = false) noexcept
{
    try {
        auto path = Windows::Storage::ApplicationData::Current().LocalFolder().Path() + L"\\windowed-control.txt";
        handle file{CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, first ? CREATE_ALWAYS : OPEN_ALWAYS, nullptr)};
        LARGE_INTEGER offset{};
        if (!file || !SetFilePointerEx(file.get(), offset, nullptr, FILE_END)) return;
        char line[180];
        int count = std::snprintf(line, sizeof(line), "pid=%lu stage=%s hresult=0x%08lx\r\n",
                                  GetCurrentProcessId(), phase, static_cast<unsigned long>(hr));
        DWORD written;
        if (count > 0 && count < sizeof(line)) WriteFile(file.get(), line, count, &written, nullptr);
    } catch (...) {}
}

constexpr wchar_t page[] = LR"HTML(<!doctype html><html lang="en"><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; form-action 'none'">
<title>Windowed Chromium control</title><style>body{font:28px system-ui;background:#122a3d;color:white;padding:4vw}button{font:inherit}</style>
<h1>Windowed Chromium control</h1><p id="state">Page opened.</p>
<button autofocus id="button">Test input</button><output id="result"></output>
<script>
const send=value=>chrome.webview.postMessage(value);
send("loaded");
setTimeout(()=>{document.getElementById("state").textContent="The document timer ran.";send("timer")},1000);
document.getElementById("button").onclick=e=>{
 if(!e.isTrusted)return;
 document.getElementById("result").textContent="Input received.";send("clicked");
};
</script></html>)HTML";

fire_and_forget openOnMain()
{
    char const* phase = "main-corewindow";
    try {
        record("on-main");
        auto main = CoreApplication::MainView();
        if (!main.Dispatcher().HasThreadAccess()) throw hresult_wrong_thread();
        auto window = main.CoreWindow();
        if (!window) throw hresult_error(E_HANDLE);
        phase = "window-reference";
        record(phase);
        auto reference = CoreWebView2ControllerWindowReference::CreateFromCoreWindow(window);
        phase = "environment";
        record(phase);
        auto profile = Windows::Storage::ApplicationData::Current().LocalCacheFolder().Path() + L"\\WindowedControl";
        auto environment = co_await CoreWebView2Environment::CreateWithOptionsAsync(L"", profile, nullptr);
        phase = "windowed-controller";
        record(phase);
        controller = co_await environment.CreateCoreWebView2ControllerAsync(reference);
        phase = "controller-bounds";
        record(phase);
        auto bounds = window.Bounds();
        controller.Bounds({0, 0, bounds.Width, bounds.Height});
        controller.IsVisible(true);
        auto web = controller.CoreWebView2();
        web.NavigationStarting([](auto const&, auto const& args) { args.Cancel(args.Uri() != L"about:blank"); });
        web.NewWindowRequested([](auto const&, auto const& args) { args.Handled(true); });
        web.WebMessageReceived([](auto const&, auto const& args) {
            if (args.Source() != L"about:blank") return;
            auto message = args.WebMessageAsJson();
            if (message == L"\"loaded\"" && !loaded) { loaded = true; record("document-loaded"); }
            if (message == L"\"timer\"" && !timer) { timer = true; record("document-timer"); }
            if (message == L"\"clicked\"" && !clicked) { clicked = true; record("trusted-click"); }
        });
        web.NavigationCompleted([](auto const&, auto const& args) {
            if (!args.IsSuccess()) record("navigation-error", static_cast<HRESULT>(args.WebErrorStatus()));
            else { record("navigation-complete"); try { controller.MoveFocus(CoreWebView2MoveFocusReason::Programmatic); } catch (...) { record("focus-failed", to_hresult()); } }
        });
        phase = "navigate";
        record(phase);
        web.NavigateToString(page);
    } catch (...) { record(phase, to_hresult()); }
}

struct Factory : implements<Factory, IActivationFactory, WindowedControl::Native::IControlStatics>
{
    IInspectable ActivateInstance() { throw hresult_not_implemented(); }
    void Start() {
        if (started.exchange(true)) return;
        record("native-start", S_OK, true);
        try {
            auto dispatcher = CoreApplication::MainView().Dispatcher();
            record("main-dispatcher");
            dispatcher.RunAsync(CoreDispatcherPriority::Normal, [] { openOnMain(); });
        } catch (...) { record("dispatch-error", to_hresult()); }
    }
};
}

extern "C" HRESULT WINAPI GetControlFactory(void* name, void** result) noexcept try {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (*reinterpret_cast<hstring const*>(&name) != L"WindowedControl.Native.Control") return CLASS_E_CLASSNOTAVAILABLE;
    *result = detach_abi(make<Factory>());
    return S_OK;
} catch (...) { return to_hresult(); }

extern "C" HRESULT WINAPI CanUnloadControl() noexcept { return S_FALSE; }
