// The existing packaged account/game host shares the XAML view with Valve's isolated WebView2.
#include <windows.h>
#include <cstdio>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Web.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.h>

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

struct SteamApplication : ApplicationT<SteamApplication>
{
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
                account.NavigationCompleted([](auto const&, auto const& event) {
                    checkpoint(event.IsSuccess() ? "account-loaded" : "account-navigation-error",
                               static_cast<HRESULT>(event.WebErrorStatus()));
                });
                account.Name(L"SteamAccountHost");
                // Windows accepts either an empty authority or the current package identity.
                Uri local{accountPage};
                Uri qualified{L"ms-appx-web://" + Windows::ApplicationModel::Package::Current().Id().Name() +
                              L"/steam-signin.html"};
                account.NavigationStarting([local, qualified](auto const&, auto const& event) {
                    auto uri = event.Uri();
                    event.Cancel(!uri || (!uri.Equals(local) && !uri.Equals(qualified)));
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
