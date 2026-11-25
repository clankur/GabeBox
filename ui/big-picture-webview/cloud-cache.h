#pragma once
#include "document-address.h"
#include <winrt/Microsoft.Web.WebView2.Core.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Storage.Streams.h>
#include <map>
#include <memory>
#include <cmath>
#include <string_view>
#include <utility>

// Serve only the original top-level client's local UI cache. JavaScript checks
// the broker session and reads the matching account's IndexedDB transaction.
class SteamUiCloudCache : public std::enable_shared_from_this<SteamUiCloudCache> {
    using Environment = winrt::Microsoft::Web::WebView2::Core::CoreWebView2Environment;
    using Browser = winrt::Microsoft::Web::WebView2::Core::CoreWebView2;
    using Args = winrt::Microsoft::Web::WebView2::Core::CoreWebView2WebResourceRequestedEventArgs;
    using Stream = winrt::Windows::Storage::Streams::IRandomAccessStream;
    using Deferral = decltype(std::declval<Args>().GetDeferral());
    struct Read { Args args; Deferral deferral; bool replying = false; };
    Environment environment;
    std::map<uint32_t, Read> pending;
    uint32_t serial = 0;
    static constexpr uint32_t maxBytes = 3u * 1024u * 1024u;
    static constexpr std::wstring_view prefix = L"https://steamloopback.host/cloudstorage/";
    std::shared_ptr<steam_document::Owner> document;

    static bool valid(std::wstring_view key) {
        if (key == L"cloud-storage-namespaces") return true;
        constexpr std::wstring_view start = L"cloud-storage-namespace-";
        if (!key.starts_with(start)) return false;
        key.remove_prefix(start.size());
        return (key.size() == 1 || (key.size() == 10 && key.substr(1) == L".modified")) &&
            (key[0] == L'1' || key[0] == L'3' || key[0] == L'4');
    }
    void respond(Args const& args, int status, Stream const& content = nullptr) {
        auto reason = status == 200 ? L"OK" : status == 404 ? L"Not Found" :
            status == 403 ? L"Forbidden" : L"Service Unavailable";
        args.Response(environment.CreateWebResourceResponse(content, status, reason,
            L"Content-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff"));
    }
    void finish(uint32_t token, int status, Stream const& content = nullptr) {
        auto it = pending.find(token);
        if (it == pending.end()) return;
        auto request = it->second;
        pending.erase(it);
        try { respond(request.args, status, content); } catch (...) {}
        try { request.deferral.Complete(); } catch (...) {}
    }
    winrt::fire_and_forget expire(uint32_t token) {
        auto lifetime = shared_from_this();
        winrt::apartment_context ui;
        try {
            co_await winrt::resume_after(std::chrono::seconds(5));
            co_await ui;
            finish(token, 503);
        } catch (...) {}
    }
    winrt::fire_and_forget write(uint32_t token, winrt::hstring body) {
        auto lifetime = shared_from_this();
        try {
            using namespace winrt::Windows::Storage::Streams;
            InMemoryRandomAccessStream stream;
            DataWriter writer(stream.GetOutputStreamAt(0));
            writer.WriteString(body);
            if (writer.UnstoredBufferLength() > maxBytes) { finish(token, 503); co_return; }
            co_await writer.StoreAsync();
            writer.DetachStream();
            stream.Seek(0);
            finish(token, 200, stream);
        } catch (...) { finish(token, 503); }
    }

public:
    SteamUiCloudCache(Environment const& value, std::shared_ptr<steam_document::Owner> owner)
        : environment(value), document(std::move(owner)) {}
    void install(Browser const& browser) {
        using namespace winrt::Microsoft::Web::WebView2::Core;
        browser.AddWebResourceRequestedFilter(L"https://steamloopback.host/cloudstorage/*", CoreWebView2WebResourceContext::All);
        browser.WebResourceRequested([self = shared_from_this()](Browser const& owner, Args const& args) {
            try {
                auto uri = args.Request().Uri();
                std::wstring_view url(uri);
                if (!url.starts_with(prefix)) return;
                const auto source = owner.Source();
                const auto ref = args.Request().Headers().GetHeader(L"Referer");
                if (!self->document->signedMode || !self->document->accepts(source) ||
                    !self->document->accepts(ref) || args.Request().Method() != L"GET") {
                    self->respond(args, 403); return;
                }
                url.remove_prefix(prefix.size());
                if (!url.ends_with(L".json") || !valid(url.substr(0, url.size()-5))) { self->respond(args, 404); return; }
                if (self->pending.size() >= 16) { self->respond(args, 503); return; }
                auto key = std::wstring(url.substr(0, url.size()-5));
                self->serial = self->serial % 0x7fffffff + 1;
                auto token = self->serial;
                self->pending.emplace(token, Read{args, args.GetDeferral()});
                self->expire(token);
                try { owner.PostWebMessageAsJson(winrt::hstring(L"{\"event\":\"xbox-cloud-cache-get\",\"token\":" +
                    std::to_wstring(token) + L",\"key\":\"" + key + L"\"}")); }
                catch (...) { self->finish(token, 503); }
            } catch (...) { try { self->respond(args, 403); } catch (...) {} }
        });
    }
    bool consume(winrt::hstring const& message) {
        std::wstring_view text(message);
        if (!text.starts_with(L"{\"method\":\"cloud-cache-result\",")) return false;
        if (pending.empty() || text.size() > maxBytes * 6u + 256u) return true;
        try {
            using winrt::Windows::Data::Json::JsonObject;
            JsonObject data{nullptr};
            if (!JsonObject::TryParse(message, data)) return true;
            const auto id = data.GetNamedNumber(L"token");
            if (id < 1 || id > 0x7fffffff || std::floor(id) != id) return true;
            auto token = static_cast<uint32_t>(id);
            auto found = pending.find(token);
            if (found == pending.end() || found->second.replying) return true;
            const auto status = data.GetNamedNumber(L"status");
            if (status == 200) {
                const auto body = data.GetNamedString(L"body");
                if (body.size() > maxBytes || body.empty()) finish(token, 503);
                else { found->second.replying = true; write(token, body); }
            } else finish(token, status == 403 ? 403 : status == 404 ? 404 : 503);
        } catch (...) {}
        return true;
    }
    void cancel() { while (!pending.empty()) finish(pending.begin()->first, 503); }
};
