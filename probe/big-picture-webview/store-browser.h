#pragma once
#include <cmath>
#include <deque>
#include <functional>
#include <memory>
#include <winrt/Windows.Data.Json.h>
#include "display-layout.h"
#include "document-address.h"

// The public Store has its own Chromium controller. It receives no signed
// local scripts, virtual host mappings, host objects or inbound message handler.
namespace steam_store {
using namespace winrt;
using namespace Microsoft::Web::WebView2::Core;
using namespace Windows::Data::Json;
using namespace Windows::UI::Composition;
using namespace Windows::UI::Core;

// Valve supplies this exact, inert initial page to MainBrowser. A new
// controller can already be at about:blank before event handlers are installed.
inline constexpr wchar_t initialPage[] = L"data:text/html,%3Cbody%3E%3C%2Fbody%3E";

inline bool allowed(hstring const& raw, bool resource = false) noexcept {
    try {
        Windows::Foundation::Uri uri(raw);
        if (uri.SchemeName() != L"https" || uri.Port() != 443 || !uri.UserName().empty() || !uri.Password().empty()) return false;
        auto name = uri.Host();
        std::wstring_view host(name);
        if (host == L"store.steampowered.com") return true;
        return resource && (host == L"api.steampowered.com" || host == L"checkout.steampowered.com" ||
            host == L"steamstatic.com" || host.ends_with(L".steamstatic.com") ||
            host == L"steamusercontent.com" || host.ends_with(L".steamusercontent.com") ||
            host == L"steamcdn-a.akamaihd.net");
    } catch (...) { return false; }
}

class Browser : public std::enable_shared_from_this<Browser> {
    unsigned id;
    CoreWebView2 owner;
    std::shared_ptr<steam_document::Owner> document;
    std::function<void(bool, int, bool)> reportNavigation;
    std::function<void(wchar_t const*, int)> reportStage;
    ContainerVisual parent, surface;
    CoreWebView2CompositionController controller{nullptr};
    Windows::Foundation::Rect bounds{};
    steam_display::Placement placement{{},1.f};
    double dpi = 1.;
    bool requested = false, shell = false, stopped = false, booted = false, focused = false, attached = false;
    hstring pendingUrl, navigating;
    uint64_t navigationId = 0;
    std::deque<hstring> keys;
    bool sendingKeys = false;

    void emit(wchar_t const* kind, JsonObject data = {}) const {
        if (stopped || !document->accepts(owner.Source())) return;
        data.SetNamedValue(L"event", JsonValue::CreateStringValue(L"store-browser"));
        data.SetNamedValue(L"id", JsonValue::CreateNumberValue(id));
        data.SetNamedValue(L"kind", JsonValue::CreateStringValue(kind));
        owner.PostWebMessageAsJson(data.Stringify());
    }
    void error(int32_t code) const {
        reportStage(L"error", code);
        JsonObject data;
        data.SetNamedValue(L"code",JsonValue::CreateNumberValue(code));
        emit(L"error",data);
    }
    void apply() {
        if (!controller || stopped) return;
        auto w = std::round(placement.pixelWidth() * bounds.Width);
        auto h = std::round(placement.pixelHeight() * bounds.Height);
        bool show = requested && shell && w > 0 && h > 0;
        controller.IsVisible(show);
        if (!show) {
            if (attached) { controller.RootVisualTarget(nullptr); attached = false; reportStage(L"hidden", 0); }
            return;
        }
        controller.BoundsMode(CoreWebView2BoundsMode::UseRawPixels);
        controller.ShouldDetectMonitorScaleChanges(false);
        controller.RasterizationScale(dpi);
        controller.Bounds({0,0,w,h});
        surface.Offset({placement.view.x + bounds.X * placement.view.width,
                        placement.view.y + bounds.Y * placement.view.height, 0});
        surface.Size({w,h});
        surface.Scale({placement.visualScaleX(),placement.visualScaleY(),1.f});
        if (!attached) { controller.RootVisualTarget(surface); attached = true; reportStage(L"visible", 0); }
    }
    fire_and_forget dispatchKeys() {
        auto self = shared_from_this();
        sendingKeys = true;
        try {
            while (!keys.empty() && !stopped && controller && requested && shell && focused) {
                auto params = std::move(keys.front());
                keys.pop_front();
                co_await controller.CoreWebView2().CallDevToolsProtocolMethodAsync(L"Input.dispatchKeyEvent", params);
            }
        } catch (...) {}
        keys.clear();
        sendingKeys = false;
    }
    void input(hstring const& key, bool down) {
        if (!controller || !controller.IsVisible() || !focused || keys.size() >= 32) return;
        struct Binding { const wchar_t* wire; const wchar_t* name; int vk; };
        static constexpr Binding bindings[] = {
            {L"enter",L"Enter",13},{L"back",L"Escape",27},{L"left",L"ArrowLeft",37},
            {L"up",L"ArrowUp",38},{L"right",L"ArrowRight",39},{L"down",L"ArrowDown",40}};
        for (auto const& button : bindings) if (key == button.wire) {
            JsonObject params;
            params.SetNamedValue(L"type",JsonValue::CreateStringValue(down ? L"keyDown" : L"keyUp"));
            params.SetNamedValue(L"key",JsonValue::CreateStringValue(button.name));
            params.SetNamedValue(L"code",JsonValue::CreateStringValue(button.name));
            params.SetNamedValue(L"windowsVirtualKeyCode",JsonValue::CreateNumberValue(button.vk));
            keys.push_back(params.Stringify());
            if (!sendingKeys) dispatchKeys();
            break;
        }
    }
public:
    Browser(unsigned clientId, CoreWebView2 const& host, std::shared_ptr<steam_document::Owner> doc,
            ContainerVisual const& root, std::function<void(bool, int, bool)> report,
            std::function<void(wchar_t const*, int)> progress) : id(clientId), owner(host),
            document(std::move(doc)), reportNavigation(std::move(report)), reportStage(std::move(progress)), parent(root),
            surface(root.Compositor().CreateContainerVisual()) { parent.Children().InsertAtTop(surface); }
    void layout(steam_display::Placement value, double scale, bool visible) {
        placement = value; dpi = scale; shell = visible; apply();
    }
    void close() {
        if (stopped) return;
        emit(L"closed"); stopped = true;
        if (controller) controller.Close();
        controller = nullptr;
        parent.Children().Remove(surface);
    }
    fire_and_forget start(CoreWebView2Environment env, CoreWindow window) {
        auto self = shared_from_this();
        try {
            reportStage(L"creating-controller", 0);
            auto result = co_await env.CreateCoreWebView2CompositionControllerAsync(
                CoreWebView2ControllerWindowReference::CreateFromCoreWindow(window));
            if (stopped) { result.Close(); co_return; }
            controller = result;
            reportStage(L"configuring-controller", 0);
            auto page = result.CoreWebView2();
            auto settings = page.Settings();
            settings.AreHostObjectsAllowed(false); settings.IsWebMessageEnabled(false);
            settings.AreDefaultContextMenusEnabled(false); settings.AreDevToolsEnabled(false);
            settings.UserAgent(settings.UserAgent() + L" Valve Steam Gamepad");
            page.NewWindowRequested([](auto const& sender, auto const& e) {
                e.Handled(true);
                if (e.IsUserInitiated() && allowed(e.Uri())) sender.Navigate(e.Uri());
            });
            page.PermissionRequested([](auto const&, auto const& e) { e.State(CoreWebView2PermissionState::Deny); });
            page.DownloadStarting([](auto const&, auto const& e) { e.Cancel(true); });
            page.AddWebResourceRequestedFilter(L"*",CoreWebView2WebResourceContext::All);
            page.WebResourceRequested([env](auto const&, auto const& e) {
                auto url = e.Request().Uri();
                if (url == L"about:blank" || std::wstring_view(url).starts_with(L"data:") ||
                    std::wstring_view(url).starts_with(L"blob:https://store.steampowered.com/") || allowed(url,true)) return;
                e.Response(env.CreateWebResourceResponse(nullptr,403,L"Forbidden",L""));
            });
            auto weak = weak_from_this();
            page.FrameNavigationStarting([](auto const&, auto const& e) {
                if (e.Uri() != L"about:blank" && !allowed(e.Uri(),true)) e.Cancel(true);
            });
            page.NavigationStarting([weak](auto const&, auto const& e) {
                auto s = weak.lock();
                if (!s || s->stopped || (e.Uri() != initialPage && e.Uri() != L"about:blank" && !allowed(e.Uri()))) { e.Cancel(true); return; }
                if (e.Uri() == L"about:blank") return;
                s->navigationId = e.NavigationId(); s->navigating = e.Uri();
                if (allowed(e.Uri())) s->reportNavigation(false, 0, false);
                else s->reportStage(L"bootstrap-started", 0);
                JsonObject data; data.SetNamedValue(L"url",JsonValue::CreateStringValue(e.Uri() == initialPage ? hstring(L"about:blank") : e.Uri())); s->emit(L"start",data);
            });
            page.NavigationCompleted([weak](auto const& sender, auto const& e) {
                auto s = weak.lock();
                if (!s || s->stopped || e.NavigationId() != s->navigationId) return;
                JsonObject data;
                const bool blank = s->navigating == initialPage;
                data.SetNamedValue(L"url",JsonValue::CreateStringValue(blank ? hstring(L"about:blank") : s->navigating));
                int http = 0;
                if (!blank) try { http = e.HttpStatusCode(); }
                catch (hresult_error const& hr) { s->reportStage(L"http-status-unavailable", hr.code().value); }
                const bool ok = e.IsSuccess() && (blank || (allowed(sender.Source()) && http >= 200 && http < 300));
                data.SetNamedValue(L"success",JsonValue::CreateBooleanValue(ok));
                data.SetNamedValue(L"code",JsonValue::CreateNumberValue(http > 0 ? http : static_cast<int>(e.WebErrorStatus())));
                if (!blank) s->reportNavigation(true, http, ok);
                s->emit(L"complete",data);
                if (blank) s->reportStage(ok ? L"bootstrap-complete" : L"bootstrap-failed", static_cast<int>(e.WebErrorStatus()));
                if (!s->booted && blank && ok) {
                    s->booted = true;
                    if (!s->pendingUrl.empty()) sender.Navigate(std::exchange(s->pendingUrl, hstring{}));
                }
            });
            page.DocumentTitleChanged([weak](auto const& sender, auto const&) {
                if (auto s = weak.lock()) {
                    JsonObject data; data.SetNamedValue(L"title",JsonValue::CreateStringValue(sender.DocumentTitle()));
                    s->emit(L"title",data);
                }
            });
            page.ProcessFailed([weak](auto const&, auto const& e) {
                if (auto s = weak.lock(); s && !s->stopped)
                    s->reportStage(L"browser-process-failed", static_cast<int>(e.ProcessFailedKind()));
            });
            page.HistoryChanged([weak](auto const& sender, auto const&) {
                if (auto s = weak.lock()) {
                    JsonObject data; data.SetNamedValue(L"canGoBack",JsonValue::CreateBooleanValue(sender.CanGoBack()));
                    data.SetNamedValue(L"canGoForward",JsonValue::CreateBooleanValue(sender.CanGoForward()));
                    s->emit(L"history",data);
                }
            });
            apply();
            reportStage(L"opening-bootstrap", 0);
            page.Navigate(initialPage);
        } catch (...) { if (!stopped) error(to_hresult().value); }
    }
    void command(unsigned target, hstring const& action, JsonObject const& data) {
        if (target != id || stopped) return;
        if (action == L"navigate") {
            auto url = data.GetNamedString(L"url");
            if (!allowed(url)) { error(E_INVALIDARG); return; }
            reportStage(L"public-requested", 0);
            if (booted) controller.CoreWebView2().Navigate(url);
            else pendingUrl = url;
        } else if (action == L"bounds") {
            auto fraction = [&](const wchar_t* field) {
                double n = data.GetNamedNumber(field);
                return std::isfinite(n) ? static_cast<float>(std::clamp(n, 0., 1.)) : 0.f;
            };
            auto x = fraction(L"x"), y = fraction(L"y");
            bounds = {x,y,std::min(1.f-x,fraction(L"width")),std::min(1.f-y,fraction(L"height"))}; apply();
        } else if (action == L"visible") { requested = data.GetNamedBoolean(L"value"); apply(); }
        else if (action == L"focus") {
            focused = data.GetNamedBoolean(L"value");
            JsonObject state; state.SetNamedValue(L"value",JsonValue::CreateBooleanValue(focused && requested && shell));
            emit(L"focus",state);
        } else if (action == L"input") input(data.GetNamedString(L"key"),data.GetNamedBoolean(L"down"));
        else if (controller) {
            auto page = controller.CoreWebView2();
            if (action == L"back" && page.CanGoBack()) page.GoBack();
            if (action == L"forward" && page.CanGoForward()) page.GoForward();
            if (action == L"reload") page.Reload();
        }
    }
    unsigned identifier() const { return id; }
};
}
