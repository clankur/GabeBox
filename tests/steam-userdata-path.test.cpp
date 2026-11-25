#include "../runtime/steam/steam-userdata-path.h"
#include <cassert>

int main() {
    using steam_userdata::Redirect;
    constexpr std::wstring_view root = L"Q:\\Program Files\\WindowsApps\\SteamCore_1.0_x64__abc";
    constexpr std::wstring_view target = L"Q:\\Users\\U\\AppData\\Local\\Packages\\SteamCore_abc\\LocalState\\SteamUserData";

    // Steam builds remote-storage paths with forward slashes on its install root.
    auto remote = Redirect(L"Q:\\Program Files\\WindowsApps\\SteamCore_1.0_x64__abc/userdata/123/2868840/remote/profile1/saves/progress.save",
                           root, target);
    assert(remote && *remote == std::wstring(target) + L"\\123\\2868840\\remote\\profile1\\saves\\progress.save");

    // The folder itself, case differences, doubled separators and the verbatim prefix all map.
    assert(Redirect(L"q:\\program files\\windowsapps\\steamcore_1.0_x64__ABC\\UserData", root, target) == std::wstring(target));
    assert(Redirect(L"Q:\\Program Files\\WindowsApps\\SteamCore_1.0_x64__abc\\\\userdata\\\\123\\", root, target) ==
           std::wstring(target) + L"\\123");
    assert(Redirect(L"\\\\?\\Q:\\Program Files\\WindowsApps\\SteamCore_1.0_x64__abc\\userdata\\1\\*", root, target) ==
           std::wstring(target) + L"\\1\\*");
    assert(Redirect(L"Q:\\Program Files\\WindowsApps\\SteamCore_1.0_x64__abc\\userdata\\1", std::wstring(root) + L"\\",
                    std::wstring(target) + L"\\") == std::wstring(target) + L"\\1");

    // Everything else stays where Steam asked for it.
    assert(!Redirect(L"Q:\\Program Files\\WindowsApps\\SteamCore_1.0_x64__abc\\config\\config.vdf", root, target));
    assert(!Redirect(L"Q:\\Program Files\\WindowsApps\\SteamCore_1.0_x64__abc\\userdata2\\1", root, target));
    assert(!Redirect(L"Q:\\Program Files\\WindowsApps\\SteamCore_1.0_x64__abcd\\userdata\\1", root, target));
    assert(!Redirect(L"Q:\\Program Files\\WindowsApps\\SteamCore_1.0_x64__abc", root, target));
    assert(!Redirect(L"Q:\\Program Files\\WindowsApps\\SteamCore_1.0_x64__abc\\userdata\\..\\config", root, target));
    assert(!Redirect(L"userdata\\123", root, target));
    assert(!Redirect(L"D:\\SteamLibrary\\userdata\\1", root, target));
    assert(!Redirect(L"Q:\\Program Files\\WindowsApps\\SteamCore_1.0_x64__abc\\userdata\\1", L"", target));
    assert(!Redirect(L"Q:\\Program Files\\WindowsApps\\SteamCore_1.0_x64__abc\\userdata\\1", root, L""));
    return 0;
}
