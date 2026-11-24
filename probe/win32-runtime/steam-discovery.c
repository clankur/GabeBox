#include <limits.h>
#include <stdbool.h>
#include "../steam-handoff-control.h"
#include "loader-trace-output.h"

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

static DWORD InitSteamSdk(DWORD *report, void *context) {
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
            keep = context && (*report & STEAM_HANDOFF_SDK_MATCHED);
            if (!keep) shutdown();
        }
    }
    if (!keep) FreeLibrary(sdk);
    return code;
}

typedef DWORD (*DiscoveryAction)(DWORD *report, void *context);

static DWORD WithSteamDiscovery(HMODULE client, LPCWSTR privateHive, DWORD *report,
        DiscoveryAction action, void *context, DWORD *cleanupStatus) {
    typedef LSTATUS (WINAPI *LoadHive)(LPCWSTR, PHKEY, REGSAM, DWORD, DWORD);
    typedef LSTATUS (WINAPI *CreateKey)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM,
                                       const SECURITY_ATTRIBUTES *, PHKEY, LPDWORD);
    typedef LSTATUS (WINAPI *SetValue)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE *, DWORD);
    typedef LSTATUS (WINAPI *OverrideKey)(HKEY, HKEY);
    typedef LSTATUS (WINAPI *CloseKey)(HKEY);

    WCHAR clientPath[1024];
    DWORD pathLength = client ? GetModuleFileNameW(client, clientPath, ARRAYSIZE(clientPath)) : 0;
    DWORD pid = (DWORD)ReadNumber(L"XBOX_STEAM_HANDOFF_PID");
    if (!pathLength || pathLength >= ARRAYSIZE(clientPath) || !pid || !*privateHive) return ERROR_INVALID_DATA;
    HMODULE registry = LoadLibraryExW(L"advapi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!registry) return GetLastError();
    LoadHive loadHive = (LoadHive)(void *)GetProcAddress(registry, "RegLoadAppKeyW");
    CreateKey createKey = (CreateKey)(void *)GetProcAddress(registry, "RegCreateKeyExW");
    SetValue setValue = (SetValue)(void *)GetProcAddress(registry, "RegSetValueExW");
    OverrideKey overrideKey = (OverrideKey)(void *)GetProcAddress(registry, "RegOverridePredefKey");
    CloseKey closeKey = (CloseKey)(void *)GetProcAddress(registry, "RegCloseKey");
    DWORD status = ERROR_PROC_NOT_FOUND;
    if (loadHive && createKey && setValue && overrideKey && closeKey) {
        HKEY root = NULL, key = NULL;
        status = loadHive(privateHive, &root, KEY_READ | KEY_WRITE, REG_PROCESS_APPKEY, 0);
        if (status == ERROR_SUCCESS) {
            *report |= STEAM_HANDOFF_SDK_HIVE;
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
                    *report |= STEAM_HANDOFF_SDK_DISCOVERY;
                    status = action(report, context);
                }
                const DWORD restored = overrideKey(HKEY_CURRENT_USER, NULL);
                if (cleanupStatus) *cleanupStatus = restored;
            }
            closeKey(root);
        }
    }
    FreeLibrary(registry);
    return status;
}

DWORD WINAPI XboxSteamInitialize(BOOL retain) {
    if (!ReadSetting(L"XBOX_STEAM_HANDOFF_SDK", sdkPath, ARRAYSIZE(sdkPath)) ||
            !ReadSetting(L"XBOX_STEAM_HANDOFF_HIVE", hivePath, ARRAYSIZE(hivePath)))
        return STEAM_HANDOFF_SDK_REPORT | ERROR_INVALID_DATA;
    DWORD report = STEAM_HANDOFF_SDK_REPORT;
    HMODULE client = LoadPackagedLibrary(L"steamclient64.dll", 0);
    DWORD result = WithSteamDiscovery(client, hivePath, &report, InitSteamSdk, retain ? &report : NULL, NULL);
    if (client) FreeLibrary(client);
    return report | (result & 0xffffu);
}

enum LegacyOperation { LegacyInit, LegacyIsRunning, LegacyRestart };
typedef struct LegacyCall {
    enum LegacyOperation operation;
    HMODULE sdk;
    FARPROC entry;
    DWORD app;
    DWORD lastError;
    bool called;
    bool result;
} LegacyCall;

static SRWLOCK legacyLock = SRWLOCK_INIT;
static volatile LONG legacyTraceEvents[3];

static bool MatchLegacyAccount(HMODULE sdk) {
    typedef void *(__cdecl *GetUser)(void);
    typedef bool (__cdecl *IsLoggedOn)(void *);
    typedef ULONGLONG (__cdecl *GetSteamId)(void *);
    GetUser getUser = (GetUser)(void *)GetProcAddress(sdk, "SteamAPI_SteamUser_v023");
    IsLoggedOn loggedOn = (IsLoggedOn)(void *)GetProcAddress(sdk, "SteamAPI_ISteamUser_BLoggedOn");
    GetSteamId getId = (GetSteamId)(void *)GetProcAddress(sdk, "SteamAPI_ISteamUser_GetSteamID");
    ULONGLONG expected = ReadNumber(L"XBOX_STEAM_HANDOFF_ACCOUNT");
    void *user = getUser ? getUser() : NULL;
    return expected && user && loggedOn && getId && loggedOn(user) && getId(user) == expected;
}

static DWORD CallLegacySdk(DWORD *report, void *context) {
    typedef bool (__cdecl *Query)(void);
    typedef bool (__cdecl *Restart)(DWORD);
    typedef void (__cdecl *Shutdown)(void);
    LegacyCall *call = context;
    SetLastError(call->lastError);
    call->result = call->operation == LegacyRestart ? ((Restart)(void *)call->entry)(call->app) :
        ((Query)(void *)call->entry)();
    call->lastError = GetLastError();
    call->called = true;
    if (call->operation == LegacyInit && call->result) {
        *report |= STEAM_HANDOFF_SDK_INITIALIZED;
        if (MatchLegacyAccount(call->sdk)) {
            *report |= STEAM_HANDOFF_SDK_MATCHED;
        } else {
            Shutdown shutdown = (Shutdown)(void *)GetProcAddress(call->sdk, "SteamAPI_Shutdown");
            if (shutdown) shutdown();
            call->result = false;
        }
    }
    return ERROR_SUCCESS;
}

static char *TraceError(char *out, const char *prefix, DWORD error) {
    static const char digits[] = "0123456789ABCDEF";
    while (*prefix) *out++ = *prefix++;
    for (int digit = 7; digit >= 0; --digit) *out++ = digits[(error >> (4 * digit)) & 15u];
    return out;
}

static void TraceLegacyResult(char const *name, DWORD report, LegacyCall const *call,
        DWORD discovery, DWORD cleanup) {
    if (InterlockedIncrement(&legacyTraceEvents[call->operation]) > 4) return;
    char line[180];
    char *out = line;
    const char *parts[] = {"asset steam-legacy-", name, call->called ? (call->result ? "=true" : "=false") : "=unavailable",
        report & STEAM_HANDOFF_SDK_DISCOVERY ? " discovery=private" : " discovery=default",
        report & STEAM_HANDOFF_SDK_MATCHED ? " account=matched" :
            report & STEAM_HANDOFF_SDK_INITIALIZED ? " account=unverified" : ""};
    for (unsigned i = 0; i < ARRAYSIZE(parts); ++i)
        for (const char *part = parts[i]; *part; ++part) *out++ = *part;
    if (discovery) out = TraceError(out, " discovery-error=0x", discovery);
    if (cleanup) out = TraceError(out, " cleanup-error=0x", cleanup);
    if (!call->called) out = TraceError(out, " sdk-error=0x", call->lastError);
    *out++ = '\r';
    *out++ = '\n';
    AppendTrace(line, (DWORD)(out - line));
}

static bool InvokeLegacy(enum LegacyOperation operation, DWORD app, const char *symbol, const char *trace) {
    const DWORD before = GetLastError();
    WCHAR enabled[2];
    if (!ReadSetting(L"XBOX_STEAM_HANDOFF_LEGACY", enabled, ARRAYSIZE(enabled)) || enabled[0] != L'1') {
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }

    AcquireSRWLockExclusive(&legacyLock);
    LegacyCall call = {0};
    call.operation = operation;
    call.app = app;
    call.lastError = ERROR_INVALID_DATA;
    DWORD report = 0;
    DWORD discovery = 0;
    DWORD cleanup = 0;
    if (ReadSetting(L"XBOX_STEAM_HANDOFF_SDK", sdkPath, ARRAYSIZE(sdkPath)) &&
            ReadSetting(L"XBOX_STEAM_HANDOFF_HIVE", hivePath, ARRAYSIZE(hivePath))) {
        // The host verifies this private, staged image. Resolve only its already-loaded copy.
        WCHAR actual[1024];
        call.sdk = GetModuleHandleW(L"steam_api64.dll");
        DWORD length = call.sdk ? GetModuleFileNameW(call.sdk, actual, ARRAYSIZE(actual)) : 0;
        if (length && length < ARRAYSIZE(actual) &&
                CompareStringOrdinal(sdkPath, -1, actual, -1, TRUE) == CSTR_EQUAL) {
            call.entry = GetProcAddress(call.sdk, symbol);
            call.lastError = call.entry ? before : GetLastError();
            if (call.entry) {
                HMODULE client = LoadPackagedLibrary(L"steamclient64.dll", 0);
                discovery = WithSteamDiscovery(client, hivePath, &report, CallLegacySdk, &call, &cleanup);
                // If discovery is unavailable, preserve the original SDK's decision.
                if (!call.called) CallLegacySdk(&report, &call);
                if (client) FreeLibrary(client);
            }
        }
    }
    TraceLegacyResult(trace, report, &call, discovery, cleanup);
    ReleaseSRWLockExclusive(&legacyLock);
    SetLastError(call.lastError);
    return call.result;
}

bool __cdecl XboxLegacySteamInit(void) {
    return InvokeLegacy(LegacyInit, 0, "SteamAPI_Init", "init");
}

bool __cdecl XboxLegacySteamIsRunning(void) {
    return InvokeLegacy(LegacyIsRunning, 0, "SteamAPI_IsSteamRunning", "is-running");
}

bool __cdecl XboxLegacySteamRestart(DWORD app) {
    return InvokeLegacy(LegacyRestart, app, "SteamAPI_RestartAppIfNecessary", "restart");
}
