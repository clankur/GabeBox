// Independent XAML app. The native game presenter and this control subscribe
// through the same native-panel.h, on each application's own foreground view.
#include <windows.h>
#include <cstdio>
#include <chrono>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include "native-panel.h"
#ifdef MOUSE_PROBE_GAMEINPUT_V0
#include "native-gameinput.h"
#endif

using namespace winrt;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Core;
using Windows::UI::Xaml::Media::SolidColorBrush;

extern "C" HRESULT FhdLocalPath(WCHAR* buffer, DWORD capacity) {
    try {
        auto path = Windows::Storage::ApplicationData::Current().LocalFolder().Path();
        if (path.size() >= capacity) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        std::copy_n(path.c_str(), path.size() + 1, buffer);
        return S_OK;
    } catch (...) { return to_hresult(); }
}
namespace {
void checkpoint(char const* name, char const* text) noexcept {
    WCHAR path[1024];
    const auto leaf = std::string_view(name) == "startup" ? L"\\mouse-startup.txt" :
        std::string_view(name) == "independent" ? L"\\mouse-independent.txt" : L"\\mouse-capture.txt";
    if (FAILED(FhdLocalPath(path, ARRAYSIZE(path))) || !FhdFrameTrace::suffix(path, ARRAYSIZE(path), leaf)) return;
    file_handle file{CreateFile2(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, CREATE_ALWAYS, nullptr)};
    DWORD written;
    if (file) WriteFile(file.get(), text, static_cast<DWORD>(strlen(text)), &written, nullptr);
}
}
#ifdef MOUSE_PROBE_INDEPENDENT
#include "native-independent.h"
#endif
namespace {
struct MouseApp : ApplicationT<MouseApp> {
#ifdef MOUSE_PROBE_INDEPENDENT
    std::shared_ptr<IndependentMouseProbe> independent = std::make_shared<IndependentMouseProbe>();
#endif
    std::shared_ptr<NativePanelMouseProbe> probe = std::make_shared<NativePanelMouseProbe>();
    DispatcherTimer timer{nullptr};
    Grid target{nullptr};
    TextBlock status{nullptr};
    CoreWindow core{nullptr};
    unsigned attempts = 0, accepted = 0, confirmed = 0, outside = 0, releases = 0, lost = 0;
    unsigned keyDown = 0, keyUp = 0;
    HRESULT keyRegistration = E_PENDING;
    uint32_t id = 0;
    HRESULT last = E_PENDING;
    int present = -1;
    bool captured = false, ready = false, listed = false;
    uint64_t runId = 0;
    void startup(char const* state, bool visible, HRESULT hr) {
        char text[320];
        std::snprintf(text, sizeof(text),
            "run_id=%llu\r\nprobe=native-mouse-capture\r\nstatus=%s\r\nvisible=%u\r\nregistered=0x%x\r\nstart_tick=%llu\r\nhr=%08lx\r\n",
            static_cast<unsigned long long>(runId), state, unsigned(visible), probe->registered, probe->startTick, static_cast<unsigned long>(hr));
        checkpoint("startup", text);
    }
    MouseApp() {
        WCHAR path[1024];
        UINT size = ARRAYSIZE(path);
        file_handle marker;
        if (GetCurrentPackagePath(&size, path) == ERROR_SUCCESS &&
            FhdFrameTrace::suffix(path, ARRAYSIZE(path), L"\\mouse-controller-control.txt")) {
            marker.attach(CreateFile2(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr));
        }
        RequiresPointerMode(marker ? ApplicationRequiresPointerMode::Auto : ApplicationRequiresPointerMode::WhenRequested);
    }
    void snapshot() {
        const bool visible = core.Visible() && core.ActivationMode() == CoreWindowActivationMode::ActivatedInForeground;
        probe->report(visible);
#ifdef MOUSE_PROBE_INDEPENDENT
        independent->snapshot(visible, runId);
#endif
        if (!ready && visible) {
            const bool complete = probe->registered == 0x3f;
            HRESULT hr = S_OK;
            for (HRESULT subscription : probe->subscriptions) if (FAILED(subscription)) { hr = subscription; break; }
            startup(complete ? "mouse-capture-ready" : "mouse-capture-error", visible, hr);
            ready = true;
#ifdef MOUSE_PROBE_GAMEINPUT_V0
            NativeMouseDevices::start(runId, probe);
#endif
        }
        if (!visible && captured) target.ReleasePointerCaptures();
        char record[640];
        std::snprintf(record, sizeof(record),
            "run_id=%llu probe=native-mouse-capture start_tick=%llu tick=%llu visible=%u mouse_present=%d registered=0x%x\r\n"
            "xaml_attempts=%u xaml_accepted=%u verified_pointer_in_capture_list=%u current_capture=%u "
            "same_pointer_moves_outside=%u releases=%u lost=%u last_hr=%08lx\r\n"
            "core_key_down=%u core_key_up=%u key_registration_hr=%08lx\r\n",
            runId, probe->startTick, GetTickCount64(), unsigned(visible), present, probe->registered,
            attempts, accepted, confirmed, unsigned(captured), outside, releases, lost, static_cast<unsigned long>(last),
            keyDown, keyUp, static_cast<unsigned long>(keyRegistration));
        checkpoint("capture", record);
#ifdef MOUSE_PROBE_INDEPENDENT
        status.Text(L"Mouse movements  " + to_hstring(independent->moves.load()) + L"     Presses  " +
                    to_hstring(independent->down.load()) + L"     Wheel events  " + to_hstring(independent->wheel.load()));
#else
        status.Text(L"Relative movements   " + to_hstring(static_cast<uint32_t>(InterlockedCompareExchange(&probe->nonzeroDeltas, 0, 0))) +
                    L"     Confirmed captures   " + to_hstring(confirmed) +
                    L"     Outside movements   " + to_hstring(outside));
#endif
    }
    void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs const&) {
        auto view = Window::Current();
        if (view.Content()) { view.Activate(); return; }
        try {
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            runId = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
            startup("mouse-capture-starting", false, S_OK);
            core = view.CoreWindow();
            probe->open(core);
            try {
                core.KeyDown([this](auto const& sender, auto const&) {
                    if (sender.Visible() && sender.ActivationMode() == CoreWindowActivationMode::ActivatedInForeground) ++keyDown;
                });
                core.KeyUp([this](auto const& sender, auto const&) {
                    if (sender.Visible() && sender.ActivationMode() == CoreWindowActivationMode::ActivatedInForeground) ++keyUp;
                });
                keyRegistration = S_OK;
            } catch (...) { keyRegistration = to_hresult(); }
            try { present = Windows::Devices::Input::MouseCapabilities{}.MousePresent(); } catch (...) {}
            Grid root;
            root.Background(SolidColorBrush(Windows::UI::Color{255, 22, 30, 45}));
            target = Grid{};
            target.Width(640);
            target.Height(200);
            target.Background(SolidColorBrush(Windows::UI::Color{255, 29, 105, 118}));
            StackPanel lines;
            lines.VerticalAlignment(VerticalAlignment::Center);
            lines.HorizontalAlignment(HorizontalAlignment::Center);
            auto label = [&](wchar_t const* text, double size) {
                TextBlock line;
                line.Text(text);
                line.FontSize(size);
                line.Foreground(SolidColorBrush(Windows::UI::Color{255, 255, 255, 255}));
                lines.Children().Append(line);
                return line;
            };
            label(L"Xbox mouse capture", 32);
#ifdef MOUSE_PROBE_INDEPENDENT
            label(L"Move the physical mouse, click, then scroll on the purple panel.", 18);
#else
            label(L"Hold the mouse button here, drag outside the box, then release.", 18);
#endif
            status = label(L"Listening", 16);
            lines.IsHitTestVisible(false);
            target.Children().Append(lines);
            target.PointerPressed([this](auto const&, auto const& event) {
                if (!core.Visible() || core.ActivationMode() != CoreWindowActivationMode::ActivatedInForeground || captured) return;
                auto pointer = event.Pointer();
                if (pointer.PointerDeviceType() != NativePanelMouseProbe::Kind::Mouse || !pointer.IsInContact()) return;
                listed = false;
                ++attempts;
                try {
                    if (target.CapturePointer(pointer)) {
                        ++accepted;
                        id = pointer.PointerId();
                        captured = true;
                        for (auto const& item : target.PointerCaptures()) {
                            if (item.PointerId() == id) { listed = true; ++confirmed; break; }
                        }
                    }
                    last = S_OK;
                } catch (...) { last = to_hresult(); }
            });
            target.PointerMoved([this](auto const&, auto const& event) {
                if (!captured || !listed || !event.Pointer().IsInContact() || event.Pointer().PointerId() != id) return;
                auto p = event.GetCurrentPoint(target).Position();
                if (p.X < 0 || p.Y < 0 || p.X >= target.ActualWidth() || p.Y >= target.ActualHeight()) {
                    for (auto const& item : target.PointerCaptures())
                        if (item.PointerId() == id) { ++outside; break; }
                }
            });
            target.PointerReleased([this](auto const&, auto const& event) {
                if (!captured || event.Pointer().PointerId() != id) return;
                ++releases;
                target.ReleasePointerCapture(event.Pointer());
                captured = false;
            });
            target.PointerCaptureLost([this](auto const&, auto const& event) {
                if (event.Pointer().PointerId() == id) { ++lost; captured = false; }
            });
#ifdef MOUSE_PROBE_INDEPENDENT
            independent->panel = SwapChainPanel{};
            root.Children().Append(independent->panel);
            target.IsHitTestVisible(false);
            target.Background(nullptr);

            auto stop = [weak = std::weak_ptr<IndependentMouseProbe>(independent)](auto const&, auto const&) {
                if (auto state = weak.lock()) state->close();
            };
            core.Closed(stop);
            core.VisibilityChanged([weak = std::weak_ptr<IndependentMouseProbe>(independent)](auto const&, auto const& event) {
                if (!event.Visible()) if (auto state = weak.lock(); state && state->visibleOnce) state->close();
            });
            core.Activated([weak = std::weak_ptr<IndependentMouseProbe>(independent)](auto const&, auto const& event) {
                if (event.WindowActivationState() == CoreWindowActivationState::Deactivated)
                    if (auto state = weak.lock(); state && state->visibleOnce) state->close();
            });
#endif
            root.Children().Append(target);
            view.Content(root);
            view.Activate();
            snapshot();
            timer = DispatcherTimer{};
            timer.Interval(std::chrono::seconds(1));
            timer.Tick([this](auto const&, auto const&) { snapshot(); });
            timer.Start();

        } catch (...) {
            startup("mouse-capture-error", false, to_hresult());
            throw;
        }
    }
};
}
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        init_apartment(apartment_type::multi_threaded);
        checkpoint("startup", "stage=entry\r\n");
        Application::Start([](auto const&) { make<MouseApp>(); });
        return 0;
    } catch (...) { return to_hresult(); }
}
