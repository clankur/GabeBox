#pragma once
#include <string>
#include <string_view>

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
}
