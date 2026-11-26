/* An opt-in loader trace for unsigned, locally staged Windows images. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include "loader-trace-output.h"
#include "hosted-xinput-load.h"
#include "cpu-sets.h"
#include <appmodel.h>
#include <d3dcommon.h>
#include <dxgi1_2.h>
#include <objbase.h>
#include "d3d12-pipeline-replay.h"

static volatile LONG64 loaderEvents[128];
static volatile LONG faultTraceStarted;
static volatile LONG exitStackObserved;
static volatile LONG cxxExceptionsObserved;
static volatile LONG workingDirectoryStarted;
static volatile LONG originalImageObserved;
static volatile LONG assetEvents;
static volatile LONG deviceEvents;
static volatile LONG devicePrimed;
static volatile LONG processorsPlaced;
static volatile LONG failedCommits;
static DWORD faultTraceCount;
static const DWORD delayModuleMissing = 0xC06D007E;
static const DWORD delayExportMissing = 0xC06D007F;
static const DWORD msvcCxxException = 0xE06D7363;
typedef BOOL (WINAPI *FindImage)(DWORD, LPCWSTR, HMODULE *);
static FindImage findImage;
typedef HRESULT (WINAPI *CreateD3D12Device)(IUnknown *, UINT, const IID *, void **);
static PVOID volatile d3d12CreateDevice;
static const IID adapter1Iid = {0x29038F61, 0x3839, 0x4626, {0x91,0xFD,0x08,0x68,0x79,0x01,0x1A,0x05}};
static const IID factory1Iid = {0x770AAE78, 0xF26F, 0x4DBA, {0xA8,0x29,0x25,0x3C,0x83,0xD1,0xB3,0x87}};
static const IID deviceIid = {0x189819F1, 0x1DB6, 0x4B57, {0xBE,0x54,0x18,0x21,0x33,0x9B,0x85,0xF7}};
static IUnknown *startupDevice;

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

static BOOL SameName(const char *left, const char *right) {
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
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

static BOOL SameClass(REFCLSID clsid, const GUID *expected) {
    if (!clsid || clsid->Data1 != expected->Data1 || clsid->Data2 != expected->Data2 || clsid->Data3 != expected->Data3)
        return FALSE;
    for (UINT index = 0; index < ARRAYSIZE(expected->Data4); ++index)
        if (clsid->Data4[index] != expected->Data4[index]) return FALSE;
    return TRUE;
}

static void RecordActivation(const char *name, HRESULT result) {
    char line[72];
    char *out = Text(Text(line, "asset "), name);
    out = Hex(Text(out, " result=0x"), (DWORD)result, 8);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

static HMODULE LoadSharedPackagedAdapter(PVOID volatile *slot, LPCWSTR name) {
    HMODULE adapter = (HMODULE)InterlockedCompareExchangePointer(slot, NULL, NULL);
    if (!adapter) {
        adapter = LoadPackagedLibrary(name, 0);
        if (!adapter) return NULL;
        HMODULE existing = (HMODULE)InterlockedCompareExchangePointer(slot, adapter, NULL);
        if (existing) {
            FreeLibrary(adapter);
            adapter = existing;
        }
    }
    return adapter;
}

static HRESULT DefaultAudioEnumerator(REFIID iid, LPVOID *object) {
    // The returned COM interfaces and pending activation callbacks own code in this DLL.
    static PVOID volatile loaded;
    const HMODULE adapter = LoadSharedPackagedAdapter(&loaded, L"XboxWin32DefaultAudio.dll");
    if (!adapter) return HRESULT_FROM_WIN32(GetLastError());
    typedef HRESULT (WINAPI *CreateEnumerator)(REFIID, LPVOID *);
    const CreateEnumerator factory = (CreateEnumerator)(void *)GetProcAddress(adapter, "XboxCreateDefaultAudioEnumerator");
    if (!factory) return HRESULT_FROM_WIN32(GetLastError());
    return factory(iid, object);
}

HRESULT WINAPI XboxCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID *object) {
    static const GUID mmDeviceEnumerator =
        {0xbcde0395, 0xe52f, 0x467c, {0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
    static const GUID mmDeviceEnumeratorIid =
        {0xa95664d2, 0x9614, 0x4f35, {0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
    static const GUID legacyXAudio =
        {0x5a508685, 0xa254, 0x4fba, {0x9b,0x82,0x9a,0x24,0xb0,0x03,0x06,0xaf}};
    HRESULT result = CoCreateInstance(clsid, outer, context, iid, object);
    if (SameClass(clsid, &mmDeviceEnumerator)) {
        const DWORD error = GetLastError();
        RecordActivation("mmdevice-enumerator-activation", result);
        WCHAR enabled[2];
        if ((result == REGDB_E_CLASSNOTREG || result == CLASS_E_CLASSNOTAVAILABLE || result == E_NOTIMPL) &&
                !outer && object && context == CLSCTX_INPROC_SERVER && SameClass(iid, &mmDeviceEnumeratorIid) &&
                GetEnvironmentVariableW(L"XBOX_WIN32_DEFAULT_AUDIO", enabled, ARRAYSIZE(enabled)) == 1 &&
                enabled[0] == L'1') {
            const HRESULT fallback = DefaultAudioEnumerator(iid, object);
            RecordActivation("mmdevice-default-render-fallback", fallback);
            if (SUCCEEDED(fallback)) result = fallback;
        }
        SetLastError(error);
        return result;
    }
    if (result != REGDB_E_CLASSNOTREG || outer || !iid || !object || context != CLSCTX_INPROC_SERVER ||
            !SameClass(clsid, &legacyXAudio)) return result;

    // The launch profile supplies a verified redistributable next to the staged executable.
    // Keep its server loaded while an object returned directly from its class factory may live.
    const DWORD error = GetLastError();
    HMODULE audio = NULL;
    if (GetModuleHandleExW(0, L"XAudio2_7.dll", &audio)) {
        typedef HRESULT (WINAPI *GetClassObject)(REFCLSID, REFIID, LPVOID *);
        static const GUID classFactory = {1, 0, 0, {0xC0,0,0,0,0,0,0,0x46}};
        const GetClassObject getFactory = SameName(ModuleLocation(audio), "adjacent") ?
            (GetClassObject)(void *)GetProcAddress(audio, "DllGetClassObject") : NULL;
        if (getFactory) {
            IClassFactory *factory = NULL;
            result = getFactory(clsid, &classFactory, (void **)&factory);
            if (SUCCEEDED(result) && !factory) result = E_UNEXPECTED;
            if (SUCCEEDED(result)) {
                HMODULE pinned = NULL;
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                        (LPCWSTR)(const void *)getFactory, &pinned)) {
                    result = IClassFactory_CreateInstance(factory, NULL, iid, object);
                } else {
                    result = HRESULT_FROM_WIN32(GetLastError());
                }
            }
            if (factory) IClassFactory_Release(factory);
        }
        FreeLibrary(audio);
    }
    RecordActivation("xaudio27-activation", result);
    SetLastError(error);
    return result;
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

static void ObserveWorkingDirectory(const char *prefix) {
    WCHAR current[512], approved[512];
    const DWORD actual = GetCurrentDirectoryW(ARRAYSIZE(current), current);
    const DWORD expected = GetEnvironmentVariableW(L"XBOX_WIN32_INSTALL_ROOT", approved, ARRAYSIZE(approved));
    const char *state = "unknown";
    if (actual && actual < ARRAYSIZE(current) && expected && expected < ARRAYSIZE(approved)) {
        DWORD actualLength = actual, expectedLength = expected;
        while (actualLength > 3 && current[actualLength - 1] == L'\\') --actualLength;
        while (expectedLength > 3 && approved[expectedLength - 1] == L'\\') --expectedLength;
        state = CompareStringOrdinal(current, actualLength, approved, expectedLength, TRUE) == CSTR_EQUAL
            ? "install-root" : "other";
    }
    char line[60];
    const char *end = Text(Text(Text(line, prefix), state), "\r\n");
    AppendTrace(line, (DWORD)(end - line));
}

static void RestoreWorkingDirectory(void) {
    if (InterlockedExchange(&workingDirectoryStarted, 1)) return;
    const WCHAR variable[] = L"XBOX_WIN32_WORKING_DIRECTORY";
    const DWORD capacity = GetEnvironmentVariableW(variable, NULL, 0);
    if (capacity <= 1) return;
    ObserveWorkingDirectory("asset cwd-before=");
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
    ObserveWorkingDirectory("asset cwd-after=");
}

static DWORD RuntimeError(HMODULE runtime) {
    typedef int (__cdecl *GetError)(int *);
    GetError getError = (GetError)(void *)GetProcAddress(runtime, "_get_errno");
    int error = 0;
    return getError && getError(&error) == 0 ? (DWORD)error : ERROR_PROC_NOT_FOUND;
}

static BOOL NewLoaderEvent(const char *text, DWORD size) {
    ULONGLONG key = 14695981039346656037ULL;
    for (DWORD i = 0; i < size; ++i) key = (key ^ (unsigned char)text[i]) * 1099511628211ULL;
    if (!key) key = 1;
    const UINT first = key % ARRAYSIZE(loaderEvents);
    for (UINT i = 0; i < ARRAYSIZE(loaderEvents); ++i) {
        const UINT slot = (first + i) % ARRAYSIZE(loaderEvents);
        const LONG64 previous = InterlockedCompareExchange64(&loaderEvents[slot], (LONG64)key, 0);
        if (!previous) return TRUE;
        if ((ULONGLONG)previous == key) return FALSE;
    }
    return FALSE;
}

static void Record(const char *name, HANDLE loaded, int isLoad, int success, DWORD error) {
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
    const DWORD size = (DWORD)(out - buffer);
    if (NewLoaderEvent(buffer, size)) AppendTrace(buffer, size);
}

static char *RecordImageAddress(char *out, const void *address) {
    HMODULE module = NULL;
    if (findImage && findImage(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)address, &module)) {
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
        out = Hex(Text(out, " rva=0x"), (DWORD)((ULONG_PTR)address - (ULONG_PTR)module), 8);
    }
    return out;
}

static LONG WINAPI ObserveFault(EXCEPTION_POINTERS *exception) {
    const EXCEPTION_RECORD *fault = exception->ExceptionRecord;
    const BOOL access = fault->ExceptionCode == EXCEPTION_ACCESS_VIOLATION;
    const BOOL breakpoint = fault->ExceptionCode == EXCEPTION_BREAKPOINT;
    const BOOL delayModule = fault->ExceptionCode == delayModuleMissing;
    const BOOL delayExport = fault->ExceptionCode == delayExportMissing;
    if ((!access && !breakpoint && !delayModule && !delayExport) ||
            __atomic_fetch_add(&faultTraceCount, 1u, __ATOMIC_RELAXED) >= 8) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD error = GetLastError();
    char line[150];
    char *out = Text(line, access ? "fault access-violation" : breakpoint ? "fault breakpoint" :
                           delayModule ? "fault delay-load-module" : "fault delay-load-export");
    out = RecordImageAddress(out, fault->ExceptionAddress);
    if (access && fault->NumberParameters >= 2) {
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
    ObserveWorkingDirectory("asset cwd=");
    typedef PVOID (WINAPI *RegisterHandler)(ULONG, PVECTORED_EXCEPTION_HANDLER);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    RegisterHandler registerHandler = kernel ? (RegisterHandler)(void *)GetProcAddress(kernel, "AddVectoredExceptionHandler") : NULL;
    findImage = kernel ? (FindImage)(void *)GetProcAddress(kernel, "GetModuleHandleExW") : NULL;
    const char *line = registerHandler && registerHandler(1, ObserveFault)
        ? "fault observer=ready\r\n" : "fault observer=unavailable\r\n";
    DWORD length = 0;
    while (line[length]) ++length;
    AppendTrace(line, length);
}

static void RecordFaultCall(const char *api, DWORD code, const void *caller) {
    const DWORD error = GetLastError();
    StartFaultTrace();
    char line[150];
    char *out = Text(Text(line, "fault "), api);
    out = Hex(Text(out, " code=0x"), code, 8);
    out = RecordImageAddress(out, caller);
    *out++ = '\r';
    *out++ = '\n';
    AppendTrace(line, (DWORD)(out - line));
    SetLastError(error);
}

static __attribute__((noinline)) void RecordStack(const char *prefix) {
    const DWORD error = GetLastError();
    typedef USHORT (WINAPI *CaptureStack)(ULONG, ULONG, PVOID *, PULONG);
    HMODULE module = GetModuleHandleW(L"kernel32.dll");
    FARPROC captureExport = module ? GetProcAddress(module, "RtlCaptureStackBackTrace") : NULL;
    if (!captureExport) {
        module = GetModuleHandleW(L"ntdll.dll");
        if (module) captureExport = GetProcAddress(module, "RtlCaptureStackBackTrace");
    }
    const CaptureStack capture = (CaptureStack)(void *)captureExport;
    PVOID frames[8];
    const USHORT count = capture && findImage ? capture(1, ARRAYSIZE(frames), frames, NULL) : 0;
    BOOL recorded = FALSE;
    for (USHORT i = 0; i < count; ++i) {
        char line[150];
        char *frame = Hex(Text(Text(line, prefix), " frame=0x"), i, 2);
        char *out = RecordImageAddress(frame, frames[i]);
        if (out == frame) continue;
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
        recorded = TRUE;
    }
    if (!recorded) {
        const char *state = !capture ? "export-unavailable" : !findImage ? "module-lookup-unavailable"
            : !count ? "empty" : "unmapped";
        char line[60];
        const char *end = Text(Text(Text(Text(line, prefix), "="), state), "\r\n");
        AppendTrace(line, (DWORD)(end - line));
    }
    SetLastError(error);
}

DECLSPEC_NORETURN void WINAPI XboxExitProcess(UINT code) {
    RecordFaultCall("exit-process", code, __builtin_return_address(0));
    if (code == MAXDWORD && !InterlockedExchange(&exitStackObserved, 1)) RecordStack("fault exit-stack");
    ExitProcess(code);
}

BOOL WINAPI XboxTerminateProcess(HANDLE process, UINT code) {
    RecordFaultCall("terminate-process", code, __builtin_return_address(0));
    if (code == MAXDWORD) {
        const DWORD error = GetLastError();
        const BOOL self = process == GetCurrentProcess();
        SetLastError(error);
        if (self && !InterlockedExchange(&exitStackObserved, 1)) RecordStack("fault exit-stack");
    }
    return TerminateProcess(process, code);
}

void WINAPI XboxRaiseException(DWORD code, DWORD flags, DWORD count, const ULONG_PTR *arguments) {
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_ACCESS_VIOLATION ||
            code == delayModuleMissing || code == delayExportMissing)
        RecordFaultCall("raise-exception", code, __builtin_return_address(0));
    if (code == msvcCxxException && InterlockedIncrement(&cxxExceptionsObserved) <= 4) {
        RecordFaultCall("raise-exception", code, __builtin_return_address(0));
        RecordStack("fault cxx-stack");
    }
    RaiseException(code, flags, count, arguments);
}

LPVOID WINAPI XboxTraceVirtualAlloc(LPVOID address, SIZE_T bytes, DWORD type, DWORD protection) {
    const LPVOID allocation = VirtualAlloc(address, bytes, type, protection);
    if (allocation || !(type & MEM_COMMIT)) return allocation;

    const DWORD error = GetLastError();
    if (InterlockedIncrement(&failedCommits) <= 12) {
        char line[180];
        char *out = Hex(Text(line, "fault virtual-alloc bytes=0x"), (DWORD)((UINT64)bytes >> 32), 8);
        out = Hex(out, (DWORD)bytes, 8);
        out = Hex(Text(out, " error=0x"), error, 8);
        MEMORYSTATUSEX memory;
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory)) {
            const UINT64 availableKiB = memory.ullAvailPageFile / 1024;
            out = Hex(Text(out, " commit-left-kib=0x"), (DWORD)(availableKiB >> 32), 8);
            out = Hex(out, (DWORD)availableKiB, 8);
        }
        out = RecordImageAddress(out, __builtin_return_address(0));
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
    }
    SetLastError(error);
    return allocation;
}

DWORD WINAPI XboxGetFileType(HANDLE file) {
    const DWORD statusInvalidHandle = 0xC0000008u;
    __try {
        return GetFileType(file);
    } __except (GetExceptionCode() == statusInvalidHandle ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FILE_TYPE_UNKNOWN;
    }
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

static HMODULE LoadVerifiedSystemCompanion(LPCWSTR name, DWORD *error) {
    if (!name) return NULL;
    WCHAR approved[1024], path[512];
    const DWORD length = GetEnvironmentVariableW(L"XBOX_WIN32_SYSTEM_COMPANIONS", approved, ARRAYSIZE(approved));
    if (!length || length >= ARRAYSIZE(approved)) return NULL;

    const WCHAR *start = approved;
    for (const WCHAR *end = approved; *end; ++end) {
        if (*end != L'|') continue;
        const DWORD size = (DWORD)(end - start);
        if (size && CompareStringOrdinal(start, (int)size, name, -1, TRUE) == CSTR_EQUAL) {
            DWORD directory = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
            if (!directory || directory >= ARRAYSIZE(path)) return NULL;
            while (directory && path[directory - 1] != L'\\') --directory;
            if (!directory || directory + size >= ARRAYSIZE(path)) return NULL;
            for (DWORD i = 0; i < size; ++i) path[directory + i] = start[i];
            path[directory + size] = 0;
            const DWORD attributes = GetFileAttributesW(path);
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                    (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return NULL;
            const HMODULE library = LoadLibraryExW(path, NULL,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            *error = library ? ERROR_SUCCESS : GetLastError();
            return library;
        }
        start = end + 1;
    }
    return NULL;
}

static HMODULE LoadHostedControllerNotifications(LPCSTR name) {
    WCHAR enabled[2], wide[ARRAYSIZE(L"cfgmgr32.dll")];
    if (!name || GetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_CONTROLLER_NOTIFY", enabled, ARRAYSIZE(enabled)) != 1 ||
            enabled[0] != L'1' || !MultiByteToWideChar(CP_ACP, 0, name, -1, wide, ARRAYSIZE(wide)) ||
            CompareStringOrdinal(wide, -1, L"cfgmgr32.dll", -1, TRUE) != CSTR_EQUAL ||
            GetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", NULL, 0) <= 1) return NULL;
    return LoadPackagedLibrary(L"XboxWin32ControllerNotify.dll", 0);
}

HANDLE WINAPI XboxLoadLibraryA(const char *name) {
    const DWORD previous = GetLastError();
    RestoreWorkingDirectory();
    StartFaultTrace();
    const HMODULE hosted = LoadHostedControllerNotifications(name);
    if (hosted) {
        const char trace[] = "input controller-notifications=hosted\r\n";
        AppendTrace(trace, sizeof(trace) - 1);
        Record(name, hosted, 1, TRUE, previous);
        SetLastError(previous);
        return hosted;
    }
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

static PVOID volatile hostedOpenGlModule;

static BOOL HostedWglEnabled(void) {
    WCHAR enabled[2];
    return GetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_WGL", enabled, ARRAYSIZE(enabled)) == 1 && enabled[0] == L'1';
}

static HMODULE LoadHostedOpenGl(LPCWSTR name) {
    if (!name || CompareStringOrdinal(name, -1, L"opengl32.dll", -1, TRUE) != CSTR_EQUAL ||
            !HostedWglEnabled()) return NULL;
    HMODULE module = LoadPackagedLibrary(L"opengl32.dll", 0);
    if (!module || module == InterlockedCompareExchangePointer(&hostedOpenGlModule, NULL, NULL)) return module;
    WCHAR expected[512], actual[512];
    UINT32 length = ARRAYSIZE(expected);
    const WCHAR suffix[] = L"\\opengl32.dll";
    const DWORD actualLength = GetModuleFileNameW(module, actual, ARRAYSIZE(actual));
    if (!actualLength || actualLength >= ARRAYSIZE(actual) ||
            GetCurrentPackagePath(&length, expected) != ERROR_SUCCESS || !length ||
            length - 1 + ARRAYSIZE(suffix) > ARRAYSIZE(expected)) {
        FreeLibrary(module);
        return NULL;
    }
    for (UINT i = 0; i < ARRAYSIZE(suffix); ++i) expected[length - 1 + i] = suffix[i];
    HMODULE retained = NULL;
    if (CompareStringOrdinal(actual, -1, expected, -1, TRUE) != CSTR_EQUAL ||
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)module, &retained)) {
        FreeLibrary(module);
        return NULL;
    }
    // Keep the verified module identity while the caller owns its own reference.
    const HMODULE existing = (HMODULE)InterlockedCompareExchangePointer(&hostedOpenGlModule, retained, NULL);
    if (existing) FreeLibrary(retained);
    return module;
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
    HMODULE openGl = LoadHostedOpenGl(name);
    if (openGl) {
        const char trace[] = "asset opengl-module=packaged\r\n";
        AppendTrace(trace, sizeof(trace) - 1);
        RecordWideLoad(name, openGl, previous);
        SetLastError(previous);
        return openGl;
    }
    SetLastError(previous);
    HMODULE result = LoadHostedXInput(name);
    if (result) {
        const char trace[] = "input xinput-library=hosted\r\n";
        AppendTrace(trace, sizeof(trace) - 1);
        RecordWideLoad(name, result, previous);
        SetLastError(previous);
        return result;
    }
    WCHAR staged[512];
    const BOOL companion = DynamicCompanionsEnabled() && FindStagedCompanion(name, staged, ARRAYSIZE(staged));
    SetLastError(previous);
    result = LoadLibraryW(companion ? staged : name);
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
    DWORD error = GetLastError();
    if (!result && error == ERROR_MOD_NOT_FOUND && !file && flags == LOAD_LIBRARY_SEARCH_SYSTEM32)
        result = LoadVerifiedSystemCompanion(name, &error);
    RecordWideLoad(name, result, error);
    SetLastError(error);
    return result;
}

HMODULE WINAPI XboxLoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
    const DWORD previous = GetLastError();
    WCHAR wide[512], staged[512];
    const BOOL companion = !file && name && DynamicCompanionsEnabled() &&
        MultiByteToWideChar(CP_ACP, 0, name, -1, wide, ARRAYSIZE(wide)) &&
        FindStagedCompanion(wide, staged, ARRAYSIZE(staged));
    SetLastError(previous);
    HMODULE result = companion ? LoadLibraryExW(staged, file, flags) : LoadLibraryExA(name, file, flags);
    DWORD error = GetLastError();
    if (!result && error == ERROR_MOD_NOT_FOUND && !file && flags == LOAD_LIBRARY_SEARCH_SYSTEM32 && name &&
            MultiByteToWideChar(CP_ACP, 0, name, -1, wide, ARRAYSIZE(wide)))
        result = LoadVerifiedSystemCompanion(wide, &error);
    if (!result && error == ERROR_MOD_NOT_FOUND && !file && !flags && name) {
        WCHAR option[2];
        if (SameName(name, "sl.interposer.dll") &&
                GetEnvironmentVariableW(L"XBOX_WIN32_STREAMLINE_UNAVAILABLE", option, ARRAYSIZE(option)) == 1 &&
                option[0] == L'1') {
            result = LoadPackagedLibrary(L"XboxWin32LoaderTrace.dll", 0);
            if (result) {
                error = previous;
                const char trace[] = "asset streamline-delay-load=unavailable-adapter\r\n";
                AppendTrace(trace, sizeof(trace) - 1);
            }
        }
    }
    Record(name, result, 1, result != NULL, error);
    SetLastError(error);
    return result;
}

int __cdecl XboxStreamlineSetD3DDevice(void *device) {
    (void)device;
    // NVIDIA Streamline's sl::Result::eErrorInitNotCalled.
    return 23;
}

static BOOL DescribeD3D12Adapter(IUnknown *adapter, DXGI_ADAPTER_DESC1 *desc) {
    IDXGIAdapter1 *dxgi = NULL;
    if (!adapter || FAILED(IUnknown_QueryInterface(adapter, &adapter1Iid, (void **)&dxgi))) return FALSE;
    const HRESULT result = IDXGIAdapter1_GetDesc1(dxgi, desc);
    IDXGIAdapter1_Release(dxgi);
    return SUCCEEDED(result);
}

static BOOL SameAdapter(LUID left, LUID right) {
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

static BOOL PipelineReplayEnabled(void) {
    WCHAR enabled[2];
    return GetEnvironmentVariableW(L"XBOX_WIN32_D3D12_PIPELINE_REPLAY", enabled, ARRAYSIZE(enabled)) == 1 &&
        enabled[0] == L'1';
}

// Records the title's pipelines and precompiles them on the next run (d3d12-pipeline-replay.h).
static void InstallPipelineReplay(void *device) {
    if (!device || !PipelineReplayEnabled()) return;
    WCHAR path[1024];
    if (!XboxPipelineReplayDefaultPath(path, ARRAYSIZE(path))) {
        const char line[] = "asset pso-replay open status=no-path\r\n";
        AppendTrace(line, sizeof(line) - 1);
        return;
    }
    XboxPipelineReplayInstall((ID3D12Device *)device, path, NULL, TRUE, AppendTrace);
}

static BOOL DefaultD3D12AdapterEnabled(void) {
    WCHAR enabled[2];
    return GetEnvironmentVariableW(L"XBOX_WIN32_D3D12_DEFAULT_ADAPTER", enabled, ARRAYSIZE(enabled)) == 1 &&
        enabled[0] == L'1';
}

static BOOL HostProcessorsDisabled(void) {
    WCHAR value[2];
    return GetEnvironmentVariableW(L"XBOX_WIN32_HOST_PROCESSORS", value, ARRAYSIZE(value)) == 1 && value[0] == L'0';
}

void WINAPI XboxGetStartupInfoW(LPSTARTUPINFOW startup) {
    GetStartupInfoW(startup);
    const DWORD previous = GetLastError();
    // The CRT asks for startup information before the game creates its worker threads.
    if (!HostProcessorsDisabled() && InterlockedExchange(&processorsPlaced, 1) == 0) {
        const XboxCpuSetResult placed = XboxUseHostProcessors();
        char line[160];
        AppendTrace(line, (DWORD)(XboxFormatProcessors(line, &placed) - line));
    }
    if (DefaultD3D12AdapterEnabled() && InterlockedExchange(&devicePrimed, 1) == 0) {
        const HMODULE library = LoadLibraryW(L"d3d12.dll");
        const CreateD3D12Device create = library ?
            (CreateD3D12Device)(PVOID)GetProcAddress(library, "D3D12CreateDevice") : NULL;
        // Keep the first device until process exit so the game can reuse the native singleton.
        const HRESULT result = create ?
            create(NULL, D3D_FEATURE_LEVEL_11_0, &deviceIid, (void **)&startupDevice) :
            HRESULT_FROM_WIN32(GetLastError());
        char line[90];
        char *out = Hex(Text(line, "asset d3d12-crt-entry result=0x"), (DWORD)result, 8);
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
        if (SUCCEEDED(result)) InstallPipelineReplay(startupDevice);
    }
    SetLastError(previous);
}

static BOOL UseDefaultD3D12Adapter(const DXGI_ADAPTER_DESC1 *requested) {
    if (!DefaultD3D12AdapterEnabled() || (requested->Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return FALSE;
    typedef HRESULT (WINAPI *CreateFactory)(const IID *, void **);
    const HMODULE module = GetModuleHandleW(L"dxgi.dll");
    const CreateFactory factoryEntry = module ? (CreateFactory)(PVOID)GetProcAddress(module, "CreateDXGIFactory1") : NULL;
    IDXGIFactory1 *factory = NULL;
    if (!factoryEntry || FAILED(factoryEntry(&factory1Iid, (void **)&factory))) return FALSE;
    IDXGIAdapter *primary = NULL;
    BOOL matches = FALSE;
    if (SUCCEEDED(IDXGIFactory1_EnumAdapters(factory, 0, &primary))) {
        DXGI_ADAPTER_DESC1 desc;
        matches = DescribeD3D12Adapter((IUnknown *)primary, &desc) &&
            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && SameAdapter(desc.AdapterLuid, requested->AdapterLuid);
        IDXGIAdapter_Release(primary);
    }
    IDXGIFactory1_Release(factory);
    return matches;
}

static HRESULT WINAPI TraceD3D12CreateDevice(IUnknown *adapter, UINT level, const IID *iid, void **device) {
    const DWORD previous = GetLastError();
    const CreateD3D12Device create = (CreateD3D12Device)InterlockedCompareExchangePointer(&d3d12CreateDevice, NULL, NULL);
    DXGI_ADAPTER_DESC1 desc;
    const BOOL described = DescribeD3D12Adapter(adapter, &desc);
    const BOOL useDefault = described && UseDefaultD3D12Adapter(&desc);
    if (useDefault) {
        const char checkpoint[] = "asset d3d12-default-adapter=matched\r\n";
        AppendTrace(checkpoint, sizeof(checkpoint) - 1);
    }
    SetLastError(previous);
    const HRESULT result = create(useDefault ? NULL : adapter, level, iid, device);
    const DWORD error = GetLastError();
    if (SUCCEEDED(result) && iid && device && *device) InstallPipelineReplay(*device);
    if (InterlockedIncrement(&deviceEvents) <= 16) {
        char line[170];
        char *out = Text(line, adapter && !useDefault ? "asset d3d12-create adapter=explicit level=0x" :
                                                       "asset d3d12-create adapter=default level=0x");
        out = Hex(out, level, 4);
        if (iid) out = Hex(Text(out, " iid-first=0x"), iid->Data1, 8);
        out = Hex(Text(out, " result=0x"), (DWORD)result, 8);
        if (described) {
            out = Hex(Text(out, " vendor=0x"), desc.VendorId, 4);
            out = Hex(Text(out, " device=0x"), desc.DeviceId, 4);
            out = Hex(Text(out, " flags=0x"), desc.Flags, 8);
        }
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
    }
    SetLastError(error);
    return result;
}

static FARPROC HostedDxgiProcedure(HMODULE module, const char *name) {
    const BOOL combined = SameName(name, "D3D11CreateDeviceAndSwapChain");
    if (combined) {
        if (module != GetModuleHandleW(L"d3d11.dll")) return NULL;
    } else if (!(SameName(name, "CreateDXGIFactory2") || SameName(name, "CreateDXGIFactory1") ||
            SameName(name, "CreateDXGIFactory")) || module != GetModuleHandleW(L"dxgi.dll")) return NULL;
    WCHAR enabled[2];
    if (GetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_DXGI", enabled, ARRAYSIZE(enabled)) != 1 ||
            enabled[0] != L'1' || (combined && !GetProcAddress(module, "D3D11CreateDevice"))) return NULL;
    static PVOID volatile loaded;
    const HMODULE adapter = LoadSharedPackagedAdapter(&loaded, L"XboxWin32Dxgi.dll");
    return adapter ? GetProcAddress(adapter, name) : NULL;
}

static FARPROC HostedWindowProcedure(const char *name) {
    static PVOID volatile loaded;
    const HMODULE adapter = LoadSharedPackagedAdapter(&loaded, L"XboxWin32Window.dll");
    return adapter ? GetProcAddress(adapter, name) : NULL;
}

static FARPROC HostedWindowFrameProcedure(HMODULE module, const char *name) {
    if (!(SameName(name, "AdjustWindowRectExForDpi") || SameName(name, "GetDpiForWindow")) ||
            module != GetModuleHandleW(L"user32.dll")) return NULL;
    WCHAR enabled[2];
    if (GetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_WINDOW_FRAME", enabled, ARRAYSIZE(enabled)) != 1 ||
            enabled[0] != L'1') return NULL;
    return HostedWindowProcedure(name);
}

FARPROC WINAPI XboxGetProcAddress(HANDLE module, const char *name) {
    const DWORD previous = GetLastError();
    if (module && (ULONG_PTR)name <= 0xffffu) {
        if (module == GetModuleHandleW(L"XboxWin32XInput910.dll")) {
            Record(name, module, 0, FALSE, ERROR_PROC_NOT_FOUND);
            SetLastError(ERROR_PROC_NOT_FOUND);
            return NULL;
        }
        SetLastError(previous);
    }
    FARPROC result = GetProcAddress(module, name);
    DWORD error = GetLastError();
    Record(name, module, 0, result != 0, error);
    if (module && (ULONG_PTR)name > 0xffffu) {
        if (name[0] == 'w' && name[1] == 'g' && name[2] == 'l' &&
                module == InterlockedCompareExchangePointer(&hostedOpenGlModule, NULL, NULL) && HostedWglEnabled()) {
            const FARPROC hosted = HostedWindowProcedure(name);
            if (hosted) {
                const char trace[] = "asset opengl-wgl=hosted\r\n";
                AppendTrace(trace, sizeof(trace) - 1);
                if (!result) error = previous;
            } else {
                // The packaged desktop WGL functions cannot use hosted handles.
                error = ERROR_PROC_NOT_FOUND;
            }
            result = hosted;
        } else if (result && name[0] == 'D' && SameName(name, "D3D12CreateDevice") &&
                module == GetModuleHandleW(L"d3d12.dll")) {
            const PVOID first = InterlockedCompareExchangePointer(&d3d12CreateDevice, (PVOID)result, NULL);
            if (!first || first == (PVOID)result) result = (FARPROC)(PVOID)TraceD3D12CreateDevice;
        } else if ((result && name[0] == 'C') || name[0] == 'D') {
            const FARPROC hosted = HostedDxgiProcedure(module, name);
            if (hosted) {
                const char dxgi[] = "asset dxgi-factory=hosted\r\n";
                const char d3d11[] = "asset d3d11-combined=hosted\r\n";
                if (name[0] == 'D') AppendTrace(d3d11, sizeof(d3d11) - 1);
                else AppendTrace(dxgi, sizeof(dxgi) - 1);
                if (!result) error = previous;
                result = hosted;
            }
        } else if (name[0] == 'A' || name[0] == 'G') {
            const FARPROC hosted = HostedWindowFrameProcedure(module, name);
            if (hosted) {
                const char trace[] = "asset window-frame=hosted\r\n";
                AppendTrace(trace, sizeof(trace) - 1);
                if (!result) error = previous;
                result = hosted;
            }
        }
    }
    SetLastError(error);
    return result;
}

DWORD WINAPI XboxOriginalGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) {
    const DWORD previous = GetLastError();
    if (filename && size && (!module || module == GetModuleHandleW(NULL))) {
        RestoreWorkingDirectory();
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

// Redirect simple desktop pipe names into the namespace shared by this app's
// AppContainer processes. Explicit namespaces and ordinary paths keep their meaning.
static LPCWSTR LocalPipePath(LPCWSTR path, WCHAR *buffer, DWORD capacity) {
    const WCHAR prefix[] = L"\\\\.\\pipe\\";
    const WCHAR local[] = L"LOCAL\\";
    const DWORD prefixLength = ARRAYSIZE(prefix) - 1;
    if (!path) return NULL;
    for (DWORD i = 0; i < prefixLength; ++i) {
        WCHAR character = path[i];
        if (character >= L'A' && character <= L'Z') character += L'a' - L'A';
        if (character != prefix[i]) return NULL;
        buffer[i] = path[i];
    }
    const WCHAR *suffix = path + prefixLength;
    const DWORD start = prefixLength + ARRAYSIZE(local) - 1;
    if (!*suffix || capacity <= start) return NULL;
    for (DWORD i = 0; i < ARRAYSIZE(local) - 1; ++i) buffer[prefixLength + i] = local[i];
    for (DWORD i = 0; i < capacity - start; ++i) {
        const WCHAR character = suffix[i];
        if (character == L'\\' || character == L'/' || character == L':') return NULL;
        buffer[start + i] = character;
        if (!character) return buffer;
    }
    return NULL;
}

static void ObserveNativeResult(const char *operation, BOOL success, DWORD error) {
    if (InterlockedIncrement(&assetEvents) <= 64) {
        char line[120];
        char *out = Text(Text(line, "asset "), operation);
        out = success ? Text(out, "=ok") : Hex(Text(out, " error=0x"), error, 8);
        *out++ = '\r';
        *out++ = '\n';
        AppendTrace(line, (DWORD)(out - line));
    }
    SetLastError(error);
}

HANDLE WINAPI XboxCreateNamedPipeW(LPCWSTR path, DWORD access, DWORD mode, DWORD instances,
        DWORD outputSize, DWORD inputSize, DWORD timeout, LPSECURITY_ATTRIBUTES security) {
    WCHAR local[256];
    const LPCWSTR mapped = LocalPipePath(path, local, ARRAYSIZE(local));
    HANDLE handle = CreateNamedPipeW(mapped ? mapped : path, access, mode, instances,
        outputSize, inputSize, timeout, security);
    ObserveNativeResult(mapped ? "pipe-create-local" : "pipe-create", handle != INVALID_HANDLE_VALUE, GetLastError());
    return handle;
}

BOOL WINAPI XboxWaitNamedPipeW(LPCWSTR path, DWORD timeout) {
    WCHAR local[256];
    const LPCWSTR mapped = LocalPipePath(path, local, ARRAYSIZE(local));
    BOOL available = WaitNamedPipeW(mapped ? mapped : path, timeout);
    ObserveNativeResult(mapped ? "pipe-wait-local" : "pipe-wait", available, GetLastError());
    return available;
}

BOOL WINAPI XboxTraceCreateProcessW(LPCWSTR application, LPWSTR command, LPSECURITY_ATTRIBUTES processSecurity,
        LPSECURITY_ATTRIBUTES threadSecurity, BOOL inherit, DWORD flags, LPVOID environment, LPCWSTR directory,
        LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process) {
    BOOL created = CreateProcessW(application, command, processSecurity, threadSecurity, inherit, flags,
        environment, directory, startup, process);
    ObserveNativeResult("process-create", created, GetLastError());
    return created;
}

HANDLE WINAPI XboxTraceCreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
        DWORD creation, DWORD attributes, HANDLE templateFile) {
    WCHAR local[256];
    const LPCWSTR mapped = LocalPipePath(path, local, ARRAYSIZE(local));
    HANDLE result = CreateFileW(mapped ? mapped : path, access, share, security, creation, attributes, templateFile);
    const DWORD error = GetLastError();
    if (mapped) ObserveNativeResult("pipe-open-local", result != INVALID_HANDLE_VALUE, error);
    else ObserveAsset(path, "open", result != INVALID_HANDLE_VALUE, error);
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
