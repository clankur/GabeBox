#include <limits.h>
#include <stdbool.h>
#include "../steam-handoff-control.h"

static const WCHAR steamKeyPath[] = L"Software\\Valve\\Steam\\ActiveProcess";
static WCHAR sdkPath[1024], hivePath[1024];

static bool ReadSetting(LPCWSTR name, LPWSTR value, DWORD capacity) {
    DWORD length = GetEnvironmentVariableW(name, value, capacity);
    return length && length < capacity;
}

static ULONGLONG ReadNumber(LPCWSTR name) {
    WCHAR value[21];
    if (!ReadSetting(name, value, ARRAYSIZE(value))) return 0;
    ULONGLONG number = 0;
    for (const WCHAR *digit = value; *digit; ++digit) {
        if (*digit < L'0' || *digit > L'9' || number > (ULLONG_MAX - (*digit - L'0')) / 10) return 0;
        number = number * 10 + (*digit - L'0');
    }
    return number;
}

DWORD SteamHandoffDiscovery(void) {
    typedef LSTATUS (WINAPI *OpenKey)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
    typedef LSTATUS (WINAPI *QueryValue)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
    typedef LSTATUS (WINAPI *CloseKey)(HKEY);
    HMODULE registry = LoadLibraryExW(L"advapi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!registry) return GetLastError();
    OpenKey openKey = (OpenKey)(void *)GetProcAddress(registry, "RegOpenKeyExW");
    QueryValue queryValue = (QueryValue)(void *)GetProcAddress(registry, "RegQueryValueExW");
    CloseKey closeKey = (CloseKey)(void *)GetProcAddress(registry, "RegCloseKey");
    DWORD status = ERROR_PROC_NOT_FOUND;
    if (openKey && queryValue && closeKey) {
        HKEY key = NULL;
        status = (DWORD)openKey(HKEY_CURRENT_USER, steamKeyPath, 0, KEY_READ | KEY_WOW64_64KEY, &key);
        if (status == ERROR_SUCCESS) {
            DWORD type = 0, size = 0;
            status = (DWORD)queryValue(key, L"SteamClientDll64", NULL, &type, NULL, &size);
            if (status == ERROR_SUCCESS && (type != REG_SZ || size <= sizeof(WCHAR))) status = ERROR_INVALID_DATA;
            closeKey(key);
        }
    }
    FreeLibrary(registry);
    return status;
}

static DWORD InitSteamSdk(DWORD *report, BOOL retain) {
    typedef int (__cdecl *Init)(char *);
    typedef void (__cdecl *Shutdown)(void);
    typedef void *(__cdecl *GetUser)(void);
    typedef bool (__cdecl *IsLoggedOn)(void *);
    typedef ULONGLONG (__cdecl *GetSteamId)(void *);

    HMODULE sdk = LoadLibraryExW(sdkPath, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!sdk) return GetLastError();
    *report |= STEAM_HANDOFF_SDK_MODULE;
    Init init = (Init)(void *)GetProcAddress(sdk, "SteamAPI_InitFlat");
    Shutdown shutdown = (Shutdown)(void *)GetProcAddress(sdk, "SteamAPI_Shutdown");
    GetUser getUser = (GetUser)(void *)GetProcAddress(sdk, "SteamAPI_SteamUser_v023");
    IsLoggedOn loggedOn = (IsLoggedOn)(void *)GetProcAddress(sdk, "SteamAPI_ISteamUser_BLoggedOn");
    GetSteamId getId = (GetSteamId)(void *)GetProcAddress(sdk, "SteamAPI_ISteamUser_GetSteamID");
    bool keep = false;
    DWORD code = ERROR_PROC_NOT_FOUND;
    if (init && shutdown && getUser && loggedOn && getId) {
        char error[1024];
        *report |= STEAM_HANDOFF_SDK_INIT_CALLED;
        code = (DWORD)init(error);
        if (code == 0) {
            *report |= STEAM_HANDOFF_SDK_INITIALIZED;
            void *user = getUser();
            if (user && loggedOn(user)) {
                *report |= STEAM_HANDOFF_SDK_LOGGED_ON;
                if (getId(user) == ReadNumber(L"XBOX_STEAM_HANDOFF_ACCOUNT")) *report |= STEAM_HANDOFF_SDK_MATCHED;
            }
            keep = retain && (*report & STEAM_HANDOFF_SDK_MATCHED);
            if (!keep) shutdown();
        }
    }
    if (!keep) FreeLibrary(sdk);
    return code;
}

static DWORD SteamSdkControl(HMODULE client, BOOL retain) {
    typedef LSTATUS (WINAPI *LoadHive)(LPCWSTR, PHKEY, REGSAM, DWORD, DWORD);
    typedef LSTATUS (WINAPI *CreateKey)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM,
                                       const SECURITY_ATTRIBUTES *, PHKEY, LPDWORD);
    typedef LSTATUS (WINAPI *SetValue)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE *, DWORD);
    typedef LSTATUS (WINAPI *OverrideKey)(HKEY, HKEY);
    typedef LSTATUS (WINAPI *CloseKey)(HKEY);

    DWORD report = STEAM_HANDOFF_SDK_REPORT;
    WCHAR clientPath[1024];
    DWORD pathLength = client ? GetModuleFileNameW(client, clientPath, ARRAYSIZE(clientPath)) : 0;
    DWORD pid = (DWORD)ReadNumber(L"XBOX_STEAM_HANDOFF_PID");
    if (!pathLength || pathLength >= ARRAYSIZE(clientPath) || !pid ||
            !ReadSetting(L"XBOX_STEAM_HANDOFF_HIVE", hivePath, ARRAYSIZE(hivePath))) return report | ERROR_INVALID_DATA;
    HMODULE registry = LoadLibraryExW(L"advapi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!registry) return report | (GetLastError() & 0xffffu);
    LoadHive loadHive = (LoadHive)(void *)GetProcAddress(registry, "RegLoadAppKeyW");
    CreateKey createKey = (CreateKey)(void *)GetProcAddress(registry, "RegCreateKeyExW");
    SetValue setValue = (SetValue)(void *)GetProcAddress(registry, "RegSetValueExW");
    OverrideKey overrideKey = (OverrideKey)(void *)GetProcAddress(registry, "RegOverridePredefKey");
    CloseKey closeKey = (CloseKey)(void *)GetProcAddress(registry, "RegCloseKey");
    DWORD status = ERROR_PROC_NOT_FOUND;
    if (loadHive && createKey && setValue && overrideKey && closeKey) {
        HKEY root = NULL, key = NULL;
        status = loadHive(hivePath, &root, KEY_READ | KEY_WRITE, REG_PROCESS_APPKEY, 0);
        if (status == ERROR_SUCCESS) {
            report |= STEAM_HANDOFF_SDK_HIVE;
            status = createKey(root, steamKeyPath, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key, NULL);
            if (status == ERROR_SUCCESS) {
                status = setValue(key, L"SteamClientDll64", 0, REG_SZ, (const BYTE *)clientPath,
                                  (pathLength + 1) * sizeof(WCHAR));
                if (status == ERROR_SUCCESS) status = setValue(key, L"pid", 0, REG_DWORD, (const BYTE *)&pid, sizeof(pid));
                closeKey(key);
            }
            if (status == ERROR_SUCCESS) status = overrideKey(HKEY_CURRENT_USER, root);
            if (status == ERROR_SUCCESS) {
                status = SteamHandoffDiscovery();
                if (status == ERROR_SUCCESS) {
                    report |= STEAM_HANDOFF_SDK_DISCOVERY;
                    status = InitSteamSdk(&report, retain);
                }
                overrideKey(HKEY_CURRENT_USER, NULL);
            }
            closeKey(root);
        }
    }
    FreeLibrary(registry);
    return report | (status & 0xffffu);
}

DWORD WINAPI XboxSteamInitialize(BOOL retain) {
    if (!ReadSetting(L"XBOX_STEAM_HANDOFF_SDK", sdkPath, ARRAYSIZE(sdkPath)))
        return STEAM_HANDOFF_SDK_REPORT | ERROR_INVALID_DATA;
    HMODULE client = LoadPackagedLibrary(L"steamclient64.dll", 0);
    DWORD result = SteamSdkControl(client, retain);
    if (client) FreeLibrary(client);
    return result;
}
