#pragma once
#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <iterator>
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

// Distinct from every external origin and from the controller's implicit
// about:blank. WebResourceRequested serves only this exact empty HTML page.
inline constexpr wchar_t initialPage[] = L"https://xbox-store-bootstrap.invalid/index.html";

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
    CoreWebView2Environment environment{nullptr};
    CoreWindow window{nullptr};
    Windows::Foundation::Rect bounds{};
    steam_display::Placement placement{{},1.f};
    double dpi = 1.;
    bool requested = false, shell = false, stopped = false, booted = false, focused = false, attached = false;
    hstring pendingUrl, expectedUrl, navigating;
    uint64_t navigationId = 0, controllerGeneration = 0;
    unsigned requestId = 0, navigationRequest = 0;
    bool replaceController = false;
    struct Binding { const wchar_t* wire; const wchar_t* name; int vk; };
    static constexpr Binding bindings[] = {
        {L"enter",L"Enter",13},{L"back",L"Escape",27},{L"left",L"ArrowLeft",37},
        {L"up",L"ArrowUp",38},{L"right",L"ArrowRight",39},{L"down",L"ArrowDown",40}};
    struct KeyEdge { size_t index; bool down; uint64_t epoch; };
    std::deque<KeyEdge> keys;
    std::array<bool, std::size(bindings)> acceptedKeys{}, dispatchedKeys{};
    uint64_t inputEpoch = 0;
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
        data.SetNamedValue(L"request",JsonValue::CreateNumberValue(requestId));
        emit(L"error",data);
    }
    void navigate(CoreWebView2 const& page, hstring const& url) {
        navigationId = 0;
        expectedUrl = url;
        page.Navigate(url);
    }
    void apply() {
        if (!controller || stopped) return;
        auto w = std::round(placement.pixelWidth() * bounds.Width);
        auto h = std::round(placement.pixelHeight() * bounds.Height);
        bool show = requested && shell && w > 0 && h > 0;
        if (!show) {
            releaseKeys();
            controller.IsVisible(false);
            if (attached) { controller.RootVisualTarget(nullptr); attached = false; reportStage(L"hidden", 0); }
            return;
        }
        controller.IsVisible(true);
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
        const auto generation = controllerGeneration;
        sendingKeys = true;
        while (!keys.empty() && !stopped && generation == controllerGeneration) {
            const auto edge = keys.front();
            keys.pop_front();
            try {
                if (edge.down && (edge.epoch != inputEpoch || !requested || !shell || !focused || !controller.IsVisible()))
                    continue;
                // A release belongs to the controller that received the down.
                // It still needs to run after the Steam surface hides or blurs.
                if (dispatchedKeys[edge.index] == edge.down) continue;
                const auto& button = bindings[edge.index];
                JsonObject params;
                params.SetNamedValue(L"type",JsonValue::CreateStringValue(edge.down ? L"keyDown" : L"keyUp"));
                params.SetNamedValue(L"key",JsonValue::CreateStringValue(button.name));
                params.SetNamedValue(L"code",JsonValue::CreateStringValue(button.name));
                params.SetNamedValue(L"windowsVirtualKeyCode",JsonValue::CreateNumberValue(button.vk));
                auto page = controller.CoreWebView2();
                dispatchedKeys[edge.index] = edge.down;
                co_await page.CallDevToolsProtocolMethodAsync(L"Input.dispatchKeyEvent", params.Stringify());
            } catch (...) { /* Continue draining releases for the current controller. */ }
        }
        if (generation == controllerGeneration) sendingKeys = false;
    }
    void releaseKeys() {
        ++inputEpoch;
        for (size_t i = 0; i < acceptedKeys.size(); ++i) {
            if (!std::exchange(acceptedKeys[i], false)) continue;
            keys.push_back({i, false, inputEpoch});
        }
        if (controller && !keys.empty() && !sendingKeys) dispatchKeys();
    }
    void input(hstring const& key, bool down) {
        if (!controller || (down && (!requested || !shell || !focused || !controller.IsVisible() || keys.size() >= 32))) return;
        for (size_t i = 0; i < acceptedKeys.size(); ++i) if (key == bindings[i].wire) {
            if (acceptedKeys[i] == down) return;
            acceptedKeys[i] = down;
            keys.push_back({i, down, inputEpoch});
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
    fire_and_forget start(CoreWebView2Environment env, CoreWindow coreWindow) {
        auto self = shared_from_this();
        environment = env;
        window = coreWindow;
        const auto generation = ++controllerGeneration;
        try {
            reportStage(L"creating-controller", 0);
            auto result = co_await env.CreateCoreWebView2CompositionControllerAsync(
                CoreWebView2ControllerWindowReference::CreateFromCoreWindow(coreWindow));
            if (stopped || generation != controllerGeneration) { result.Close(); co_return; }
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
            auto weak = weak_from_this();
            auto current = [weak, generation]() -> std::shared_ptr<Browser> {
                auto s = weak.lock();
                return s && !s->stopped && s->controllerGeneration == generation ? s : nullptr;
            };
            page.WebResourceRequested([env, current](auto const&, auto const& e) {
                auto url = e.Request().Uri();
                if (url == initialPage) {
                    e.Response(env.CreateWebResourceResponse(nullptr, 200, L"OK",
                        L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nContent-Security-Policy: default-src 'none'"));
                    if (auto s = current()) s->reportStage(L"bootstrap-served", 0);
                    return;
                }
                if (url == L"about:blank" || std::wstring_view(url).starts_with(L"data:") ||
                    std::wstring_view(url).starts_with(L"blob:https://store.steampowered.com/") || allowed(url,true)) return;
                e.Response(env.CreateWebResourceResponse(nullptr,403,L"Forbidden",L""));
            });
            page.FrameNavigationStarting([](auto const&, auto const& e) {
                if (e.Uri() != L"about:blank" && !allowed(e.Uri(),true)) e.Cancel(true);
            });
            page.NavigationStarting([current](auto const&, auto const& e) {
                auto s = current();
                if (!s || s->replaceController) { e.Cancel(true); return; }
                const auto url = e.Uri();
                if (url != initialPage && url != L"about:blank" && !allowed(url)) {
                    if (!s->booted) s->reportStage(L"bootstrap-denied", 0);
                    e.Cancel(true); return;
                }
                if (url == L"about:blank" && !s->booted) {
                    s->reportStage(L"initial-about-blank", 0);
                    return;
                }
                const bool blank = url == initialPage || url == L"about:blank";
                const bool bootstrap = blank && !s->booted;
                if (e.IsRedirected()) {
                    if (!s->navigationId || s->navigationId != e.NavigationId()) { e.Cancel(true); return; }
                } else {
                    if ((!s->booted && !bootstrap) || (!s->expectedUrl.empty() && url != s->expectedUrl)) {
                        e.Cancel(true); return;
                    }
                    s->navigationId = e.NavigationId();
                    s->navigationRequest = bootstrap ? 0 : s->requestId;
                    s->expectedUrl = hstring{};
                }
                s->navigating = url;
                if (bootstrap) s->reportStage(L"bootstrap-started", 0);
                else if (!blank) {
                    s->reportStage(L"public-started", 0);
                    s->reportNavigation(false, 0, false);
                }
                JsonObject data;
                data.SetNamedValue(L"url",JsonValue::CreateStringValue(blank ? hstring(L"about:blank") : url));
                data.SetNamedValue(L"request",JsonValue::CreateNumberValue(s->navigationRequest));
                s->emit(L"start",data);
            });
            page.NavigationCompleted([current](auto const& sender, auto const& e) {
                auto s = current();
                if (!s || !s->navigationId || e.NavigationId() != s->navigationId) return;
                s->navigationId = 0;
                JsonObject data;
                const bool blank = s->navigating == initialPage || s->navigating == L"about:blank";
                const bool bootstrap = blank && !s->booted;
                data.SetNamedValue(L"url",JsonValue::CreateStringValue(blank ? hstring(L"about:blank") : s->navigating));
                data.SetNamedValue(L"request",JsonValue::CreateNumberValue(s->navigationRequest));
                int http = 0;
                if (!blank) try { http = e.HttpStatusCode(); }
                catch (hresult_error const& hr) { s->reportStage(L"http-status-unavailable", hr.code().value); }
                const bool ok = e.IsSuccess() && (blank || (allowed(sender.Source()) && http >= 200 && http < 300));
                data.SetNamedValue(L"success",JsonValue::CreateBooleanValue(ok));
                data.SetNamedValue(L"code",JsonValue::CreateNumberValue(http > 0 ? http : static_cast<int>(e.WebErrorStatus())));
                if (!blank) s->reportNavigation(true, http, ok);
                s->emit(L"complete",data);
                if (bootstrap) s->reportStage(ok ? L"bootstrap-complete" : L"bootstrap-failed", static_cast<int>(e.WebErrorStatus()));
                if (bootstrap && ok) {
                    s->booted = true;
                    if (!s->pendingUrl.empty()) s->navigate(sender, std::exchange(s->pendingUrl, hstring{}));
                }
            });
            page.DocumentTitleChanged([current](auto const& sender, auto const&) {
                if (auto s = current()) {
                    JsonObject data; data.SetNamedValue(L"title",JsonValue::CreateStringValue(sender.DocumentTitle()));
                    s->emit(L"title",data);
                }
            });
            page.ProcessFailed([current](auto const&, auto const& e) {
                if (auto s = current()) {
                    auto kind = e.ProcessFailedKind();
                    s->reportStage(L"browser-process-failed", static_cast<int>(kind));
                    if (kind == CoreWebView2ProcessFailedKind::BrowserProcessExited ||
                        kind == CoreWebView2ProcessFailedKind::RenderProcessExited) {
                        s->navigationId = 0;
                        s->replaceController = true;
                        s->error(E_FAIL);
                    }
                }
            });
            page.HistoryChanged([current](auto const& sender, auto const&) {
                if (auto s = current()) {
                    JsonObject data; data.SetNamedValue(L"canGoBack",JsonValue::CreateBooleanValue(sender.CanGoBack()));
                    data.SetNamedValue(L"canGoForward",JsonValue::CreateBooleanValue(sender.CanGoForward()));
                    s->emit(L"history",data);
                }
            });
            apply();
            // Some Xbox controllers already own a complete empty document and
            // emit no initial navigation events. Inspect only our script-free
            // initial context, before any external navigation is permitted.
            bool hasInitialDocument = false;
            try {
                auto initial = co_await page.ExecuteScriptAsync(
                    L"location.href === 'about:blank' && window === window.top && !window.opener && "
                    L"document.readyState === 'complete' && !!document.body && !document.body.hasChildNodes()");
                hasInitialDocument = initial == L"true";
            } catch (hresult_error const&) {}
            if (!current()) co_return;
            if (hasInitialDocument) {
                booted = true;
                reportStage(L"initial-document-confirmed", 0);
                JsonObject data;
                data.SetNamedValue(L"url", JsonValue::CreateStringValue(L"about:blank"));
                emit(L"initial-document", data);
                if (!pendingUrl.empty()) navigate(page, std::exchange(pendingUrl, hstring{}));
                co_return;
            }
            reportStage(L"opening-bootstrap", 0);
            page.Navigate(initialPage);
        } catch (...) { if (!stopped && generation == controllerGeneration) error(to_hresult().value); }
    }
    void command(unsigned target, hstring const& action, JsonObject const& data) {
        if (target != id || stopped) return;
        if (action == L"navigate") {
            auto url = data.GetNamedString(L"url");
            auto request = data.GetNamedNumber(L"request");
            if (!(request >= 1 && request <= 0x7fffffff && std::floor(request) == request)) return;
            requestId = static_cast<unsigned>(request);
            if (!allowed(url)) { error(E_INVALIDARG); return; }
            reportStage(L"public-requested", 0);
            // WebView2 supplies the native ID only with the first start. A new
            // controller isolates an unseen old start, including same-URL retry.
            if (replaceController || !expectedUrl.empty()) {
                ++controllerGeneration;
                keys.clear();
                acceptedKeys.fill(false);
                dispatchedKeys.fill(false);
                sendingKeys = false;
                if (controller) controller.Close();
                controller = nullptr;
                booted = attached = replaceController = false;
                navigationId = 0;
                expectedUrl = hstring{};
                pendingUrl = url;
                JsonObject state;
                state.SetNamedValue(L"request", JsonValue::CreateNumberValue(requestId));
                emit(L"controller-reset", state);
                start(environment, window);
            } else if (booted) {
                navigate(controller.CoreWebView2(), url);
            }
            else pendingUrl = url;
        } else if (action == L"stop") {
            if (data.GetNamedNumber(L"request") != requestId) return;
            requestId = 0;
            pendingUrl = hstring{};
            if (controller && booted) {
                replaceController = replaceController || !expectedUrl.empty();
                expectedUrl = hstring{};
                navigationId = 0;
                controller.CoreWebView2().Stop();
            }
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
            if (!focused) releaseKeys();
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
