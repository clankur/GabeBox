#include "../runtime/steam/steam-download-queue.h"

#include <cassert>

int main() {
    constexpr std::uint32_t selected = 21;
    const std::wstring other = L"\r\nsteam_download_active=other\r\nsteam_download_other_active_app=";
    const std::wstring focused = L"\r\nsteam_download_active=selected\r\nsteam_download_other_active_app=0";
    const std::wstring idle = L"\r\nsteam_download_active=none\r\nsteam_download_other_active_app=0";

    assert(DescribeSteamDownloadActivity(selected, 37) == other + L"37");
    assert(DescribeSteamDownloadActivity(selected, selected) == focused);
    assert(DescribeSteamDownloadActivity(selected, 37) == other + L"37");
    assert(DescribeSteamDownloadActivity(selected, 0) == idle);
    assert(DescribeSteamDownloadActivity(selected, 37) == other + L"37");
    assert(DescribeSteamDownloadActivity(selected, 38) == other + L"38");
}
