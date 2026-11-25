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

/* Long current directories.

   The process current directory cannot exceed MAX_PATH on Xbox, so
   SetCurrentDirectoryW fails with ERROR_FILENAME_EXCED_RANGE for a deep but
   existing directory that the file APIs still reach through \\?\ paths.
   Godot's DirAccess::change_dir sets the directory, reads it back with
   GetCurrentDirectoryW and immediately restores the previous one; when the
   change fails, ShaderGLES3::initialize treats the existing shader-cache
   folder as uncreatable and returns before it records the texture-unit
   limit. For that error only, an existing absolute or cwd-relative directory
   becomes a virtual current directory: SetCurrentDirectoryW succeeds and
   GetCurrentDirectoryW reports it until the next successful change. The
   process directory itself does not move, so other relative file operations
   still resolve against the last directory the system accepted. Paths with
   "." or ".." components keep the original failure. */
static SRWLOCK currentLock = SRWLOCK_INIT;
static WCHAR *virtualCurrent;
static DWORD virtualLength;
static volatile LONG virtualTraced;

static BOOL HasDotComponent(const WCHAR *path) {
    for (const WCHAR *part = path; *part;) {
        if (IsSeparator(*part)) {
            ++part;
            continue;
        }
        const WCHAR *end = part;
        while (*end && !IsSeparator(*end)) ++end;
        if ((end - part == 1 && part[0] == L'.') || (end - part == 2 && part[0] == L'.' && part[1] == L'.'))
            return TRUE;
        part = end;
    }
    return FALSE;
}

/* Returns a heap copy of the directory path in DOS form with backslashes. */
static WCHAR *LongDirectory(const WCHAR *path, DWORD *length) {
    if (!path || !*path || HasDotComponent(path)) return NULL;
    const WCHAR *dos = IsExtendedDosPath(path) ? path + 4 : path;
    const WCHAR *base = NULL;
    WCHAR *real = NULL;
    DWORD baseLength = 0;
    if (!HasDrive(dos)) {
        if (IsSeparator(*dos)) return NULL;
        if (virtualCurrent) {
            base = virtualCurrent;
            baseLength = virtualLength;
        } else {
            const DWORD size = GetCurrentDirectoryW(0, NULL);
            real = size ? (WCHAR *)HeapAlloc(GetProcessHeap(), 0, size * sizeof(WCHAR)) : NULL;
            baseLength = real ? GetCurrentDirectoryW(size, real) : 0;
            if (!baseLength || baseLength >= size) {
                if (real) HeapFree(GetProcessHeap(), 0, real);
                return NULL;
            }
            base = real;
        }
    }
    DWORD tail = 0;
    while (dos[tail]) ++tail;
    while (tail && IsSeparator(dos[tail - 1])) --tail;
    while (baseLength && IsSeparator(base[baseLength - 1])) --baseLength;
    const DWORD total = (base ? baseLength + 1 : 0) + tail;
    WCHAR *joined = tail && total < 32000 ? (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (total + 1) * sizeof(WCHAR)) : NULL;
    if (joined) {
        DWORD out = 0;
        if (base) {
            for (DWORD i = 0; i < baseLength; ++i) joined[out++] = base[i] == L'/' ? L'\\' : base[i];
            joined[out++] = L'\\';
        }
        for (DWORD i = 0; i < tail; ++i) {
            if (IsSeparator(dos[i]) && out && joined[out - 1] == L'\\') continue;
            joined[out++] = dos[i] == L'/' ? L'\\' : dos[i];
        }
        joined[out] = 0;
        *length = out;
    }
    if (real) HeapFree(GetProcessHeap(), 0, real);
    return joined;
}

static BOOL IsExistingDirectory(const WCHAR *dos, DWORD length) {
    WCHAR *extended = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (length + 5) * sizeof(WCHAR));
    if (!extended) return FALSE;
    extended[0] = extended[1] = extended[3] = L'\\';
    extended[2] = L'?';
    for (DWORD i = 0; i <= length; ++i) extended[4 + i] = dos[i];
    const DWORD attributes = GetFileAttributesW(extended);
    HeapFree(GetProcessHeap(), 0, extended);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

BOOL WINAPI XboxTraceSetCurrentDirectoryW(LPCWSTR path) {
    const DWORD entryError = GetLastError();
    AcquireSRWLockExclusive(&currentLock);
    BOOL ok = SetCurrentDirectoryW(path);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    ObservePath(Change, path, ok);
    WCHAR *replaced = NULL;
    if (ok) {
        replaced = virtualCurrent;
        virtualCurrent = NULL;
        virtualLength = 0;
    } else if (error == ERROR_FILENAME_EXCED_RANGE) {
        DWORD length = 0;
        WCHAR *directory = LongDirectory(path, &length);
        if (directory && IsExistingDirectory(directory, length)) {
            replaced = virtualCurrent;
            virtualCurrent = directory;
            virtualLength = length;
            directory = NULL;
            ok = TRUE;
            error = entryError;
            if (!InterlockedExchange(&virtualTraced, 1)) RecordResult("SetCurrentDirectoryW", "virtual-long",
                "existing", ContainsShaderCache(path), TRUE, ERROR_SUCCESS);
        }
        if (directory) HeapFree(GetProcessHeap(), 0, directory);
    }
    ReleaseSRWLockExclusive(&currentLock);
    if (replaced) HeapFree(GetProcessHeap(), 0, replaced);
    SetLastError(error);
    return ok;
}

DWORD WINAPI XboxTraceGetCurrentDirectoryW(DWORD size, LPWSTR buffer) {
    AcquireSRWLockShared(&currentLock);
    if (!virtualCurrent) {
        ReleaseSRWLockShared(&currentLock);
        return GetCurrentDirectoryW(size, buffer);
    }
    DWORD result = virtualLength + 1;
    if (buffer && size > virtualLength) {
        for (DWORD i = 0; i <= virtualLength; ++i) buffer[i] = virtualCurrent[i];
        result = virtualLength;
    }
    ReleaseSRWLockShared(&currentLock);
    return result;
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
