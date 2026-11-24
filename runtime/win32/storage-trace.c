/* Observe compatibility call results without persisting file or directory names. */
#define WIN32_LEAN_AND_MEAN
#include "loader-trace-output.h"

enum StorageOperation { Create, Change, Attributes };
static volatile LONG operations[3];
static volatile LONG cacheOperations[3];
static volatile LONG terminationCalls;
static volatile LONG shortNameObserved;

static BOOL IsSeparator(WCHAR c) {
    return c == L'\\' || c == L'/';
}

static BOOL HasDrive(const WCHAR *path) {
    const WCHAR drive = path[0] | 0x20;
    return drive >= L'a' && drive <= L'z' && path[1] == L':' && IsSeparator(path[2]);
}

static BOOL IsExtendedDosPath(const WCHAR *path) {
    return path[0] == L'\\' && path[1] == L'\\' && path[2] == L'?' && path[3] == L'\\' && HasDrive(path + 4);
}

static BOOL IsUnder(const WCHAR *path, const WCHAR *variable) {
    WCHAR root[512];
    DWORD size = GetEnvironmentVariableW(variable, root, ARRAYSIZE(root));
    if (!size || size >= ARRAYSIZE(root)) return FALSE;
    const WCHAR *base = root;
    if (IsExtendedDosPath(root)) {
        base += 4;
        size -= 4;
    }
    while (size && IsSeparator(base[size - 1])) --size;
    DWORD i = 0;
    while (i < size && path[i]) {
        if (!IsSeparator(path[i]) || !IsSeparator(base[i])) {
            if (CompareStringOrdinal(path + i, 1, base + i, 1, TRUE) != CSTR_EQUAL) return FALSE;
        }
        ++i;
    }
    return i && i == size && (!path[i] || IsSeparator(path[i]));
}

static BOOL ContainsShaderCache(const WCHAR *path) {
    const WCHAR cache[] = L"shader_cache";
    for (const WCHAR *part = path; *part;) {
        if (IsSeparator(*part)) {
            ++part;
            continue;
        }
        const WCHAR *end = part;
        while (*end && !IsSeparator(*end)) ++end;
        if (end - part == ARRAYSIZE(cache) - 1 &&
                CompareStringOrdinal(part, ARRAYSIZE(cache) - 1, cache, -1, TRUE) == CSTR_EQUAL) return TRUE;
        part = end;
    }
    return FALSE;
}

static char *CopyText(char *out, const char *text) {
    while (*text) *out++ = *text++;
    return out;
}

static void RecordResult(const char *operation, const char *form, const char *scope,
        BOOL cache, BOOL success, DWORD code) {
    char line[160];
    char *out = CopyText(CopyText(line, "asset storage="), operation);
    if (form) {
        out = CopyText(CopyText(out, " form="), form);
        out = CopyText(CopyText(out, " scope="), scope);
        out = CopyText(out, cache ? " shader-cache=1" : " shader-cache=0");
        out = CopyText(out, success ? " ok=1" : " ok=0");
    }
    out = CopyText(out, " code=0x");
    for (unsigned shift = 32; shift; shift -= 4) {
        *out++ = "0123456789ABCDEF"[(code >> (shift - 4)) & 15];
    }
    out = CopyText(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

static void ObserveShortName(const WCHAR *path, const char *scope) {
    if (InterlockedExchange(&shortNameObserved, 1)) return;
    typedef DWORD (WINAPI *GetShortPath)(LPCWSTR, LPWSTR, DWORD);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    GetShortPath getShortPath = kernel ? (GetShortPath)(void *)GetProcAddress(kernel, "GetShortPathNameW") : NULL;
    if (!getShortPath) {
        RecordResult("GetShortPathNameW", "unavailable", scope, TRUE, FALSE, ERROR_PROC_NOT_FOUND);
        return;
    }
    WCHAR output[1024];
    const DWORD used = getShortPath(path, output, ARRAYSIZE(output));
    if (!used || used >= ARRAYSIZE(output)) {
        RecordResult("GetShortPathNameW", "unavailable", scope, TRUE, FALSE,
            used ? ERROR_INSUFFICIENT_BUFFER : GetLastError());
        return;
    }
    DWORD original = 0;
    while (original < ARRAYSIZE(output) && path[original]) ++original;
    const char *form = used < MAX_PATH - 1 ? "fits-legacy-path" : used < original ? "shorter" : "unchanged";
    RecordResult("GetShortPathNameW", form, scope, TRUE, TRUE, ERROR_SUCCESS);
}

static void ObservePath(enum StorageOperation operation, const WCHAR *path, BOOL success) {
    const DWORD previous = GetLastError();
    if (path) {
        const BOOL cache = ContainsShaderCache(path);
        volatile LONG *counter = cache ? cacheOperations + operation : operations + operation;
        if (*counter < 16 && InterlockedIncrement(counter) <= 16) {
            const BOOL extended = IsExtendedDosPath(path);
            const WCHAR *dos = extended ? path + 4 : path;
            const char *form = extended ? "extended-dos" :
                HasDrive(path) ? "dos" : IsSeparator(*path) ? "rooted" : "relative";
            const BOOL roaming = IsUnder(dos, L"APPDATA");
            const BOOL local = !roaming && IsUnder(dos, L"LOCALAPPDATA");
            const char *scope = roaming ? "roaming" : local ? "local" :
                IsUnder(dos, L"XBOX_WIN32_INSTALL_ROOT") ? "install" : "other";
            const char *const names[] = {"CreateDirectoryW", "SetCurrentDirectoryW", "GetFileAttributesW"};
            RecordResult(names[operation], form, scope, cache, success, success ? 0 : previous);
            if (operation == Change && cache && !success && (roaming || local) &&
                    previous == ERROR_FILENAME_EXCED_RANGE) ObserveShortName(path, scope);
        }
    }
    SetLastError(previous);
}

BOOL WINAPI XboxTraceCreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES security) {
    const BOOL ok = CreateDirectoryW(path, security);
    ObservePath(Create, path, ok);
    return ok;
}

BOOL WINAPI XboxTraceSetCurrentDirectoryW(LPCWSTR path) {
    const BOOL ok = SetCurrentDirectoryW(path);
    ObservePath(Change, path, ok);
    return ok;
}

DWORD WINAPI XboxTraceStorageAttributesW(LPCWSTR path) {
    const DWORD result = GetFileAttributesW(path);
    ObservePath(Attributes, path, result != INVALID_FILE_ATTRIBUTES);
    return result;
}

DECLSPEC_NORETURN void WINAPI XboxTraceExitProcess(UINT code) {
    RecordResult("ExitProcess", NULL, NULL, FALSE, FALSE, code);
    ExitProcess(code);
}

BOOL WINAPI XboxTraceTerminateProcess(HANDLE process, UINT code) {
    const DWORD previous = GetLastError();
    if ((process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId()) &&
            InterlockedIncrement(&terminationCalls) <= 2) {
        RecordResult("TerminateProcess", NULL, NULL, FALSE, FALSE, code);
    }
    SetLastError(previous);
    return TerminateProcess(process, code);
}
