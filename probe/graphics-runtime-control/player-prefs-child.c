/* Exercise the scoped registry in two child processes using a fresh hive. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <appmodel.h>
#include "../child-process.h"

DECLSPEC_IMPORT DECLSPEC_NORETURN void WINAPI ExitProcess(UINT code);

static const WCHAR fixturePrefix[] = L"Software\\Xbox Portability Test ";
static const WCHAR product[] = L"\\Prefs";
static const char sample[] = "registry fixture";
static const ULONGLONG wideValue = 0x3ff0000000000000ULL;

static BOOL EndsWith(LPCWSTR value, LPCWSTR suffix) {
    DWORD length = 0, size = 0;
    while (value[length]) ++length;
    while (suffix[size]) ++size;
    if (length < size) return FALSE;
    for (DWORD i = 0; i < size; ++i) if (value[length - size + i] != suffix[i]) return FALSE;
    return TRUE;
}

static UINT Worker(BOOL write) {
    typedef LSTATUS (WINAPI *CreateKey)(HKEY, LPCWSTR, PHKEY);
    typedef LSTATUS (WINAPI *OpenKey)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
    typedef LSTATUS (WINAPI *CloseKey)(HKEY);
    typedef LSTATUS (WINAPI *SetValue)(HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD);
    typedef LSTATUS (WINAPI *QueryValue)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
    typedef LSTATUS (WINAPI *DeleteKey)(HKEY, LPCWSTR);
    WCHAR scope[128], company[128];
    const DWORD scopeLength = GetEnvironmentVariableW(L"XBOX_WIN32_PREFS_KEY", scope, ARRAYSIZE(scope));
    const DWORD prefixLength = ARRAYSIZE(fixturePrefix) - 1;
    if (scopeLength != prefixLength + 8 + ARRAYSIZE(product) - 1 ||
            CompareStringOrdinal(scope, prefixLength, fixturePrefix, prefixLength, FALSE) != CSTR_EQUAL ||
            !EndsWith(scope, product)) return 0x7200;
    for (DWORD i = 0; i < prefixLength + 8; ++i) company[i] = scope[i];
    company[prefixLength + 8] = 0;
    const HMODULE adapter = LoadPackagedLibrary(L"XboxWin32PlayerPrefs.dll", 0);
    const HMODULE registry = LoadLibraryExW(L"advapi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!adapter || !registry) return 0x7201;
    const CreateKey create = (CreateKey)(void *)GetProcAddress(adapter, "RegCreateKeyW");
    const OpenKey open = (OpenKey)(void *)GetProcAddress(adapter, "RegOpenKeyExW");
    const CloseKey close = (CloseKey)(void *)GetProcAddress(registry, "RegCloseKey");
    const SetValue set = (SetValue)(void *)GetProcAddress(registry, "RegSetValueExA");
    const QueryValue query = (QueryValue)(void *)GetProcAddress(registry, "RegQueryValueExA");
    if (!create || !open || !close || !query || !set) return 0x7202;
    HKEY key = NULL;
    LSTATUS status;
    if (write) {
        status = create(HKEY_CURRENT_USER, scope, &key);
        if (status) return 0x72030000u | (status & 0xffffu);
        status = set(key, "fixture_float", 0, REG_DWORD, (const BYTE *)&wideValue, sizeof(wideValue));
        if (!status) status = set(key, "fixture_string", 0, REG_BINARY, (const BYTE *)sample, sizeof(sample));
        close(key);
        return status ? 0x72040000u | (status & 0xffffu) : PROBE_EXPECTED_CHILD_EXIT;
    }

    status = open(HKEY_CURRENT_USER, scope, 0, KEY_READ, &key);
    if (status) return 0x72050000u | (status & 0xffffu);
    ULONGLONG value = 0;
    DWORD type = 0, bytes = sizeof(value);
    if (query(key, "fixture_float", NULL, &type, (BYTE *)&value, &bytes) ||
            type != REG_DWORD || bytes != sizeof(value) || value != wideValue ||
            query(key, "fixture_string", NULL, NULL, NULL, NULL)) return 0x7206;
    char readback[sizeof(sample)];
    bytes = 1;
    if (query(key, "fixture_string", NULL, &type, (BYTE *)readback, &bytes) != ERROR_MORE_DATA ||
            bytes != sizeof(sample)) return 0x7207;
    if (query(key, "fixture_string", NULL, &type, (BYTE *)readback, &bytes) || type != REG_BINARY) return 0x7208;
    for (DWORD i = 0; i < sizeof(sample); ++i) if (readback[i] != sample[i]) return 0x7208;
    close(key);

    const HMODULE shell = LoadLibraryExW(L"shlwapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const DeleteKey delete = shell ? (DeleteKey)(void *)GetProcAddress(shell, "SHDeleteKeyW") : NULL;
    if (!delete) return 0x7209;
    status = open(HKEY_CURRENT_USER, company, 0, DELETE, &key);
    if (status) return 0x720a0000u | (status & 0xffffu);
    status = delete(key, product + 1);
    close(key);
    if (status) return 0x720b0000u | (status & 0xffffu);
    status = open(HKEY_CURRENT_USER, scope, 0, KEY_READ, &key);
    if (!status) close(key);
    return status == ERROR_FILE_NOT_FOUND ? PROBE_EXPECTED_CHILD_EXIT : 0x720c0000u | (status & 0xffffu);
}

static WCHAR *Append(WCHAR *out, LPCWSTR text) {
    while (*text) *out++ = *text++;
    *out = 0;
    return out;
}

static UINT RunWorker(LPCWSTR suffix) {
    static WCHAR image[1024], command[1056];
    const DWORD size = GetModuleFileNameW(NULL, image, ARRAYSIZE(image));
    if (!size || size >= ARRAYSIZE(image)) return 0x7210;
    Append(Append(Append(command, L"\""), image), suffix);
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION child;
    startup.cb = sizeof(startup);
    if (!CreateProcessW(image, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &child))
        return 0x72110000u | (GetLastError() & 0xffffu);
    CloseHandle(child.hThread);
    DWORD result = 0x7212;
    if (WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &result);
    else if (TerminateProcess(child.hProcess, ERROR_TIMEOUT)) WaitForSingleObject(child.hProcess, 1000);
    CloseHandle(child.hProcess);
    return result;
}

static WCHAR *AppendToken(WCHAR *end, DWORD token) {
    for (int shift = 28; shift >= 0; shift -= 4) *end++ = L"0123456789abcdef"[(token >> shift) & 15];
    *end = 0;
    return end;
}

static UINT VerifyPersistence(void) {
    WCHAR folder[1024];
    const DWORD length = GetTempPathW(ARRAYSIZE(folder), folder);
    if (!length || length + 64 >= ARRAYSIZE(folder)) return 0x7220;
    WCHAR *end = Append(folder + length, L"xbox-player-prefs-");
    const DWORD token = GetCurrentProcessId() ^ GetTickCount();
    end = AppendToken(end, token);
    if (!CreateDirectoryW(folder, NULL)) return 0x72210000u | (GetLastError() & 0xffffu);
    Append(end, L"\\registry.dat");
    WCHAR scope[128];
    Append(AppendToken(Append(scope, fixturePrefix), token), product);
    UINT result = 0x7222;
    if (SetEnvironmentVariableW(L"XBOX_WIN32_PREFS_KEY", scope) &&
            SetEnvironmentVariableW(L"XBOX_WIN32_PREFS_HIVE", folder)) {
        result = RunWorker(L"\" --prefs-write");
        if (result == PROBE_EXPECTED_CHILD_EXIT) {
            const DWORD attributes = GetFileAttributesW(folder);
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
                result = 0x7223;
            else result = RunWorker(L"\" --prefs-read");
        }
    }
    static const WCHAR *const files[] = {L"\\registry.dat", L"\\registry.dat.LOG", L"\\registry.dat.LOG1", L"\\registry.dat.LOG2"};
    for (UINT i = 0; i < ARRAYSIZE(files); ++i) {
        Append(end, files[i]);
        DeleteFileW(folder);
    }
    *end = 0;
    RemoveDirectoryW(folder);
    return result;
}

void WINAPI ChildEntry(void) {
    LPCWSTR command = GetCommandLineW();
    if (EndsWith(command, L" --prefs-write")) ExitProcess(Worker(TRUE));
    if (EndsWith(command, L" --prefs-read")) ExitProcess(Worker(FALSE));
    ExitProcess(VerifyPersistence());
}
