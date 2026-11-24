#pragma once
#include <string_view>
#include <cstdint>

namespace steam_document {
constexpr wchar_t entry[] = L"https://steamloopback.host/index.html";
constexpr wchar_t signedEntry[] = L"https://steamloopback.host/index.html?IN_CLIENT=true";

inline std::wstring_view page(std::wstring_view uri) {
    return uri.substr(0, uri.find(L'#'));
}

// Valve uses history.pushState in its original document. These aliases identify
// its current address, but must never be accepted as actual navigation URLs.
inline bool historyAddress(std::wstring_view uri) {
    constexpr std::wstring_view prefix = L"https://steamloopback.host/routes/";
    if (uri.size() > 2048 || !uri.starts_with(prefix)) return false;
    auto path = page(uri).substr(prefix.size());
    return path.find(L"//") == path.npos &&
        path.find_first_not_of(L"/abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == path.npos;
}

struct Owner {
    bool signedMode = false;
    bool committed = false;
    uint64_t navigation = 0;
    bool start(std::wstring_view uri, uint64_t id) {
        if (page(uri) != (signedMode ? signedEntry : entry)) return false;
        committed = false;
        navigation = id;
        return true;
    }
    void content(uint64_t id, bool errorPage) {
        if (id && navigation == id) committed = !errorPage;
    }
    bool accepts(std::wstring_view uri) const {
        return committed && (page(uri) == (signedMode ? signedEntry : entry) || (signedMode && historyAddress(uri)));
    }
};
}
