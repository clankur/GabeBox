#pragma once
#include <algorithm>
#include <compare>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace steam_webview {
// WebView2's ProcessDescription is free-form, potentially future runtime text.
// Emit only known fixed labels along with documented enums and an exit code.
inline std::wstring_view processDescription(std::wstring_view value) {
    if (value.empty()) return L"none";
    if (value == L"Audio Service") return L"audio";
    if (value == L"Network Service") return L"network";
    if (value == L"Video Capture") return L"video-capture";
    return L"other";
}
inline std::wstring_view failureReason(int value) {
    constexpr std::wstring_view names[]{L"unexpected", L"unresponsive", L"terminated", L"crashed",
        L"launch-failed", L"out-of-memory", L"profile-deleted", L"normal-exit"};
    return value >= 0 && value < 8 ? names[value] : L"unknown";
}

// COREWEBVIEW2_PROCESS_KIND, in the SDK's order.
inline std::string_view processKind(int value) {
    constexpr std::string_view names[]{"browser", "renderer", "utility", "spare-renderer", "gpu",
        "ppapi-plugin", "ppapi-broker"};
    return value >= 0 && value < 7 ? names[value] : "unknown";
}

struct WebProcess {
    uint32_t pid = 0;
    int kind = 0;
    auto operator<=>(WebProcess const&) const = default;
};

// " count=N added=kind:pid,... removed=kind:pid,..." between two process sets. Both
// lists are sorted and de-duplicated first; an empty change list is written as "-".
inline std::string processChanges(std::vector<WebProcess> before, std::vector<WebProcess> after) {
    for (auto *list : {&before, &after}) {
        std::sort(list->begin(), list->end());
        list->erase(std::unique(list->begin(), list->end()), list->end());
    }
    auto format = [](std::vector<WebProcess> const& processes) {
        std::string text;
        for (auto const& process : processes) {
            if (!text.empty()) text += ',';
            text += processKind(process.kind);
            text += ':';
            text += std::to_string(process.pid);
        }
        return text.empty() ? std::string("-") : text;
    };
    std::vector<WebProcess> added, removed;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(), std::back_inserter(added));
    std::set_difference(before.begin(), before.end(), after.begin(), after.end(), std::back_inserter(removed));
    return " count=" + std::to_string(after.size()) + " added=" + format(added) + " removed=" + format(removed);
}

// Opt-in browser arguments from the package root: whitespace-separated, each one of the
// switches below, at most once. Anything else rejects the whole file, and the browser
// keeps its default arguments. The last two control Chromium's one-shot, non-sandboxed
// GPU process that collects DX12 and Vulkan info 120 s after the browser starts: the
// first removes it, the second runs it at browser start-up instead.
inline bool browserArguments(std::wstring_view text, std::wstring& arguments) {
    constexpr std::wstring_view allowed[]{L"--disable-component-update", L"--disable-background-networking",
        L"--disable-gpu-process-for-dx12-info-collection", L"--no-delay-for-dx12-vulkan-info-collection"};
    arguments.clear();
    if (text.size() > 256) return false;
    bool seen[std::size(allowed)]{};
    const auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    size_t index = 0;
    while (index < text.size()) {
        if (isSpace(text[index])) {
            ++index;
            continue;
        }
        size_t end = index;
        while (end < text.size() && !isSpace(text[end])) ++end;
        const auto token = text.substr(index, end - index);
        const auto match = std::find(std::begin(allowed), std::end(allowed), token);
        if (match == std::end(allowed) || seen[match - std::begin(allowed)]) {
            arguments.clear();
            return false;
        }
        seen[match - std::begin(allowed)] = true;
        if (!arguments.empty()) arguments += L' ';
        arguments += token;
        index = end;
    }
    return !arguments.empty();
}
}
