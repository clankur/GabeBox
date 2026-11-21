#pragma once

#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "SteamCloudManifest.h"

namespace steam_cloud {

using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::Foundation::Uri;
using winrt::Windows::Web::Http::HttpClient;

struct ListCall {
    using Send = std::uint64_t (__cdecl *)(void *, const char *, const void *, std::uint32_t, std::uint64_t);
    using ReadInfo = bool (__cdecl *)(void *, std::uint64_t, std::uint32_t *, std::int32_t *);
    using ReadData = bool (__cdecl *)(void *, std::uint64_t, void *, std::uint32_t, bool);
    using Release = bool (__cdecl *)(void *, std::uint64_t);

    SteamSignInHandles *session = nullptr;
    std::uint64_t account = 0;
    std::int32_t pipe = 0;
    void *adapter = nullptr;
    std::uintptr_t const *methods = nullptr;
    std::uint64_t handle = 0;

    bool MatchesSession() const {
        return session && session == liveSteamSession && session->accountId == account && session->pipe == pipe;
    }

    bool Open(SteamSignInHandles &active) {
        using GetInterface = void *(__cdecl *)(void *, std::int32_t, std::int32_t, const char *);
        const auto base = reinterpret_cast<std::uintptr_t>(active.module);
        int result = -1;
        auto client = active.factory("SteamClient023", &result);
        auto clientMethods = SteamMethods(client);
        // Valve 1794536281. The ordinary generic getter uses the version string to obtain
        // the adapter for this already-authenticated Steam user and pipe.
        if (result != 0 || reinterpret_cast<std::uintptr_t>(clientMethods) != base + 0x133ae18 ||
                clientMethods[12] != base + 0x7366e0) return false;
        adapter = reinterpret_cast<GetInterface>(clientMethods[12])(
            client, active.user, active.pipe, "STEAMUNIFIEDMESSAGES_INTERFACE_VERSION001");
        methods = SteamMethods(adapter);
        if (reinterpret_cast<std::uintptr_t>(methods) != base + 0x1339238 ||
                methods[0] != base + 0x734a80 || methods[1] != base + 0x734a60 ||
                methods[2] != base + 0x734a40 || methods[3] != base + 0x734a70) return false;
        session = &active;
        account = active.accountId;
        pipe = active.pipe;
        return true;
    }

    ~ListCall() {
        if (!handle) return;
        std::lock_guard lock(steamSessionMutex);
        if (MatchesSession()) reinterpret_cast<Release>(methods[3])(adapter, handle);
    }
};

class CloudDeadline {
    using ThreadPoolTimer = winrt::Windows::System::Threading::ThreadPoolTimer;
    ThreadPoolTimer timer_;

public:
    template <typename Operation>
    CloudDeadline(Operation operation, ULONGLONG deadline) : timer_(nullptr) {
        const auto now = GetTickCount64();
        if (now >= deadline) {
            operation.Cancel();
            throw winrt::hresult_canceled{};
        }
        timer_ = ThreadPoolTimer::CreateTimer([operation](ThreadPoolTimer const&) {
            try { operation.Cancel(); } catch (winrt::hresult_error const&) {}
        }, std::chrono::milliseconds(deadline - now));
    }

    ~CloudDeadline() {
        try {
            if (timer_) timer_.Cancel();
        } catch (winrt::hresult_error const&) {}
    }
};

inline bool ValidCloudUri(Uri const& uri) {
    if (!uri || uri.SchemeName() != L"https" || !uri.UserName().empty() || !uri.Password().empty() ||
            (uri.Port() != 443 && uri.Port() != -1)) return false;
    const auto name = uri.Host();
    const std::wstring_view host{name};
    if (host == L"store.steampowered.com" || host.ends_with(L".steamusercontent.com") ||
            host.ends_with(L".steamcontent.com")) return true;
    for (auto suffix : {std::wstring_view(L".storage.googleapis.com"), std::wstring_view(L".blob.core.windows.net")}) {
        if (!host.ends_with(suffix)) continue;
        auto bucket = host.substr(0, host.size() - suffix.size());
        return (bucket.starts_with(L"steamcloud") || bucket.starts_with(L"steamugc")) &&
            bucket.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789-") == std::wstring_view::npos;
    }
    return false;
}

inline IAsyncOperation<winrt::hstring> StageFile(HttpClient client, File candidate,
        winrt::Windows::Storage::StorageFile stageFile, ULONGLONG deadline) {
    using namespace winrt::Windows::Security::Cryptography;
    using namespace winrt::Windows::Security::Cryptography::Core;
    using namespace winrt::Windows::Storage;
    using namespace winrt::Windows::Storage::Streams;
    using namespace winrt::Windows::Web::Http;

    HttpResponseMessage response{nullptr};
    Uri uri{winrt::to_hstring(candidate.url)};
    for (unsigned redirects = 0; redirects <= 3; ++redirects) {
        if (!ValidCloudUri(uri)) co_return L"transfer-untrusted-host";
        auto operation = client.GetAsync(uri, HttpCompletionOption::ResponseHeadersRead);
        CloudDeadline requestDeadline{operation, deadline};
        response = co_await operation;
        const auto status = static_cast<int>(response.StatusCode());
        if (status == 200) break;
        if (redirects == 3 || (status != 301 && status != 302 && status != 303 && status != 307 && status != 308)) {
            co_return L"transfer-http-error";
        }
        uri = response.Headers().Location();
        response.Close();
    }

    auto bodyOperation = response.Content().ReadAsInputStreamAsync();
    CloudDeadline bodyDeadline{bodyOperation, deadline};
    auto body = co_await bodyOperation;
    auto output = co_await stageFile.OpenAsync(FileAccessMode::ReadWrite);
    auto hash = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Sha1()).CreateHash();
    std::uint32_t received = 0;
    for (;;) {
        const auto wanted = (std::min)(65536u, candidate.bytes - received + 1);
        auto read = body.ReadAsync(Buffer(wanted), wanted, InputStreamOptions::Partial);
        CloudDeadline readDeadline{read, deadline};
        auto chunk = co_await read;
        const auto length = chunk.Length();
        if (!length) break;
        if (length > candidate.bytes - received) co_return L"transfer-length-mismatch";
        hash.Append(chunk);
        if (co_await output.WriteAsync(chunk) != length) co_return L"stage-write-error";
        received += length;
    }
    if (received != candidate.bytes) co_return L"transfer-length-mismatch";
    const auto expected = CryptographicBuffer::DecodeFromHexString(winrt::to_hstring(candidate.sha1));
    if (!CryptographicBuffer::Compare(hash.GetValueAndReset(), expected)) {
        co_return L"transfer-hash-mismatch";
    }
    if (!(co_await output.FlushAsync())) co_return L"stage-write-error";
    output.Close();
    body.Close();
    response.Close();
    co_return L"ready";
}

inline winrt::hstring Result(std::wstring_view status, std::int32_t steamResult = 0,
        winrt::Windows::Data::Json::JsonArray const& files = nullptr) {
    using namespace winrt::Windows::Data::Json;
    JsonObject reply;
    reply.SetNamedValue(L"status", JsonValue::CreateStringValue(winrt::hstring(status)));
    if (steamResult) reply.SetNamedValue(L"steamResult", JsonValue::CreateNumberValue(steamResult));
    reply.SetNamedValue(L"files", (files ? files : JsonArray{}).as<IJsonValue>());
    return reply.Stringify();
}

} // namespace steam_cloud

// The caller supplies an empty, private app-local staging folder and owns cleanup.
// No cloud URL, request context, or account identifier leaves this component.
static winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> StageSteamCloudFiles(
        std::uint32_t app, winrt::Windows::Storage::StorageFolder stage) {
    using namespace steam_cloud;
    using namespace winrt::Windows::Data::Json;
    using namespace winrt::Windows::Storage;
    using namespace winrt::Windows::Web::Http::Filters;

    if (!app || !stage) co_return Result(L"invalid-request");
    try {
        winrt::apartment_context caller;
        ListCall call;
        const auto request = ListRequest(app);
        {
            std::lock_guard lock(steamSessionMutex);
            auto session = liveSteamSession;
            if (!session || !session->OwnsSession()) co_return Result(L"session-unavailable");
            if (session->downloadApp || session->launchCall) co_return Result(L"busy");
            if (std::find(session->games.begin(), session->games.end(), app) == session->games.end()) {
                co_return Result(L"not-owned");
            }
            if (!call.Open(*session)) co_return Result(L"unsupported-steam-client");
            call.handle = reinterpret_cast<ListCall::Send>(call.methods[0])(
                call.adapter, "Cloud.EnumerateUserFiles#1", request.data(), static_cast<std::uint32_t>(request.size()), 0);
            if (!call.handle) co_return Result(L"cloud-request-rejected");
        }

        std::string listing;
        bool complete = false;
        const auto requestDeadline = GetTickCount64() + 20000;
        while (GetTickCount64() < requestDeadline) {
            {
                std::lock_guard lock(steamSessionMutex);
                if (!call.MatchesSession() || !call.session->OwnsSession()) co_return Result(L"session-unavailable");
                call.session->PumpCallbacks();
                std::uint32_t bytes = 0;
                std::int32_t result = 0;
                if (reinterpret_cast<ListCall::ReadInfo>(call.methods[1])(call.adapter, call.handle, &bytes, &result)) {
                    if (result != 1) co_return Result(L"cloud-service-error", result);
                    if (bytes > maxListBytes) co_return Result(L"cloud-response-limit");
                    listing.resize(bytes);
                    if (bytes && !reinterpret_cast<ListCall::ReadData>(call.methods[2])(
                            call.adapter, call.handle, listing.data(), bytes, false)) {
                        co_return Result(L"cloud-read-error");
                    }
                    reinterpret_cast<ListCall::Release>(call.methods[3])(call.adapter, call.handle);
                    call.handle = 0;
                    complete = true;
                }
            }
            if (complete) break;
            co_await winrt::resume_after(std::chrono::milliseconds(150));
            co_await caller;
        }
        if (!complete) co_return Result(L"cloud-timeout");

        std::vector<File> candidates;
        switch (ParseManifest(listing, app, candidates)) {
        case ManifestResult::malformed: co_return Result(L"cloud-response-malformed");
        case ManifestResult::limit: co_return Result(L"cloud-response-limit");
        case ManifestResult::incomplete: co_return Result(L"cloud-response-incomplete");
        case ManifestResult::ready: break;
        }
        if (candidates.empty()) co_return Result(L"empty");

        HttpBaseProtocolFilter filter;
        filter.AllowAutoRedirect(false);
        filter.AllowUI(false);
        filter.CookieUsageBehavior(HttpCookieUsageBehavior::NoCookies);
        filter.CacheControl().WriteBehavior(HttpCacheWriteBehavior::NoCache);
        HttpClient client{filter};
        const auto transferDeadline = GetTickCount64() + 120000;
        JsonArray files;
        for (auto const& candidate : candidates) {
            {
                std::lock_guard lock(steamSessionMutex);
                if (!call.MatchesSession() || !call.session->OwnsSession()) co_return Result(L"session-unavailable");
            }
            if (GetTickCount64() >= transferDeadline) co_return Result(L"transfer-timeout");
            const auto name = L"save-" + std::to_wstring(files.Size() + 1) + L".bin";
            auto file = co_await stage.CreateFileAsync(name, CreationCollisionOption::FailIfExists);
            auto status = co_await StageFile(client, candidate, file, transferDeadline);
            if (status != L"ready") co_return Result(std::wstring_view(status));
            JsonObject entry;
            entry.SetNamedValue(L"path", JsonValue::CreateStringValue(winrt::to_hstring(candidate.path)));
            entry.SetNamedValue(L"bytes", JsonValue::CreateNumberValue(candidate.bytes));
            entry.SetNamedValue(L"sha1", JsonValue::CreateStringValue(winrt::to_hstring(candidate.sha1)));
            entry.SetNamedValue(L"stagedName", JsonValue::CreateStringValue(winrt::hstring(name)));
            files.Append(entry);
        }
        {
            std::lock_guard lock(steamSessionMutex);
            if (!call.MatchesSession() || !call.session->OwnsSession()) co_return Result(L"session-unavailable");
        }
        co_return Result(L"ready", 1, files);
    } catch (winrt::hresult_canceled const&) {
        co_return Result(L"transfer-timeout");
    } catch (winrt::hresult_error const&) {
        co_return Result(L"cloud-io-error");
    }
}
