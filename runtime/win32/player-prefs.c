/* Send only the selected HKCU preference keys to a private application hive.
   Native child handles preserve registry value and deletion semantics. */
#define WIN32_LEAN_AND_MEAN
#include "loader-trace-output.h"

static INIT_ONCE settingsOnce = INIT_ONCE_STATIC_INIT;
static INIT_ONCE hiveOnce = INIT_ONCE_STATIC_INIT;
static WCHAR scope[512], lowScope[544], hivePath[1024];
static DWORD scopeSize, lowScopeSize, companySize, lowCompanySize;
static HKEY privateHive;
static LSTATUS hiveStatus;

static FARPROC RegistryExport(LPCSTR name) {
    HMODULE module = GetModuleHandleW(L"advapi32.dll");
    if (!module) module = LoadLibraryExW(L"advapi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module ? GetProcAddress(module, name) : NULL;
}

static BOOL CALLBACK ReadProfile(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    static const WCHAR lowPrefix[] = L"Software\\AppDataLow\\";
    const DWORD length = GetEnvironmentVariableW(L"XBOX_WIN32_PREFS_KEY", scope, ARRAYSIZE(scope));
    if (length < 12 || length >= ARRAYSIZE(scope) ||
            CompareStringOrdinal(scope, 9, L"Software\\", 9, TRUE) != CSTR_EQUAL) return TRUE;
    DWORD split = 0;
    for (DWORD i = 9; i < length; ++i) {
        if (scope[i] < L' ' || scope[i] == 0x7f) return TRUE;
        if (scope[i] != L'\\') continue;
        if (split || i == 9 || i + 1 == length) return TRUE;
        split = i;
    }
    if (!split || split - 9 > 255 || length - split - 1 > 255) return TRUE;
    const DWORD offset = ARRAYSIZE(lowPrefix) - 1;
    for (DWORD i = 0; i < offset; ++i) lowScope[i] = lowPrefix[i];
    for (DWORD i = 0; i <= length; ++i) lowScope[offset + i] = scope[i];
    scopeSize = length;
    lowScopeSize = offset + length;
    companySize = split;
    lowCompanySize = offset + split;
    return TRUE;
}

static void TraceHive(void) {
    char record[] = "asset player-prefs=hive status=0x00000000\r\n";
    static const char hex[] = "0123456789ABCDEF";
    const UINT end = sizeof(record) - 3;
    for (UINT i = 0; i < 8; ++i) record[end - 1 - i] = hex[((DWORD)hiveStatus >> (i * 4)) & 15];
    AppendTrace(record, sizeof(record) - 1);
}

static BOOL CALLBACK OpenPrivateHive(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    typedef LSTATUS (WINAPI *LoadHive)(LPCWSTR, PHKEY, REGSAM, DWORD, DWORD);
    const LoadHive load = (LoadHive)(void *)RegistryExport("RegLoadAppKeyW");
    const DWORD size = GetEnvironmentVariableW(L"XBOX_WIN32_PREFS_HIVE", hivePath, ARRAYSIZE(hivePath));
    if (!size || size >= ARRAYSIZE(hivePath)) hiveStatus = ERROR_INVALID_DATA;
    else if (!load) hiveStatus = ERROR_PROC_NOT_FOUND;
    else hiveStatus = load(hivePath, &privateHive, KEY_READ | KEY_WRITE, 0, 0);
    if (hiveStatus == ERROR_SUCCESS && !privateHive) hiveStatus = ERROR_INVALID_HANDLE;
    TraceHive();
    return TRUE;
}

static BOOL MatchesKey(LPCWSTR key, LPCWSTR prefix, DWORD length, BOOL includeChildren) {
    for (DWORD i = 0; i < length; ++i) if (!key[i]) return FALSE;
    if (key[length] && (!includeChildren || key[length] != L'\\')) return FALSE;
    return CompareStringOrdinal(key, (int)length, prefix, (int)length, TRUE) == CSTR_EQUAL;
}

static LSTATUS SelectRoot(HKEY source, LPCWSTR subkey, BOOL deleteParent, PHKEY selected) {
    *selected = source;
    if (source != HKEY_CURRENT_USER || !subkey || !InitOnceExecuteOnce(&settingsOnce, ReadProfile, NULL, NULL) ||
            !scopeSize) return ERROR_SUCCESS;
    if (!MatchesKey(subkey, scope, scopeSize, TRUE) && !MatchesKey(subkey, lowScope, lowScopeSize, TRUE) &&
            (!deleteParent || (!MatchesKey(subkey, scope, companySize, FALSE) &&
                              !MatchesKey(subkey, lowScope, lowCompanySize, FALSE)))) return ERROR_SUCCESS;

    if (!InitOnceExecuteOnce(&hiveOnce, OpenPrivateHive, NULL, NULL)) return (LSTATUS)GetLastError();
    if (hiveStatus == ERROR_SUCCESS) *selected = privateHive;
    return hiveStatus;
}

LSTATUS WINAPI XboxPrefsCreateKeyW(HKEY key, LPCWSTR subkey, PHKEY result) {
    typedef LSTATUS (WINAPI *CreateKey)(HKEY, LPCWSTR, PHKEY);
    const DWORD previous = GetLastError();
    const CreateKey native = (CreateKey)(void *)RegistryExport("RegCreateKeyW");
    HKEY root;
    const LSTATUS status = native ? SelectRoot(key, subkey, FALSE, &root) : ERROR_PROC_NOT_FOUND;
    SetLastError(previous);
    return status ? status : native(root, subkey, result);
}

LSTATUS WINAPI XboxPrefsOpenKeyExW(HKEY key, LPCWSTR subkey, DWORD options, REGSAM access, PHKEY result) {
    typedef LSTATUS (WINAPI *OpenKey)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
    const DWORD previous = GetLastError();
    const OpenKey native = (OpenKey)(void *)RegistryExport("RegOpenKeyExW");
    HKEY root;
    // Unity opens the company key with DELETE before removing the product tree.
    const LSTATUS status = native ? SelectRoot(key, subkey, !options && access == DELETE, &root) : ERROR_PROC_NOT_FOUND;
    SetLastError(previous);
    return status ? status : native(root, subkey, options, access, result);
}
