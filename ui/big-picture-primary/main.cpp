// The existing packaged account/game host shares the XAML view with Valve's isolated WebView2.
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string_view>
#include <utility>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Web.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include "../big-picture-webview/shell-cursor.h"

using namespace winrt;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using Windows::Foundation::Uri;

namespace {
constexpr wchar_t accountPage[] = L"ms-appx-web:///steam-signin.html";

// Each cold run replaces the previous one. Recorded lifecycle fields are numeric.
void checkpoint(char const* stage, HRESULT status = S_OK, bool first = false, char const* details = "") noexcept
{
    try {
        auto path = Windows::Storage::ApplicationData::Current().LocalFolder().Path() +
                    L"\\big-picture-primary-startup.txt";
        file_handle file{CreateFile2(path.c_str(), first ? GENERIC_WRITE : FILE_APPEND_DATA,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, first ? CREATE_ALWAYS : OPEN_ALWAYS, nullptr)};
        if (!file) return;
        char text[256];
        int count = std::snprintf(text, sizeof(text), "pid=%lu stage=%s hresult=0x%08lx tick_ms=%llu%s\r\n",
                                  GetCurrentProcessId(), stage, static_cast<unsigned long>(status),
                                  static_cast<unsigned long long>(GetTickCount64()), details);
        DWORD written;
        if (count > 0 && count < sizeof(text)) WriteFile(file.get(), text, count, &written, nullptr);
    } catch (...) {}
}

bool lifecycleTraceEnabled() noexcept
{
    try {
        auto path = Windows::ApplicationModel::Package::Current().InstalledLocation().Path() +
                    L"\\steam-primary-lifecycle-trace.txt";
        file_handle marker{CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr)};
        return static_cast<bool>(marker);
    } catch (...) {
        return false;
    }
}

// Tick measures delivery on the same XAML apartment as Suspending. Record a
// single cold run without writing to disk in the tick callback.
struct DispatcherProgress {
    static constexpr auto interval = std::chrono::milliseconds{500};
    static constexpr auto lateThresholdMs = static_cast<unsigned long long>(3 * interval.count());
    DispatcherTimer timer{nullptr};
    DispatcherTimer::Tick_revoker tick;
    unsigned long long ticks = 0;
    unsigned long long last = 0;
    unsigned long long gap = 0;
    unsigned long long gapEnd = 0;

    void sample() noexcept
    {
        const auto now = GetTickCount64();
        const auto elapsed = now - last;
        if (elapsed >= lateThresholdMs) {
            gap = elapsed;
            gapEnd = now;
        }
        last = now;
        ++ticks;
    }

    void suspend() noexcept
    {
        if (!timer) {
            checkpoint("suspending");
            return;
        }
        char details[180];
        std::snprintf(details, sizeof(details),
                      " ui_ticks=%llu ui_last_ms=%llu ui_age_ms=%llu ui_late_gap_ms=%llu ui_late_end_ms=%llu",
                      ticks, last, GetTickCount64() - last, gap, gapEnd);
        checkpoint("suspending", S_OK, false, details);
        tick.revoke();
        try { timer.Stop(); } catch (...) { checkpoint("dispatcher-progress-stop-error", to_hresult()); }
        timer = nullptr;
    }
};

void checkpointWindowState(char const* stage, int viewId, Windows::UI::Core::CoreWindow const& window,
                           Windows::UI::Core::WindowActivatedEventArgs const& activation,
                           Windows::UI::Core::VisibilityChangedEventArgs const& visibility) noexcept
{
    try {
        auto mode = window.try_as<Windows::UI::Core::ICoreWindow5>();
        char details[104];
        std::snprintf(details, sizeof(details), " view_id=%d activation_mode=%d activation_state=%d visible=%d",
                      viewId, mode ? static_cast<int>(mode.ActivationMode()) : -1,
                      activation ? static_cast<int>(activation.WindowActivationState()) : -1,
                      static_cast<int>(visibility ? visibility.Visible() : window.Visible()));
        checkpoint(stage, S_OK, false, details);
    } catch (...) { checkpoint(stage, to_hresult()); }
}

void traceWindowLifecycle(Windows::UI::Core::CoreWindow const& window)
{
    using namespace Windows::UI::ViewManagement;
    auto view = ApplicationView::GetForCurrentView();
    const auto id = view.Id();
    window.Activated([id](auto const& sender, auto const& event) {
        checkpointWindowState("core-window-activated", id, sender, event, nullptr);
    });
    window.VisibilityChanged([id](auto const& sender, auto const& event) {
        checkpointWindowState("core-window-visibility", id, sender, nullptr, event);
    });
    view.Consolidated([id](auto const&, ApplicationViewConsolidatedEventArgs const& event) noexcept {
        try {
            auto origin = event.try_as<IApplicationViewConsolidatedEventArgs2>();
            char details[96];
            std::snprintf(details, sizeof(details), " view_id=%d user_initiated=%d app_initiated=%d", id,
                          static_cast<int>(event.IsUserInitiated()),
                          origin ? static_cast<int>(origin.IsAppInitiated()) : -1);
            checkpoint("application-view-consolidated", S_OK, false, details);
        } catch (...) { checkpoint("application-view-consolidated", to_hresult()); }
    });
}

bool sameOrdinal(hstring const& a, hstring const& b) noexcept
{
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// EdgeHTML reports a qualified and normalized package authority upon commit.
// Uri.Equals can disagree across representations even though each component
// matches. Compare the scheme and package authority case-insensitively, keeping
// the path exact and rejecting all optional URL components.
bool matchesCommittedAccount(Uri const& uri, Uri const& qualified)
{
    return uri && sameOrdinal(uri.SchemeName(), L"ms-appx-web") &&
           (uri.Host().empty() || sameOrdinal(uri.Host(), qualified.Host())) &&
           uri.Port() == qualified.Port() && uri.Path() == L"/steam-signin.html" &&
           uri.Query().empty() && uri.Fragment().empty() && uri.UserName().empty() && uri.Password().empty();
}

void configureXboxInput(Window const& window, WebView const& account) {
    if (Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() != L"Windows.Xbox") return;
    using namespace Windows::UI::Core;
    // The renderer enables hit testing when it hands the account view to a
    // game. QR, authenticated Steam, and the transition between them use focus.
    account.IsHitTestVisible(false);
    auto core = window.CoreWindow();
    auto cursor = std::make_shared<steam_input::ShellCursor<CoreCursor>>();
    auto update = [control = make_weak(account), cursor](CoreWindow const& sender) {
        if (auto host = control.get())
            cursor->update(sender, !host.IsHitTestVisible() && sender.Visible() &&
                sender.ActivationMode() == CoreWindowActivationMode::ActivatedInForeground);
    };
    account.RegisterPropertyChangedCallback(UIElement::IsHitTestVisibleProperty(),
        [win = make_weak(core), update](auto const&, auto const&) { if (auto sender = win.get()) update(sender); });
    auto changed = [update](CoreWindow const& sender, auto const&) { update(sender); };
    core.PointerMoved(changed);
    core.Activated(changed);
    core.VisibilityChanged(changed);
    update(core);
}

void configureXboxBackNavigation() {
    if (Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() != L"Windows.Xbox") return;
    // Steam and the running game already receive B from the gamepad reader.
    // Consume the duplicate system request for the lifetime of this view.
    Windows::UI::Core::SystemNavigationManager::GetForCurrentView().BackRequested(
        [](auto const&, Windows::UI::Core::BackRequestedEventArgs const& request) { request.Handled(true); });
}

struct SteamApplication : ApplicationT<SteamApplication>
{
    using MemoryManager = Windows::System::MemoryManager;
    const bool traceLifecycle;
    std::shared_ptr<DispatcherProgress> dispatcherProgress;
    MemoryManager::AppMemoryUsageLimitChanging_revoker memoryLimit;

    explicit SteamApplication(bool trace) : traceLifecycle(trace)
    {
        // Xbox defaults to driving a system cursor with the controller. The
        // QR page uses directional focus and Steam reads the gamepad directly.
        RequiresPointerMode(ApplicationRequiresPointerMode::WhenRequested);
        if (!traceLifecycle) return;
        try {
            EnteredBackground([](auto const&, auto const&) { checkpoint("entered-background"); });
            LeavingBackground([](auto const&, auto const&) { checkpoint("leaving-background"); });
            dispatcherProgress = std::make_shared<DispatcherProgress>();
            Suspending([progress = dispatcherProgress](auto const&, auto const&) noexcept { progress->suspend(); });
            Resuming([](auto const&, auto const&) { checkpoint("resuming"); });
            UnhandledException([](auto const&, UnhandledExceptionEventArgs const& event) noexcept {
                try { checkpoint("unhandled-xaml-error", event.Exception()); }
                catch (...) { checkpoint("unhandled-xaml-error", to_hresult()); }
            });
            memoryLimit = MemoryManager::AppMemoryUsageLimitChanging(auto_revoke, [](auto const&, auto const& event) noexcept {
                try {
                    char details[100];
                    std::snprintf(details, sizeof(details), " old_bytes=%llu new_bytes=%llu usage_bytes=%llu",
                                  static_cast<unsigned long long>(event.OldLimit()),
                                  static_cast<unsigned long long>(event.NewLimit()),
                                  static_cast<unsigned long long>(MemoryManager::AppMemoryUsage()));
                    checkpoint("memory-limit-changing", S_OK, false, details);
                } catch (...) { checkpoint("memory-limit-trace-error", to_hresult()); }
            });
            checkpoint("lifecycle-trace-active");
        } catch (...) { checkpoint("lifecycle-trace-error", to_hresult()); }
    }

    hstring GetRuntimeClassName() const override { return L"BigPicture.Primary.App"; }

    void startDispatcherProgress() noexcept
    {
        if (!dispatcherProgress || dispatcherProgress->timer || dispatcherProgress->last) return;
        try {
            auto& progress = *dispatcherProgress;
            DispatcherTimer timer;
            timer.Interval(DispatcherProgress::interval);
            auto tick = timer.Tick(auto_revoke, [weak = std::weak_ptr{dispatcherProgress}](auto const&, auto const&) noexcept {
                if (auto progress = weak.lock()) progress->sample();
            });
            progress.last = GetTickCount64();
            timer.Start();
            progress.timer = std::move(timer);
            progress.tick = std::move(tick);
            checkpoint("dispatcher-progress-active");
        } catch (...) {
            checkpoint("dispatcher-progress-error", to_hresult());
        }
    }

    void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs const& event)
    {
        checkpoint("launched");
        try {
            auto window = Window::Current();
            if (!window.Content()) {
                try {
                    using Windows::ApplicationModel::Activation::ApplicationExecutionState;
                    auto settings = Windows::Storage::ApplicationData::Current().LocalSettings().Values();
                    const hstring key{L"steamPrerequisitePlayActivation"};
                    if (settings.HasKey(key)) settings.Remove(key);
                    const auto arguments = event.Arguments();
                    if (event.PreviousExecutionState() == ApplicationExecutionState::Terminated &&
                            arguments.size() <= 80 && std::wstring_view(arguments).starts_with(L"steam-prerequisite-play ")) {
                        settings.Insert(key, box_value(arguments));
                    }
                } catch (...) { checkpoint("prerequisite-activation-error", to_hresult()); }
                if (traceLifecycle) {
                    try {
                        auto coreWindow = window.CoreWindow();
                        coreWindow.Closed([](auto const&, auto const&) { checkpoint("core-window-closed"); });
                        traceWindowLifecycle(coreWindow);
                    } catch (...) { checkpoint("window-lifecycle-trace-error", to_hresult()); }
                }
                // Everything behind a hosted game panel is black: this page, the account host's
                // default background and the Steam composition host. A gap in the panel's
                // composition (a hide, a reorder or a frame without content) then shows the
                // letterbox colour instead of a white flash (clankur/GabeBox#89).
                const Media::SolidColorBrush black{Windows::UI::Colors::Black()};
                Grid root;
                root.Background(black);
                window.Content(root);
                configureXboxBackNavigation();
                window.Activate();
                checkpoint("xaml-window-active");
                WebView account{WebViewExecutionMode::SameThread};
                checkpoint("edge-created");
                account.Name(L"SteamAccountHost");
                account.DefaultBackgroundColor(Windows::UI::Colors::Black());
                configureXboxInput(window, account);
                // Windows accepts either an empty authority or the current package identity.
                Uri local{accountPage};
                Uri qualified{L"ms-appx-web://" + Windows::ApplicationModel::Package::Current().Id().Name() +
                              L"/steam-signin.html"};
                auto matchesAccountPage = [local, qualified](Uri const& uri) {
                    return uri && (uri.Equals(local) || uri.Equals(qualified));
                };
                auto expectedNavigation = std::make_shared<bool>(false);
                account.NavigationStarting([matchesAccountPage, expectedNavigation](WebView const& sender, auto const& event) {
                    sender.Tag(nullptr);
                    bool approved = matchesAccountPage(event.Uri());
                    *expectedNavigation = approved;
                    checkpoint(approved ? "account-navigation-approved" : "account-navigation-other");
                    event.Cancel(!approved);
                    if (!approved) checkpoint("account-navigation-denied", E_ACCESSDENIED);
                });
                account.NavigationCompleted([local, qualified, expectedNavigation](WebView const& sender, auto const& event) {
                    sender.Tag(nullptr);
                    bool approvedStart = *expectedNavigation;
                    *expectedNavigation = false;
                    if (!event.IsSuccess()) {
                        checkpoint("account-navigation-error", static_cast<HRESULT>(event.WebErrorStatus()));
                        return;
                    }
                    if (!matchesCommittedAccount(event.Uri(), qualified) || !approvedStart) {
                        checkpoint("account-document-unrecognized", E_ACCESSDENIED);
                        return;
                    }
                    // Only native code installs this receipt. The renderer reads the
                    // successfully committed document without accessing WebView.Source.
                    sender.Tag(local);
                    if (!sender.IsHitTestVisible()) sender.Focus(FocusState::Programmatic);
                    checkpoint("account-document-verified");
                });
                account.FrameNavigationStarting([](auto const&, auto const& event) { event.Cancel(true); });
                account.NewWindowRequested([](auto const&, auto const& event) { event.Handled(true); });
                account.UnsupportedUriSchemeIdentified([](auto const&, auto const& event) { event.Handled(true); });
                root.Children().Append(account);

                Canvas steam;
                steam.Name(L"SteamCompositionHost");
                steam.Background(black);
                steam.Visibility(Visibility::Collapsed);
                steam.IsHitTestVisible(false);
                root.Children().Append(steam);

                account.Navigate(local);
                checkpoint("account-navigating");
            }
            window.Activate();
            if (traceLifecycle) startDispatcherProgress();
        } catch (...) {
            checkpoint("launch-failed", to_hresult());
            throw;
        }
    }
};
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try {
        // UWP creates the application's ASTA for its XAML view. Startup runs on the MTA.
        init_apartment(apartment_type::multi_threaded);
        checkpoint("entry", S_OK, true);
        const bool traceLifecycle = lifecycleTraceEnabled();
        Application::Start([traceLifecycle](auto const&) {
            checkpoint("xaml-start");
            make<SteamApplication>(traceLifecycle);
            checkpoint("application-created");
        });
        if (traceLifecycle) checkpoint("application-returned");
        return 0;
    } catch (...) {
        auto hr = to_hresult();
        checkpoint("startup-failed", hr);
        return hr;
    }
}
