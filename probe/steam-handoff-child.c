#include <stdbool.h>
#include "steam-handoff-control.h"

static DWORD SteamHandoffControl(HMODULE client) {
    typedef int (__cdecl *CreatePipe)(void);
    typedef int (__cdecl *ConnectUser)(int);
    typedef bool (__cdecl *LoggedOn)(int, int);
    typedef void (__cdecl *ReleaseUser)(int, int);
    typedef bool (__cdecl *ReleasePipe)(int);

    DWORD report = STEAM_HANDOFF_REPORT | (SteamHandoffDiscovery() & 0xffffu);
    if (!client) return report;
    report |= STEAM_HANDOFF_MODULE;
    CreatePipe createPipe = (CreatePipe)(void *)GetProcAddress(client, "Steam_CreateSteamPipe");
    ConnectUser connectUser = (ConnectUser)(void *)GetProcAddress(client, "Steam_ConnectToGlobalUser");
    LoggedOn loggedOn = (LoggedOn)(void *)GetProcAddress(client, "Steam_BLoggedOn");
    ReleaseUser releaseUser = (ReleaseUser)(void *)GetProcAddress(client, "Steam_ReleaseUser");
    ReleasePipe releasePipe = (ReleasePipe)(void *)GetProcAddress(client, "Steam_BReleaseSteamPipe");
    if (!createPipe || !connectUser || !loggedOn || !releaseUser || !releasePipe) return report;
    report |= STEAM_HANDOFF_EXPORTS;

    int pipe = createPipe();
    if (pipe <= 0) return report;
    report |= STEAM_HANDOFF_PIPE;
    int user = connectUser(pipe);
    if (user > 0) {
        report |= STEAM_HANDOFF_USER;
        if (loggedOn(user, pipe)) report |= STEAM_HANDOFF_LOGGED_ON;
        releaseUser(pipe, user);
    }
    if (releasePipe(pipe)) report |= STEAM_HANDOFF_RELEASED;
    return report;
}

void WINAPI ChildEntry(void) {
    if (GetEnvironmentVariableW(L"XBOX_STEAM_HANDOFF_SDK", NULL, 0) > 1)
        ExitProcess(XboxSteamInitialize(FALSE));

    HMODULE client = LoadPackagedLibrary(L"steamclient64.dll", 0);
    DWORD report = SteamHandoffControl(client);
    if (client) FreeLibrary(client);
    ExitProcess(report);
}
