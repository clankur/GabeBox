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
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <algorithm>
#include <array>
#include <deque>
#include <string>
#include "cloud-cache.h"
#include "display-layout.h"
#include "library-capture.h"
#include "process-failure.h"
#include "shell-cursor.h"
#include "store-browser.h"

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
IFrameworkView primaryOwner{nullptr};
int32_t viewId = 0;
bool opening = false;
bool closeRequested = false;
hstring openingAccount;
hstring status = L"closed";
std::deque<hstring> requests;
hstring captureResult;

void checkpoint(std::wstring_view stage) {
    slim_lock_guard lock(gate);
    // Keep the first terminal renderer observation until the host starts again.
    if (!std::wstring_view(status).starts_with(L"process-error:")) status = hstring(stage);
}

struct View : implements<View, IFrameworkView> {
    CoreWindow window{nullptr};
    Compositor compositor{nullptr};
    CompositionTarget target{nullptr};
    Windows::UI::Xaml::Controls::Canvas primaryPanel{nullptr};
    Windows::UI::Xaml::Controls::WebView accountPanel{nullptr};
    bool primaryShellVisible = false;
    ContainerVisual root{nullptr};
    ContainerVisual mainSurface{nullptr};
    ContainerVisual popupSurface{nullptr};
    std::shared_ptr<steam_store::Browser> store;
    CoreWebView2Environment environment{nullptr};
    CoreWebView2CompositionController controller{nullptr};
    CoreWebView2CompositionController popup{nullptr};
    std::array<Windows::Gaming::Input::Gamepad, 8> gamepadSlots{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    bool safeBounds = false;
    bool gameForeground = false;
    bool openingPopup = false;
    bool popupRequestedVisible = false;
    bool ownerAttached = false, popupAttached = false;
    CoreWebView2 web{nullptr};
    std::shared_ptr<steam_document::Owner> document = std::make_shared<steam_document::Owner>();
    std::shared_ptr<SteamUiCloudCache> cloudCache;
    hstring assets;
    bool closed = false;
    explicit View(bool warm, hstring const& owner) : account(owner), warming(warm) {}
    hstring const account;
    bool warming;
    bool frameOpen = false;
    bool xboxDisplay = false;
    steam_input::ShellCursor<CoreCursor> shellCursor;
    float backingCorrection = 1.f;
    uint64_t frameGeneration = 0;
    bool capturePending = false;
    bool authenticatedEmptyLibrary = false;
    uint64_t popupVisibilityEpoch = 0;
    CoreWindow::PointerMoved_revoker moved;
    CoreWindow::PointerPressed_revoker pressed;
    CoreWindow::PointerReleased_revoker released;
    CoreWindow::PointerWheelChanged_revoker wheel;
    CoreWindow::SizeChanged_revoker sized;
    CoreWindow::VisibilityChanged_revoker visibility;
    CoreWindow::Closed_revoker closing;
    CoreWindow::Activated_revoker activated;
    steam_display::Placement placement{{}, 1.f};

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

    static double textScale() {
        try { return UISettings().TextScaleFactor(); } catch (hresult_error const&) { return 1.; }
    }

    void resize() {
        if (!controller) return;
        auto display = Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
        const float scale = static_cast<float>(display.RawPixelsPerViewPixel());
        auto bounds = window.Bounds();
        auto visible = ApplicationView::GetForCurrentView().VisibleBounds();
        const steam_display::Bounds full{bounds.X, bounds.Y, bounds.Width, bounds.Height};
        const float backingScale = xboxDisplay ? steam_display::fhdScale(full, scale,
            static_cast<float>(display.ScreenWidthInRawPixels()), static_cast<float>(display.ScreenHeightInRawPixels())) * backingCorrection : scale;
        placement = {steam_display::viewport(full, {visible.X, visible.Y, visible.Width, visible.Height}, safeBounds, gameForeground), backingScale};
        root.Size({bounds.Width, bounds.Height});
        auto place = [&](CoreWebView2CompositionController const& browser, ContainerVisual const& surface) {
            browser.BoundsMode(CoreWebView2BoundsMode::UseRawPixels);
            if (xboxDisplay) {
                // Xbox reports a 1920-pixel FHD surface and a 150% display.
                // Use the display DPI for UI layout independently of the backing budget.
                browser.ShouldDetectMonitorScaleChanges(false);
                browser.RasterizationScale(steam_display::rasterScale(scale, textScale()));
            }
            browser.Bounds({0.f, 0.f, placement.pixelWidth(), placement.pixelHeight()});
            surface.Offset({placement.view.x, placement.view.y, 0});
            surface.Size({placement.pixelWidth(), placement.pixelHeight()});
            surface.Scale({placement.visualScaleX(), placement.visualScaleY(), 1.f});
        };
        place(controller, mainSurface);
        if (popup) place(popup, popupSurface);
        layoutStore();
    }

    void layoutStore() {
        if (store && controller) store->layout(placement, controller.RasterizationScale(),
            !warming && !gameForeground && window.Visible() && gamepadVisible());
    }

    void storeCommand(Windows::Data::Json::JsonObject const& message) {
        if (!document->signedMode || !document->accepts(web.Source())) return;
        try {
            auto numeric = message.GetNamedNumber(L"id");
            if (!(numeric >= 1 && numeric <= 0x7fffffff && std::floor(numeric) == numeric)) return;
            auto id = static_cast<unsigned>(numeric);
            auto action = message.GetNamedString(L"action");
            if (action == L"create") {
                if (store) return;
                store = std::make_shared<steam_store::Browser>(id, web, document, root);
                layoutStore();
                store->start(environment, window);
            } else if (store && store->identifier() == id) {
                if (action == L"close") { store->close(); store.reset(); }
                else store->command(id,action,message);
            }
        } catch (hresult_error const&) {}
    }

    fire_and_forget suspendHidden(CoreWebView2 browser) {
        auto lifetime = get_strong();
        try {
            // A view may be shown again before Chromium completes suspension.
            if (co_await browser.TrySuspendAsync(); !closed && !gameForeground && (warming || window.Visible())) browser.Resume();
        } catch (hresult_error const&) {
            // Chromium can decline suspension for an active browser operation.
        }
    }

    void showPrimaryShell(bool show) {
        if (!primaryPanel) return;
        const bool acceptPointer = !show && (!xboxDisplay || gameForeground);
        if (accountPanel.IsHitTestVisible() != acceptPointer) accountPanel.IsHitTestVisible(acceptPointer);
        if (primaryShellVisible == show) return;
        primaryShellVisible = show;
        using namespace Windows::UI::Xaml;
        primaryPanel.Visibility(show ? Visibility::Visible : Visibility::Collapsed);
        accountPanel.Opacity(show ? 0.0 : 1.0);
        if (!show) accountPanel.Focus(FocusState::Programmatic);
    }

    void updateShellCursor(bool visible) {
        // The XAML primary owns its cursor from the first QR frame.
        if (xboxDisplay && !account.empty() && !primaryPanel) shellCursor.update(window, visible);
    }

    void applyVisibility() {
        if (closed || !controller) return;
        const bool visible = !warming && !gameForeground && window.Visible();
        const bool runBrowser = warming || visible;
        const bool showPopup = visible && popup && popupRequestedVisible;
        const bool showOwner = visible && !showPopup;
        showPrimaryShell(visible);
        updateShellCursor(visible);
        if (showOwner != ownerAttached) {
            controller.RootVisualTarget(showOwner ? mainSurface : nullptr);
            ownerAttached = showOwner;
        }
        if (popup && showPopup != popupAttached) {
            popup.RootVisualTarget(showPopup ? popupSurface : nullptr);
            popupAttached = showPopup;
            ++popupVisibilityEpoch;
        }
        // The owner runs Valve's scheduler, IPC and React portals. Keep its
        // document active when just its drawing surface is obscured.
        controller.IsVisible(runBrowser);
        if (popup) popup.IsVisible(showPopup);
        layoutStore();
        for (auto const& view : {controller, popup}) {
            if (view) {
                auto browser = view.CoreWebView2();
                const bool presented = view == controller ? showOwner : showPopup;
                if (auto memory = browser.try_as<ICoreWebView2_19>()) {
                    // A detached warmup still runs Valve scripts. Low asks
                    // Edge to release unused caches before the Xbox view
                    // switch reduces our foreground allocation from 5 to 1 GiB.
                    memory.MemoryUsageTargetLevel(presented ? CoreWebView2MemoryUsageTargetLevel::Normal
                                                           : CoreWebView2MemoryUsageTargetLevel::Low);
                }
                if (runBrowser) {
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
        resize();
        applyVisibility();
        publishWindow();
        if (primaryPanel && !value) focusVisible();
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
            L",\"warming\":" + (warming ? L"true" : L"false") +
            L",\"layoutPopup\":" + (popupRequestedVisible ? L"true" : L"false") +
            L",\"popupOpen\":" + (gamepadSurface() ? L"true" : L"false") +
            L",\"popupVisible\":" + (gamepadVisible() ? L"true" : L"false") +
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

    CoreWebView2CompositionController gamepadSurface() const {
        return frameOpen ? controller : popup;
    }

    bool gamepadVisible() const {
        auto surface = gamepadSurface();
        return !warming && surface && popupRequestedVisible && surface.IsVisible();
    }

    void focusVisible() {
        if (!warming && !gameForeground && window.Visible() && window.ActivationMode() == CoreWindowActivationMode::ActivatedInForeground)
            (popup && popup.IsVisible() ? popup : controller).MoveFocus(CoreWebView2MoveFocusReason::Programmatic);
    }

    // The Xbox UI receives controller navigation directly. The desktop and
    // standalone smoke composition views still receive native mouse input.
    void mouse(PointerEventArgs const& args, CoreWebView2MouseEventKind kind) {
        auto route = steam_input::pointerRoute(!closed && !gameForeground && gamepadVisible() && window.Visible(),
                                                xboxDisplay, !account.empty());
        if (route == steam_input::PointerRoute::otherView) return;
        if (route == steam_input::PointerRoute::controllerOnly) {
            updateShellCursor(true);
            args.Handled(true);
            return;
        }
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
        Point pixels{placement.localPixelX(pos.X), placement.localPixelY(pos.Y)};
        auto keys = props.IsLeftButtonPressed() ? CoreWebView2MouseEventVirtualKeys::LeftButton :
            CoreWebView2MouseEventVirtualKeys::None;
        if (props.IsRightButtonPressed()) keys = keys | CoreWebView2MouseEventVirtualKeys::RightButton;
        if (kind == CoreWebView2MouseEventKind::LeftButtonDown) focusVisible();
        gamepadSurface().SendMouseInput(kind, keys, static_cast<uint32_t>(props.MouseWheelDelta()), pixels);
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
        if (!warming && !gameForeground && window.Visible() && window.ActivationMode() == CoreWindowActivationMode::ActivatedInForeground) {
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
        } else if (message == L"\"xbox-window-frame-open\"" || message == L"\"xbox-window-frame-closed\"") {
            if (!document->signedMode) return true;
            const bool open = message == L"\"xbox-window-frame-open\"";
            if (frameOpen != open) {
                frameOpen = open;
                popupRequestedVisible = open;
                ++frameGeneration;
                ++popupVisibilityEpoch;
                publishWindow();
                if (open) {
                    focusVisible();
                    checkpoint(L"gamepad-window-created");
                    observeGamepad(controller);
                } else {
                    checkpoint(L"gamepad-window-error:closed");
                }
            }
        } else if (message == L"\"xbox-window-focus\"") focusVisible();
        else if (message == L"\"xbox-window-show\"" || message == L"\"xbox-window-hide\"") {
            if (gamepadSurface()) {
                const bool show = message == L"\"xbox-window-show\"";
                if (frameOpen && popupRequestedVisible != show) ++popupVisibilityEpoch;
                popupRequestedVisible = show;
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
        browser.ProcessFailed([owner = allowMessages](auto const&, auto const& args) {
            auto kind = args.ProcessFailedKind();
            if (kind == CoreWebView2ProcessFailedKind::GpuProcessExited) return; // Edge recreates this process.
            std::wstring message = owner ? L"process-error:owner:" : L"process-error:popup:";
            message += std::to_wstring(static_cast<int>(kind));
            try {
                message += L":";
                message += steam_webview::failureReason(static_cast<int>(args.Reason()));
                message += L":" + std::to_wstring(static_cast<uint32_t>(args.ExitCode())) + L":";
                message += steam_webview::processDescription(args.ProcessDescription());
            } catch (hresult_error const&) {
                message += L":details-unavailable";
            }
            checkpoint(message);
        });
        auto settings = browser.Settings();
        settings.IsWebMessageEnabled(allowMessages);
        settings.AreHostObjectsAllowed(false);
        settings.AreDefaultContextMenusEnabled(false);
        settings.AreDevToolsEnabled(false);
        browser.PermissionRequested([](auto const&, auto const& args) { args.State(CoreWebView2PermissionState::Deny); });
        browser.DownloadStarting([](auto const&, auto const& args) { args.Cancel(true); });
    }

    // Signed production uses the owner's single WebView. Read the exact same
    // Valve document inside its full-viewport frame. Unsigned smoke still uses
    // the standalone popup controller.
    hstring inGamepad(wchar_t const* script, bool layoutOnly = false) const {
        if (!frameOpen) return hstring(script);
        return hstring(std::wstring(LR"JS((() => {
            const element = document.getElementById('xbox-steam-gamepad-surface');
            if (!element || element.parentElement !== document.documentElement || element.tagName !== 'IFRAME' ||
                !element.checkVisibility({opacityProperty:true, visibilityProperty:true})) return null;
            if ()JS") + (layoutOnly ? L"element.getBoundingClientRect().width <= 0" :
                L"document.elementFromPoint(innerWidth/2, innerHeight/2) !== element") + LR"JS() return null;
            const child = element.contentWindow;
            if (child?.opener !== window || !(child.name === 'SP BPM' || /^SP BPM_uid[0-9]+$/.test(child.name))) return null;
            return ((window) => {
                const {document,innerWidth,innerHeight} = window;
                const getComputedStyle = window.getComputedStyle.bind(window);
                return )JS" + script + L"; })(child); })()");
    }

    bool canCapture(CoreWebView2CompositionController const& surface, uint64_t visibilityEpoch) {
        return visibilityEpoch == popupVisibilityEpoch && !closed && surface && surface == gamepadSurface() && gamepadVisible() && (frameOpen ? ownerAttached : popupAttached) && !gameForeground && window.Visible() &&
            window.ActivationMode() == CoreWindowActivationMode::ActivatedInForeground && surface.IsVisible() &&
            web && document->signedMode && document->accepts(web.Source());
    }

    fire_and_forget captureGamepad(bool libraryOnly) {
        using namespace Windows::Storage;
        using namespace Windows::Storage::Streams;
        if (capturePending) co_return;
        auto lifetime = get_strong();
        capturePending = true;
        auto surface = gamepadSurface();
        std::wstring selector = libraryOnly ? steam_capture::library : steam_capture::surface;
        if (libraryOnly && authenticatedEmptyLibrary)
            selector = std::wstring(steam_capture::emptyLibrary) + L" && " + steam_capture::emptyForeground;
        const auto filename = libraryOnly ? L"big-picture-library.png" : L"big-picture-surface.png";
        const auto visibilityEpoch = popupVisibilityEpoch;
        StorageFile temporary{nullptr};
        std::wstring state = L"not-visible";
        uint32_t width = 0, height = 0, browserPid = 0;
        int32_t sourceView = 0;
        try {
            sourceView = ApplicationView::GetForCurrentView().Id();
            if (web) browserPid = web.BrowserProcessId();
            if (canCapture(surface, visibilityEpoch) && co_await surface.CoreWebView2().ExecuteScriptAsync(inGamepad(selector.c_str())) == L"true" &&
                canCapture(surface, visibilityEpoch)) {
                auto folder = ApplicationData::Current().LocalFolder();
                temporary = co_await folder.CreateFileAsync(L"big-picture-library.capture.tmp", CreationCollisionOption::GenerateUniqueName);
                auto stream = co_await temporary.OpenAsync(FileAccessMode::ReadWrite);
                if (canCapture(surface, visibilityEpoch)) {
                    co_await surface.CoreWebView2().CapturePreviewAsync(CoreWebView2CapturePreviewImageFormat::Png, stream);
                    co_await stream.FlushAsync();
                    // Read the PNG IHDR dimensions without decoding or re-encoding any pixels.
                    DataReader header(stream.GetInputStreamAt(0));
                    header.ByteOrder(ByteOrder::BigEndian);
                    if (co_await header.LoadAsync(24) != 24 || header.ReadUInt32() != 0x89504e47 ||
                        header.ReadUInt32() != 0x0d0a1a0a || header.ReadUInt32() != 13 || header.ReadUInt32() != 0x49484452)
                        throw hresult_error(E_FAIL);
                    width = header.ReadUInt32();
                    height = header.ReadUInt32();
                    if (!width || !height) throw hresult_error(E_FAIL);
                    header.Close();
                }
                stream.Close();
                if (width && canCapture(surface, visibilityEpoch) && co_await surface.CoreWebView2().ExecuteScriptAsync(inGamepad(selector.c_str())) == L"true" &&
                    canCapture(surface, visibilityEpoch)) {
                    co_await temporary.RenameAsync(filename, NameCollisionOption::ReplaceExisting);
                    temporary = nullptr;
                    state = L"saved";
                }
            }
        } catch (...) { state = L"error"; }
        if (temporary) {
            try { co_await temporary.DeleteAsync(StorageDeleteOption::PermanentDelete); } catch (...) {}
        }
        capturePending = false;
        if (!closed) {
            slim_lock_guard lock(gate);
            const std::wstring method = libraryOnly ? L"capture-library-result" : L"capture-surface-result";
            captureResult = hstring(L"{\"method\":\"" + method + L"\",\"state\":\"" + state +
                L"\",\"width\":" + std::to_wstring(state == L"saved" ? width : 0) +
                L",\"height\":" + std::to_wstring(state == L"saved" ? height : 0) +
                L",\"viewId\":" + std::to_wstring(sourceView) + L",\"browserPid\":" + std::to_wstring(browserPid) + L"}");
        }
    }

    void closePopup() {
        frameOpen = false;
        ++frameGeneration;
        ++popupVisibilityEpoch;
        auto closing = std::exchange(popup, nullptr);
        if (closing) closing.Close();
        if (popupSurface) root.Children().Remove(popupSurface);
        popupSurface = nullptr;
        popupAttached = false;
        applyVisibility();
        publishWindow();
    }

    fire_and_forget measureSurface(CoreWebView2CompositionController surface, uint64_t generation) {
        using Windows::Data::Json::JsonArray;
        auto lifetime = get_strong();
        apartment_context ui;
        try {
            // Read only fixed numeric geometry after the real gamepad surface
            // has mounted. Verify a mismatch twice before adjusting its backing.
            float firstCorrection = 1.f;
            for (int sample = 0; sample < 3; ++sample) {
                if (sample) {
                    co_await resume_after(std::chrono::milliseconds(180));
                    co_await ui;
                }
                auto current = [&] {
                    return !closed && frameOpen && generation == frameGeneration && !gameForeground &&
                        surface == gamepadSurface() && gamepadVisible() && window.Visible();
                };
                if (!current()) co_return;
                auto raw = co_await web.ExecuteScriptAsync(inGamepad(L"[innerWidth,innerHeight,Math.round(window.devicePixelRatio*1000)]"));
                JsonArray metrics{nullptr};
                if (!current() || !JsonArray::TryParse(raw, metrics) || metrics.Size() != 3) co_return;
                const auto cssWidth = metrics.GetNumberAt(0), cssHeight = metrics.GetNumberAt(1), dpr = metrics.GetNumberAt(2);
                if (!(cssWidth > 0 && cssWidth <= 16384 && cssHeight > 0 && cssHeight <= 16384 && dpr > 0 && dpr <= 16000)) co_return;
                const auto bounds = surface.Bounds();
                const auto correction = steam_display::overrunCorrection(bounds.Width, bounds.Height,
                    static_cast<float>(cssWidth * dpr / 1000), static_cast<float>(cssHeight * dpr / 1000));
                if (xboxDisplay && backingCorrection == 1.f && correction < 1.f) {
                    if (!sample) { firstCorrection = correction; continue; }
                    if (sample == 1 && std::abs(correction - firstCorrection) < .005f) {
                        backingCorrection = correction;
                        ++popupVisibilityEpoch;
                        resize();
                        continue;
                    }
                }
                const auto display = Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
                const auto view = window.Bounds();
                // Protocol order is shared with the trusted WWA report. Scales
                // use fixed-point thousandths; lengths are read from their owner.
                const double values[] = {view.Width, view.Height, static_cast<double>(display.ScreenWidthInRawPixels()),
                    static_cast<double>(display.ScreenHeightInRawPixels()), display.RawPixelsPerViewPixel() * 1000,
                    bounds.Width, bounds.Height, surface.RasterizationScale() * 1000, cssWidth, cssHeight, dpr,
                    placement.pixelsPerDip * 1000};
                std::wstring result = L"{\"method\":\"surface-sizing\",\"values\":[";
                for (auto value : values) {
                    if (!std::isfinite(value) || value < 0 || value > 16384) co_return;
                    if (result.back() != L'[') result += L",";
                    result += std::to_wstring(std::lround(value));
                }
                slim_lock_guard lock(gate);
                if (requests.size() < maxPending) requests.emplace_back(result + L"]}");
                co_return;
            }
        } catch (hresult_error const&) { /* The surface can close while it is being measured. */ }
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
            const tour = target?.querySelector('._16IUnAiPD_1s1KQ0g8vJpq');
            if (visible(tour) && tour.textContent.trim()) return 1;
            if (target?.contains(shell) && shell?.classList.contains('_3LYP1SIxyoky-yW9Ug_1fq') &&
                visible(shell.querySelector('.ZBBhezbBR4VJYysmVhF3_, ._2_cRkGdbegoIbiRsy_-I7'))) return 2;
            const library = target?.querySelector('#GamepadUI_Full_Root._3LYP1SIxyoky-yW9Ug_1fq #Main [role=tabpanel][id$="AllGames_Content"] [role=grid]');
            if (visible(library) && visible(library.querySelector('[role=gridcell] [role=link]'))) return 2;
            const connect = target?.querySelector('._1wlvJjVURYzQHWJlITdVCA button[role=menuitem]._21kxLhOa-E8amdNn4Q_zBY');
            if (visible(connect) && !connect.disabled && connect.querySelector('svg')) return 1;
            const choices = target?.querySelectorAll('._2RjHR32VwDeGwslJx6LcCN[role=listbox] .zzXtdadESGEkT4IFqLvjm[role=option][lang]');
            if (choices && new Set([...choices].filter(el => visible(el) && el.textContent.trim()).map(el => el.lang)).size > 1) return 1;
            if (!target) return 3;
            if (!target.firstElementChild) return 4;
            if (target.innerText.includes('Error Reference:')) return 5;
            if (target.querySelector('[role=grid]')) return 6;
            return target.innerText.trim() ? 7 : 8;
        })())JS";
        auto lifetime = get_strong();
        apartment_context ui;
        const auto generation = frameGeneration;
        try {
            bool setup = false;
            bool measured = false;
            hstring previous;
            for (int sample = 0; sample < 240 || setup;) {
                co_await resume_after(std::chrono::milliseconds(setup ? 1500 : 500));
                co_await ui;
                if (closed || gamepadSurface() != browser || generation != frameGeneration) co_return;
                const bool layoutOnly = warming;
                auto foreground = [&] {
                    return !gameForeground && window.Visible() && (!document->signedMode ||
                        (gamepadVisible() && window.ActivationMode() == CoreWindowActivationMode::ActivatedInForeground));
                };
                if (!layoutOnly && !foreground()) continue;
                if (!setup) ++sample;
                auto result = co_await browser.CoreWebView2().ExecuteScriptAsync(inGamepad(check, layoutOnly));
                if (authenticatedEmptyLibrary && result != L"1" && result != L"2" &&
                    co_await browser.CoreWebView2().ExecuteScriptAsync(inGamepad(steam_capture::emptyLibrary, layoutOnly)) == L"true")
                    result = L"2";
                if (closed || gamepadSurface() != browser || generation != frameGeneration) co_return;
                if (layoutOnly) {
                    if (warming && (result == L"1" || result == L"2"))
                        checkpoint(result == L"1" ? L"warmup-setup-layout" : L"warmup-gamepad-layout");
                    continue; // Never turn a detached document into a foreground readiness signal.
                }
                if (warming || !foreground()) continue;
                if (frameOpen && !measured && (result == L"1" || result == L"2")) {
                    measured = true;
                    measureSurface(browser, generation);
                }
                if (result != previous && result.size() == 1 && result[0] >= L'1' && result[0] <= L'8') {
                    previous = result;
                    slim_lock_guard lock(gate);
                    if (requests.size() < maxPending) requests.push_back(hstring(L"{\"method\":\"surface-state\",\"state\":" + std::wstring(result) + L"}"));
                }
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
            if (!closed && gamepadSurface() == browser && generation == frameGeneration) checkpoint(L"gamepad-observation-error:" + std::to_wstring(to_hresult().value));
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
            if (!document->signedMode && !popup && !openingPopup && document->accepts(sender.Source()) && source &&
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
            using Windows::UI::Xaml::Hosting::ElementCompositionPreview;
            if (primaryPanel) compositor = ElementCompositionPreview::GetElementVisual(primaryPanel).Compositor();
            else {
                compositor = Compositor();
                target = compositor.CreateTargetForCurrentView();
            }
            root = compositor.CreateContainerVisual();
            if (primaryPanel) ElementCompositionPreview::SetElementChildVisual(primaryPanel, root);
            else target.Root(root);
            mainSurface = compositor.CreateContainerVisual();
            root.Children().InsertAtBottom(mainSurface);

            step = L"environment";
            checkpoint(step);
            auto cache = Windows::Storage::ApplicationData::Current().LocalCacheFolder();
            auto profile = cache.Path() + L"\\WebView2";
            if (!account.empty()) {
                auto profiles = co_await cache.CreateFolderAsync(L"SteamWebProfiles", Windows::Storage::CreationCollisionOption::OpenIfExists);
                profile = profiles.Path() + L"\\" + account;
            }
            environment = co_await CoreWebView2Environment::CreateWithOptionsAsync(L"", profile, nullptr);
            if (closed) co_return;

            step = L"controller";
            checkpoint(step);
            auto parent = CoreWebView2ControllerWindowReference::CreateFromCoreWindow(window);
            auto browser = co_await environment.CreateCoreWebView2CompositionControllerAsync(parent);
            if (closed) { browser.Close(); co_return; }
            controller = browser;
            try {
                xboxDisplay = Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() == L"Windows.Xbox";
            } catch (hresult_error const&) { xboxDisplay = false; }
            step = L"surface";
            checkpoint(step);
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
                else {
                    cloudCache->cancel();
                    if (store) { store->close(); store.reset(); }
                }
            });
            web.ContentLoading([this](auto const&, auto const& args) {
                document->content(args.NavigationId(), args.IsErrorPage());
            });
            web.FrameNavigationStarting([this](auto const&, auto const& args) {
                // The signed gamepad and its nested menus need only their
                // initial inherited blank DOM created by the pinned owner.
                if (document->signedMode && args.Uri() != L"about:blank") args.Cancel(true);
            });
            web.AddWebResourceRequestedFilter(L"*", CoreWebView2WebResourceContext::Script);
            web.WebResourceRequested([this](auto const&, CoreWebView2WebResourceRequestedEventArgs const& args) {
                if (document->signedMode && args.ResourceContext() == CoreWebView2WebResourceContext::Script &&
                    !std::wstring_view(args.Request().Uri()).starts_with(L"https://steamloopback.host/")) {
                    args.Response(environment.CreateWebResourceResponse(nullptr, 403, L"Forbidden", L""));
                }
            });
            web.NewWindowRequested({this, &View::onPopup});
            web.WebMessageReceived([this](auto const& sender, auto const& args) {
                // Frame messages have a separate WebView2Frame event. The bridge
                // only accepts bounded top-level messages from this exact document.
                if (!document->accepts(sender.Source()) || !document->accepts(args.Source())) return;
                auto message = args.WebMessageAsJson();
                if (cloudCache->consume(message)) return;
                if (message.empty() || message.size() > 4096 || handleWindowCommand(message)) return;
                Windows::Data::Json::JsonObject parsed{nullptr};
                if (!Windows::Data::Json::JsonObject::TryParse(message, parsed)) return;
                const auto method = parsed.GetNamedString(L"method", L"");
                if (method == L"store-browser") { storeCommand(parsed); return; }
                if (method == L"capture-library-result" || method == L"capture-surface-result" || method == L"surface-sizing") return;
                message = parsed.Stringify(); // WWA receives exactly the object validated above.
                slim_lock_guard lock(gate);
                if (requests.size() < maxPending) requests.push_back(message);
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
            document->signedMode = !adapter.empty();
            if (document->signedMode && account.empty()) throw hresult_access_denied();
            auto seed = L"const xboxNativeWindow = Object.freeze(Object.assign(" + windowState() + L"," + platformState() +
                L",{framedGamepad:" + (document->signedMode ? L"true" : L"false") + L"}));\n";
            co_await web.AddScriptToExecuteOnDocumentCreatedAsync(script + hstring(seed) + adapter + L"\n}");
            if (closed) co_return;
            web.Navigate(document->signedMode ? signedEntry : entry);
            if (!warming) controller.MoveFocus(CoreWebView2MoveFocusReason::Programmatic);
        } catch (...) {
            if (!closed) checkpoint(std::wstring(step) + L"-error:" + std::to_wstring(to_hresult().value));
        }
    }

    void listen() {
        auto pointer = [this](auto const&, PointerEventArgs const& args) { mouse(args, CoreWebView2MouseEventKind::Move); };
        moved = window.PointerMoved(auto_revoke, pointer);
        pressed = window.PointerPressed(auto_revoke, pointer);
        released = window.PointerReleased(auto_revoke, pointer);
        wheel = window.PointerWheelChanged(auto_revoke, [this](auto const&, PointerEventArgs const& args) { mouse(args, CoreWebView2MouseEventKind::Wheel); });
        sized = window.SizeChanged(auto_revoke, {this, &View::onSize});
        visibility = window.VisibilityChanged(auto_revoke, [this](auto const&, auto const&) { applyVisibility(); publishWindow(); });
        closing = window.Closed(auto_revoke, {this, &View::onClosed});
        activated = window.Activated(auto_revoke, {this, &View::onActivated});
    }
    void Run() {
        try {
            { slim_lock_guard lock(gate); if (closeRequested) return; }
            listen();
            window.Activate();
            startBrowser();
            window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessUntilQuit);
        } catch (...) { checkpoint(L"view-error:" + std::to_wstring(to_hresult().value)); }
    }
    void onSize(CoreWindow const&, WindowSizeChangedEventArgs const&) { resize(); publishWindow(); }
    void onClosed(CoreWindow const&, CoreWindowEventArgs const&) { close(); }
    void onActivated(CoreWindow const&, WindowActivatedEventArgs const& args) {
        if (args.WindowActivationState() == CoreWindowActivationState::Deactivated) ++popupVisibilityEpoch;
        updateShellCursor(!warming && !gameForeground && window.Visible() &&
            args.WindowActivationState() != CoreWindowActivationState::Deactivated);
        if (controller && args.WindowActivationState() != CoreWindowActivationState::Deactivated)
            focusVisible();
        publishWindow();
    }
    void close() {
        if (closed) return;
        auto lifetime = get_strong();
        closed = true;
        updateShellCursor(false);
        if (cloudCache) cloudCache->cancel();
        if (store) { store->close(); store.reset(); }
        closePopup();
        if (controller) controller.Close();
        web = nullptr;
        controller = nullptr;
        if (primaryPanel) {
            showPrimaryShell(false);
            Windows::UI::Xaml::Hosting::ElementCompositionPreview::SetElementChildVisual(primaryPanel, nullptr);
            Uninitialize();
        } else window.Dispatcher().StopProcessEvents();
    }
    void Uninitialize() {
        closed = true;
        slim_lock_guard lock(gate);
        viewId = 0;
        active = {};
        dispatcher = nullptr;
        secondary = nullptr;
        primaryOwner = nullptr;
        captureResult = hstring{};
        opening = false;
        openingAccount = hstring{};
        closeRequested = false;
        requests.clear();
    }
};

struct Source : implements<Source, IFrameworkViewSource> {
    explicit Source(bool warm, hstring const& owner) : warming(warm), account(owner) {}
    bool warming;
    hstring const account;
    IFrameworkView CreateView() { return make<View>(warming, account); }
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
    void Open() { openView(false); }
    void OpenForWarmup() { openView(true); }
    static void validateAccount(hstring const& owner) {
        std::wstring_view id(owner);
        if (id.size() < 17 || id.size() > 20 || id.front() == L'0' ||
            id.find_first_not_of(L"0123456789") != id.npos ||
            (id.size() == 20 && id > L"18446744073709551615")) throw hresult_invalid_argument();
    }
    void OpenForAccount(hstring const& owner) { validateAccount(owner); openView(true, owner); }
    void OpenForPrimaryAccount(hstring const& owner) { validateAccount(owner); openView(true, owner, true); }
    static void startPrimary(hstring const& owner) {
        std::wstring_view step = L"primary-window";
        try {
            using namespace Windows::UI::Xaml;
            auto window = Window::Current();
            if (!window) throw hresult_error(E_UNEXPECTED);
            step = L"primary-root";
            auto grid = window.Content().try_as<Controls::Grid>();
            if (!grid || grid.Children().Size() != 2) throw hresult_error(E_UNEXPECTED);
            step = L"primary-account";
            auto account = grid.Children().GetAt(0).try_as<Controls::WebView>();
            if (!account || account.Name() != L"SteamAccountHost") throw hresult_access_denied();
            step = L"primary-panel";
            auto panel = grid.Children().GetAt(1).try_as<Controls::Canvas>();
            if (!panel || panel.Name() != L"SteamCompositionHost") throw hresult_access_denied();

            // The signed primary host records the exact successful top-level
            // navigation on this control, revoking it when navigation begins.
            step = L"primary-account-document";
            auto documentUri = account.Tag().try_as<Uri>();
            Uri local{L"ms-appx-web:///steam-signin.html"};
            if (!documentUri || documentUri.RawUri() != local.RawUri()) throw hresult_access_denied();
            step = L"primary-renderer";
            auto view = make_self<View>(true, owner);
            {
                slim_lock_guard lock(gate);
                if (closeRequested) { opening = closeRequested = false; return; }
                primaryOwner = view.as<IFrameworkView>();
            }
            view->primaryPanel = panel;
            view->accountPanel = account;
            step = L"primary-corewindow";
            view->SetWindow(window.CoreWindow());
            step = L"primary-events";
            view->listen();
            step = L"primary-browser";
            view->startBrowser();
        } catch (...) {
            auto hr = to_hresult();
            slim_lock_guard lock(gate);
            opening = false;
            status = hstring(std::wstring(step) + L"-error:" + std::to_wstring(hr.value));
        }
    }
    void openView(bool warm, hstring const& owner = {}, bool primary = false) {
        {
            slim_lock_guard lock(gate);
            if (opening && (closeRequested || openingAccount != owner)) throw hresult_error(HRESULT_FROM_WIN32(ERROR_BUSY));
            if (opening) return;
            opening = true;
            openingAccount = owner;
            status = L"creating-view";
        }
        try {
            if (primary) {
                auto main = CoreApplication::MainView().CoreWindow().Dispatcher();
                main.RunAsync(CoreDispatcherPriority::Normal, [owner] { startPrimary(owner); });
                return;
            }
            auto newView = CoreApplication::CreateNewView(make<Source>(warm, owner));
            slim_lock_guard lock(gate);
            if (opening) secondary = newView;
        }
        catch (...) { slim_lock_guard lock(gate); opening = false; throw; }
    }
    int32_t ViewId() { slim_lock_guard lock(gate); return viewId; }
    hstring Status() { slim_lock_guard lock(gate); return status; }
    hstring Read() {
        slim_lock_guard lock(gate);
        if (!captureResult.empty()) return std::exchange(captureResult, hstring{});
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
            if (message == L"{\"nativeWarmupComplete\":true}") {
                if (view.warming && !view.closed) {
                    view.warming = false;
                    view.applyVisibility();
                    view.publishWindow();
                    if (view.controller) view.focusVisible();
                }
            } else if (message == L"{\"nativeEmptyLibrary\":true}") view.authenticatedEmptyLibrary = !view.account.empty();
            else if (message == L"{\"nativeEmptyLibrary\":false}") view.authenticatedEmptyLibrary = false;
            else if (message == L"{\"nativeGameForeground\":true}") view.setGameForeground(true);
            else if (message == L"{\"nativeGameForeground\":false}") view.setGameForeground(false);
            else if (message == L"{\"nativeRestartClient\":true}") view.restartClient();
            else if (message == L"{\"nativeCaptureLibrary\":true}") view.captureGamepad(true);
            else if (message == L"{\"nativeCaptureSurface\":true}") view.captureGamepad(false);
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
