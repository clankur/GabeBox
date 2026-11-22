// The existing packaged account/game host shares the XAML view with Valve's isolated WebView2.
#include <windows.h>
#include <cstdio>
#include <memory>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Web.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.h>
#include "../big-picture-webview/shell-cursor.h"

using namespace winrt;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using Windows::Foundation::Uri;

namespace {
constexpr wchar_t accountPage[] = L"ms-appx-web:///steam-signin.html";

// Fixed enum-like stage labels plus numeric PID and HRESULT only. Each cold run
// replaces the previous one; storage diagnostics must not obstruct activation.
void checkpoint(char const* stage, HRESULT status = S_OK, bool first = false) noexcept
{
    try {
        auto path = Windows::Storage::ApplicationData::Current().LocalFolder().Path() +
                    L"\\big-picture-primary-startup.txt";
        handle file{CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, first ? CREATE_ALWAYS : OPEN_ALWAYS, nullptr)};
        if (!file) return;
        LARGE_INTEGER offset{};
        if (!SetFilePointerEx(file.get(), offset, nullptr, FILE_END)) return;
        char text[128];
        int count = std::snprintf(text, sizeof(text), "pid=%lu stage=%s hresult=0x%08lx\r\n",
                                  GetCurrentProcessId(), stage, static_cast<unsigned long>(status));
        DWORD written;
        if (count > 0 && count < sizeof(text)) WriteFile(file.get(), text, count, &written, nullptr);
    } catch (...) {}
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

struct SteamApplication : ApplicationT<SteamApplication>
{
    SteamApplication()
    {
        // Xbox defaults to driving a system cursor with the controller. The
        // QR page uses directional focus and Steam reads the gamepad directly.
        RequiresPointerMode(ApplicationRequiresPointerMode::WhenRequested);
    }

    hstring GetRuntimeClassName() const override { return L"BigPicture.Primary.App"; }

    void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs const&)
    {
        checkpoint("launched");
        try {
            auto window = Window::Current();
            if (!window.Content()) {
                Grid root;
                window.Content(root);
                window.Activate();
                checkpoint("xaml-window-active");
                WebView account{WebViewExecutionMode::SameThread};
                checkpoint("edge-created");
                account.Name(L"SteamAccountHost");
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
                steam.Visibility(Visibility::Collapsed);
                steam.IsHitTestVisible(false);
                root.Children().Append(steam);

                account.Navigate(local);
                checkpoint("account-navigating");
            }
            window.Activate();
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
        Application::Start([](auto const&) {
            checkpoint("xaml-start");
            make<SteamApplication>();
            checkpoint("application-created");
        });
        return 0;
    } catch (...) {
        auto hr = to_hresult();
        checkpoint("startup-failed", hr);
        return hr;
    }
}
