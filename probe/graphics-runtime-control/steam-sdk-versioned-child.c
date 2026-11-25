/* Run the same SDK import redirection and return-value checks as an installed title. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include "../../runtime/shared/child-process.h"

DECLSPEC_IMPORT void __cdecl SteamFixtureConfigure(int result, ULONGLONG user, const char *versions, char *message);
DECLSPEC_IMPORT DWORD __cdecl SteamFixtureCounts(void);
DECLSPEC_IMPORT bool __cdecl SteamAPI_RestartAppIfNecessary(DWORD app);
DECLSPEC_IMPORT int __cdecl SteamInternal_SteamAPI_Init(const char *versions, char *message);

static bool SameText(const char *left, const char *right) {
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

static unsigned RunSdkChecks(void) {
    WCHAR path[MAX_PATH];
    HMODULE sdk = GetModuleHandleW(L"steam_api64.dll");
    const DWORD length = sdk ? GetModuleFileNameW(sdk, path, ARRAYSIZE(path)) : 0;
    if (!length || length >= ARRAYSIZE(path)) return 1;
    if (!SetEnvironmentVariableW(L"XBOX_STEAM_HANDOFF_SDK", path) ||
            !SetEnvironmentVariableW(L"XBOX_STEAM_HANDOFF_HIVE", L"fixture-no-hive") ||
            !SetEnvironmentVariableW(L"XBOX_STEAM_HANDOFF_ACCOUNT", L"2001") ||
            !SetEnvironmentVariableW(L"XBOX_STEAM_HANDOFF_LEGACY", L"1")) return 2;

    if (!SteamAPI_RestartAppIfNecessary(427520) || GetLastError() != ERROR_BUSY ||
            SteamAPI_RestartAppIfNecessary(1) || GetLastError() != ERROR_BUSY) return 3;

    const char versions[] = "SteamUser023\0STEAMREMOTESTORAGE_INTERFACE_VERSION016\0";
    char message[1024];
    message[0] = 'z';
    message[1] = '\0';
    SteamFixtureConfigure(0, 2001, versions, message);
    if (SteamInternal_SteamAPI_Init(versions, message) || GetLastError() != ERROR_RETRY ||
            !SameText(message, "z") || SteamFixtureCounts() != 1) return 4;

    SteamFixtureConfigure(3, 2001, versions, message);
    if (SteamInternal_SteamAPI_Init(versions, message) != 3 || GetLastError() != ERROR_RETRY ||
            !SameText(message, "fixture SDK error") || SteamFixtureCounts() != 1) return 5;

    SteamFixtureConfigure(0, 2002, versions, message);
    if (SteamInternal_SteamAPI_Init(versions, message) != 1 || GetLastError() != ERROR_RETRY ||
            !SameText(message, "The Steam SDK did not match the signed-in Steam account.") ||
            SteamFixtureCounts() != 0x10001) return 6;

    SteamFixtureConfigure(2, 2001, versions, NULL);
    if (SteamInternal_SteamAPI_Init(versions, NULL) != 2 || SteamFixtureCounts() != 1) return 7;

    SteamFixtureConfigure(0, 2001, versions, message);
    if (!SetEnvironmentVariableW(L"XBOX_STEAM_HANDOFF_SDK", L"incorrect\\steam_api64.dll") ||
            SteamInternal_SteamAPI_Init(versions, message) != 1 || SteamFixtureCounts()) return 8;
    if (!SetEnvironmentVariableW(L"XBOX_STEAM_HANDOFF_SDK", path) ||
            !SetEnvironmentVariableW(L"XBOX_STEAM_HANDOFF_LEGACY", NULL) ||
            SteamInternal_SteamAPI_Init(versions, message) != 1 || SteamFixtureCounts()) return 9;
    return 0;
}

void WINAPI ChildEntry(void) {
    const unsigned result = RunSdkChecks();
    ExitProcess(result ? 0x74300000u | result : PROBE_EXPECTED_CHILD_EXIT);
}
