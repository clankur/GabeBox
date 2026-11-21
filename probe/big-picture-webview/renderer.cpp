#include <windows.h>
#include <winrt/BigPictureView.Native.h>
#include <winrt/Microsoft.Web.WebView2.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <algorithm>
#include <array>
#include <deque>
#include <string>
#include "cloud-cache.h"
#include "display-layout.h"

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation;
using namespace Windows::UI::Core;
using namespace Windows::UI::Composition;
using namespace Windows::UI::ViewManagement;
using namespace Microsoft::Web::WebView2::Core;

namespace {
using steam_document::entry;
using steam_document::signedEntry;
constexpr size_t maxPending = 32;

slim_mutex gate;
weak_ref<IFrameworkView> active;
CoreDispatcher dispatcher{nullptr};
CoreApplicationView secondary{nullptr};
int32_t viewId = 0;
bool opening = false;
bool closeRequested = false;
hstring status = L"closed";
std::deque<hstring> requests;

void checkpoint(std::wstring_view stage) {
    slim_lock_guard lock(gate);
    status = hstring(stage);
}

struct View : implements<View, IFrameworkView> {
    CoreWindow window{nullptr};
    Compositor compositor{nullptr};
    CompositionTarget target{nullptr};
    ContainerVisual root{nullptr};
    ContainerVisual mainSurface{nullptr};
    ContainerVisual popupSurface{nullptr};
    CoreWebView2Environment environment{nullptr};
    CoreWebView2CompositionController controller{nullptr};
    CoreWebView2CompositionController popup{nullptr};
    std::array<Windows::Gaming::Input::Gamepad, 8> gamepadSlots{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    bool safeBounds = false;
    bool gameForeground = false;
    bool openingPopup = false;
    bool popupRequestedVisible = false;
    CoreWebView2 web{nullptr};
    std::shared_ptr<steam_document::Owner> document = std::make_shared<steam_document::Owner>();
    std::shared_ptr<SteamUiCloudCache> cloudCache;
    hstring assets;
    bool closed = false;

    void Initialize(CoreApplicationView const&) {}
    void SetWindow(CoreWindow const& value) {
        window = value;
        auto thread = window.Dispatcher();
        auto id = ApplicationView::GetForCurrentView().Id();
        slim_lock_guard lock(gate);
        active = make_weak(get_strong().as<IFrameworkView>());
        dispatcher = thread;
        viewId = id;
    }
    void Load(hstring const&) {}

    void resize() {
        if (!controller) return;
        float scale = static_cast<float>(Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel());
        auto bounds = window.Bounds();
        auto visible = ApplicationView::GetForCurrentView().VisibleBounds();
        const auto layout = steam_display::viewport({bounds.X, bounds.Y, bounds.Width, bounds.Height},
            {visible.X, visible.Y, visible.Width, visible.Height}, scale, safeBounds, gameForeground);
        Rect pixels{0.f, 0.f, layout.width, layout.height};
        controller.Bounds(pixels);
        root.Size({bounds.Width * scale, bounds.Height * scale});
        mainSurface.Offset({layout.x, layout.y, 0});
        mainSurface.Size({layout.width, layout.height});
        if (popup) {
            popup.Bounds(pixels);
            popupSurface.Offset(mainSurface.Offset());
            popupSurface.Size(mainSurface.Size());
        }
    }

    fire_and_forget suspendHidden(CoreWebView2 browser) {
        auto lifetime = get_strong();
        try {
            // A view may be shown again before Chromium completes suspension.
            if (co_await browser.TrySuspendAsync(); !closed && !gameForeground && window.Visible()) browser.Resume();
        } catch (hresult_error const&) {
            // Chromium can decline suspension for an active browser operation.
        }
    }

    void applyVisibility() {
        if (closed || !controller) return;
        const bool visible = !gameForeground && window.Visible();
        controller.IsVisible(visible);
        if (popup) popup.IsVisible(visible && popupRequestedVisible);
        // A foreground game owns the GPU presenter. Keep the IPC renderer
        // resident while Chromium trims and suspends background execution.
        auto targetLevel = visible ? CoreWebView2MemoryUsageTargetLevel::Normal : CoreWebView2MemoryUsageTargetLevel::Low;
        for (auto const& view : {controller, popup}) {
            if (view) {
                auto browser = view.CoreWebView2();
                if (auto memory = browser.try_as<ICoreWebView2_19>())
                    memory.MemoryUsageTargetLevel(targetLevel);
                if (visible) {
                    if (browser.IsSuspended()) browser.Resume();
                } else if (!browser.IsSuspended()) {
                    suspendHidden(browser);
                }
            }
        }
    }

    void setGameForeground(bool value) {
        if (closed || !controller || gameForeground == value) return;
        gameForeground = value;
        if (value) {
            applyVisibility();
            controller.RootVisualTarget(nullptr);
            if (popup) popup.RootVisualTarget(nullptr);
            resize();
        } else {
            resize();
            controller.RootVisualTarget(mainSurface);
            if (popup) popup.RootVisualTarget(popupSurface);
            applyVisibility();
        }
        publishWindow();
    }

    // IDs 1 and 2 are local to this pair of WebViews. The PID is the
    // environment's actual Edge browser process, not a desktop Steam process.
    std::wstring windowState() {
        auto bounds = window.Bounds();
        auto visible = ApplicationView::GetForCurrentView().VisibleBounds();
        auto display = Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
        std::wstring keyboard;
        try {
            keyboard = L",\"keyboardPresent\":" + std::to_wstring(
                Windows::Devices::Input::KeyboardCapabilities().KeyboardPresent());
        } catch (hresult_error const&) {}
        return L"{\"viewId\":" + std::to_wstring(ApplicationView::GetForCurrentView().Id()) +
            L",\"pid\":" + std::to_wstring(web.BrowserProcessId()) +
            L",\"ownerId\":1,\"popupId\":2,\"visible\":" + (!gameForeground && window.Visible() ? L"true" : L"false") +
            L",\"focused\":" + (window.ActivationMode() == CoreWindowActivationMode::ActivatedInForeground ? L"true" : L"false") +
            L",\"popupOpen\":" + (popup ? L"true" : L"false") +
            L",\"popupVisible\":" + (popup && popup.IsVisible() ? L"true" : L"false") +
            L",\"x\":" + std::to_wstring(bounds.X) + L",\"y\":" + std::to_wstring(bounds.Y) +
            L",\"width\":" + std::to_wstring(bounds.Width) + L",\"height\":" + std::to_wstring(bounds.Height) +
            L",\"usableX\":" + std::to_wstring(visible.X) + L",\"usableY\":" + std::to_wstring(visible.Y) +
            L",\"usableWidth\":" + std::to_wstring(visible.Width) + L",\"usableHeight\":" + std::to_wstring(visible.Height) +
            L",\"scale\":" + std::to_wstring(display.RawPixelsPerViewPixel()) +
            L",\"screenWidth\":" + std::to_wstring(display.ScreenWidthInRawPixels()) +
            L",\"screenHeight\":" + std::to_wstring(display.ScreenHeightInRawPixels()) + keyboard + L"}";
    }

    std::wstring platformState() {
        std::wstring family = L"null", version = L"null";
        try {
            auto info = Windows::System::Profile::AnalyticsInfo::VersionInfo();
            auto name = info.DeviceFamily();
            if (name == L"Windows.Xbox" || name == L"Windows.Desktop")
                family = L"\"" + std::wstring(name) + L"\"";
            auto value = info.DeviceFamilyVersion();
            std::wstring_view digits(value);
            if (!digits.empty() && digits.size() <= 20 && digits.find_first_not_of(L"0123456789") == digits.npos &&
                (digits.size() < 20 || digits <= L"18446744073709551615"))
                version = L"\"" + std::wstring(digits) + L"\"";
        } catch (hresult_error const&) {
            // Some device contracts can withhold version information.
        }
        return L"{\"nativeGamepads\":true,\"deviceFamily\":" + family + L",\"deviceFamilyVersion\":" + version +
            L",\"devToolsEnabled\":" + (web.Settings().AreDevToolsEnabled() ? L"true" : L"false") + L"}";
    }

    void publishWindow() {
        if (!closed && web && document->accepts(web.Source()))
            web.PostWebMessageAsJson(hstring(L"{\"event\":\"xbox-window\",\"state\":" + windowState() + L"}"));
    }

    void focusVisible() {
        if (!gameForeground && window.Visible() && window.ActivationMode() == CoreWindowActivationMode::ActivatedInForeground)
            (popup && popup.IsVisible() ? popup : controller).MoveFocus(CoreWebView2MoveFocusReason::Programmatic);
    }

    // A composition controller has no child HWND. Forward CoreWindow pointer
    // input to the visible Valve surface in physical pixels, including underscan.
    void mouse(PointerEventArgs const& args, CoreWebView2MouseEventKind kind) {
        if (closed || gameForeground || !popup || !popup.IsVisible() || !window.Visible()) return;
        auto point = args.CurrentPoint();
        if (point.PointerDevice().PointerDeviceType() != Windows::Devices::Input::PointerDeviceType::Mouse) return;
        auto props = point.Properties();
        using Update = Windows::UI::Input::PointerUpdateKind;
        switch (props.PointerUpdateKind()) {
        case Update::LeftButtonPressed: kind = CoreWebView2MouseEventKind::LeftButtonDown; break;
        case Update::LeftButtonReleased: kind = CoreWebView2MouseEventKind::LeftButtonUp; break;
        case Update::RightButtonPressed: kind = CoreWebView2MouseEventKind::RightButtonDown; break;
        case Update::RightButtonReleased: kind = CoreWebView2MouseEventKind::RightButtonUp; break;
        default: break;
        }
        auto pos = point.Position();
        auto offset = popupSurface.Offset();
        auto scale = static_cast<float>(Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel());
        Point pixels{pos.X * scale - offset.x, pos.Y * scale - offset.y};
        auto keys = props.IsLeftButtonPressed() ? CoreWebView2MouseEventVirtualKeys::LeftButton :
            CoreWebView2MouseEventVirtualKeys::None;
        if (props.IsRightButtonPressed()) keys = keys | CoreWebView2MouseEventVirtualKeys::RightButton;
        if (kind == CoreWebView2MouseEventKind::LeftButtonDown) focusVisible();
        popup.SendMouseInput(kind, keys, static_cast<uint32_t>(props.MouseWheelDelta()), pixels);
        args.Handled(true);
    }

    void restartClient() {
        if (closed || gameForeground || !web || !document->signedMode || !document->accepts(web.Source())) return;
        closePopup();
        cloudCache->cancel();
        document->committed = false;
        { slim_lock_guard lock(gate); requests.clear(); }
        checkpoint(L"navigation");
        web.Navigate(signedEntry);
    }

    void publishGamepads() {
        std::wstring result = L"{\"event\":\"xbox-gamepads\",\"pads\":[";
        bool first = true;
        if (!gameForeground && window.Visible() && window.ActivationMode() == CoreWindowActivationMode::ActivatedInForeground) {
            try {
                auto pads = Windows::Gaming::Input::Gamepad::Gamepads();
                uint32_t index;
                for (auto& slot : gamepadSlots)
                    if (slot && !pads.IndexOf(slot, index)) slot = nullptr;
                for (const auto& pad : pads) {
                    auto slot = std::find(gamepadSlots.begin(), gamepadSlots.end(), pad);
                    if (slot == gamepadSlots.end()) {
                        slot = std::find(gamepadSlots.begin(), gamepadSlots.end(), nullptr);
                        if (slot == gamepadSlots.end()) break;
                        *slot = pad;
                    }
                    const auto reading = pad.GetCurrentReading();
                    if (!first) result += L",";
                    first = false;
                    result += L"{\"slot\":" + std::to_wstring(slot - gamepadSlots.begin()) +
                        L",\"bits\":" + std::to_wstring(static_cast<uint32_t>(reading.Buttons)) +
                        L",\"lx\":" + std::to_wstring(reading.LeftThumbstickX) +
                        L",\"ly\":" + std::to_wstring(reading.LeftThumbstickY) +
                        L",\"rx\":" + std::to_wstring(reading.RightThumbstickX) +
                        L",\"ry\":" + std::to_wstring(reading.RightThumbstickY) +
                        L",\"lt\":" + std::to_wstring(reading.LeftTrigger) +
                        L",\"rt\":" + std::to_wstring(reading.RightTrigger) + L"}";
                }
            } catch (hresult_error const&) {
                // A controller can disconnect between enumeration and reading.
                // Retry from the fresh WinRT collection on the next input tick.
            }
        }
        web.PostWebMessageAsJson(hstring(result + L"]}"));
    }

    bool handleWindowCommand(hstring const& message) {
        if (message == L"\"xbox-read-gamepads\"") publishGamepads();
        else if (message == L"\"xbox-display-safe-bounds\"" || message == L"\"xbox-display-full-bounds\"") {
            safeBounds = message == L"\"xbox-display-safe-bounds\"";
            // The compositor also uses VisibleBounds directly when the platform
            // does not honor a desired-bounds-mode change for this view.
            try {
                ApplicationView::GetForCurrentView().SetDesiredBoundsMode(safeBounds ?
                    ApplicationViewBoundsMode::UseVisible : ApplicationViewBoundsMode::UseCoreWindow);
            } catch (hresult_error const&) {}
            resize();
            publishWindow();
        } else if (message == L"\"xbox-app-initialized\"") {
            // Steam signals post-login service setup separately from a visible screen.
            slim_lock_guard lock(gate);
            if (status == L"document-loaded" || status == L"gamepad-window-created") status = L"app-initialized";
        } else if (message == L"\"xbox-window-focus\"") focusVisible();
        else if (message == L"\"xbox-window-show\"" || message == L"\"xbox-window-hide\"") {
            if (popup) {
                popupRequestedVisible = message == L"\"xbox-window-show\"";
                applyVisibility();
                focusVisible();
                publishWindow();
            }
        } else if (message == L"\"xbox-window-primary-display\"") {
            resize();
            publishWindow();
        } else return false;
        return true;
    }

    void configure(CoreWebView2 const& browser, bool allowMessages) {
        // WebView2 virtual hosts are scoped to one CoreWebView2. Valve's
        // independently composed SP BPM child loads its own CSS and fonts.
        browser.SetVirtualHostNameToFolderMapping(L"steamloopback.host", assets, CoreWebView2HostResourceAccessKind::Deny);
        auto settings = browser.Settings();
        settings.IsWebMessageEnabled(allowMessages);
        settings.AreHostObjectsAllowed(false);
        settings.AreDefaultContextMenusEnabled(false);
        settings.AreDevToolsEnabled(false);
        browser.PermissionRequested([](auto const&, auto const& args) { args.State(CoreWebView2PermissionState::Deny); });
        browser.DownloadStarting([](auto const&, auto const& args) { args.Cancel(true); });
    }

    void closePopup() {
        auto closing = std::exchange(popup, nullptr);
        if (closing) closing.Close();
        if (popupSurface) root.Children().Remove(popupSurface);
        popupSurface = nullptr;
        publishWindow();
    }

    fire_and_forget observeGamepad(CoreWebView2CompositionController browser) {
        // These selectors identify the shell layout, home carousel and library in
        // Valve's pinned build 1794536281. Ignore blank windows and loading views.
        constexpr wchar_t check[] = LR"JS((()=>{
            const target = document.getElementById('popup_target');
            function visible(node) {
                if (!node || getComputedStyle(node).visibility !== 'visible') return false;
                const b = node.getBoundingClientRect();
                return b.width > 0 && b.height > 0 && b.right > 0 && b.bottom > 0 && b.left < innerWidth && b.top < innerHeight;
            }
            const shell = document.getElementById('Header.Main.Footer');
            if (target?.contains(shell) && shell?.classList.contains('_3LYP1SIxyoky-yW9Ug_1fq') &&
                visible(shell.querySelector('.ZBBhezbBR4VJYysmVhF3_, ._2_cRkGdbegoIbiRsy_-I7'))) return 2;
            const library = target?.querySelector('#GamepadUI_Full_Root._3LYP1SIxyoky-yW9Ug_1fq #Main [role=tabpanel][id$="AllGames_Content"] [role=grid]');
            if (visible(library) && visible(library.querySelector('[role=gridcell] [role=link]'))) return 2;
            const connect = target?.querySelector('._1wlvJjVURYzQHWJlITdVCA button[role=menuitem]._21kxLhOa-E8amdNn4Q_zBY');
            if (visible(connect) && !connect.disabled && connect.querySelector('svg')) return 1;
            const choices = target?.querySelectorAll('._2RjHR32VwDeGwslJx6LcCN[role=listbox] .zzXtdadESGEkT4IFqLvjm[role=option][lang]');
            if (choices && new Set([...choices].filter(el => visible(el) && el.textContent.trim()).map(el => el.lang)).size > 1) return 1;
            return 0;
        })())JS";
        auto lifetime = get_strong();
        apartment_context ui;
        try {
            bool setup = false;
            for (int sample = 0; sample < 80 || setup;) {
                co_await resume_after(std::chrono::milliseconds(setup ? 1500 : 500));
                co_await ui;
                if (closed || popup != browser) co_return;
                if (gameForeground || !window.Visible()) continue;
                if (!setup) ++sample;
                auto result = co_await browser.CoreWebView2().ExecuteScriptAsync(check);
                if (result == L"1" && !setup) {
                    setup = true;
                    checkpoint(L"setup-ready");
                }
                if (result == L"2") {
                    checkpoint(L"gamepad-ready");
                    co_return;
                }
            }
        } catch (...) {
            if (!closed && popup == browser) checkpoint(L"gamepad-observation-error:" + std::to_wstring(to_hresult().value));
        }
    }

    fire_and_forget onPopup(CoreWebView2 const& sender, CoreWebView2NewWindowRequestedEventArgs args) {
        // Valve uses a same-origin blank child as the React portal for SP BPM.
        // Only its shared context can create that one surface. Grant no native
        // message or navigation authority to the child.
        args.Handled(true);
        auto lifetime = get_strong();
        auto deferral = args.GetDeferral();
        bool creating = false;
        try {
            hstring url = args.Uri();
            auto source = args.OriginalSourceFrameInfo();
            auto name = args.Name();
            auto scopedName = L"SP BPM_uid" + std::to_wstring(sender.BrowserProcessId());
            if (!popup && !openingPopup && document->accepts(sender.Source()) && source &&
                source.FrameKind() == CoreWebView2FrameKind::MainFrame && document->accepts(source.Source()) &&
                (name == L"SP BPM" || name == scopedName) && url.size() <= 2048 && std::wstring_view(url).starts_with(L"about:blank?createflags=")) {
                creating = openingPopup = true;
                auto parent = CoreWebView2ControllerWindowReference::CreateFromCoreWindow(window);
                auto browser = co_await environment.CreateCoreWebView2CompositionControllerAsync(parent);
                if (closed) {
                    browser.Close();
                } else {
                    popup = browser;
                    popupSurface = compositor.CreateContainerVisual();
                    root.Children().InsertAtTop(popupSurface);
                    if (!gameForeground) popup.RootVisualTarget(popupSurface);
                    resize();
                    auto child = popup.CoreWebView2();
                    configure(child, false);
                    auto initial = args.Uri();
                    child.NavigationStarting([initial](auto const&, auto const& event) {
                        if (event.Uri() != L"about:blank" && event.Uri() != initial) event.Cancel(true);
                    });
                    // The gamepad window renders its local menu into an inherited
                    // about:blank frame. Keep that frame on the signed UI origin.
                    child.FrameNavigationStarting([](auto const&, auto const& event) {
                        if (event.Uri() != L"about:blank") event.Cancel(true);
                    });
                    child.NewWindowRequested([](auto const&, auto const& event) { event.Handled(true); });
                    child.WindowCloseRequested([weak = get_weak()](auto const&, auto const&) {
                        if (auto view = weak.get(); view && !view->closed) {
                            view->closePopup();
                            checkpoint(L"gamepad-window-error:closed");
                        }
                    });
                    args.NewWindow(child);
                    popupRequestedVisible = true;
                    applyVisibility();
                    focusVisible();
                    publishWindow();
                    checkpoint(L"gamepad-window-created");
                    observeGamepad(popup);
                }
            }
        } catch (...) {
            if (creating) closePopup();
            checkpoint(L"popup-error:" + std::to_wstring(to_hresult().value));
        }
        if (creating) openingPopup = false;
        deferral.Complete();
    }

    fire_and_forget startBrowser() {
        auto lifetime = get_strong();
        std::wstring_view step = L"composition";
        try {
            checkpoint(step);
            compositor = Compositor();
            target = compositor.CreateTargetForCurrentView();
            root = compositor.CreateContainerVisual();
            target.Root(root);
            mainSurface = compositor.CreateContainerVisual();
            root.Children().InsertAtBottom(mainSurface);

            step = L"environment";
            checkpoint(step);
            auto cache = Windows::Storage::ApplicationData::Current().LocalCacheFolder().Path();
            environment = co_await CoreWebView2Environment::CreateWithOptionsAsync(L"", cache + L"\\WebView2", nullptr);
            if (closed) co_return;

            step = L"controller";
            checkpoint(step);
            auto parent = CoreWebView2ControllerWindowReference::CreateFromCoreWindow(window);
            auto browser = co_await environment.CreateCoreWebView2CompositionControllerAsync(parent);
            if (closed) { browser.Close(); co_return; }
            controller = browser;
            step = L"surface";
            checkpoint(step);
            controller.RootVisualTarget(mainSurface);
            resize();
            applyVisibility();
            web = controller.CoreWebView2();

            step = L"mapping";
            assets = Package::Current().InstalledLocation().Path() + L"\\ValveClient\\steamui";
            configure(web, true);
            cloudCache = std::make_shared<SteamUiCloudCache>(environment, document);
            cloudCache->install(web);
            web.NavigationStarting([this](auto const&, auto const& args) {
                if (!document->start(args.Uri(), args.NavigationId())) args.Cancel(true);
                else cloudCache->cancel();
            });
            web.ContentLoading([this](auto const&, auto const& args) {
                document->content(args.NavigationId(), args.IsErrorPage());
            });
            web.NewWindowRequested({this, &View::onPopup});
            web.WebMessageReceived([this](auto const& sender, auto const& args) {
                // Frame messages have a separate WebView2Frame event. The bridge
                // only accepts bounded top-level messages from this exact document.
                if (!document->accepts(sender.Source()) || !document->accepts(args.Source())) return;
                auto message = args.WebMessageAsJson();
                if (cloudCache->consume(message)) return;
                if (message.empty() || message.size() > 4096 || handleWindowCommand(message)) return;
                slim_lock_guard lock(gate);
                if (requests.size() < maxPending) requests.push_back(message);
            });
            web.ProcessFailed([](auto const&, auto const& args) {
                // Edge automatically recreates its GPU process. Preserve the
                // live application session and original window across that event.
                if (args.ProcessFailedKind() == CoreWebView2ProcessFailedKind::GpuProcessExited) return;
                checkpoint(L"process-error:" + std::to_wstring(static_cast<int>(args.ProcessFailedKind())));
            });
            web.NavigationCompleted([this](auto const& sender, auto const& args) {
                if (args.NavigationId() != document->navigation) return;
                if (args.IsSuccess() && document->accepts(sender.Source())) {
                    checkpoint(L"document-loaded");
                    publishWindow();
                } else checkpoint(L"navigation-error:" + std::to_wstring(static_cast<int>(args.WebErrorStatus())));
            });
            step = L"navigation";
            checkpoint(step);
            // Document-created scripts also run in new frames. Restrict this
            // adapter to Valve's signed local top-level bootstrap document.
            auto folder = co_await Windows::Storage::StorageFolder::GetFolderFromPathAsync(assets);
            auto extensions = co_await folder.TryGetItemAsync(L"xbox");
            auto extensionFolder = extensions.try_as<Windows::Storage::StorageFolder>();
            hstring adapter;
            if (extensionFolder) {
                auto item = co_await extensionFolder.TryGetItemAsync(L"bridge.js");
                if (auto file = item.try_as<Windows::Storage::StorageFile>())
                    adapter = co_await Windows::Storage::FileIO::ReadTextAsync(file);
            }
            if (closed) co_return;
            hstring script = LR"JS(if(window === window.top && ["https://steamloopback.host/index.html","https://steamloopback.host/index.html?IN_CLIENT=true"].includes(location.href.split("#")[0])){
                window.addEventListener("error", function(event) {
                    const message = String(event.message || "resource load error").slice(0,180);
                    window.chrome.webview.postMessage({method:"renderer-error", message});
                });
                window.addEventListener("unhandledrejection", function(event) {
                    const message = String(event.reason && event.reason.message || "unhandled rejection").slice(0,180);
                    window.chrome.webview.postMessage({method:"renderer-error", message});
                });
            )JS";
            auto seed = L"const xboxNativeWindow = Object.freeze(Object.assign(" + windowState() + L"," + platformState() + L"));\n";
            co_await web.AddScriptToExecuteOnDocumentCreatedAsync(script + hstring(seed) + adapter + L"\n}");
            if (closed) co_return;
            document->signedMode = !adapter.empty();
            web.Navigate(document->signedMode ? signedEntry : entry);
            controller.MoveFocus(CoreWebView2MoveFocusReason::Programmatic);
        } catch (...) {
            if (!closed) checkpoint(std::wstring(step) + L"-error:" + std::to_wstring(to_hresult().value));
        }
    }

    void Run() {
        try {
            { slim_lock_guard lock(gate); if (closeRequested) return; }
            auto pointer = [this](auto const&, PointerEventArgs const& args) { mouse(args, CoreWebView2MouseEventKind::Move); };
            window.PointerMoved(pointer);
            window.PointerPressed(pointer);
            window.PointerReleased(pointer);
            window.PointerWheelChanged([this](auto const&, PointerEventArgs const& args) { mouse(args, CoreWebView2MouseEventKind::Wheel); });
            window.SizeChanged({this, &View::onSize});
            window.VisibilityChanged([this](auto const&, auto const&) { applyVisibility(); publishWindow(); });
            window.Closed({this, &View::onClosed});
            window.Activated({this, &View::onActivated});
            window.Activate();
            startBrowser();
            window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessUntilQuit);
        } catch (...) { checkpoint(L"view-error:" + std::to_wstring(to_hresult().value)); }
    }
    void onSize(CoreWindow const&, WindowSizeChangedEventArgs const&) { resize(); publishWindow(); }
    void onClosed(CoreWindow const&, CoreWindowEventArgs const&) { close(); }
    void onActivated(CoreWindow const&, WindowActivatedEventArgs const& args) {
        if (controller && args.WindowActivationState() != CoreWindowActivationState::Deactivated)
            focusVisible();
        publishWindow();
    }
    void close() {
        if (closed) return;
        closed = true;
        if (cloudCache) cloudCache->cancel();
        closePopup();
        if (controller) controller.Close();
        web = nullptr;
        controller = nullptr;
        window.Dispatcher().StopProcessEvents();
    }
    void Uninitialize() {
        closed = true;
        slim_lock_guard lock(gate);
        viewId = 0;
        active = {};
        dispatcher = nullptr;
        secondary = nullptr;
        opening = false;
        closeRequested = false;
        requests.clear();
    }
};

struct Source : implements<Source, IFrameworkViewSource> {
    IFrameworkView CreateView() { return make<View>(); }
};

template <typename Callback> void onBrowserThread(Callback callback) {
    weak_ref<IFrameworkView> weak;
    CoreDispatcher thread{nullptr};
    {
        slim_lock_guard lock(gate);
        weak = active;
        thread = dispatcher;
    }
    if (thread) thread.RunAsync(CoreDispatcherPriority::Normal, [weak, callback] {
        if (auto view = weak.get()) callback(*get_self<View>(view));
    });
}

struct Factory : implements<Factory, IActivationFactory, BigPictureView::Native::IRendererStatics> {
    IInspectable ActivateInstance() { throw hresult_not_implemented(); }
    void Open() {
        {
            slim_lock_guard lock(gate);
            if (opening && closeRequested) throw hresult_error(HRESULT_FROM_WIN32(ERROR_BUSY));
            if (opening) return;
            opening = true;
            status = L"creating-view";
        }
        try {
            auto newView = CoreApplication::CreateNewView(make<Source>());
            slim_lock_guard lock(gate);
            if (opening) secondary = newView;
        }
        catch (...) { slim_lock_guard lock(gate); opening = false; throw; }
    }
    int32_t ViewId() { slim_lock_guard lock(gate); return viewId; }
    hstring Status() { slim_lock_guard lock(gate); return status; }
    hstring Read() {
        slim_lock_guard lock(gate);
        if (requests.empty()) return {};
        auto message = std::move(requests.front());
        requests.pop_front();
        return message;
    }
    void Reply(hstring const& message) {
        if (message.size() > 4 * 1024 * 1024) throw hresult_invalid_argument();
        onBrowserThread([message](View& view) {
            // This direction is the trusted WWA-to-native channel. Renderer
            // WebMessageReceived never accepts a presenter-visibility command.
            if (message == L"{\"nativeGameForeground\":true}") view.setGameForeground(true);
            else if (message == L"{\"nativeGameForeground\":false}") view.setGameForeground(false);
            else if (message == L"{\"nativeRestartClient\":true}") view.restartClient();
            else if (view.web && view.document->accepts(view.web.Source())) view.web.PostWebMessageAsJson(message);
        });
    }
    void Close() {
        { slim_lock_guard lock(gate); closeRequested = opening; }
        onBrowserThread([](View& view) { view.close(); });
    }
};
}

extern "C" HRESULT WINAPI GetBigPictureViewFactory(void* name, void** result) noexcept try {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (*reinterpret_cast<hstring const*>(&name) != L"BigPictureView.Native.Renderer") return CLASS_E_CLASSNOTAVAILABLE;
    *result = detach_abi(make<Factory>());
    return S_OK;
} catch (...) { return to_hresult(); }

extern "C" HRESULT WINAPI CanUnloadBigPictureView() noexcept { return S_FALSE; }
