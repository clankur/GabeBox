// The existing packaged account/game host shares the XAML view with Valve's isolated WebView2.
#include <windows.h>
#include <cstdio>
#include <algorithm>
#include <memory>
#include <string>
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

bool sameOrdinal(hstring const& a, hstring const& b) noexcept
{
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// NavigationStarting checks only package URIs. The engine may report a translated
// address on completion; match only the installed, signed page of this very package.
bool matchesCommittedAccount(Uri const& uri, Uri const& local, Uri const& qualified)
{
    if (!uri) { checkpoint("completion-uri-absent"); return false; }
    auto package = Windows::ApplicationModel::Package::Current();
    auto host = uri.Host();
    auto scheme = uri.SchemeName();
    bool clean = uri.Query().empty() && uri.Fragment().empty() && uri.UserName().empty() && uri.Password().empty();
    checkpoint(clean ? "completion-no-parameters" : "completion-parameters");
    if (!clean) return false;
    checkpoint(scheme == L"ms-appx-web" ? "completion-scheme-package-web" :
               scheme == L"ms-appx" ? "completion-scheme-package" :
               scheme == L"file" ? "completion-scheme-file" :
               scheme == L"about" ? "completion-scheme-about" :
               scheme == L"https" ? "completion-scheme-https" : "completion-scheme-other");
    checkpoint(host.empty() ? "completion-authority-empty" :
               sameOrdinal(host, package.Id().Name()) ? "completion-authority-package-name" :
               sameOrdinal(host, package.Id().FamilyName()) ? "completion-authority-package-family" :
               sameOrdinal(host, package.Id().FullName()) ? "completion-authority-package-full" :
               "completion-authority-other");
    if (uri.Equals(local) || uri.Equals(qualified)) return true;
    bool page = uri.Path() == L"/steam-signin.html";
    checkpoint(page ? "completion-path-package-page" : "completion-path-other");
    if (scheme != L"file" || !host.empty()) return false;
    std::wstring filePath{package.InstalledLocation().Path()};
    std::replace(filePath.begin(), filePath.end(), L'\\', L'/');
    Uri signedFile{L"file:///" + filePath + L"/steam-signin.html"};
    bool matches = uri.Equals(signedFile);
    checkpoint(matches ? "completion-exact-installed-file" : "completion-other-file");
    return matches;
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
                account.Name(L"SteamAccountHost");
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
                    if (!matchesCommittedAccount(event.Uri(), local, qualified) || !approvedStart) {
                        checkpoint("account-document-unrecognized", E_ACCESSDENIED);
                        return;
                    }
                    // Only native code installs this receipt. The renderer reads the
                    // successfully committed document without accessing WebView.Source.
                    sender.Tag(local);
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
