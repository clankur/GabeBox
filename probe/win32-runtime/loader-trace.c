/* An opt-in loader trace for unsigned, locally staged Windows images. */
#define WIN32_LEAN_AND_MEAN
#include "loader-trace-output.h"

static unsigned int events;
static volatile LONG faultTraceStarted;
static volatile LONG workingDirectoryStarted;
static volatile LONG originalImageObserved;
static volatile LONG assetEvents;
static DWORD faultTraceCount;
typedef BOOL (WINAPI *FindImage)(DWORD, LPCWSTR, HMODULE *);
static FindImage findImage;

static char *Text(char *out, const char *text) {
    while (*text) *out++ = *text++;
    return out;
}

static char *Token(char *out, const char *name, int basename) {
    if (!name) return Text(out, "unknown");
    if (basename) {
        for (const char *p = name; *p; ++p) {
            if (*p == '\\' || *p == '/' || *p == ':') name = p + 1;
        }
    }
    for (unsigned int i = 0; name[i] && i < 100; ++i) {
        char ch = name[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_' || ch == '.' || ch == '?' || ch == '@' || ch == '$' || ch == '#'))
            ch = '_';
        *out++ = ch;
    }
    return out;
}

static char *Hex(char *out, unsigned long value, int digits) {
    const char hex[] = "0123456789ABCDEF";
    while (digits) *out++ = hex[(value >> (4 * --digits)) & 15];
    return out;
}

static const char *ModuleLocation(HANDLE module) {
    WCHAR image[512], loaded[512];
    DWORD imageLength = GetModuleFileNameW(0, image, 512);
    DWORD loadedLength = GetModuleFileNameW(module, loaded, 512);
    if (!imageLength || imageLength >= 512 || !loadedLength || loadedLength >= 512) return "unknown";
    while (imageLength && image[imageLength - 1] != '\\') --imageLength;
    while (loadedLength && loaded[loadedLength - 1] != '\\') --loadedLength;
    if (!imageLength || !loadedLength) return "unknown";
    if (loadedLength < imageLength ||
            CompareStringOrdinal(image, (int)imageLength, loaded, (int)imageLength, TRUE) != CSTR_EQUAL)
        return "other";
    return loadedLength == imageLength ? "adjacent" : "staged-subdirectory";
}

static const WCHAR *AssetName(const WCHAR *path) {
    if (!path) return NULL;
    const WCHAR *leaf = path;
    UINT length = 0;
    for (; length < 1024 && path[length]; ++length)
        if (path[length] == L'/' || path[length] == L'\\') leaf = path + length + 1;
    if (length >= 1024 || !*leaf) return NULL;
    static const WCHAR *const extensions[] = {L".vert", L".frag", L".comp", L".shader", L".dxbc", L".cso", L".fxc", L".hlsl"};
    const WCHAR *dot = path + length;
    while (dot > leaf && dot[-1] != L'.') --dot;
    if (dot == leaf) return NULL;
    for (UINT i = 0; i < ARRAYSIZE(extensions); ++i)
        if (CompareStringOrdinal(dot - 1, -1, extensions[i], -1, TRUE) == CSTR_EQUAL) return leaf;
    return NULL;
}

static void ObserveAsset(const WCHAR *path, const char *operation, BOOL success, DWORD code) {
    const DWORD error = GetLastError();
    const WCHAR *leaf = AssetName(path);
    if (leaf && InterlockedIncrement(&assetEvents) <= 40) {
        char name[65], line[160];
        UINT i = 0;
        for (; i + 1 < ARRAYSIZE(name) && leaf[i]; ++i) name[i] = leaf[i] < 128 ? (char)leaf[i] : '_';
        name[i] = 0;
        char *out = Text(Text(Text(line, "asset "), operation), " ");
        out = Token(out, name, 0);
        out = Text(out, (path[0] == L'\\' || (path[0] && path[1] == L':')) ? " absolute" : " relative");
        out = success ? Text(out, " ok") : Hex(Text(out, " error=0x"), code, 8);
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
    }
    SetLastError(error);
}

static void ObserveAssetA(const char *path, const char *operation, BOOL success, DWORD code) {
    const DWORD error = GetLastError();
    WCHAR wide[1024];
    if (path && MultiByteToWideChar(CP_ACP, 0, path, -1, wide, ARRAYSIZE(wide)))
        ObserveAsset(wide, operation, success, code);
    SetLastError(error);
}

static void ObserveWorkingDirectory(void) {
    WCHAR current[512], approved[512];
    const DWORD actual = GetCurrentDirectoryW(ARRAYSIZE(current), current);
    const DWORD expected = GetEnvironmentVariableW(L"XBOX_WIN32_INSTALL_ROOT", approved, ARRAYSIZE(approved));
    const char *line = "asset cwd=unknown\r\n";
    if (actual && actual < ARRAYSIZE(current) && expected && expected < ARRAYSIZE(approved)) {
        line = CompareStringOrdinal(current, actual, approved, expected, TRUE) == CSTR_EQUAL
            ? "asset cwd=install-root\r\n" : "asset cwd=other\r\n";
    }
    DWORD size = 0;
    while (line[size]) ++size;
    AppendTrace(line, size);
}

static void RestoreWorkingDirectory(void) {
    if (InterlockedExchange(&workingDirectoryStarted, 1)) return;
    const WCHAR variable[] = L"XBOX_WIN32_WORKING_DIRECTORY";
    const DWORD capacity = GetEnvironmentVariableW(variable, NULL, 0);
    if (capacity <= 1) return;
    DWORD error = ERROR_FILENAME_EXCED_RANGE;
    if (capacity <= 32768) {
        HANDLE heap = GetProcessHeap();
        WCHAR *path = HeapAlloc(heap, 0, capacity * sizeof(WCHAR));
        error = ERROR_NOT_ENOUGH_MEMORY;
        if (path) {
            const DWORD length = GetEnvironmentVariableW(variable, path, capacity);
            if (length && length < capacity) error = SetCurrentDirectoryW(path) ? ERROR_SUCCESS : GetLastError();
            else error = ERROR_INVALID_NAME;
            HeapFree(heap, 0, path);
        }
    }
    char line[70];
    char *out = Text(line, "asset cwd-set=");
    out = error == ERROR_SUCCESS ? Text(out, "ok") : Hex(Text(out, "error-0x"), error, 8);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

static DWORD RuntimeError(HMODULE runtime) {
    typedef int (__cdecl *GetError)(int *);
    GetError getError = (GetError)(void *)GetProcAddress(runtime, "_get_errno");
    int error = 0;
    return getError && getError(&error) == 0 ? (DWORD)error : ERROR_PROC_NOT_FOUND;
}

static void Record(const char *name, HANDLE loaded, int isLoad, int success, DWORD error) {
    if (__atomic_fetch_add(&events, 1u, __ATOMIC_RELAXED) >= 128) return;
    char buffer[181];
    char *out = Text(buffer, isLoad ? "load " : "export ");
    if (!isLoad && (unsigned long long)name <= 0xffffu) {
        *out++ = '#';
        out = Hex(out, (unsigned long)(unsigned long long)name, 4);
    } else {
        out = Token(out, name, isLoad);
    }
    if (success) {
        out = Text(out, " ok");
        if (isLoad) {
            out = Text(out, " source=");
            out = Text(out, ModuleLocation(loaded));
        }
    } else {
        out = Text(out, " error=0x");
        out = Hex(out, error, 8);
    }
    *out++ = '\r';
    *out++ = '\n';
    AppendTrace(buffer, (DWORD)(out - buffer));
}

static LONG WINAPI ObserveFault(EXCEPTION_POINTERS *exception) {
    const EXCEPTION_RECORD *fault = exception->ExceptionRecord;
    if (fault->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
            __atomic_fetch_add(&faultTraceCount, 1u, __ATOMIC_RELAXED) >= 8) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD error = GetLastError();
    char line[150];
    char *out = Text(line, "fault access-violation");
    HMODULE module = NULL;
    if (findImage && findImage(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)fault->ExceptionAddress, &module)) {
        WCHAR path[512];
        DWORD length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
        if (length && length < ARRAYSIZE(path)) {
            const WCHAR *name = path + length;
            while (name > path && name[-1] != L'\\') --name;
            char token[57];
            unsigned int i = 0;
            for (; name[i] && i + 1 < ARRAYSIZE(token); ++i) token[i] = name[i] < 128 ? (char)name[i] : '_';
            token[i] = '\0';
            out = Token(Text(out, " module="), token, 1);
        }
        out = Hex(Text(out, " rva=0x"), (DWORD)((ULONG_PTR)fault->ExceptionAddress - (ULONG_PTR)module), 8);
    }
    if (fault->NumberParameters >= 2) {
        const ULONG_PTR operation = fault->ExceptionInformation[0];
        out = Text(out, operation == 0 ? " read" : operation == 1 ? " write" : operation == 8 ? " execute" : " other");
        if (fault->ExceptionInformation[1] < 4096) out = Text(out, " near-null");
    }
    *out++ = '\r';
    *out++ = '\n';
    AppendTrace(line, (DWORD)(out - line));
    SetLastError(error);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void StartFaultTrace(void) {
    if (InterlockedExchange(&faultTraceStarted, 1) ||
            GetEnvironmentVariableW(L"XBOX_WIN32_LOADER_TRACE", NULL, 0) <= 1) return;
    ObserveWorkingDirectory();
    typedef PVOID (WINAPI *RegisterHandler)(ULONG, PVECTORED_EXCEPTION_HANDLER);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    RegisterHandler registerHandler = kernel ? (RegisterHandler)(void *)GetProcAddress(kernel, "AddVectoredExceptionHandler") : NULL;
    findImage = kernel ? (FindImage)(void *)GetProcAddress(kernel, "GetModuleHandleExW") : NULL;
    const char *line = registerHandler && registerHandler(0, ObserveFault)
        ? "fault observer=ready\r\n" : "fault observer=unavailable\r\n";
    DWORD length = 0;
    while (line[length]) ++length;
    AppendTrace(line, length);
}

static BOOL DynamicCompanionsEnabled(void) {
    WCHAR enabled[2];
    return GetEnvironmentVariableW(L"XBOX_WIN32_DYNAMIC_COMPANIONS", enabled, ARRAYSIZE(enabled)) == 1 &&
        enabled[0] == L'1';
}

static BOOL FindStagedCompanion(LPCWSTR name, WCHAR *staged, DWORD capacity) {
    if (!name) return FALSE;
    WCHAR original[512], normalized[512];
    DWORD root = GetEnvironmentVariableW(L"XBOX_WIN32_ORIGINAL_EXECUTABLE", original, ARRAYSIZE(original));
    if (!root || root >= ARRAYSIZE(original)) return FALSE;
    for (DWORD i = 0; i < root; ++i) {
        if (original[i] == L'/') original[i] = L'\\';
    }
    while (root && original[root - 1] != L'\\') --root;
    if (!root) return FALSE;

    DWORD length = 0;
    while (length < ARRAYSIZE(normalized) && name[length]) {
        normalized[length] = name[length] == L'/' ? L'\\' : name[length];
        ++length;
    }
    if (length <= root || length == ARRAYSIZE(normalized) ||
            CompareStringOrdinal(original, (int)root, normalized, (int)root, TRUE) != CSTR_EQUAL) return FALSE;
    normalized[length] = 0;
    const WCHAR *part = normalized + root;
    for (const WCHAR *p = part; ; ++p) {
        if (!*p || *p == L'\\') {
            if (p == part || (p[-1] == L'.' || p[-1] == L' ')) return FALSE;
            if (!*p) break;
            part = p + 1;
        } else if (*p < L' ' || *p == L':') {
            return FALSE;
        }
    }
    if (length < root + 4 || CompareStringOrdinal(normalized + length - 4, 4, L".dll", 4, TRUE) != CSTR_EQUAL)
        return FALSE;

    DWORD directory = GetModuleFileNameW(NULL, staged, capacity);
    if (!directory || directory >= capacity) return FALSE;
    while (directory && staged[directory - 1] != L'\\') --directory;
    if (!directory || directory + length - root >= capacity) return FALSE;
    for (DWORD i = root; i <= length; ++i) staged[directory + i - root] = normalized[i];
    for (WCHAR *p = staged + directory; *p; ++p) {
        if (*p != L'\\') continue;
        *p = 0;
        const DWORD attributes = GetFileAttributesW(staged);
        *p = L'\\';
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return FALSE;
    }
    const DWORD attributes = GetFileAttributesW(staged);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

HANDLE WINAPI XboxLoadLibraryA(const char *name) {
    DWORD previous = GetLastError();
    RestoreWorkingDirectory();
    StartFaultTrace();
    WCHAR wide[512], staged[512];
    const BOOL companion = name && DynamicCompanionsEnabled() &&
        MultiByteToWideChar(CP_ACP, 0, name, -1, wide, ARRAYSIZE(wide)) &&
        FindStagedCompanion(wide, staged, ARRAYSIZE(staged));
    SetLastError(previous);
    HANDLE result = companion ? LoadLibraryW(staged) : LoadLibraryA(name);
    DWORD error = GetLastError();
    Record(name, result, 1, result != 0, error);
    SetLastError(error);
    return result;
}

static void RecordWideLoad(LPCWSTR path, HMODULE result, DWORD error) {
    if (!path) {
        Record(NULL, result, 1, result != NULL, error);
        return;
    }
    const WCHAR *leaf = path;
    for (const WCHAR *part = path; *part; ++part) {
        if (*part == L'\\' || *part == L'/' || *part == L':') leaf = part + 1;
    }
    char name[101];
    unsigned length = 0;
    while (length + 1 < ARRAYSIZE(name) && leaf[length]) {
        name[length] = leaf[length] < 128 ? (char)leaf[length] : '_';
        ++length;
    }
    name[length] = 0;
    Record(name, result, 1, result != NULL, error);
}

HMODULE WINAPI XboxLoadLibraryW(LPCWSTR name) {
    const DWORD previous = GetLastError();
    WCHAR staged[512];
    const BOOL companion = DynamicCompanionsEnabled() && FindStagedCompanion(name, staged, ARRAYSIZE(staged));
    SetLastError(previous);
    HMODULE result = LoadLibraryW(companion ? staged : name);
    const DWORD error = GetLastError();
    RecordWideLoad(name, result, error);
    SetLastError(error);
    return result;
}

HMODULE WINAPI XboxLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    const DWORD previous = GetLastError();
    WCHAR staged[512];
    const BOOL companion = !file && DynamicCompanionsEnabled() && FindStagedCompanion(name, staged, ARRAYSIZE(staged));
    SetLastError(previous);
    HMODULE result = LoadLibraryExW(companion ? staged : name, file, flags);
    const DWORD error = GetLastError();
    RecordWideLoad(name, result, error);
    SetLastError(error);
    return result;
}

FARPROC WINAPI XboxGetProcAddress(HANDLE module, const char *name) {
    FARPROC result = GetProcAddress(module, name);
    DWORD error = GetLastError();
    Record(name, module, 0, result != 0, error);
    SetLastError(error);
    return result;
}

DWORD WINAPI XboxOriginalGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) {
    const DWORD previous = GetLastError();
    if (filename && size && (!module || module == GetModuleHandleW(NULL))) {
        const WCHAR variable[] = L"XBOX_WIN32_ORIGINAL_EXECUTABLE";
        const DWORD capacity = GetEnvironmentVariableW(variable, NULL, 0);
        if (capacity > 1 && capacity <= 32768) {
            HANDLE heap = GetProcessHeap();
            WCHAR *path = HeapAlloc(heap, 0, capacity * sizeof(WCHAR));
            if (path) {
                const DWORD length = GetEnvironmentVariableW(variable, path, capacity);
                if (length && length < capacity) {
                    const DWORD copied = length < size ? length : size - 1;
                    for (DWORD i = 0; i < copied; ++i) filename[i] = path[i];
                    filename[copied] = 0;
                    HeapFree(heap, 0, path);
                    if (!InterlockedExchange(&originalImageObserved, 1)) {
                        const char line[] = "asset executable-path=install-image\r\n";
                        AppendTrace(line, sizeof(line) - 1);
                    }
                    SetLastError(length < size ? previous : ERROR_INSUFFICIENT_BUFFER);
                    return length < size ? length : size;
                }
                HeapFree(heap, 0, path);
            }
        }
    }
    SetLastError(previous);
    return GetModuleFileNameW(module, filename, size);
}

HANDLE WINAPI XboxTraceCreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
        DWORD creation, DWORD attributes, HANDLE templateFile) {
    HANDLE result = CreateFileW(path, access, share, security, creation, attributes, templateFile);
    ObserveAsset(path, "open", result != INVALID_HANDLE_VALUE, GetLastError());
    return result;
}

HANDLE WINAPI XboxTraceCreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
        DWORD creation, DWORD attributes, HANDLE templateFile) {
    HANDLE result = CreateFileA(path, access, share, security, creation, attributes, templateFile);
    ObserveAssetA(path, "open", result != INVALID_HANDLE_VALUE, GetLastError());
    return result;
}

DWORD WINAPI XboxTraceGetFileAttributesW(LPCWSTR path) {
    DWORD result = GetFileAttributesW(path);
    ObserveAsset(path, "attributes", result != INVALID_FILE_ATTRIBUTES, GetLastError());
    return result;
}

DWORD WINAPI XboxTraceGetFileAttributesA(LPCSTR path) {
    DWORD result = GetFileAttributesA(path);
    ObserveAssetA(path, "attributes", result != INVALID_FILE_ATTRIBUTES, GetLastError());
    return result;
}

void *__cdecl XboxTraceFopen(LPCSTR path, LPCSTR mode) {
    typedef void *(__cdecl *Open)(LPCSTR, LPCSTR);
    HMODULE runtime = GetModuleHandleW(L"ucrtbase.dll");
    Open open = runtime ? (Open)(void *)GetProcAddress(runtime, "fopen") : NULL;
    if (!open) return NULL;
    void *file = open(path, mode);
    ObserveAssetA(path, "fopen-errno", file != NULL, file ? 0 : RuntimeError(runtime));
    return file;
}

void *__cdecl XboxTraceWfsopen(LPCWSTR path, LPCWSTR mode, int sharing) {
    typedef void *(__cdecl *Open)(LPCWSTR, LPCWSTR, int);
    HMODULE runtime = GetModuleHandleW(L"ucrtbase.dll");
    Open open = runtime ? (Open)(void *)GetProcAddress(runtime, "_wfsopen") : NULL;
    if (!open) return NULL;
    void *file = open(path, mode, sharing);
    ObserveAsset(path, "wfsopen-errno", file != NULL, file ? 0 : RuntimeError(runtime));
    return file;
}
