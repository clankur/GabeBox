#pragma once

#include <cstdint>
#include <string>

inline std::wstring DescribeSteamDownloadActivity(std::uint32_t selectedApp, std::uint32_t activeApp) {
    const auto otherApp = activeApp == selectedApp ? 0 : activeApp;
    const auto activity = !activeApp ? L"none" : otherApp ? L"other" : L"selected";
    return std::wstring(L"\r\nsteam_download_active=") + activity +
        L"\r\nsteam_download_other_active_app=" + std::to_wstring(otherApp);
}
