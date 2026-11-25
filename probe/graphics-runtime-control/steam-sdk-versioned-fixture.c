/* Fixture for argument, return value, and account-check behavior. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>

static int requestedResult;
static ULONGLONG account;
static const char *expectedVersions;
static char *expectedMessage;
static DWORD initialized, shutdowns;

void __cdecl SteamFixtureConfigure(int result, ULONGLONG user, const char *versions, char *message) {
    requestedResult = result;
    account = user;
    expectedVersions = versions;
    expectedMessage = message;
    initialized = shutdowns = 0;
}

DWORD __cdecl SteamFixtureCounts(void) {
    return initialized | (shutdowns << 16);
}

bool __cdecl SteamAPI_RestartAppIfNecessary(DWORD app) {
    SetLastError(ERROR_BUSY);
    return app == 427520;
}

int __cdecl SteamInternal_SteamAPI_Init(const char *versions, char *message) {
    ++initialized;
    SetLastError(ERROR_RETRY);
    if (versions != expectedVersions || message != expectedMessage) return 99;
    if (requestedResult && message) {
        const char reason[] = "fixture SDK error";
        for (DWORD i = 0; i < sizeof(reason); ++i) message[i] = reason[i];
    }
    return requestedResult;
}

void __cdecl SteamAPI_Shutdown(void) {
    ++shutdowns;
    SetLastError(ERROR_SUCCESS);
}

void *__cdecl SteamAPI_SteamUser_v023(void) {
    return account ? &account : NULL;
}

bool __cdecl SteamAPI_ISteamUser_BLoggedOn(void *user) {
    return user == &account;
}

ULONGLONG __cdecl SteamAPI_ISteamUser_GetSteamID(void *user) {
    return user == &account ? account : 0;
}
