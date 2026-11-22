// The existing packaged account/game host shares the XAML view with Valve's isolated WebView2.
#include <windows.h>
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

struct SteamApplication : ApplicationT<SteamApplication>
{
    void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs const&)
    {
        auto window = Window::Current();
        if (!window.Content()) {
            Grid root;
            WebView account{WebViewExecutionMode::SameThread};
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

            window.Content(root);
            account.Navigate(local);
        }
        window.Activate();
    }
};
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    init_apartment(apartment_type::single_threaded);
    Application::Start([](auto const&) { make<SteamApplication>(); });
    return 0;
}
