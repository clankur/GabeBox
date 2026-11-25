/* An opt-in loader trace for unsigned, locally staged Windows images. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include "loader-trace-output.h"
#include "hosted-xinput-load.h"
#include "shell-lookup.h"
#include <d3dcommon.h>
#include <dxgi1_2.h>
#include <objbase.h>
#include <mmdeviceapi.h>

static volatile LONG64 loaderEvents[128];
static volatile LONG faultTraceStarted;
static volatile LONG workingDirectoryStarted;
static volatile LONG originalImageObserved;
static volatile LONG assetEvents;
static volatile LONG deviceEvents;
static volatile LONG devicePrimed;
static volatile LONG failedCommits;
static DWORD faultTraceCount;
static const DWORD delayModuleMissing = 0xC06D007E;
static const DWORD delayExportMissing = 0xC06D007F;
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

static BOOL LegacyXAudioClass(REFCLSID clsid) {
    static const GUID legacy = {0x5A508685, 0xA254, 0x4FBA, {0x9B,0x82,0x9A,0x24,0xB0,0x03,0x06,0xAF}};
    if (!clsid || clsid->Data1 != legacy.Data1 || clsid->Data2 != legacy.Data2 || clsid->Data3 != legacy.Data3)
        return FALSE;
    for (UINT index = 0; index < ARRAYSIZE(legacy.Data4); ++index)
        if (clsid->Data4[index] != legacy.Data4[index]) return FALSE;
    return TRUE;
}

/* DirectInput is not part of the Xbox system. Input stacks such as SDL's Windows
   joystick driver create IDirectInput8 before XInput and abandon XInput when that
   fails, so a missing class hides the hosted controller. Profiles that route the
   controller through hosted XInput receive the object below; other titles keep the
   system's failure. This object reports a
   system with no DirectInput devices: Initialize and EnumDevices succeed without
   callbacks, CreateDevice and FindDevice report an unregistered device and
   GetDeviceStatus reports it detached. Controllers stay on the XInput path. */
static const GUID directInput8Class = {0x25E609E4, 0xB259, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID directInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
static const GUID directInput8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
static const GUID unknownInterface = {0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

typedef struct EmptyDirectInput EmptyDirectInput;
typedef struct EmptyDirectInputVtbl {
    HRESULT (WINAPI *QueryInterface)(EmptyDirectInput *, REFIID, void **);
    ULONG (WINAPI *AddRef)(EmptyDirectInput *);
    ULONG (WINAPI *Release)(EmptyDirectInput *);
    HRESULT (WINAPI *CreateDevice)(EmptyDirectInput *, REFGUID, void **, IUnknown *);
    HRESULT (WINAPI *EnumDevices)(EmptyDirectInput *, DWORD, void *, void *, DWORD);
    HRESULT (WINAPI *GetDeviceStatus)(EmptyDirectInput *, REFGUID);
    HRESULT (WINAPI *RunControlPanel)(EmptyDirectInput *, HWND, DWORD);
    HRESULT (WINAPI *Initialize)(EmptyDirectInput *, HINSTANCE, DWORD);
    HRESULT (WINAPI *FindDevice)(EmptyDirectInput *, REFGUID, const void *, GUID *);
    HRESULT (WINAPI *EnumDevicesBySemantics)(EmptyDirectInput *, const void *, void *, void *, void *, DWORD);
    HRESULT (WINAPI *ConfigureDevices)(EmptyDirectInput *, void *, void *, DWORD, void *);
} EmptyDirectInputVtbl;
struct EmptyDirectInput { const EmptyDirectInputVtbl *vtbl; const GUID *iid; };

static BOOL SameGuid(const GUID *left, const GUID *right) {
    const BYTE *a = (const BYTE *)left, *b = (const BYTE *)right;
    for (unsigned i = 0; i < sizeof(GUID); ++i) if (a[i] != b[i]) return FALSE;
    return TRUE;
}
static HRESULT WINAPI EmptyQueryInterface(EmptyDirectInput *self, REFIID iid, void **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (!SameGuid(iid, &unknownInterface) && !SameGuid(iid, self->iid)) return E_NOINTERFACE;
    *out = self;
    return S_OK;
}
/* Process-lifetime singletons: reference counting cannot free them. */
static ULONG WINAPI EmptyAddRef(EmptyDirectInput *self) { (void)self; return 2; }
static ULONG WINAPI EmptyRelease(EmptyDirectInput *self) { (void)self; return 1; }
static HRESULT WINAPI EmptyCreateDevice(EmptyDirectInput *self, REFGUID device, void **out, IUnknown *outer) {
    (void)self; (void)device; (void)outer;
    if (out) *out = NULL;
    return REGDB_E_CLASSNOTREG; /* DIERR_DEVICENOTREG */
}
static HRESULT WINAPI EmptyEnumDevices(EmptyDirectInput *self, DWORD type, void *callback, void *context, DWORD flags) {
    (void)self; (void)type; (void)context; (void)flags;
    return callback ? S_OK : E_INVALIDARG;
}
static HRESULT WINAPI EmptyGetDeviceStatus(EmptyDirectInput *self, REFGUID device) {
    (void)self; (void)device;
    return S_FALSE; /* DI_NOTATTACHED */
}
static HRESULT WINAPI EmptyRunControlPanel(EmptyDirectInput *self, HWND owner, DWORD flags) {
    (void)self; (void)owner; (void)flags;
    return E_NOTIMPL; /* DIERR_UNSUPPORTED */
}
static HRESULT WINAPI EmptyInitialize(EmptyDirectInput *self, HINSTANCE instance, DWORD version) {
    (void)self; (void)instance; (void)version;
    return S_OK;
}
static HRESULT WINAPI EmptyFindDevice(EmptyDirectInput *self, REFGUID kind, const void *name, GUID *instance) {
    (void)self; (void)kind; (void)name; (void)instance;
    return REGDB_E_CLASSNOTREG;
}
static HRESULT WINAPI EmptyEnumBySemantics(EmptyDirectInput *self, const void *user, void *format, void *callback,
        void *context, DWORD flags) {
    (void)self; (void)user; (void)format; (void)context; (void)flags;
    return callback ? S_OK : E_INVALIDARG;
}
static HRESULT WINAPI EmptyConfigureDevices(EmptyDirectInput *self, void *callback, void *parameters, DWORD flags,
        void *data) {
    (void)self; (void)callback; (void)parameters; (void)flags; (void)data;
    return E_NOTIMPL;
}
static const EmptyDirectInputVtbl emptyDirectInputVtbl = {
    EmptyQueryInterface, EmptyAddRef, EmptyRelease, EmptyCreateDevice, EmptyEnumDevices, EmptyGetDeviceStatus,
    EmptyRunControlPanel, EmptyInitialize, EmptyFindDevice, EmptyEnumBySemantics, EmptyConfigureDevices,
};
static EmptyDirectInput emptyDirectInputW = {&emptyDirectInputVtbl, &directInput8W};
static EmptyDirectInput emptyDirectInputA = {&emptyDirectInputVtbl, &directInput8A};

static HRESULT EmptyDirectInput8(REFIID iid, LPVOID *object) {
    EmptyDirectInput *instance = SameGuid(iid, &directInput8W) ? &emptyDirectInputW :
        SameGuid(iid, &directInput8A) ? &emptyDirectInputA : NULL;
    if (!instance) return E_NOINTERFACE;
    *object = instance;
    const char trace[] = "input directinput8=empty\r\n";
    AppendTrace(trace, sizeof(trace) - 1);
    return S_OK;
}

HRESULT WINAPI XboxCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID *object) {
    HRESULT result = CoCreateInstance(clsid, outer, context, iid, object);
    WCHAR hostedPad[2];
    if (FAILED(result) && !outer && iid && object && clsid && (context & CLSCTX_INPROC_SERVER) &&
            SameGuid(clsid, &directInput8Class) &&
            GetEnvironmentVariableW(L"XBOX_WIN32_HOST_XINPUT", hostedPad, ARRAYSIZE(hostedPad)) == 1 &&
            hostedPad[0] == L'1') {
        const DWORD error = GetLastError();
        const HRESULT empty = EmptyDirectInput8(iid, object);
        SetLastError(error);
        return SUCCEEDED(empty) ? empty : result;
    }
    if (result != REGDB_E_CLASSNOTREG || outer || !iid || !object || context != CLSCTX_INPROC_SERVER ||
            !LegacyXAudioClass(clsid)) return result;

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
    char line[72];
    char *out = Hex(Text(line, "asset xaudio27-activation result=0x"), (DWORD)result, 8);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
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
    /* Other error-severity exceptions (stack overflow, illegal instruction, heap corruption)
       end a process without an access violation, so record their code as well. */
    const BOOL severe = (fault->ExceptionCode & 0xF0000000u) == 0xC0000000u;
    if ((!access && !breakpoint && !delayModule && !delayExport && !severe) ||
            __atomic_fetch_add(&faultTraceCount, 1u, __ATOMIC_RELAXED) >= 8) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD error = GetLastError();
    char line[170];
    char *out = Text(line, access ? "fault access-violation" : breakpoint ? "fault breakpoint" :
                           delayModule ? "fault delay-load-module" : delayExport ? "fault delay-load-export" :
                           "fault exception");
    if (!access && !breakpoint && !delayModule && !delayExport)
        out = Hex(Text(out, " code=0x"), fault->ExceptionCode, 8);
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

/* Audio output evidence.

   A person at the console is the final audio check, but the mixed signal can
   be observed without one: the default render endpoint's peak meter reports
   what the console is about to play. While loader tracing is enabled, a
   background thread samples IAudioMeterInformation every 100 ms for up to
   15 minutes and writes one line per 10-second window with the highest peak
   (in thousandths of full scale) and how many samples were audible
   (peak > 0.001). The thread only reads the meter. */
static volatile LONG audioMeterStarted;
#ifdef _MSC_VER
/* The MSVC toolchain references this marker from any floating-point code; the
   adapter links without a C runtime, which would otherwise supply it. */
int _fltused = 0;
#endif
static const GUID audioEnumeratorClass =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID audioEnumeratorInterface =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID audioMeterInterface =
    {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};

/* The first method of IAudioMeterInformation, after IUnknown. */
typedef struct AudioMeter AudioMeter;
typedef struct AudioMeterMethods {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(AudioMeter *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(AudioMeter *);
    ULONG (STDMETHODCALLTYPE *Release)(AudioMeter *);
    HRESULT (STDMETHODCALLTYPE *GetPeakValue)(AudioMeter *, float *);
} AudioMeterMethods;
struct AudioMeter {
    const AudioMeterMethods *lpVtbl;
};

static char *Unsigned(char *out, unsigned long value) {
    char digits[10];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < 10);
    while (count) *out++ = digits[--count];
    return out;
}

static void TraceAudioStage(const char *stage, HRESULT result) {
    char line[96];
    char *out = Hex(Text(Text(Text(line, "audio meter=unavailable stage="), stage), " result=0x"),
        (unsigned long)result, 8);
    *out++ = '\r';
    *out++ = '\n';
    AppendTrace(line, (DWORD)(out - line));
}

static DWORD WINAPI SampleAudioMeter(LPVOID unused) {
    (void)unused;
    HRESULT result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(result)) {
        TraceAudioStage("com", result);
        return 0;
    }
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *device = NULL;
    AudioMeter *meter = NULL;
    result = CoCreateInstance(&audioEnumeratorClass, NULL, CLSCTX_INPROC_SERVER, &audioEnumeratorInterface,
        (void **)&enumerator);
    if (FAILED(result)) TraceAudioStage("enumerator", result);
    else if (FAILED(result = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device)))
        TraceAudioStage("endpoint", result);
    else if (FAILED(result = device->lpVtbl->Activate(device, &audioMeterInterface, CLSCTX_INPROC_SERVER, NULL,
            (void **)&meter)))
        TraceAudioStage("meter", result);
    if (meter) {
        const char ready[] = "audio meter=ready\r\n";
        AppendTrace(ready, sizeof(ready) - 1);
        for (unsigned long window = 1; window <= 90; ++window) {
            float highest = 0.0f;
            unsigned long audible = 0, read = 0;
            for (int sample = 0; sample < 100; ++sample) {
                Sleep(100);
                float peak = 0.0f;
                if (FAILED(meter->lpVtbl->GetPeakValue(meter, &peak))) continue;
                ++read;
                if (peak > highest) highest = peak;
                if (peak > 0.001f) ++audible;
            }
            char line[96];
            char *out = Unsigned(Text(line, "audio window="), window);
            out = Unsigned(Text(out, " peak-milli="), (unsigned long)(int)(highest * 1000.0f));
            out = Unsigned(Text(out, " audible="), audible);
            out = Unsigned(Text(out, "/"), read);
            *out++ = '\r';
            *out++ = '\n';
            AppendTrace(line, (DWORD)(out - line));
        }
        meter->lpVtbl->Release(meter);
    }
    if (device) device->lpVtbl->Release(device);
    if (enumerator) enumerator->lpVtbl->Release(enumerator);
    CoUninitialize();
    return 0;
}

static void StartAudioMeter(void) {
    if (InterlockedExchange(&audioMeterStarted, 1)) return;
    HANDLE thread = CreateThread(NULL, 0, SampleAudioMeter, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    else TraceAudioStage("thread", HRESULT_FROM_WIN32(GetLastError()));
}

static void StartFaultTrace(void) {
    if (InterlockedExchange(&faultTraceStarted, 1) ||
            GetEnvironmentVariableW(L"XBOX_WIN32_LOADER_TRACE", NULL, 0) <= 1) return;
    ObserveWorkingDirectory();
    typedef PVOID (WINAPI *RegisterHandler)(ULONG, PVECTORED_EXCEPTION_HANDLER);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    RegisterHandler registerHandler = kernel ? (RegisterHandler)(void *)GetProcAddress(kernel, "AddVectoredExceptionHandler") : NULL;
    findImage = kernel ? (FindImage)(void *)GetProcAddress(kernel, "GetModuleHandleExW") : NULL;
    const char *line = registerHandler && registerHandler(1, ObserveFault)
        ? "fault observer=ready\r\n" : "fault observer=unavailable\r\n";
    DWORD length = 0;
    while (line[length]) ++length;
    AppendTrace(line, length);
    StartAudioMeter();
}

static void RecordProcessExit(const char *api, DWORD code, const void *caller) {
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

DECLSPEC_NORETURN void WINAPI XboxExitProcess(UINT code) {
    RecordProcessExit("exit-process", code, __builtin_return_address(0));
    ExitProcess(code);
}

BOOL WINAPI XboxTerminateProcess(HANDLE process, UINT code) {
    RecordProcessExit("terminate-process", code, __builtin_return_address(0));
    return TerminateProcess(process, code);
}

void WINAPI XboxRaiseException(DWORD code, DWORD flags, DWORD count, const ULONG_PTR *arguments) {
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_ACCESS_VIOLATION ||
            code == delayModuleMissing || code == delayExportMissing)
        RecordProcessExit("raise-exception", code, __builtin_return_address(0));
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

HANDLE WINAPI XboxLoadLibraryA(const char *name) {
    DWORD previous = GetLastError();
    RestoreWorkingDirectory();
    StartFaultTrace();
    WCHAR wide[512], staged[512];
    const BOOL converted = name && MultiByteToWideChar(CP_ACP, 0, name, -1, wide, ARRAYSIZE(wide));
    HANDLE result = converted ? LoadHostedXInput(wide) : NULL;
    if (result) {
        const char trace[] = "input xinput-library=hosted\r\n";
        AppendTrace(trace, sizeof(trace) - 1);
        Record(name, result, 1, TRUE, previous);
        SetLastError(previous);
        return result;
    }
    const BOOL companion = converted && DynamicCompanionsEnabled() && FindStagedCompanion(wide, staged, ARRAYSIZE(staged));
    SetLastError(previous);
    result = companion ? LoadLibraryW(staged) : LoadLibraryA(name);
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

static BOOL HostedDxgiEnabled(void);
static HMODULE HostedDxgiAdapter(void);

HMODULE WINAPI XboxLoadLibraryW(LPCWSTR name) {
    const DWORD previous = GetLastError();
    StartFaultTrace();
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
    DWORD error = GetLastError();
    /* A console without the system compositor still serves hosted windows, which
       only need the hosted DXGI adapter's DCompositionCreateDevice. */
    if (!result && error == ERROR_MOD_NOT_FOUND && name &&
            CompareStringOrdinal(name, -1, L"dcomp.dll", -1, TRUE) == CSTR_EQUAL && HostedDxgiEnabled()) {
        /* The caller owns one reference, as with any successful LoadLibraryW. */
        result = HostedDxgiAdapter() ? LoadPackagedLibrary(L"XboxWin32Dxgi.dll", 0) : NULL;
        if (result) {
            error = previous;
            const char trace[] = "asset dcomp-library=hosted\r\n";
            AppendTrace(trace, sizeof(trace) - 1);
        }
    }
    RecordWideLoad(name, result, error);
    SetLastError(error);
    return result;
}

HMODULE WINAPI XboxLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    const DWORD previous = GetLastError();
    StartFaultTrace();
    SetLastError(previous);
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

static BOOL DefaultD3D12AdapterEnabled(void) {
    WCHAR enabled[2];
    return GetEnvironmentVariableW(L"XBOX_WIN32_D3D12_DEFAULT_ADAPTER", enabled, ARRAYSIZE(enabled)) == 1 &&
        enabled[0] == L'1';
}

void WINAPI XboxGetStartupInfoW(LPSTARTUPINFOW startup) {
    GetStartupInfoW(startup);
    const DWORD previous = GetLastError();
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

/* Device-removed diagnostics. Before a hosted D3D12 device is created, force on
   D3D12's automatic breadcrumbs and page-fault tracking so the DXGI adapter's
   removal watchdog can report the command that was executing when the GPU
   faulted. Only the settings interface is used; no debug layer is enabled. */
typedef struct DredSettings DredSettings;
typedef struct DredSettingsMethods {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(DredSettings *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(DredSettings *);
    ULONG (STDMETHODCALLTYPE *Release)(DredSettings *);
    void (STDMETHODCALLTYPE *SetAutoBreadcrumbsEnablement)(DredSettings *, int);
    void (STDMETHODCALLTYPE *SetPageFaultEnablement)(DredSettings *, int);
    void (STDMETHODCALLTYPE *SetWatsonDumpEnablement)(DredSettings *, int);
} DredSettingsMethods;
struct DredSettings {
    const DredSettingsMethods *lpVtbl;
};
static const GUID dredSettingsIid =
    {0x82BC481C, 0x6B9B, 0x4030, {0xAE, 0xDB, 0x7E, 0xE3, 0xD1, 0xDF, 0x1E, 0x63}};

static void EnableDeviceRemovedData(void) {
    static volatile LONG enabled;
    if (InterlockedExchange(&enabled, 1)) return;
    typedef HRESULT (WINAPI *GetDebugInterface)(REFIID, void **);
    const HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    const GetDebugInterface get = d3d12 ? (GetDebugInterface)(void *)GetProcAddress(d3d12, "D3D12GetDebugInterface") : NULL;
    DredSettings *settings = NULL;
    const HRESULT result = get ? get(&dredSettingsIid, (void **)&settings) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    if (SUCCEEDED(result) && settings) {
        settings->lpVtbl->SetAutoBreadcrumbsEnablement(settings, 2 /* D3D12_DRED_ENABLEMENT_FORCED_ON */);
        settings->lpVtbl->SetPageFaultEnablement(settings, 2);
        settings->lpVtbl->Release(settings);
    }
    char line[80];
    char *out = Hex(Text(line, "asset d3d12-dred result=0x"), (DWORD)result, 8);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

/* Godot and other Agility SDK callers create devices from an ID3D12DeviceFactory,
   whose configuration is separate from the global D3D12GetDebugInterface state.
   GetConfigurationInterface is the eighth method, after IUnknown,
   InitializeFromGlobalState, ApplyToGlobalState, SetFlags and GetFlags. */
typedef HRESULT (WINAPI *GetD3D12Interface)(REFCLSID, REFIID, void **);
static PVOID volatile d3d12GetInterface;
static const GUID deviceFactoryClass =
    {0x114863BF, 0xC386, 0x4AEE, {0xB3, 0x9D, 0x8F, 0x0B, 0xBB, 0x06, 0x29, 0x55}};
static const GUID removedDataClass =
    {0x4A75BBC4, 0x9FF4, 0x4AD8, {0x9F, 0x18, 0xAB, 0xAE, 0x84, 0xDC, 0x5F, 0xF2}};

static BOOL SameGuid(const GUID *left, const GUID *right) {
    const unsigned char *a = (const unsigned char *)left, *b = (const unsigned char *)right;
    for (unsigned i = 0; i < sizeof(GUID); ++i) if (a[i] != b[i]) return FALSE;
    return TRUE;
}

static HRESULT WINAPI TraceD3D12GetInterface(REFCLSID clsid, REFIID iid, void **out) {
    const GetD3D12Interface get = (GetD3D12Interface)InterlockedCompareExchangePointer(&d3d12GetInterface, NULL, NULL);
    const HRESULT result = get(clsid, iid, out);
    if (SUCCEEDED(result) && out && *out && clsid && SameGuid(clsid, &deviceFactoryClass)) {
        const DWORD previous = GetLastError();
        typedef HRESULT (STDMETHODCALLTYPE *GetConfiguration)(void *, REFCLSID, REFIID, void **);
        void **methods = *(void ***)*out;
        DredSettings *settings = NULL;
        const HRESULT configured = ((GetConfiguration)methods[7])(*out, &removedDataClass, &dredSettingsIid,
            (void **)&settings);
        if (SUCCEEDED(configured) && settings) {
            settings->lpVtbl->SetAutoBreadcrumbsEnablement(settings, 2);
            settings->lpVtbl->SetPageFaultEnablement(settings, 2);
            settings->lpVtbl->Release(settings);
        }
        char line[80];
        char *text = Hex(Text(line, "asset d3d12-factory-dred result=0x"), (DWORD)configured, 8);
        text = Text(text, "\r\n");
        AppendTrace(line, (DWORD)(text - line));
        SetLastError(previous);
    }
    return result;
}

static HRESULT WINAPI TraceD3D12CreateDevice(IUnknown *adapter, UINT level, const IID *iid, void **device) {
    const DWORD previous = GetLastError();
    if (HostedDxgiEnabled()) EnableDeviceRemovedData();
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

static BOOL HostedDxgiEnabled(void) {
    WCHAR enabled[2];
    return GetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_DXGI", enabled, ARRAYSIZE(enabled)) == 1 && enabled[0] == L'1';
}

static HMODULE HostedDxgiAdapter(void) {
    static PVOID volatile loaded;
    HMODULE adapter = (HMODULE)InterlockedCompareExchangePointer(&loaded, NULL, NULL);
    if (!adapter) {
        adapter = LoadPackagedLibrary(L"XboxWin32Dxgi.dll", 0);
        if (!adapter) return NULL;
        HMODULE existing = (HMODULE)InterlockedCompareExchangePointer(&loaded, adapter, NULL);
        if (existing) {
            FreeLibrary(adapter);
            adapter = existing;
        }
    }
    return adapter;
}

/* Hosted windows are invisible to the system compositor, so their swap chains and
   DirectComposition devices come from the hosted DXGI adapter. */
static FARPROC HostedDxgiFactory(HMODULE module, const char *name) {
    const BOOL factory = (SameName(name, "CreateDXGIFactory2") || SameName(name, "CreateDXGIFactory1") ||
        SameName(name, "CreateDXGIFactory")) && module == GetModuleHandleW(L"dxgi.dll");
    const BOOL composition = SameName(name, "DCompositionCreateDevice") && module == GetModuleHandleW(L"dcomp.dll");
    if (!(factory || composition) || !HostedDxgiEnabled()) return NULL;
    const HMODULE adapter = HostedDxgiAdapter();
    return adapter ? GetProcAddress(adapter, name) : NULL;
}

/* With hosted XInput the Xbox host owns the controller, and the Steam client cannot
   see the hosted pad. A native Steam Input title that finds a Steam-side controller
   keeps reading Steam Input actions that never arrive, so Steam Input reports no
   connected controllers for these titles. Titles that fall back to their own gamepad
   input when Steam Input has none then read the hosted pad. */
static int __cdecl NoSteamInputControllers(void *input, unsigned long long *handles) {
    (void)input;
    (void)handles;
    static volatile LONG traced;
    if (!InterlockedExchange(&traced, 1)) {
        const char trace[] = "input steam-input-controllers=hosted-none\r\n";
        AppendTrace(trace, sizeof(trace) - 1);
    }
    return 0;
}

static FARPROC HostedSteamInput(HMODULE module, const char *name) {
    WCHAR hosted[2];
    if (!SameName(name, "SteamAPI_ISteamInput_GetConnectedControllers") ||
            GetEnvironmentVariableW(L"XBOX_WIN32_HOST_XINPUT", hosted, ARRAYSIZE(hosted)) != 1 || hosted[0] != L'1' ||
            !module || module != GetModuleHandleW(L"steam_api64.dll")) return NULL;
    return (FARPROC)(void *)NoSteamInputControllers;
}

FARPROC WINAPI XboxGetProcAddress(HANDLE module, const char *name) {
    if (module && (ULONG_PTR)name <= 0xffffu) {
        const DWORD previous = GetLastError();
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
    if (result && (ULONG_PTR)name > 0xffffu) {
        if (name[0] == 'D' && SameName(name, "D3D12CreateDevice") &&
                module == GetModuleHandleW(L"d3d12.dll")) {
            const PVOID first = InterlockedCompareExchangePointer(&d3d12CreateDevice, (PVOID)result, NULL);
            if (!first || first == (PVOID)result) result = (FARPROC)(PVOID)TraceD3D12CreateDevice;
        } else if (name[0] == 'D' && SameName(name, "D3D12GetInterface") && HostedDxgiEnabled() &&
                module == GetModuleHandleW(L"d3d12.dll")) {
            EnableDeviceRemovedData();
            const PVOID first = InterlockedCompareExchangePointer(&d3d12GetInterface, (PVOID)result, NULL);
            if (!first || first == (PVOID)result) result = (FARPROC)(PVOID)TraceD3D12GetInterface;
        } else if (name[0] == 'C' || name[0] == 'D') {
            const FARPROC hosted = HostedDxgiFactory(module, name);
            if (hosted) {
                const char factoryTrace[] = "asset dxgi-factory=hosted\r\n";
                const char compositionTrace[] = "asset dcomp-device=hosted\r\n";
                if (name[0] == 'C') AppendTrace(factoryTrace, sizeof(factoryTrace) - 1);
                else AppendTrace(compositionTrace, sizeof(compositionTrace) - 1);
                result = hosted;
            }
        } else if (name[0] == 'S' && HostedSteamInput((HMODULE)module, name)) {
            result = HostedSteamInput((HMODULE)module, name);
        } else if (name[0] == 'S') {
            const FARPROC scoped = ScopedShellExport((HMODULE)module, name);
            if (scoped) {
                const char trace[] = "asset known-folder=scoped\r\n";
                AppendTrace(trace, sizeof(trace) - 1);
                result = scoped;
            }
        }
    }
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
