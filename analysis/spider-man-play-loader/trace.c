#ifndef SOUNDBANK_MIB
#define SOUNDBANK_MIB 128
#endif
#ifndef PSO_SLOTS
#define PSO_SLOTS 1
#endif
/* Extra compile slots while available commit is at least PSO_BOOST_MIN_AVAIL_MIB (boot has over
   1 GiB; gameplay 60-400 MiB, so gameplay keeps PSO_SLOTS). 0 keeps the fixed pool. */
#ifndef PSO_BOOST_SLOTS
#define PSO_BOOST_SLOTS 0
#endif
#ifndef PSO_BOOST_MIN_AVAIL_MIB
#define PSO_BOOST_MIN_AVAIL_MIB 768
#endif
/* PSO_PRIORITY=1: render- and main-thread compiles go first (and may use one slot above the limit
   while at least PSO_URGENT_MIN_AVAIL_MIB is available); replay compiles run only when no title
   compile is waiting. 0 keeps first-come admission for every compile. */
#ifndef PSO_PRIORITY
#define PSO_PRIORITY 0
#endif
#ifndef PSO_URGENT_MIN_AVAIL_MIB
#define PSO_URGENT_MIN_AVAIL_MIB 256
#endif
/* Pipeline replay threads (1 keeps the single below-normal thread). PSO_BG_RESERVE leaves that many
   compile slots free of replay work, so a title compile starts at once. */
#ifndef PSO_REPLAY_THREADS
#define PSO_REPLAY_THREADS 1
#endif
#ifndef PSO_BG_RESERVE
#define PSO_BG_RESERVE 0
#endif
/* PSO_SKIP_WARMUP=1: hide the game's pipeline warm-up list and record the run's pipelines in a session
   list; PSO_WARMUP_LIST=1: warm up from that session list instead of the player's (pso-warmup.h). */
#ifndef PSO_SKIP_WARMUP
#define PSO_SKIP_WARMUP 0
#endif
#ifndef PSO_WARMUP_LIST
#define PSO_WARMUP_LIST 0
#endif
/* An opt-in loader trace for unsigned, locally staged Windows images. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include "loader-trace-output.h"
#include "hosted-xinput-load.h"
#include <d3dcommon.h>
#include <dxgi1_2.h>
#include <d3d12.h>
#include <appmodel.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <objbase.h>
#include <tlhelp32.h>

static volatile LONG64 loaderEvents[128];
static volatile LONG faultTraceStarted;
static volatile LONG workingDirectoryStarted;
static volatile LONG originalImageObserved;
static volatile LONG assetEvents;
static volatile LONG deviceEvents;
static volatile LONG devicePrimed;
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

static void SaveRenderFailure(const char *record, DWORD size) {
    WCHAR path[1024];
    const WCHAR suffix[] = L"-failure.txt";
    const DWORD length = GetEnvironmentVariableW(L"XBOX_SDL_TRACE_FILE", path, ARRAYSIZE(path));
    if (length < 5 || length > ARRAYSIZE(path) - ARRAYSIZE(suffix) || path[length - 4] != L'.') return;
    for (UINT i = 0; i < ARRAYSIZE(suffix); ++i) path[length - 4 + i] = suffix[i];
    HANDLE file = CreateFile2FromAppW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        OPEN_ALWAYS, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, record, size, &written, NULL);
    CloseHandle(file);
}


typedef struct PsoSnapshotInput {
    char semantic[32];
    DWORD index, format, slot, offset, category, step;
} PsoSnapshotInput;

typedef struct PsoSnapshot {
    DWORD magic, version, ordinal, descriptorBytes;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC descriptor;
    PsoSnapshotInput input[32];
} PsoSnapshot;

__attribute__((noinline)) static void SaveWorldPsoState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, DWORD ordinal) {
    if (desc->StreamOutput.NumEntries || desc->StreamOutput.NumStrides ||
        desc->InputLayout.NumElements > 32) return;
    WCHAR path[1024];
    const WCHAR suffix[] = L"-world-pso.bin";
    const DWORD length = GetEnvironmentVariableW(L"XBOX_SDL_TRACE_FILE", path, ARRAYSIZE(path));
    if (length < 5 || length > ARRAYSIZE(path) - ARRAYSIZE(suffix) || path[length - 4] != L'.') return;

    static PsoSnapshot snapshot;
    __builtin_memset_inline(&snapshot, 0, sizeof(snapshot));
    snapshot.magic = 0x4f535057u;
    snapshot.version = 1;
    snapshot.ordinal = ordinal;
    snapshot.descriptorBytes = sizeof(*desc);
    __builtin_memcpy_inline(&snapshot.descriptor, desc, sizeof(*desc));
    snapshot.descriptor.pRootSignature = NULL;
    snapshot.descriptor.VS.pShaderBytecode = NULL;
    snapshot.descriptor.PS.pShaderBytecode = NULL;
    snapshot.descriptor.DS.pShaderBytecode = NULL;
    snapshot.descriptor.HS.pShaderBytecode = NULL;
    snapshot.descriptor.GS.pShaderBytecode = NULL;
    snapshot.descriptor.StreamOutput.pSODeclaration = NULL;
    snapshot.descriptor.StreamOutput.pBufferStrides = NULL;
    snapshot.descriptor.InputLayout.pInputElementDescs = NULL;
    snapshot.descriptor.CachedPSO.pCachedBlob = NULL;
    for (DWORD i = 0; i < desc->InputLayout.NumElements; ++i) {
        const D3D12_INPUT_ELEMENT_DESC *element = &desc->InputLayout.pInputElementDescs[i];
        PsoSnapshotInput *out = &snapshot.input[i];
        for (DWORD k = 0; k + 1 < ARRAYSIZE(out->semantic); ++k) {
            const char ch = element->SemanticName[k];
            if (!ch) break;
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '_')) return;
            out->semantic[k] = ch;
        }
        out->index = element->SemanticIndex;
        out->format = element->Format;
        out->slot = element->InputSlot;
        out->offset = element->AlignedByteOffset;
        out->category = element->InputSlotClass;
        out->step = element->InstanceDataStepRate;
    }
    for (DWORD i = 0; i < ARRAYSIZE(suffix); ++i) path[length - 4 + i] = suffix[i];
    const HANDLE file = CreateFile2FromAppW(path, GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    const DWORD size = (DWORD)(FIELD_OFFSET(PsoSnapshot, input) + desc->InputLayout.NumElements * sizeof(PsoSnapshotInput));
    DWORD written = 0;
    WriteFile(file, &snapshot, size, &written, NULL);
    FlushFileBuffers(file);
    CloseHandle(file);
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

static void TraceAllocationStack(const char *prefix);

typedef DWORD (__cdecl *CreateFidelityFxContext)(void **, void *, const void *);
static PVOID volatile createFidelityFxContext;
static volatile LONG fidelityFxReached;

static void TraceFaultCallers(const CONTEXT *faultContext) {
    if (!faultContext) return;
    CONTEXT frame;
    __builtin_memcpy_inline(&frame, faultContext, sizeof(frame));
    ULONG_PTR low, high;
    GetCurrentThreadStackLimits(&low, &high);
    for (DWORD depth = 0; depth < 14 && frame.Rip && frame.Rsp >= low && frame.Rsp <= high - sizeof(ULONG_PTR); ++depth) {
        char line[145];
        char *out = Hex(Text(line, "fault context-caller=0x"), depth, 2);
        out = RecordImageAddress(out, (const void *)(ULONG_PTR)frame.Rip);
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
        const DWORD64 previousSP = frame.Rsp;
        DWORD64 base, establisher;
        PVOID data;
        const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(frame.Rip, &base, NULL);
        if (function) {
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, frame.Rip, function, &frame, &data, &establisher, NULL);
        } else {
            frame.Rip = *(const ULONG_PTR *)(ULONG_PTR)frame.Rsp;
            frame.Rsp += sizeof(ULONG_PTR);
        }
        if (frame.Rsp <= previousSP) break;
    }
}

/* Build 23986256 trial. These traps replace only instructions already on
   allocation failure paths. Continue with the same register effects. */
static void TraceScaleformBudget(const char *phase, DWORD original, DWORD adjusted) {
    const BYTE *image = (const BYTE *)GetModuleHandleW(NULL);
    SIZE_T read = 0;
    DWORD capacity = 0;
    DWORD64 used = 0;
    HANDLE self = GetCurrentProcess();
    if (image) {
        ReadProcessMemory(self, image + 0x0628E458u, &capacity, sizeof(capacity), &read);
        ReadProcessMemory(self, image + 0x07B84868u, &used, sizeof(used), &read);
    }
    MEMORYSTATUSEX memory = {0};
    memory.dwLength = sizeof(memory);
    const BOOL queried = GlobalMemoryStatusEx(&memory);
    char line[174];
    char *out = Text(Text(line, "asset scaleform-budget="), phase);
    out = Hex(Text(out, " stock-kib=0x"), original >> 10, 8);
    out = Hex(Text(out, " trial-kib=0x"), adjusted >> 10, 8);
    out = Hex(Text(out, " arena-kib=0x"), capacity >> 10, 8);
    out = Hex(Text(out, " used-kib=0x"), (DWORD)(used >> 10), 8);
    out = Hex(Text(out, " available-commit-kib=0x"), queried ? (DWORD)(memory.ullAvailPageFile >> 10) : MAXDWORD, 8);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

static BOOL ObserveScaleformBudget(EXCEPTION_POINTERS *exception) {
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT || !exception->ContextRecord)
        return FALSE;
    const ULONG_PTR base = (ULONG_PTR)GetModuleHandleW(NULL);
    const ULONG_PTR address = (ULONG_PTR)exception->ExceptionRecord->ExceptionAddress;
    CONTEXT *context = exception->ContextRecord;
    if (!base || address != base + 0x01D26732u || (context->Rip != address && context->Rip != address + 1))
        return FALSE;
    const BYTE *code = (const BYTE *)address;
    if (code[0] != 0xcc || code[1] != 0x90 || code[5] != 0x90) return FALSE;
    const DWORD previous = GetLastError();
    const DWORD original = (DWORD)context->Rbx;
    const DWORD ceiling = 48u * 1024u * 1024u + 128u * 1024u;
    const DWORD increment = 16u * 1024u * 1024u;
    DWORD amount = original;
    if (original >= 16u * 1024u * 1024u && original < ceiling)
        amount = ceiling - original > increment ? original + increment : ceiling;

    // Reproduce the original CMP EBX, 0x20000 status flags for the adjusted amount.
    const DWORD threshold = 0x20000u;
    const DWORD result = amount - threshold;
    DWORD flags = 0;
    if (amount < threshold) flags |= 0x01;
    if (!(__builtin_parity(result & 0xffu))) flags |= 0x04;
    if ((amount ^ threshold ^ result) & 0x10u) flags |= 0x10;
    if (!result) flags |= 0x40;
    if (result & 0x80000000u) flags |= 0x80;
    if (((amount ^ threshold) & (amount ^ result)) & 0x80000000u) flags |= 0x800;
    context->Rbx = amount;
    context->EFlags = (context->EFlags & ~(DWORD)0x8d5) | flags;
    context->Rip = address + 6;
    TraceScaleformBudget("startup", original, amount);
    SetLastError(previous);
    return TRUE;
}

static BOOL ObserveWorldAllocationFailure(EXCEPTION_POINTERS *exception) {
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT || !exception->ContextRecord)
        return FALSE;
    const ULONG_PTR base = (ULONG_PTR)GetModuleHandleW(NULL);
    const ULONG_PTR address = (ULONG_PTR)exception->ExceptionRecord->ExceptionAddress;
    CONTEXT *context = exception->ContextRecord;
    if (!base || address < base || (context->Rip != address && context->Rip != address + 1))
        return FALSE;
    const BYTE *bytes = (const BYTE *)address;
    static volatile LONG count;
    const char *stage;
    const ULONG_PTR rva = address - base;
    if (rva == 0x031C318Fu && bytes[0] == 0xcc && bytes[1] == 0x90 && bytes[2] == 0x90) {
        context->Rdx = context->Rbx;
        context->Rip = address + 3;
        stage = "slot-table-null";
    } else if ((rva == 0x031C4B15u || rva == 0x031C4B56u || rva == 0x031C3198u) &&
            bytes[0] == 0xcc && bytes[1] == 0x90) {
        context->Rax = rva == 0x031C3198u ? context->Rax & ~(DWORD64)0xff : 0;
        context->EFlags = (context->EFlags & ~(DWORD)0x8c5) | 0x44;
        context->Rip = address + 2;
        stage = rva == 0x031C4B15u ? "object-null" : rva == 0x031C4B56u ? "handle-null" : "pool-growth-null";
    } else {
        return FALSE;
    }
    const DWORD previous = GetLastError();
    if (InterlockedIncrement(&count) <= 32) {
        MEMORYSTATUSEX memory = {0};
        memory.dwLength = sizeof(memory);
        const BOOL queried = GlobalMemoryStatusEx(&memory);
        char line[150];
        char *out = Text(Text(line, "asset world-allocation="), stage);
        out = Hex(Text(out, " rva=0x"), (DWORD)rva, 8);
        out = Hex(Text(out, " thread=0x"), GetCurrentThreadId(), 8);
        out = Hex(Text(out, " available-commit-kib=0x"), queried ? (DWORD)(memory.ullAvailPageFile >> 10) : MAXDWORD, 8);
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
        const BOOL object = rva == 0x031C4B15u;
        if (object || rva == 0x031C318Fu || rva == 0x031C3198u) {
            const ULONG_PTR owner = object ? (ULONG_PTR)context->Rsi : (ULONG_PTR)context->Rdi + 8u;
            ULONG_PTR heap = 0, table = 0, target = 0;
            SIZE_T read = 0;
            HANDLE self = GetCurrentProcess();
            if (ReadProcessMemory(self, (const void *)owner, &heap, sizeof(heap), &read) && read == sizeof(heap) &&
                heap && ReadProcessMemory(self, (const void *)heap, &table, sizeof(table), &read) &&
                read == sizeof(table) && table &&
                ReadProcessMemory(self, (const void *)(table + (object ? 0x50u : 0x48u)),
                    &target, sizeof(target), &read) && read == sizeof(target) && target) {
                char descriptor[152];
                char *tail = Text(descriptor, object ? "asset world-allocator=object" : "asset world-allocator=pool");
                tail = RecordImageAddress(tail, (const void *)target);
                tail = Text(tail, "\r\n");
                AppendTrace(descriptor, (DWORD)(tail - descriptor));
            }
        }
    }
    SetLastError(previous);
    return TRUE;
}


typedef struct PipelineTrace {
    DWORD owner;
    DWORD ordinal;
    D3D12_SHADER_BYTECODE stages[5];
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *descriptor;
    BOOL graphics;
} PipelineTrace;
static PipelineTrace currentPipeline;

static BOOL MatchesPreviousCompilerFault(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc) {
    static const BYTE expected[2][20] = {
        {0x44,0x58,0x42,0x43,0x1A,0x88,0x97,0x74,0x33,0xE4,0x8D,0xBA,0x18,0xC1,0x01,0x62,0x78,0xA0,0x3D,0x86},
        {0x44,0x58,0x42,0x43,0x39,0xBE,0xD9,0x28,0x2E,0xAC,0x82,0x5E,0x20,0xDB,0xDD,0x49,0x59,0x2E,0x60,0x21},
    };
    if (desc->VS.BytecodeLength != 0x39B8 || desc->PS.BytecodeLength != 0x11930) return FALSE;
    const void *shaders[] = {desc->VS.pShaderBytecode, desc->PS.pShaderBytecode};
    for (UINT index = 0; index < ARRAYSIZE(shaders); ++index) {
        BYTE header[20];
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), shaders[index], header, sizeof(header), &read) ||
                read != sizeof(header)) return FALSE;
        for (UINT byte = 0; byte < sizeof(header); ++byte)
            if (header[byte] != expected[index][byte]) return FALSE;
    }
    return TRUE;
}

static void TracePipelineFault(void) {
    if (currentPipeline.owner != GetCurrentThreadId()) return;
    if (currentPipeline.graphics && currentPipeline.descriptor)
        SaveWorldPsoState(currentPipeline.descriptor, currentPipeline.ordinal);
    MEMORYSTATUSEX memory = {0};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        char line[115];
        char *out = Hex(Text(line, "asset fault-pso-commit available-kib=0x"),
            (DWORD)(memory.ullAvailPageFile >> 10), 8);
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
        SaveRenderFailure(line, (DWORD)(out - line));
    }
    const char *names[] = {"vs", "ps", "ds", "hs", "gs"};
    const DWORD count = currentPipeline.graphics ? ARRAYSIZE(names) : 1;
    const HANDLE self = GetCurrentProcess();
    for (DWORD i = 0; i < count; ++i) {
        const D3D12_SHADER_BYTECODE shader = currentPipeline.stages[i];
        BYTE header[20];
        SIZE_T read = 0;
        if (shader.BytecodeLength < sizeof(header) || !shader.pShaderBytecode ||
            !ReadProcessMemory(self, shader.pShaderBytecode, header, sizeof(header), &read) ||
            read != sizeof(header)) continue;
        char line[172];
        const char *name = currentPipeline.graphics ? names[i] : "cs";
        char *out = Text(Text(line, "asset fault-pso stage="), name);
        out = Hex(Text(out, " ordinal=0x"), currentPipeline.ordinal, 8);
        out = Hex(Text(out, " size=0x"), (DWORD)shader.BytecodeLength, 8);
        out = Text(out, " container=0x");
        for (DWORD j = 0; j < sizeof(header); ++j) out = Hex(out, header[j], 2);
        out = RecordImageAddress(out, shader.pShaderBytecode);
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
        SaveRenderFailure(line, (DWORD)(out - line));
    }
}

static BOOL ObserveWorldZoneLookup(EXCEPTION_POINTERS *exception) {
    const EXCEPTION_RECORD *fault = exception->ExceptionRecord;
    CONTEXT *context = exception->ContextRecord;
    if (fault->ExceptionCode != EXCEPTION_BREAKPOINT || !context) return FALSE;
    const ULONG_PTR image = (ULONG_PTR)GetModuleHandleW(NULL);
    const ULONG_PTR address = (ULONG_PTR)fault->ExceptionAddress;
    if (!image || address != image + 0x01626648u ||
        (context->Rip != address && context->Rip != address + 1)) return FALSE;
    const BYTE *at = (const BYTE *)address;
    if (at[0] != 0xcc || at[1] != 0x90 || at[5] != 0x90) return FALSE;
    const DWORD lastError = GetLastError();
    DWORD high = 0;
    ReadProcessMemory(GetCurrentProcess(), (const void *)(ULONG_PTR)(context->Rsp + 0x8c), &high, sizeof(high), NULL);
    char line[160];
    char *out = Hex(Text(line, "asset world-zone-lookup id=0x"), high, 8);
    out = Hex(out, (DWORD)context->Rbx, 8);
    out = Hex(Text(out, " lookup-key-high=0x"), (DWORD)(context->Rbx >> 32), 8);
    out = Hex(Text(out, " thread=0x"), GetCurrentThreadId(), 8);
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        char digits[24];
        int count = 0;
        unsigned long long value = (unsigned long long)now.QuadPart;
        do digits[count++] = (char)('0' + value % 10); while (value /= 10);
        out = Text(out, " qpc=");
        while (count) *out++ = digits[--count];
    }
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
    SaveRenderFailure(line, (DWORD)(out - line));
    context->R9 = 4;
    context->Rip = address + 6;
    SetLastError(lastError);
    return TRUE;
}


static volatile LONG arrayResizes[2];
static volatile LONG arrayFailures[2];

static BOOL ObserveScaleformArrayResize(EXCEPTION_POINTERS *exception) {
    const EXCEPTION_RECORD *fault = exception->ExceptionRecord;
    CONTEXT *context = exception->ContextRecord;
    if (fault->ExceptionCode != EXCEPTION_BREAKPOINT || !context) return FALSE;
    const ULONG_PTR image = (ULONG_PTR)GetModuleHandleW(NULL);
    const ULONG_PTR address = (ULONG_PTR)fault->ExceptionAddress;
    const ULONG_PTR rva = address - image;
    const BOOL reallocation = rva == 0x035231FEu || rva == 0x0354D15Eu;
    const BOOL narrowArray = rva == 0x0354D15Eu || rva == 0x0354D189u;
    if (!image || (!reallocation && rva != 0x03523229u && rva != 0x0354D189u) ||
        (context->Rip != address && context->Rip != address + 1)) return FALSE;
    const BYTE *code = (const BYTE *)address;
    if (code[0] != 0xcc || code[1] != 0x8b || code[2] != 0x7c || code[3] != 0x24 || code[4] != 0x38)
        return FALSE;

    // Both staged sites follow the allocator result and replace "mov rdi,[rsp+38h]".
    const DWORD error = GetLastError();
    const UINT64 requestedCapacity = context->Rdi;
    const UINT64 bytes = requestedCapacity * (narrowArray ? 16u : 32u);
    const BOOL failed = context->Rax == 0 && requestedCapacity != 0;
    const UINT kind = reallocation ? 1 : 0;
    const LONG requests = InterlockedIncrement(&arrayResizes[kind]);
    const LONG failures = failed ? InterlockedIncrement(&arrayFailures[kind]) : 0;
    if ((failed && failures <= 8) || (requestedCapacity && requests <= 2)) {
        MEMORYSTATUSEX memory = {0};
        memory.dwLength = sizeof(memory);
        const BOOL queried = GlobalMemoryStatusEx(&memory);
        const HANDLE self = GetCurrentProcess();
        ULONG_PTR allocator = 0, vtable = 0, method = 0;
        const ULONG_PTR vtableOffset = reallocation ? 0x58 : 0x70;
        if (ReadProcessMemory(self, (const void *)(image + 0x07DD6D48u), &allocator, sizeof(allocator), NULL) &&
            allocator && ReadProcessMemory(self, (const void *)allocator, &vtable, sizeof(vtable), NULL) && vtable)
            ReadProcessMemory(self, (const void *)(vtable + vtableOffset), &method, sizeof(method), NULL);
        char line[282];
        char *out = Text(line, reallocation ? "asset scaleform-array-realloc=" : "asset scaleform-array-alloc=");
        out = Text(out, failed ? "null" : "ok");
        out = Hex(Text(out, " requests=0x"), (DWORD)requests, 8);
        out = Hex(Text(out, " element=0x"), narrowArray ? 16 : 32, 2);
        out = Hex(Text(out, " capacity=0x"), (DWORD)(requestedCapacity >> 32), 8);
        out = Hex(out, (DWORD)requestedCapacity, 8);
        out = Hex(Text(out, " bytes=0x"), (DWORD)(bytes >> 32), 8);
        out = Hex(out, (DWORD)bytes, 8);
        out = Hex(Text(out, " thread=0x"), GetCurrentThreadId(), 8);
        out = Hex(Text(out, " available-commit-kib=0x"), queried ? (DWORD)(memory.ullAvailPageFile >> 10) : MAXDWORD, 8);
        if (method) out = RecordImageAddress(out, (const void *)method);
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
        if (failed) {
            SaveRenderFailure(line, (DWORD)(out - line));
            TraceScaleformBudget("array-resize-null", 0, 0);
        }
    }
    context->Rdi = *(const DWORD64 *)(ULONG_PTR)(context->Rsp + 0x38u);
    context->Rip = address + 5;
    SetLastError(error);
    return TRUE;
}

static LONG WINAPI ObserveFault(EXCEPTION_POINTERS *exception) {
    if (ObserveScaleformBudget(exception) || ObserveWorldAllocationFailure(exception) || ObserveWorldZoneLookup(exception) ||
        ObserveScaleformArrayResize(exception)) return EXCEPTION_CONTINUE_EXECUTION;
    const EXCEPTION_RECORD *fault = exception->ExceptionRecord;
    const BOOL access = fault->ExceptionCode == EXCEPTION_ACCESS_VIOLATION;
    if (access && fault->NumberParameters >= 2 && fault->ExceptionInformation[1] < 4096) {
        const ULONG_PTR image = (ULONG_PTR)GetModuleHandleW(NULL);
        const ULONG_PTR address = (ULONG_PTR)fault->ExceptionAddress;
        if (image && (address == image + 0x031C50ABu || address == image + 0x034DBB90u ||
                      address == image + 0x034C3445u || address == image + 0x0354BE97u)) {
            const DWORD previous = GetLastError();
            TraceScaleformBudget("world-fault", 0, 0);
            if ((address == image + 0x034C3445u || address == image + 0x0354BE97u) &&
                exception->ContextRecord) {
                char line[178];
                char *out = Hex(Text(line, "asset scaleform-array-fault-alloc-calls=0x"), arrayResizes[0], 8);
                out = Hex(Text(out, " realloc-calls=0x"), arrayResizes[1], 8);
                out = Hex(Text(out, " alloc-null=0x"), arrayFailures[0], 8);
                out = Hex(Text(out, " realloc-null=0x"), arrayFailures[1], 8);
                out = Hex(Text(out, " thread=0x"), GetCurrentThreadId(), 8);
                out = Text(out, "\r\n");
                AppendTrace(line, (DWORD)(out - line));
                SaveRenderFailure(line, (DWORD)(out - line));
            }
            if (address == image + 0x034DBB90u && exception->ContextRecord) {
                ULONG_PTR table = 0, target = 0;
                SIZE_T read = 0;
                HANDLE self = GetCurrentProcess();
                const ULONG_PTR object = (ULONG_PTR)exception->ContextRecord->R15;
                if (object && ReadProcessMemory(self, (const void *)object, &table, sizeof(table), &read) &&
                    read == sizeof(table) && table &&
                    ReadProcessMemory(self, (const void *)(table + 0x70u), &target, sizeof(target), &read) &&
                    read == sizeof(target) && target) {
                    char line[140];
                    char *out = RecordImageAddress(Text(line, "asset world-provider=null-output"), (const void *)target);
                    out = Text(out, "\r\n");
                    AppendTrace(line, (DWORD)(out - line));
                }
            }
            SetLastError(previous);
        }
    }
    if (access || (fault->ExceptionCode == 0xE06D7363u && InterlockedCompareExchange(&fidelityFxReached, 0, 0))) {
        static volatile LONG fxFaults;
        if (InterlockedIncrement(&fxFaults) <= 2) {
            const DWORD previous = GetLastError();
            if (access) TracePipelineFault();
            TraceFaultCallers(exception->ContextRecord);
            SetLastError(previous);
        }
    }
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
    if (access && fault->NumberParameters >= 2 && exception->ContextRecord &&
            (ULONG_PTR)fault->ExceptionAddress - (ULONG_PTR)GetModuleHandleW(NULL) == 0x016DC8AEu) {
        const CONTEXT *context = exception->ContextRecord;
        MEMORY_BASIC_INFORMATION base, target;
        const BOOL baseValid = VirtualQuery((LPCVOID)(ULONG_PTR)context->R15, &base, sizeof(base)) != 0;
        const BOOL targetValid = VirtualQuery((LPCVOID)fault->ExceptionInformation[1], &target, sizeof(target)) != 0;
        char detail[175];
        char *tail = Hex(Text(detail, "fault asset-tail size=0x"), (DWORD)context->Rax, 8);
        tail = Hex(Text(tail, " base-state=0x"), baseValid ? base.State : 0, 8);
        tail = Hex(Text(tail, " tail-state=0x"), targetValid ? target.State : 0, 8);
        tail = Hex(Text(tail, " base-protect=0x"), baseValid ? base.Protect : 0, 8);
        tail = Hex(Text(tail, " tail-protect=0x"), targetValid ? target.Protect : 0, 8);
        tail = Text(tail, baseValid && targetValid && base.AllocationBase == target.AllocationBase ?
            " allocation=same\r\n" : " allocation=other\r\n");
        AppendTrace(detail, (DWORD)(tail - detail));
    }
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
    const char *line = registerHandler && registerHandler(1, ObserveFault)
        ? "fault observer=ready\r\n" : "fault observer=unavailable\r\n";
    DWORD length = 0;
    while (line[length]) ++length;
    AppendTrace(line, length);
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

static void TraceAllocationStack(const char *prefix);

DECLSPEC_NORETURN void WINAPI XboxExitProcess(UINT code) {
    RecordProcessExit("exit-process", code, __builtin_return_address(0));
    TraceAllocationStack("fault exit-process-caller=0x");
    ExitProcess(code);
}

BOOL WINAPI XboxTerminateProcess(HANDLE process, UINT code) {
    RecordProcessExit("terminate-process", code, __builtin_return_address(0));
    TraceAllocationStack("fault terminate-process-caller=0x");
    return TerminateProcess(process, code);
}

void WINAPI XboxRaiseException(DWORD code, DWORD flags, DWORD count, const ULONG_PTR *arguments) {
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_ACCESS_VIOLATION ||
            code == delayModuleMissing || code == delayExportMissing)
        RecordProcessExit("raise-exception", code, __builtin_return_address(0));
    RaiseException(code, flags, count, arguments);
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

static BOOL DefaultD3D12AdapterEnabled(void) {
    WCHAR enabled[2];
    return GetEnvironmentVariableW(L"XBOX_WIN32_D3D12_DEFAULT_ADAPTER", enabled, ARRAYSIZE(enabled)) == 1 &&
        enabled[0] == L'1';
}

static void TraceRemainingCommit(const char *prefix, SIZE_T bytes, DWORD attributes, const void *caller) {
    MEMORYSTATUSEX memory;
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) return;
    char line[185];
    char *out = Hex(Text(line, prefix), (DWORD)((UINT64)bytes >> 32), 8);
    out = Hex(out, (DWORD)bytes, 8);
    out = Hex(Text(out, " flags=0x"), attributes, 8);
    out = Hex(Text(out, " available-commit-kib=0x"), (DWORD)(memory.ullAvailPageFile >> 10), 8);
    if (caller) out = RecordImageAddress(out, caller);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

__declspec(noinline) static void TraceAllocationStack(const char *prefix) {
    PVOID stack[8];
    const USHORT count = RtlCaptureStackBackTrace(2, ARRAYSIZE(stack), stack, NULL);
    char marker[90];
    char *out = Hex(Text(marker, "asset d3d12-heap-stack-count=0x"), count, 2);
    out = Text(out, "\r\n");
    AppendTrace(marker, (DWORD)(out - marker));
    for (USHORT frame = 0; frame < count; ++frame) {
        char line[150];
        char *out = Hex(Text(line, prefix), frame, 2);
        out = RecordImageAddress(out, stack[frame]);
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
    }
}

#include "owners.h"
#include "lazy.h"
#ifdef MEM_CENSUS
#include "memcensus.h"
#endif

LPVOID WINAPI XboxTraceVirtualAlloc(LPVOID address, SIZE_T bytes, DWORD type, DWORD protection) {
    const ULONG_PTR lazyCaller = (ULONG_PTR)__builtin_return_address(0) - (ULONG_PTR)GetModuleHandleW(NULL);
    const LPVOID lazy = LazyTakeCommit(address, bytes, type, protection,
        lazyCaller == 0x01C64D22u, lazyCaller == 0x01C64DD2u);
    if (lazy) return lazy;
    const LPVOID allocation = VirtualAlloc(address, bytes, type, protection);
    const DWORD error = GetLastError();
    if (allocation && (type & MEM_COMMIT)) OwnerCommit(allocation, bytes);
#ifdef MEM_CENSUS
    if (allocation && (type & MEM_COMMIT)) McRecord(allocation, bytes, protection);
#endif
    if (bytes >= 32ull * 1024u * 1024u || ((type & MEM_COMMIT) && !allocation)) {
        TraceRemainingCommit(allocation ? "asset virtualalloc bytes=0x" : "asset virtualalloc-failed bytes=0x",
            bytes, type, __builtin_return_address(0));
        if ((type & MEM_COMMIT) && (bytes >= 256ull * 1024u * 1024u || !allocation)) {
            TraceAllocationStack("asset virtualalloc-caller=0x");
        }
    }
    SetLastError(error);
    return allocation;
}

typedef struct OwnedCompressionShader {
    SIZE_T inputSize;
    DWORD outputSize;
    BYTE inputHash[32];
    BYTE outputHash[32];
} OwnedCompressionShader;

static const OwnedCompressionShader ownedCompression[] = {
    {67160, 104092, {0x3f,0x25,0x3c,0xfd,0x5b,0xb2,0xb0,0x97,0x91,0x44,0x8b,0x75,0x4e,0x15,0x31,0x94,0x14,0xa9,0xf4,0x56,0x71,0xdd,0xe6,0xc7,0x07,0xe5,0x35,0xea,0xe7,0x4f,0x86,0xaa}, {0x50,0x71,0x84,0x22,0x70,0x97,0x37,0x5a,0xe4,0x2c,0x12,0x61,0x9b,0x4b,0x5c,0xd7,0xc1,0xef,0x3f,0x66,0xe5,0xe7,0xa1,0x09,0x52,0xb2,0x84,0xd2,0x3b,0xfd,0xaa,0xde}},
    {61016, 80380, {0x75,0x7f,0xb9,0x04,0x86,0xaf,0xea,0xa1,0xcb,0xc3,0x6f,0x03,0x3d,0x7d,0xc7,0x2e,0x43,0xa0,0x31,0x39,0x59,0xf2,0x62,0x67,0xc7,0x9b,0x6f,0xd3,0x3b,0x8a,0x1a,0x68}, {0x4c,0x44,0x06,0xa7,0x08,0xe3,0x02,0x66,0x6f,0x69,0x98,0xd1,0x6a,0x49,0xb2,0xf5,0x17,0xcf,0x0a,0xfc,0x7d,0x1a,0x9f,0x72,0xa4,0x4e,0xe9,0x30,0x81,0x70,0xe7,0x80}},
    {67936, 93832, {0x2a,0x6c,0x2e,0x8c,0xe2,0x34,0x44,0x25,0xd9,0xc9,0xa0,0xe3,0xf8,0x43,0x30,0x6f,0xf8,0xf3,0x66,0x44,0x2d,0xc0,0x16,0x4c,0x9b,0x05,0xb2,0xee,0xaa,0xda,0xf7,0xeb}, {0x6e,0x87,0xed,0xbd,0x07,0x0c,0x35,0x5e,0x8e,0xaa,0x97,0xac,0xbd,0xe8,0x0c,0x1d,0x94,0x57,0x54,0xdf,0xbe,0x00,0x9e,0x74,0x99,0x1a,0xcb,0x64,0xf2,0xfd,0xd8,0x32}},
    {11488, 8436, {0x76,0xce,0xf3,0xdf,0xd9,0x89,0xf5,0x86,0x1f,0x87,0x48,0x18,0xbb,0x81,0x93,0x14,0x91,0x8c,0xb8,0xe5,0x46,0xa2,0xd8,0x02,0x2e,0x66,0x61,0xf1,0xbf,0x7b,0x94,0x19}, {0x4d,0xa1,0xff,0xdb,0xfa,0xe5,0x4d,0x36,0xa9,0x4c,0x52,0x43,0x7c,0xf1,0x8c,0xfb,0x10,0x30,0x83,0x12,0x64,0x79,0x97,0x16,0x02,0xc0,0x1c,0xfd,0xd8,0x8f,0xba,0x62}},
};

static BOOL ShaderHashMatches(const void *bytes, DWORD size, const BYTE expected[32]) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BYTE digest[32];
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) return FALSE;
    const NTSTATUS result = BCryptHash(algorithm, NULL, 0, (PUCHAR)bytes, size, digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0) return FALSE;
    for (UINT i = 0; i < ARRAYSIZE(digest); ++i) if (digest[i] != expected[i]) return FALSE;
    return TRUE;
}


typedef struct WorldShader {
    const WCHAR *file;
    DWORD inputSize, outputSize;
    BYTE inputHash[32], outputHash[32];
} WorldShader;

static const WorldShader worldShaders[] = {
    {L"\\world-vs.dxil", 16072, 15161, {0xfd,0x48,0x6e,0xac,0x7e,0x5f,0xc3,0x43,0x1b,0x4a,0xc6,0x10,0xcb,0x96,0xfd,0x87,0x91,0x91,0x3a,0xca,0x06,0x61,0x30,0xa1,0xbd,0x71,0xb6,0x57,0x04,0xb0,0x6d,0x7f}, {0x42,0x99,0x2d,0xb0,0x51,0x4b,0x51,0x45,0xe7,0x73,0x7f,0xe8,0x5c,0xf2,0x42,0x5f,0x4b,0x75,0xcb,0x95,0xdd,0x16,0x8b,0xe9,0xc9,0xc7,0xe2,0xc3,0x71,0x0c,0xb7,0x9b}},
    {L"\\world-ps.dxil", 11848, 13100, {0xf2,0x74,0x58,0x53,0x33,0xd6,0xe5,0x56,0x37,0xb6,0x4a,0xc6,0x6d,0xc3,0x17,0x1b,0xe2,0x72,0x96,0x8e,0xff,0xf7,0xc2,0x55,0x1f,0x6a,0x87,0xb7,0xbc,0xbd,0xc8,0x66}, {0xfd,0x00,0x8a,0xe5,0x3a,0xdb,0xa9,0x50,0x9d,0xc9,0x01,0x62,0xaa,0xe0,0xec,0xe0,0x4d,0x7c,0x2d,0x95,0x9e,0x1c,0xfd,0x55,0x99,0x95,0x77,0xf0,0xf4,0xeb,0xf0,0x99}},
};

// Called under the existing pipeline compile lock. The converted pair stays
// resident for the lifetime of the game and is shared by each matching PSO.
static BOOL ReplaceWorldShaderPair(D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc) {
    if (desc->VS.BytecodeLength != worldShaders[0].inputSize ||
        desc->PS.BytecodeLength != worldShaders[1].inputSize ||
        desc->DS.BytecodeLength || desc->HS.BytecodeLength || desc->GS.BytecodeLength) return FALSE;
    const D3D12_SHADER_BYTECODE stock[] = {desc->VS, desc->PS};
    for (DWORD i = 0; i < ARRAYSIZE(worldShaders); ++i) {
        if (!stock[i].pShaderBytecode || !ShaderHashMatches(stock[i].pShaderBytecode,
                worldShaders[i].inputSize, worldShaders[i].inputHash)) return FALSE;
    }

    static BYTE loaded[ARRAYSIZE(worldShaders)][15161];
    static BOOL checked, ready;
    if (!checked) {
        checked = TRUE;
        ready = TRUE;
        for (DWORD i = 0; i < ARRAYSIZE(worldShaders); ++i) {
            WCHAR path[1024];
            UINT32 length = ARRAYSIZE(path);
            if (GetCurrentPackagePath(&length, path) != ERROR_SUCCESS || !length) {
                ready = FALSE;
                break;
            }
            --length;
            const WCHAR *suffix = worldShaders[i].file;
            while (*suffix && length + 1 < ARRAYSIZE(path)) path[length++] = *suffix++;
            path[length] = 0;
            HANDLE file = *suffix ? INVALID_HANDLE_VALUE :
                CreateFile2(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL);
            if (file == INVALID_HANDLE_VALUE) {
                ready = FALSE;
                break;
            }
            LARGE_INTEGER fileSize;
            DWORD actual = 0;
            const DWORD expected = worldShaders[i].outputSize;
            ready = GetFileSizeEx(file, &fileSize) && fileSize.QuadPart == expected &&
                ReadFile(file, loaded[i], expected, &actual, NULL) && actual == expected &&
                ShaderHashMatches(loaded[i], actual, worldShaders[i].outputHash);
            CloseHandle(file);
            if (!ready) break;
        }
        TraceRemainingCommit(ready ? "asset own-world-dxil-ready bytes=0x" :
            "asset own-world-dxil-unavailable bytes=0x", 0, 0, NULL);
    }
    if (!ready) return FALSE;
    desc->VS = (D3D12_SHADER_BYTECODE){loaded[0], worldShaders[0].outputSize};
    desc->PS = (D3D12_SHADER_BYTECODE){loaded[1], worldShaders[1].outputSize};
    // The caller's cached blob belongs to the legacy shader pair.
    desc->CachedPSO = (D3D12_CACHED_PIPELINE_STATE){0};
    return TRUE;
}

static D3D12_SHADER_BYTECODE SelectCompressionShader(D3D12_SHADER_BYTECODE shader) {
    static BYTE loaded[ARRAYSIZE(ownedCompression)][104092];
    static BOOL checked[ARRAYSIZE(ownedCompression)];
    static BOOL ready[ARRAYSIZE(ownedCompression)];
    for (UINT variant = 0; variant < ARRAYSIZE(ownedCompression); ++variant) {
        const OwnedCompressionShader *entry = &ownedCompression[variant];
        if (shader.BytecodeLength != entry->inputSize || !shader.pShaderBytecode ||
            !ShaderHashMatches(shader.pShaderBytecode, (DWORD)shader.BytecodeLength, entry->inputHash)) continue;
        if (!checked[variant]) {
            checked[variant] = TRUE;
            WCHAR path[1024];
            UINT32 capacity = ARRAYSIZE(path);
            WCHAR suffix[] = L"\\owned-compression-0.dxil";
            suffix[19] += (WCHAR)variant;
            DWORD error = GetCurrentPackagePath(&capacity, path);
            if (!error && capacity > 0 && capacity + ARRAYSIZE(suffix) <= ARRAYSIZE(path)) {
                for (UINT i = 0; i < ARRAYSIZE(suffix); ++i) path[capacity - 1 + i] = suffix[i];
                const HANDLE file = CreateFile2(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL);
                if (file != INVALID_HANDLE_VALUE) {
                    DWORD length = 0;
                    LARGE_INTEGER size;
                    ready[variant] = GetFileSizeEx(file, &size) && size.QuadPart == entry->outputSize &&
                        ReadFile(file, loaded[variant], entry->outputSize, &length, NULL) &&
                        length == entry->outputSize &&
                        ShaderHashMatches(loaded[variant], length, entry->outputHash);
                    CloseHandle(file);
                }
            }
            TraceRemainingCommit(ready[variant] ? "asset own-compression-dxil-ready bytes=0x" :
                "asset own-compression-dxil-unavailable bytes=0x", entry->outputSize, variant, NULL);
        }
        if (ready[variant]) {
            D3D12_SHADER_BYTECODE replacement = {loaded[variant], entry->outputSize};
            return replacement;
        }
        break;
    }
    return shader;
}

static void TraceDeviceRemoval(ID3D12Device *device, D3D12_SHADER_BYTECODE shader, const void *caller) {
    const HRESULT reason = ID3D12Device_GetDeviceRemovedReason(device);
    TraceRemainingCommit("asset d3d12-device-removed-reason bytes=0x", shader.BytecodeLength, (DWORD)reason, caller);
    if (shader.pShaderBytecode && shader.BytecodeLength <= 4u * 1024u * 1024u) {
        const BYTE *bytes = (const BYTE *)shader.pShaderBytecode;
        DWORD digest = 2166136261u;
        for (SIZE_T index = 0; index < shader.BytecodeLength; ++index) digest = (digest ^ bytes[index]) * 16777619u;
        TraceRemainingCommit("asset d3d12-device-last-shader bytes=0x", shader.BytecodeLength, digest, bytes);
    }
    static const GUID iid = {0x98931d33,0x5ae8,0x4791,{0xaa,0x3c,0x1a,0x73,0xa2,0x93,0x4e,0x71}};
    ID3D12DeviceRemovedExtendedData *data = NULL;
    const HRESULT query = ID3D12Device_QueryInterface(device, &iid, (void **)&data);
    TraceRemainingCommit("asset d3d12-dred-query bytes=0x", 0, (DWORD)query, NULL);
    if (FAILED(query) || !data) return;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT commands = {0};
    D3D12_DRED_PAGE_FAULT_OUTPUT fault = {0};
    const HRESULT breadcrumbs = ID3D12DeviceRemovedExtendedData_GetAutoBreadcrumbsOutput(data, &commands);
    const HRESULT pageFault = ID3D12DeviceRemovedExtendedData_GetPageFaultAllocationOutput(data, &fault);
    TraceRemainingCommit("asset d3d12-dred-breadcrumbs bytes=0x", commands.pHeadAutoBreadcrumbNode != NULL, (DWORD)breadcrumbs, NULL);
    TraceRemainingCommit("asset d3d12-dred-page-fault bytes=0x", fault.PageFaultVA != 0, (DWORD)pageFault, NULL);
    if (SUCCEEDED(breadcrumbs)) {
        const D3D12_AUTO_BREADCRUMB_NODE *node = commands.pHeadAutoBreadcrumbNode;
        for (UINT index = 0; node && index < 64; ++index, node = node->pNext) {
            if (!node->pLastBreadcrumbValue || !node->pCommandHistory) continue;
            const UINT completed = *node->pLastBreadcrumbValue;
            if (completed >= node->BreadcrumbCount) continue;
            TraceRemainingCommit("asset d3d12-dred-first-incomplete bytes=0x",
                node->BreadcrumbCount - completed, (DWORD)node->pCommandHistory[completed], NULL);
        }
    }
    ID3D12DeviceRemovedExtendedData_Release(data);
}


static void TraceLiveThread(const char *, DWORD, DWORD, const void *);
static volatile LONG startupThread;
static volatile LONG graphicsCalls, computeCalls, pipelineOwner;
static volatile LONG renderWaits, renderWakes, renderTimeouts, renderWaitErrors;

static DWORD RenderThreadState(void) {
    struct { DWORD state, flags; UINT64 threadId; } info = {0};
    const BYTE *image = (const BYTE *)GetModuleHandleW(NULL);
    SIZE_T read = 0;
    // This trial is staged only for the verified Spider-Man executable build.
    if (!ReadProcessMemory(GetCurrentProcess(), image + 0x07A2FD68u, &info, sizeof(info), &read)
            || read != sizeof(info) || info.threadId > MAXDWORD) return 0;
    TraceLiveThread("render-state=0x", (DWORD)info.threadId, info.state, NULL);
    TraceLiveThread("render-flags=0x", (DWORD)info.threadId, info.flags, NULL);
    TraceLiveThread("render-waits=0x", (DWORD)InterlockedCompareExchange(&renderWaits, 0, 0),
        (DWORD)InterlockedCompareExchange(&renderWakes, 0, 0), NULL);
    TraceLiveThread("render-timeouts=0x", (DWORD)InterlockedCompareExchange(&renderTimeouts, 0, 0),
        (DWORD)InterlockedCompareExchange(&renderWaitErrors, 0, 0), NULL);
    return (DWORD)info.threadId;
}

static void TraceLiveThread(const char *kind, DWORD threadId, DWORD value, const void *address) {
    char line[165];
    char *out = Hex(Text(Text(line, "asset live-thread-"), kind), threadId, 8);
    out = Hex(Text(out, " detail=0x"), value, 8);
    if (address) out = RecordImageAddress(out, address);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

static BOOL ImageTextRange(ULONG_PTR *first, ULONG_PTR *last) {
    const BYTE *image = (const BYTE *)GetModuleHandleW(NULL);
    if (!image) return FALSE;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)image;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) return FALSE;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.NumberOfSections > 96) return FALSE;
    const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    for (UINT i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        *first = (ULONG_PTR)(image + section[i].VirtualAddress);
        *last = *first + section[i].Misc.VirtualSize;
        return *last > *first;
    }
    return FALSE;
}

static void SampleLiveThread(DWORD threadId, BOOL includeReturns) {
    const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_GET_CONTEXT |
        THREAD_SUSPEND_RESUME, FALSE, threadId);
    if (!thread) {
        TraceLiveThread("open-error=0x", threadId, GetLastError(), NULL);
        return;
    }
    if (GetProcessIdOfThread(thread) != GetCurrentProcessId()) {
        CloseHandle(thread);
        return;
    }
    CONTEXT context;
    context.Rip = 0;
    context.Rsp = 0;
    context.ContextFlags = CONTEXT_CONTROL;
    ULONG_PTR candidates[192];
    SIZE_T bytesRead = 0;
    DWORD error = 0;
    if (SuspendThread(thread) == (DWORD)-1) {
        error = GetLastError();
    } else {
        if (GetThreadContext(thread, &context)) {
            MEMORY_BASIC_INFORMATION region;
            if (includeReturns && context.Rsp && VirtualQuery((const void *)(ULONG_PTR)context.Rsp, &region, sizeof(region)) &&
                    region.State == MEM_COMMIT && !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                const ULONG_PTR end = (ULONG_PTR)region.BaseAddress + region.RegionSize;
                const SIZE_T available = end > (ULONG_PTR)context.Rsp ? end - (ULONG_PTR)context.Rsp : 0;
                const SIZE_T amount = available < sizeof(candidates) ? available : sizeof(candidates);
                if (!amount || !ReadProcessMemory(GetCurrentProcess(), (const void *)(ULONG_PTR)context.Rsp,
                        candidates, amount, &bytesRead)) bytesRead = 0;
            }
        } else {
            error = GetLastError();
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    if (error || !context.Rip) {
        TraceLiveThread("context-error=0x", threadId, error, NULL);
        return;
    }
    TraceLiveThread("instruction=0x", threadId, 0, (const void *)(ULONG_PTR)context.Rip);
    ULONG_PTR first, last;
    if (!bytesRead || !ImageTextRange(&first, &last)) return;
    DWORD reported = 0;
    for (SIZE_T i = 0; i < bytesRead / sizeof(candidates[0]) && reported < 14; ++i) {
        const ULONG_PTR candidate = candidates[i];
        if (candidate < first + 6 || candidate >= last) continue;
        const BYTE *instruction = (const BYTE *)candidate;
        if (instruction[-5] != 0xE8 && !(instruction[-6] == 0xFF && instruction[-5] == 0x15)
                && !(instruction[-3] == 0xFF && (instruction[-2] & 0x38) == 0x10)) continue;
        TraceLiveThread("return-candidate=0x", threadId, (DWORD)(i * sizeof(candidates[0])), instruction);
        ++reported;
    }
}

static BOOL LegacyXAudioClass(REFCLSID clsid) {
    static const GUID legacy = {0x5A508685, 0xA254, 0x4FBA, {0x9B,0x82,0x9A,0x24,0xB0,0x03,0x06,0xAF}};
    if (!clsid || clsid->Data1 != legacy.Data1 || clsid->Data2 != legacy.Data2 || clsid->Data3 != legacy.Data3)
        return FALSE;
    for (UINT index = 0; index < ARRAYSIZE(legacy.Data4); ++index)
        if (clsid->Data4[index] != legacy.Data4[index]) return FALSE;
    return TRUE;
}

static HRESULT CreatePackagedAudio(REFCLSID clsid, REFIID iid, void **object) {
    typedef HRESULT (WINAPI *GetClassObject)(REFCLSID, REFIID, LPVOID *);
    static const GUID classFactory = {1, 0, 0, {0xC0,0,0,0,0,0,0,0x46}};
    HMODULE audio = NULL;
    if (!GetModuleHandleExW(0, L"XAudio2_7.dll", &audio)) return REGDB_E_CLASSNOTREG;
    GetClassObject getFactory = SameName(ModuleLocation(audio), "adjacent") ?
        (GetClassObject)(void *)GetProcAddress(audio, "DllGetClassObject") : NULL;
    IClassFactory *factory = NULL;
    HRESULT result = getFactory ? getFactory(clsid, &classFactory, (void **)&factory) : REGDB_E_CLASSNOTREG;
    TraceRemainingCommit("asset xaudio27-class-factory bytes=0x", 0, (DWORD)result, NULL);
    if (SUCCEEDED(result) && !factory) result = E_UNEXPECTED;
    if (SUCCEEDED(result)) {
        HMODULE pinned = NULL;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                (LPCWSTR)(const void *)getFactory, &pinned)) {
            result = HRESULT_FROM_WIN32(GetLastError());
        } else {
            result = IClassFactory_CreateInstance(factory, NULL, iid, object);
        }
    }
    if (factory) IClassFactory_Release(factory);
    FreeLibrary(audio);
    TraceRemainingCommit("asset xaudio27-create-instance bytes=0x", SUCCEEDED(result) && *object != NULL,
        (DWORD)result, NULL);
    return result;
}

HRESULT WINAPI XboxTraceCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID *object) {
    HRESULT result = CoCreateInstance(clsid, outer, context, iid, object);
    const DWORD saved = GetLastError();
    static volatile LONG calls;
    if (InterlockedIncrement(&calls) <= 48) {
        char line[160];
        char *out = Hex(Text(line, "asset com-create class=0x"), clsid ? clsid->Data1 : 0, 8);
        out = Hex(Text(out, " interface=0x"), iid ? iid->Data1 : 0, 8);
        out = Hex(Text(out, " context=0x"), context, 8);
        out = Hex(Text(out, " result=0x"), (DWORD)result, 8);
        out = RecordImageAddress(out, __builtin_return_address(0));
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
    }
    if (result == REGDB_E_CLASSNOTREG && !outer && iid && object &&
            context == CLSCTX_INPROC_SERVER && LegacyXAudioClass(clsid)) {
        result = CreatePackagedAudio(clsid, iid, object);
    }
    SetLastError(saved);
    return result;
}

HRESULT WINAPI XboxTraceCoInitializeEx(LPVOID reserved, DWORD flags) {
    const HRESULT result = CoInitializeEx(reserved, flags);
    const DWORD saved = GetLastError();
    static volatile LONG calls;
    if (InterlockedIncrement(&calls) <= 24) {
        char line[138];
        char *out = Hex(Text(line, "asset com-apartment flags=0x"), flags, 8);
        out = Hex(Text(out, " result=0x"), (DWORD)result, 8);
        out = RecordImageAddress(out, __builtin_return_address(0));
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
    }
    SetLastError(saved);
    return result;
}

static void TraceSoundBank(const char *kind, ULONG_PTR bank) {
    BYTE data[0x64];
    if (!bank || !ReadProcessMemory(GetCurrentProcess(), (const void *)bank, data, sizeof(data), NULL)) {
        TraceLiveThread(kind, MAXDWORD, 0, NULL);
        return;
    }
    DWORD bytes = 0;
    __builtin_memcpy_inline(&bytes, data + 0x60, sizeof(bytes));
    DWORD flags = ((DWORD)data[0] << 16) | ((DWORD)data[0x42] << 8) | data[0x43];
    TraceLiveThread(kind, flags, bytes, NULL);
}

static void SampleMenuZoneLoad(const BYTE *image, const BYTE *zone) {
    BYTE header[0x38];
    if (!ReadProcessMemory(GetCurrentProcess(), zone, header, sizeof(header), NULL)) return;
    WORD refs = 0;
    ULONG_PTR payload = 0;
    __builtin_memcpy_inline(&refs, header + 4, sizeof(refs));
    __builtin_memcpy_inline(&payload, header + 0x30, sizeof(payload));
    const DWORD status = ((DWORD)header[2] << 24) | ((DWORD)header[3] << 16) | refs;
    TraceLiveThread("menu-zone-asset-state=0x", header[0], status, NULL);
    DWORD magic = MAXDWORD;
    if (payload)
        ReadProcessMemory(GetCurrentProcess(), (const void *)payload, &magic, sizeof(magic), NULL);
    TraceLiveThread("menu-zone-payload=0x", magic, payload != 0, NULL);

    WORD queue[2] = {0xffff, 0xffff};
    BYTE activation = 0xff;
    if (ReadProcessMemory(GetCurrentProcess(), zone + 0x2D4u, queue, sizeof(queue), NULL) &&
            ReadProcessMemory(GetCurrentProcess(), zone + 0x306u, &activation, sizeof(activation), NULL))
        TraceLiveThread("menu-zone-queues=0x", ((DWORD)queue[0] << 16) | queue[1], activation, NULL);

    const BYTE *manager = 0;
    const BYTE *entries = 0;
    ULONG_PTR stride = 0;
    DWORD limit = 0, epoch = MAXDWORD;
    const DWORD *requested = 0;
    const unsigned type = header[2] & 0x7f;
    if (!ReadProcessMemory(GetCurrentProcess(), image + 0x06042E70u, &epoch, sizeof(epoch), NULL) ||
            !ReadProcessMemory(GetCurrentProcess(), image + 0x074227D0u + type * sizeof(manager),
                               &manager, sizeof(manager), NULL) || !manager ||
            !ReadProcessMemory(GetCurrentProcess(), manager + 0x40u, &entries, sizeof(entries), NULL) ||
            !ReadProcessMemory(GetCurrentProcess(), manager + 0x48u, &stride, sizeof(stride), NULL) ||
            !ReadProcessMemory(GetCurrentProcess(), manager + 0x70u, &limit, sizeof(limit), NULL) ||
            !ReadProcessMemory(GetCurrentProcess(), manager + 0xA0u, &requested, sizeof(requested), NULL) ||
            !entries || !requested || stride != 0x388u || (ULONG_PTR)zone < (ULONG_PTR)entries) return;
    const ULONG_PTR offset = (ULONG_PTR)zone - (ULONG_PTR)entries;
    if (offset % stride || offset / stride > limit) return;
    DWORD required = MAXDWORD;
    if (ReadProcessMemory(GetCurrentProcess(), requested + offset / stride, &required, sizeof(required), NULL))
        TraceLiveThread("menu-zone-loader-epoch=0x", epoch, required, NULL);
}

static void SampleLobbyMode(const BYTE *image) {
    ULONG_PTR vtable = 0;
    DWORD modes[3] = {MAXDWORD, MAXDWORD, MAXDWORD};
    if (!ReadProcessMemory(GetCurrentProcess(), image + 0x06C64B90u, &vtable, sizeof(vtable), NULL) ||
            vtable != (ULONG_PTR)(image + 0x0382D1A8u)) return;
    if (!ReadProcessMemory(GetCurrentProcess(), image + 0x06C65478u, modes, sizeof(modes), NULL)) return;
    TraceLiveThread("mode-current=0x", modes[0], modes[1], NULL);
    if (modes[0] != 2) return;

    const BYTE *lobby = image + 0x06C65518u;
    if (!ReadProcessMemory(GetCurrentProcess(), lobby, &vtable, sizeof(vtable), NULL) ||
            vtable != (ULONG_PTR)(image + 0x0382CFA8u)) return;

    DWORD phase = MAXDWORD, transition = MAXDWORD, pending = MAXDWORD, saveOverlay = MAXDWORD;
    BYTE readiness[8] = {0};
    ReadProcessMemory(GetCurrentProcess(), lobby + 0x8u, &phase, sizeof(phase), NULL);
    ReadProcessMemory(GetCurrentProcess(), lobby + 0x10u, &transition, sizeof(transition), NULL);
    ReadProcessMemory(GetCurrentProcess(), lobby + 0x194u, &pending, sizeof(pending), NULL);
    ReadProcessMemory(GetCurrentProcess(), lobby + 0x200u, &saveOverlay, sizeof(saveOverlay), NULL);
    for (UINT index = 0; index < ARRAYSIZE(readiness); ++index)
        ReadProcessMemory(GetCurrentProcess(), image + 0x06057D4Cu + index * 40u,
            &readiness[index], 1, NULL);
    DWORD taskState[2];
    __builtin_memcpy_inline(taskState, readiness, sizeof(taskState));
    TraceLiveThread("lobby-phase=0x", phase, transition, NULL);
    DWORD frontendCount = MAXDWORD, menuState = MAXDWORD;
    BYTE frontendActive = 0;
    ReadProcessMemory(GetCurrentProcess(), image + 0x06D93C64u, &frontendCount, sizeof(frontendCount), NULL);
    ReadProcessMemory(GetCurrentProcess(), image + 0x06D93C68u, &frontendActive, sizeof(frontendActive), NULL);
    ReadProcessMemory(GetCurrentProcess(), lobby + 0x14u, &menuState, sizeof(menuState), NULL);
    TraceLiveThread("lobby-menu-state=0x", menuState, 0, NULL);
    TraceLiveThread("lobby-frontend-count=0x", frontendCount, frontendActive, NULL);
    TraceRemainingCommit("asset lobby-commit bytes=0x", 0, phase, NULL);

    TraceLiveThread("lobby-request=0x", pending, saveOverlay, NULL);
    TraceLiveThread("lobby-readiness=0x", taskState[0], taskState[1], NULL);
    DWORD lobbyFlags = 0, selectionFlags = 0;
    DWORD userSlots[6] = {MAXDWORD, MAXDWORD, MAXDWORD, MAXDWORD, MAXDWORD, MAXDWORD};
    DWORD selected[2] = {MAXDWORD, MAXDWORD};
    DWORD flow[2] = {MAXDWORD, MAXDWORD};
    DWORD occupied = 0;
    BYTE drawEnabled = 0, frontendState = 0, loadGate = 0;
    ULONG_PTR flowAddress = 0;
    ReadProcessMemory(GetCurrentProcess(), lobby + 0x0Cu, &lobbyFlags, sizeof(lobbyFlags), NULL);
    ReadProcessMemory(GetCurrentProcess(), lobby + 0x1F4u, &selectionFlags, sizeof(selectionFlags), NULL);
    ReadProcessMemory(GetCurrentProcess(), lobby + 0x1Cu, userSlots, sizeof(userSlots), NULL);
    ReadProcessMemory(GetCurrentProcess(), lobby + 0x150u, selected, sizeof(selected), NULL);
    for (UINT index = 0; index < ARRAYSIZE(userSlots); ++index)
        if (userSlots[index] != MAXDWORD) occupied |= 1u << index;
    ReadProcessMemory(GetCurrentProcess(), image + 0x077FD2B0u, &flowAddress, sizeof(flowAddress), NULL);
    if (flowAddress) {
        ReadProcessMemory(GetCurrentProcess(), (const BYTE *)flowAddress + 0x08u, &flow[0], sizeof(flow[0]), NULL);
        ReadProcessMemory(GetCurrentProcess(), (const BYTE *)flowAddress + 0x10u, &flow[1], sizeof(flow[1]), NULL);
    }
    ReadProcessMemory(GetCurrentProcess(), image + 0x07A88AD0u, &drawEnabled, sizeof(drawEnabled), NULL);
    ReadProcessMemory(GetCurrentProcess(), image + 0x05D9665Cu, &frontendState, sizeof(frontendState), NULL);
    ReadProcessMemory(GetCurrentProcess(), image + 0x05D96E15u, &loadGate, sizeof(loadGate), NULL);
    TraceLiveThread("lobby-display-flags=0x", lobbyFlags, drawEnabled, NULL);
    TraceLiveThread("lobby-selection-flags=0x", selectionFlags, occupied, NULL);
    TraceLiveThread("lobby-selected-slots=0x", selected[0], selected[1], NULL);
    TraceLiveThread("lobby-flow=0x", flow[0], flow[1], NULL);
    TraceLiveThread("lobby-view-state=0x", frontendState, loadGate, NULL);
    // ModeLobby submits Main_Menu_Cine_01.zone through this context.
    ULONG_PTR menuZone = 0;
    DWORD zoneFlags = MAXDWORD, sceneParts = MAXDWORD;
    ReadProcessMemory(GetCurrentProcess(), lobby + 0x240u, &menuZone, sizeof(menuZone), NULL);
    ReadProcessMemory(GetCurrentProcess(), lobby + 0x248u, &sceneParts, sizeof(sceneParts), NULL);
    if (menuZone)
        ReadProcessMemory(GetCurrentProcess(), (const BYTE *)menuZone + 0x2B8u, &zoneFlags, sizeof(zoneFlags), NULL);
    TraceLiveThread("menu-zone-flags=0x", zoneFlags, sceneParts, NULL);
    if (menuZone && zoneFlags != MAXDWORD) SampleMenuZoneLoad(image, (const BYTE *)menuZone);



    ULONG_PTR saves = 0, users = 0;
    DWORD count = 0;
    ReadProcessMemory(GetCurrentProcess(), image + 0x07A420D8u, &saves, sizeof(saves), NULL);
    if (saves) {
        ReadProcessMemory(GetCurrentProcess(), (const BYTE *)saves + 0x18u, &users, sizeof(users), NULL);
        ReadProcessMemory(GetCurrentProcess(), (const BYTE *)saves + 0x20u, &count, sizeof(count), NULL);
    }
    TraceLiveThread("lobby-save-count=0x", count, users != 0, NULL);
    for (DWORD index = 0; users && index < count && index < 4; ++index) {
        const BYTE *record = (const BYTE *)users + index * 0x258u;
        DWORD state = MAXDWORD;
        ReadProcessMemory(GetCurrentProcess(), record + 0x138u, &state, sizeof(state), NULL);
        TraceLiveThread("lobby-save-state=0x", index, state, NULL);
    }
}

static void SampleBootMode(void) {
    const BYTE *image = (const BYTE *)GetModuleHandleW(NULL);
    struct { ULONG_PTR vtable; DWORD flags, phase; } boot = {0};
    DWORD modeSlot = MAXDWORD, movieState = MAXDWORD;
    SIZE_T got;
    // Read only the fields identified in the build-pinned source image.
    if (!ReadProcessMemory(GetCurrentProcess(), image + 0x06C654A0u, &boot, sizeof(boot), &got) ||
            got != sizeof(boot) || boot.vtable != (ULONG_PTR)(image + 0x0382CED8u)) return;
    ReadProcessMemory(GetCurrentProcess(), image + 0x06C65480u, &modeSlot, sizeof(modeSlot), NULL);
    ReadProcessMemory(GetCurrentProcess(), image + 0x07085960u, &movieState, sizeof(movieState), NULL);
    SampleLobbyMode(image);
    TraceLiveThread("mode-boot-phase=0x", boot.phase, boot.flags & 0xFFu, NULL);
    TraceLiveThread("mode-boot-slot=0x", modeSlot, movieState, NULL);
    ULONG_PTR banks[2] = {0};
    DWORD inWwise = MAXDWORD, queued = MAXDWORD, managerFlags = MAXDWORD;
    ReadProcessMemory(GetCurrentProcess(), image + 0x060A4210u, banks, sizeof(banks), NULL);
    ReadProcessMemory(GetCurrentProcess(), image + 0x060A4220u, &inWwise, sizeof(inWwise), NULL);
    ReadProcessMemory(GetCurrentProcess(), image + 0x060A427Cu, &queued, sizeof(queued), NULL);
    ReadProcessMemory(GetCurrentProcess(), image + 0x060A3160u, &managerFlags, sizeof(managerFlags), NULL);
    TraceSoundBank("init-bank-active=0x", banks[0]);
    TraceSoundBank("init-bank-prospective=0x", banks[1]);
    TraceLiveThread("init-bank-queue=0x", queued, inWwise, NULL);
    TraceLiveThread("sound-manager-flags=0x", managerFlags, 0, NULL);
}

static DWORD WINAPI WatchLobbyMode(void *unused) {
    (void)unused;
    const BYTE *image = (const BYTE *)GetModuleHandleW(NULL);
    DWORD peak = 0;
    for (DWORD sample = 0; sample < 215; ++sample) {
        SleepEx(1000, FALSE);
        if (!(sample & 1) && sample < 90) SampleLobbyMode(image);
        MEMORYSTATUSEX memory = {0};
        memory.dwLength = sizeof(memory);
        if (!GlobalMemoryStatusEx(&memory) || memory.ullAvailPageFile > memory.ullTotalPageFile)
            continue;
        const DWORD committed = (DWORD)((memory.ullTotalPageFile - memory.ullAvailPageFile) >> 10);
        if (committed > peak) peak = committed;
        if (peak != committed && (sample % 5)) continue;
        char line[151];
        char *out = Hex(Text(line, "asset sampled-commit-kib=0x"), committed, 8);
        out = Hex(Text(out, " peak-kib=0x"), peak, 8);
        out = Hex(Text(out, " limit-kib=0x"), (DWORD)(memory.ullTotalPageFile >> 10), 8);
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
    }
    return 0;
}

#include "census.h"
#include "d3d12-small-array-copy.h"
static void SacTrace(const char *event, unsigned long long a, unsigned long long b) {
    char line[120], *out = Text(Text(Text(line, "asset "), event), " n=");
    out = CensusDec(out, a);
    out = CensusDec(Text(out, " value="), b);
    CensusEmit(line, out);
}
#include "allocs.h"
#ifdef HEAP_ADDR
#include "heapaddr.h"
#endif

static DWORD WINAPI WatchLiveStartup(void *unused) {
    (void)unused;
    const HANDLE lobby = CreateThread(NULL, 0, WatchLobbyMode, NULL, 0, NULL);
    if (lobby) CloseHandle(lobby);
    const DWORD mainThread = (DWORD)startupThread;
    for (DWORD second = 0; second < 10; ++second) {
        SleepEx(1000, FALSE);
        SampleBootMode();
    }
    const DWORD delays[] = {2000, 15000, 25000, 30000, 60000, 60000, 60000, 60000, 60000,
        60000, 60000, 60000, 60000, 60000, 60000, 60000, 60000, 60000, 60000, 60000};
    for (UINT sample = 0; sample < ARRAYSIZE(delays); ++sample) {
        SleepEx(delays[sample], FALSE);
        SampleBootMode();
        TraceLiveThread("sample=0x", mainThread, sample + 1, NULL);
        SampleLiveThread(mainThread, TRUE);
        const DWORD renderThread = RenderThreadState();
        if (renderThread && renderThread != mainThread) SampleLiveThread(renderThread, TRUE);
        const DWORD owner = (DWORD)InterlockedCompareExchange(&pipelineOwner, 0, 0);
        const LONG graphics = InterlockedCompareExchange(&graphicsCalls, 0, 0);
        const LONG compute = InterlockedCompareExchange(&computeCalls, 0, 0);
        TraceRemainingCommit("asset live-pipeline-graphics bytes=0x", (SIZE_T)graphics, owner, NULL);
        TraceRemainingCommit("asset live-pipeline-compute bytes=0x", (SIZE_T)compute, owner, NULL);
        if (owner && owner != mainThread && owner != renderThread) SampleLiveThread(owner, TRUE);
        if (sample != 1 && sample != 3 && sample != 11 && sample != 13 && sample != 15) continue;
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            TraceLiveThread("list-error=0x", mainThread, GetLastError(), NULL);
            continue;
        }
        THREADENTRY32 entry;
        entry.dwSize = sizeof(entry);
        DWORD threads = 0;
        if (Thread32First(snapshot, &entry)) do {
            if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != mainThread &&
                    entry.th32ThreadID != GetCurrentThreadId()) {
                SampleLiveThread(entry.th32ThreadID, FALSE);
                if (++threads == 36) break;
            }
            entry.dwSize = sizeof(entry);
        } while (Thread32Next(snapshot, &entry));
        CloseHandle(snapshot);
    }
    return 0;
}


static void TraceBootWait(const char *name, const void *caller, DWORD timeout, DWORD result, BOOL entered) {
    if ((DWORD)startupThread != GetCurrentThreadId() || timeout == 0) return;
    static volatile LONG traceCount;
    if (InterlockedIncrement(&traceCount) > 80) return;
    const DWORD saved = GetLastError();
    char line[168];
    char *out = Text(Text(line, "asset boot-wait-"), name);
    out = Hex(Text(out, " timeout=0x"), timeout, 8);
    if (!entered) out = Hex(Text(out, " result=0x"), result, 8);
    else out = Text(out, " enter");
    out = RecordImageAddress(out, caller);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
    SetLastError(saved);
}

DWORD WINAPI XboxTraceWaitForSingleObject(HANDLE handle, DWORD timeout) {
    const void *caller = __builtin_return_address(0);
    TraceBootWait("single", caller, timeout, 0, TRUE);
    DWORD result = WaitForSingleObject(handle, timeout);
    TraceBootWait("single", caller, timeout, result, FALSE);
    return result;
}

DWORD WINAPI XboxTraceWaitForSingleObjectEx(HANDLE handle, DWORD timeout, BOOL alertable) {
    const void *caller = __builtin_return_address(0);
    TraceBootWait("single-ex", caller, timeout, 0, TRUE);
    DWORD result = WaitForSingleObjectEx(handle, timeout, alertable);
    TraceBootWait("single-ex", caller, timeout, result, FALSE);
    return result;
}

DWORD WINAPI XboxTraceWaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL all, DWORD timeout) {
    const void *caller = __builtin_return_address(0);
    TraceBootWait("multiple", caller, timeout, 0, TRUE);
    DWORD result = WaitForMultipleObjects(count, handles, all, timeout);
    TraceBootWait("multiple", caller, timeout, result, FALSE);
    return result;
}

DWORD WINAPI XboxTraceWaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, BOOL all, DWORD timeout, BOOL alertable) {
    const void *caller = __builtin_return_address(0);
    TraceBootWait("multiple-ex", caller, timeout, 0, TRUE);
    DWORD result = WaitForMultipleObjectsEx(count, handles, all, timeout, alertable);
    TraceBootWait("multiple-ex", caller, timeout, result, FALSE);
    return result;
}

BOOL WINAPI XboxTraceSleepConditionVariableSRW(PCONDITION_VARIABLE cond, PSRWLOCK lock, DWORD timeout, ULONG flags) {
    const void *caller = __builtin_return_address(0);
    TraceBootWait("condition-srw", caller, timeout, 0, TRUE);
    BOOL result = SleepConditionVariableSRW(cond, lock, timeout, flags);
    TraceBootWait("condition-srw", caller, timeout, result, FALSE);
    return result;
}

BOOL WINAPI XboxTraceSleepConditionVariableCS(PCONDITION_VARIABLE cond, PCRITICAL_SECTION lock, DWORD timeout) {
    const void *caller = __builtin_return_address(0);
    TraceBootWait("condition-cs", caller, timeout, 0, TRUE);
    const DWORD incoming = GetLastError();
    const ULONG_PTR image = (ULONG_PTR)GetModuleHandleW(NULL);
    const BOOL render = (ULONG_PTR)caller - image == 0x01BBF3FAu &&
        (ULONG_PTR)cond - image == 0x07A2FDB0u && (DWORD)startupThread == GetCurrentThreadId();
    if (render) InterlockedIncrement(&renderWaits);
    SetLastError(incoming);
    BOOL result = SleepConditionVariableCS(cond, lock, timeout);
    const DWORD error = GetLastError();
    if (render) InterlockedIncrement(result ? &renderWakes :
        error == ERROR_TIMEOUT ? &renderTimeouts : &renderWaitErrors);
    TraceBootWait("condition-cs", caller, timeout, result, FALSE);
    SetLastError(error);
    return result;
}

static void TraceShellRequest(const char *call, BOOL result, DWORD error, const void *caller) {
    char line[168];
    char *out = Text(Text(line, "asset shell-request "), call);
    out = Hex(Text(out, " result=0x"), result, 1);
    out = Hex(Text(out, " error=0x"), error, 8);
    out = RecordImageAddress(out, caller);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

BOOL WINAPI XboxTraceShellExecuteExW(SHELLEXECUTEINFOW *request) {
    typedef BOOL (WINAPI *ShellExecuteEx)(SHELLEXECUTEINFOW*);
    HMODULE shell = GetModuleHandleW(L"shell32.dll");
    const ShellExecuteEx execute = shell ? (ShellExecuteEx)(void *)GetProcAddress(shell, "ShellExecuteExW") : NULL;
    if (!execute) SetLastError(ERROR_PROC_NOT_FOUND);
    const BOOL result = execute ? execute(request) : FALSE;
    const DWORD error = GetLastError();
    TraceShellRequest("extended", result, error, __builtin_return_address(0));
    SetLastError(error);
    return result;
}

HINSTANCE WINAPI XboxTraceShellExecuteW(HWND window, LPCWSTR verb, LPCWSTR file, LPCWSTR args,
        LPCWSTR directory, INT show) {
    typedef HINSTANCE (WINAPI *Execute)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
    HMODULE shell = GetModuleHandleW(L"shell32.dll");
    const Execute execute = shell ? (Execute)(void *)GetProcAddress(shell, "ShellExecuteW") : NULL;
    if (!execute) SetLastError(ERROR_PROC_NOT_FOUND);
    const HINSTANCE result = execute ? execute(window, verb, file, args, directory, show) : (HINSTANCE)SE_ERR_NOASSOC;
    const DWORD error = GetLastError();
    const BOOL ok = (ULONG_PTR)result > 32;
    TraceShellRequest("basic", ok, ok ? 0 : error, __builtin_return_address(0));
    SetLastError(error);
    return result;
}

#include "cpu-sets.h"
#include "d3d12-pipeline-replay.h"
#include "hooks.h"
#include "placer.h"
#include "perfsec.h"

#include "d3d12-compile-slots.h"

/* The compile pool (d3d12-compile-slots.h). PSO_SLOTS bounds concurrent compiles: one at a time
   was tried after shader-compiler faults with 6-24 MB of commit left (see the note). */
static SRWLOCK pipelineReplaceLock = SRWLOCK_INIT;

static void PipelineSlots(void) {
    static volatile LONG configured;
    if (InterlockedCompareExchange(&configured, 1, 0)) return;
    XboxCompileSlotsConfigure(PSO_SLOTS, PSO_BOOST_SLOTS, PSO_BOOST_MIN_AVAIL_MIB,
        PSO_PRIORITY ? PSO_URGENT_MIN_AVAIL_MIB : 0xFFFFFFFFu, AppendTrace);
    XboxCompileSlotsReserveBackground(PSO_BG_RESERVE);
    char line[220], *out = CensusDec(Text(line, "asset pso-parallel slots="), PSO_SLOTS);
    out = CensusDec(Text(out, " boost="), PSO_BOOST_SLOTS);
    out = CensusDec(Text(out, " boost-min-mib="), PSO_BOOST_MIN_AVAIL_MIB);
    out = CensusDec(Text(out, " priority="), PSO_PRIORITY);
    out = CensusDec(Text(out, " replay-threads="), PSO_REPLAY_THREADS);
    out = CensusDec(Text(out, " bg-reserve="), PSO_BG_RESERVE);
    out = CensusDec(Text(out, " skip-warmup="), PSO_SKIP_WARMUP);
    out = CensusDec(Text(out, " warmup-list="), PSO_WARMUP_LIST);
    CensusEmit(line, out);
}

/* The thread whose compiles stall frames: the render thread (build-matched address, as in
   RenderThreadState) or the boot thread. */
static int PipelineClass(void) {
    if (!PSO_PRIORITY) return XboxCompileNormal;
    static volatile LONG renderThread;
    DWORD render = (DWORD)renderThread;
    if (!render) {
        struct { DWORD state, flags; UINT64 threadId; } info = {0};
        SIZE_T read = 0;
        if (ReadProcessMemory(GetCurrentProcess(), (const BYTE *)GetModuleHandleW(NULL) + 0x07A2FD68u, &info,
                sizeof(info), &read) && read == sizeof(info) && info.threadId <= MAXDWORD) {
            render = (DWORD)info.threadId;
            if (render) InterlockedExchange(&renderThread, (LONG)render);
        }
    }
    const DWORD self = GetCurrentThreadId();
    return self == render || self == (DWORD)startupThread ? XboxCompileUrgent : XboxCompileNormal;
}

/* A replay thread's admission, before it claims an entry. Each replay thread keeps its own ticket. */
static void ReplaySlot(BOOL enter) {
    static struct { volatile LONG owner; XboxCompileSlotTicket ticket; } tickets[16];
    const LONG self = (LONG)GetCurrentThreadId();
    UINT i = 0;
    while (i < ARRAYSIZE(tickets) && tickets[i].owner != self) ++i;
    if (i == ARRAYSIZE(tickets))
        for (i = 0; i < ARRAYSIZE(tickets) && InterlockedCompareExchange(&tickets[i].owner, self, 0); ++i) {}
    if (i == ARRAYSIZE(tickets)) i = 0; /* more threads than tickets: XboxPipelineReplaySetThreads caps at 16 */
    if (enter) XboxCompileSlotsEnter(PSO_PRIORITY ? XboxCompileBackground : XboxCompileNormal, &tickets[i].ticket);
    else XboxCompileSlotsLeave(&tickets[i].ticket);
}

/* The replay thread's compile; ReplaySlot already admitted it. */
static HRESULT ReplayCompile(ID3D12Device *device, BOOL graphics, const void *desc, REFIID iid, void **object) {
    PipelineSlots();
    LARGE_INTEGER frequency, before, after;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&before);
    const HRESULT result = graphics ?
        ID3D12Device_CreateGraphicsPipelineState(device, (const D3D12_GRAPHICS_PIPELINE_STATE_DESC *)desc, iid, object) :
        ID3D12Device_CreateComputePipelineState(device, (const D3D12_COMPUTE_PIPELINE_STATE_DESC *)desc, iid, object);
    QueryPerformanceCounter(&after);
    const unsigned long long ms = CensusMicros(before, after, frequency) / 1000;
    static volatile LONG replayLines;
    if (ms >= 30 && InterlockedIncrement(&replayLines) <= 4000) {
        char line[160], *out = Text(line, "asset pso-replay-compile ms=");
        out = CensusDec(out, ms);
        const UINT64 key = PerfShaderKey(graphics, desc);
        out = Hex(Text(out, " sh=0x"), (unsigned long)(key >> 32), 8);
        out = Hex(out, (unsigned long)key, 8);
        out = CensusDec(Text(out, " qpc="), (unsigned long long)after.QuadPart);
        CensusEmit(line, out);
    }
    return result;
}

static void ReplayInstall(ID3D12Device *device) {
    WCHAR path[1024];
    if (!XboxPipelineReplayDefaultPath(path, ARRAYSIZE(path))) {
        char line[64], *out = Text(line, "asset pso-replay open status=no-path");
        CensusEmit(line, out);
        return;
    }
    PipelineSlots();
    XboxPipelineReplaySetSlot(ReplaySlot);
    XboxPipelineReplaySetThreads(PSO_REPLAY_THREADS);
    XboxPipelineReplayInstall(device, path, ReplayCompile, FALSE, AppendTrace);
}

#ifdef PSO_BLOB_PROBE
/* Late, one-shot: does this device return cached blobs or create a pipeline library? */
static ID3D12PipelineState *volatile probePipeline;
static DWORD WINAPI CacheBlobProbe(void *unused) {
    (void)unused;
    Sleep(PSO_BLOB_PROBE * 1000u);
    ID3D12PipelineState *pipeline = (ID3D12PipelineState *)InterlockedCompareExchangePointer(
        (PVOID volatile *)&probePipeline, NULL, NULL);
    ID3D12Device *device = xprState.device;
    if (!pipeline || !device) return 0;
    LARGE_INTEGER frequency, a, b;
    QueryPerformanceFrequency(&frequency);
    ID3DBlob *blob = NULL;
    QueryPerformanceCounter(&a);
    const HRESULT cached = ID3D12PipelineState_GetCachedBlob(pipeline, &blob);
    QueryPerformanceCounter(&b);
    const SIZE_T blobSize = blob ? ID3D10Blob_GetBufferSize(blob) : 0;
    const HRESULT removedAfterBlob = ID3D12Device_GetDeviceRemovedReason(device);
    static const IID device1Iid = {0x77ACCE80, 0x638E, 0x4E65, {0x88,0x95,0xC1,0xF2,0x33,0x86,0x86,0x3E}};
    static const IID libraryIid = {0xC64226A8, 0x9201, 0x46AF, {0xB4,0xCC,0x53,0xFB,0x9F,0xF7,0x41,0x4F}};
    ID3D12Device1 *device1 = NULL;
    HRESULT library = E_NOINTERFACE;
    void *created = NULL;
    if (SUCCEEDED(ID3D12Device_QueryInterface(device, &device1Iid, (void **)&device1)) && device1) {
        library = ID3D12Device1_CreatePipelineLibrary(device1, NULL, 0, &libraryIid, &created);
        ID3D12Device1_Release(device1);
    }
    const HRESULT removedAfterLibrary = ID3D12Device_GetDeviceRemovedReason(device);
    char line[300], *out = Text(line, "asset pso-cache-probe getcachedblob=0x");
    out = Hex(out, (unsigned long)cached, 8);
    out = CensusDec(Text(out, " blob-bytes="), (unsigned long long)blobSize);
    out = CensusDec(Text(out, " blob-us="), CensusMicros(a, b, frequency));
    out = Hex(Text(out, " removed-after-blob=0x"), (unsigned long)removedAfterBlob, 8);
    out = Hex(Text(out, " library=0x"), (unsigned long)library, 8);
    out = Hex(Text(out, " removed-after-library=0x"), (unsigned long)removedAfterLibrary, 8);
    CensusEmit(line, out);
    if (blob) ID3D10Blob_Release(blob);
    if (created) ((IUnknown *)created)->lpVtbl->Release((IUnknown *)created);
    return 0;
}
#endif

UINT64 WINAPI XboxGetStartupInfoW(LPSTARTUPINFOW startup, const D3D12_HEAP_DESC *description,
        const IID *iid, void **heap, UINT32 audioFlags, UINT32 audioDevice, const void *effectChain) {
    const ULONG_PTR image = (ULONG_PTR)GetModuleHandleW(NULL);
    const ULONG_PTR caller = (ULONG_PTR)__builtin_return_address(0) - image;
    if (caller == 0x01C99236u) {
        typedef HRESULT (WINAPI *Initialize)(void *, UINT32, UINT32);
        const Initialize initialize = (Initialize)(*(void ***)startup)[5];
        const HRESULT result = initialize(startup, 0, MAXDWORD);
        TraceRemainingCommit("asset xaudio27-initialize bytes=0x", 0, (DWORD)result, __builtin_return_address(0));
        return (DWORD)result;
    }
    if (caller == 0x01C9926Du) {
        typedef HRESULT (WINAPI *GetCount)(void *, UINT32 *);
        const GetCount getCount = (GetCount)(*(void ***)startup)[3];
        UINT32 *count = (UINT32 *)description;
        const HRESULT result = getCount(startup, count);
        TraceRemainingCommit("asset xaudio27-get-device-count bytes=0x", SUCCEEDED(result) ? *count : 0,
            (DWORD)result, __builtin_return_address(0));
        return (DWORD)result;
    }
    if (caller == 0x01C992E0u) {
        typedef HRESULT (WINAPI *GetDetails)(void *, UINT32, void *);
        const GetDetails getDetails = (GetDetails)(*(void ***)startup)[4];
        const UINT32 device = (UINT32)(ULONG_PTR)description;
        const HRESULT result = getDetails(startup, device, (void *)iid);
        TraceRemainingCommit("asset xaudio27-get-device-details bytes=0x", device, (DWORD)result,
            __builtin_return_address(0));
        return (DWORD)result;
    }
    if (caller == 0x01C9938Bu) {
        typedef HRESULT (WINAPI *CreateMaster)(void *, void **, UINT32, UINT32, UINT32, UINT32, const void *);
        const CreateMaster create = (CreateMaster)(*(void ***)startup)[10];
        const HRESULT result = create(startup, (void **)description, 0, 0, audioFlags, audioDevice, effectChain);
        if (SUCCEEDED(result) && description) PerfXAudioCreated(startup, *(void **)description);
        TraceRemainingCommit("asset xaudio27-create-mastering-voice bytes=0x", audioDevice, (DWORD)result,
            __builtin_return_address(0));
        return (DWORD)result;
    }
    if (caller == 0x018CE2D2u || caller == 0x018CE491u || caller == 0x018CE776u) {
        ID3D12Device *device = (ID3D12Device *)startup;
        const BOOL graphics = caller == 0x018CE2D2u;
        PipelineSlots();
        const DWORD incomingError = GetLastError();
        LARGE_INTEGER psoFrequency, psoRequested, psoLocked, psoDone;
        QueryPerformanceFrequency(&psoFrequency);
        QueryPerformanceCounter(&psoRequested);
        D3D12_COMPUTE_PIPELINE_STATE_DESC compute;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsDesc;
        D3D12_SHADER_BYTECODE shader;
        BOOL replaced = FALSE;
        /* The replacements load their outputs lazily; they used to run under the compile slot. */
        AcquireSRWLockExclusive(&pipelineReplaceLock);
        if (graphics) {
            __builtin_memcpy_inline(&graphicsDesc, description, sizeof(graphicsDesc));
            replaced = ReplaceWorldShaderPair(&graphicsDesc);
            shader = graphicsDesc.PS;
        } else {
            __builtin_memcpy_inline(&compute, description, sizeof(compute));
            shader = compute.CS = SelectCompressionShader(compute.CS);
        }
        ReleaseSRWLockExclusive(&pipelineReplaceLock);
        XboxPipelineTicket ticket;
        {
            HRESULT replayed;
            if (XboxPipelineReplayLookup(device, graphics, graphics ? (const void *)&graphicsDesc : (const void *)&compute,
                    iid, heap, &ticket, &replayed)) {
                QueryPerformanceCounter(&psoDone);
                const LONG count = InterlockedIncrement(graphics ? &graphicsCalls : &computeCalls);
                if (SUCCEEDED(replayed) && heap && *(void **)heap)
                    PtRegister(*(void **)heap, (DWORD)count, graphics, graphics ? &graphicsDesc.VS : &compute.CS, graphics ? &graphicsDesc.PS : NULL);
                static volatile LONG hitLines;
                if (InterlockedIncrement(&hitLines) <= 4000) {
                    char line[200], *out = Text(line, graphics ? "asset pso-replay hit kind=graphics n=" :
                        "asset pso-replay hit kind=compute n=");
                    out = CensusDec(out, (unsigned long long)count);
                    out = CensusDec(Text(out, " waited="), ticket.outcome == XboxPipelineReplayHitWaited);
                    out = CensusDec(Text(out, " ms="), CensusMicros(psoRequested, psoDone, psoFrequency) / 1000);
                    out = Hex(Text(out, " tid=0x"), GetCurrentThreadId(), 4);
                    out = CensusDec(Text(out, " qpc="), (unsigned long long)psoDone.QuadPart);
                    CensusEmit(line, out);
                }
                SetLastError(incomingError);
                return (DWORD)replayed;
            }
        }
        XboxCompileSlotTicket slot;
        XboxCompileSlotsEnter(PipelineClass(), &slot);
        QueryPerformanceCounter(&psoLocked);
        InterlockedExchange(&pipelineOwner, (LONG)GetCurrentThreadId());
        const LONG count = InterlockedIncrement(graphics ? &graphicsCalls : &computeCalls);
        const SIZE_T shaderBytes = shader.BytecodeLength;
        const BOOL baselinePair = graphics && MatchesPreviousCompilerFault(&graphicsDesc);
        if (baselinePair)
            TraceRemainingCommit("asset old-compiler-shaders-before bytes=0x", count, 0, __builtin_return_address(0));
        const BOOL tracePipeline = count <= 2 || !(count & 63) || (graphics && count >= 215 && count <= 230);
        if (tracePipeline) {
            TraceRemainingCommit(graphics ? "asset graphics-pso-before bytes=0x" :
                "asset compute-pso-before bytes=0x", shaderBytes, count, __builtin_return_address(0));
        }
        currentPipeline.ordinal = (DWORD)count;
        currentPipeline.graphics = graphics;
        currentPipeline.descriptor = graphics ? &graphicsDesc : NULL;
        if (graphics) {
            const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc = &graphicsDesc;
            currentPipeline.stages[0] = desc->VS;
            currentPipeline.stages[1] = desc->PS;
            currentPipeline.stages[2] = desc->DS;
            currentPipeline.stages[3] = desc->HS;
            currentPipeline.stages[4] = desc->GS;
        } else {
            currentPipeline.stages[0] = shader;
        }
        currentPipeline.owner = GetCurrentThreadId();
        if (replaced) {
            TraceRemainingCommit("asset own-world-dxil-before bytes=0x",
                (DWORD)count, 0, __builtin_return_address(0));
        }
        SetLastError(incomingError);
        const HRESULT result = graphics ?
            ID3D12Device_CreateGraphicsPipelineState(device,
                &graphicsDesc, iid, heap) :
            ID3D12Device_CreateComputePipelineState(device, &compute, iid, heap);
        const DWORD error = GetLastError();
        if (SUCCEEDED(result) && heap && *(void **)heap)
            PtRegister(*(void **)heap, (DWORD)count, graphics, graphics ? &graphicsDesc.VS : &compute.CS, graphics ? &graphicsDesc.PS : NULL);
        QueryPerformanceCounter(&psoDone);
        XboxPipelineReplayFinish(&ticket, graphics ? (const void *)&graphicsDesc : (const void *)&compute, result);
#ifdef PSO_BLOB_PROBE
        if (SUCCEEDED(result) && heap && *(void **)heap && graphics && count == 2) {
            ID3D12PipelineState_AddRef((ID3D12PipelineState *)*(void **)heap);
            probePipeline = (ID3D12PipelineState *)*(void **)heap;
            HANDLE probe = CreateThread(NULL, 0, CacheBlobProbe, NULL, 0, NULL);
            if (probe) CloseHandle(probe);
        }
#endif
        {
            const unsigned long long waitMs = CensusMicros(psoRequested, psoLocked, psoFrequency) / 1000;
            const unsigned long long compileMs = CensusMicros(psoLocked, psoDone, psoFrequency) / 1000;
            InterlockedIncrement(&perfPsoCount);
            InterlockedAdd64(&perfPsoMicros, (LONG64)CensusMicros(psoLocked, psoDone, psoFrequency));
            static volatile LONG slowPipelines;
            if (waitMs + compileMs >= 30 && InterlockedIncrement(&slowPipelines) <= 4000) {
                char line[200], *out = Text(line, graphics ? "asset pso-slow kind=graphics n=" : "asset pso-slow kind=compute n=");
                out = CensusDec(out, (unsigned long long)count);
                out = CensusDec(Text(out, " wait-ms="), waitMs);
                out = CensusDec(Text(out, " compile-ms="), compileMs);
                out = Hex(Text(out, " tid=0x"), GetCurrentThreadId(), 4);
                out = Hex(Text(out, " hr=0x"), (unsigned long)result, 8);
                out = CensusDec(Text(out, " replay="), (unsigned long long)ticket.outcome);
                {
                    const UINT64 key = PerfShaderKey(graphics, graphics ? (const void *)&graphicsDesc : (const void *)&compute);
                    out = Hex(Text(out, " sh=0x"), (unsigned long)(key >> 32), 8);
                    out = Hex(out, (unsigned long)key, 8);
                }
                out = CensusDec(Text(out, " qpc="), (unsigned long long)psoDone.QuadPart);
                CensusEmit(line, out);
            }
        }
        currentPipeline.owner = 0;
        if (baselinePair)
            TraceRemainingCommit("asset old-compiler-shaders-after bytes=0x", count, (DWORD)result, __builtin_return_address(0));
        if (replaced) TraceRemainingCommit("asset own-world-dxil-after bytes=0x",
            (DWORD)count, (DWORD)result, __builtin_return_address(0));
        static volatile LONG removalRecorded;
        if ((DWORD)result == 0x887A0005u && !InterlockedCompareExchange(&removalRecorded, 1, 0)) {
            TraceDeviceRemoval(device, shader, __builtin_return_address(0));
        }
        InterlockedExchange(&pipelineOwner, 0);
        XboxCompileSlotsLeave(&slot);
        if (FAILED(result) || tracePipeline) {
            TraceRemainingCommit(graphics ? "asset graphics-pso-after bytes=0x" :
                "asset compute-pso-after bytes=0x", shaderBytes, (DWORD)result, __builtin_return_address(0));
        }
        SetLastError(error);
        return (DWORD)result;
    }
    if (caller == 0x0175794Fu) {
        typedef UINT64 (WINAPI *GetPoolSize)(const char *);
        const GetPoolSize query = (GetPoolSize)(PVOID)(image + 0x01C2BB20u);
        const char *name = (const char *)startup;
        const UINT64 original = query(name);
        const DWORD error = GetLastError();
        static const struct { const char *name, *trace; UINT mib; } budgets[] = {
            { "AssetHeap", "asset engine-pool-AssetHeap bytes=0x", 828 },
            { "DynamicHeap", "asset engine-pool-DynamicHeap bytes=0x", 208 },
            { "SoundBankAlloc", "asset engine-pool-SoundBankAlloc bytes=0x", SOUNDBANK_MIB },
            { "RenderAlloc", "asset engine-pool-RenderAlloc bytes=0x", 64 },
            { "Physics", "asset engine-pool-Physics bytes=0x", 128 },
            { "ComponentHeap", "asset engine-pool-ComponentHeap bytes=0x", 48 },
            { "DDLHeap", "asset engine-pool-DDLHeap bytes=0x", 4 },
        };
        UINT64 effective = original;
        for (UINT index = 0; index < ARRAYSIZE(budgets); ++index) {
            if (!SameName(name, budgets[index].name)) continue;
            const UINT64 limit = (UINT64)budgets[index].mib << 20;
            if (original > limit) effective = limit;
            if (index == 0) LazyExpect(effective);
            TraceRemainingCommit(budgets[index].trace, effective, (DWORD)(original >> 20), NULL);
            break;
        }
        SetLastError(error);
        return effective;
    }
    if (caller == 0x018EA33Bu) {
        if (description->SizeInBytes >= 32ull * 1024u * 1024u) TraceRemainingCommit("asset d3d12-heap-before bytes=0x", description->SizeInBytes, description->Flags, NULL);
        const HRESULT result = ID3D12Device_CreateHeap((ID3D12Device*)startup, description, iid, heap);
        const DWORD error = GetLastError();
        static volatile LONG events;
        if (FAILED(result) || InterlockedIncrement(&events) <= 16) {
            char line[180];
            char* out = Hex(Text(line,"asset d3d12-heap bytes=0x"),(DWORD)(description->SizeInBytes >> 32u),8);
            out = Hex(out, (DWORD)description->SizeInBytes, 8);
            out = Hex(Text(out," type=0x"), (DWORD)description->Properties.Type, 2);
            out = Hex(Text(out," page=0x"), (DWORD)description->Properties.CPUPageProperty, 2);
            out = Hex(Text(out," pool=0x"), (DWORD)description->Properties.MemoryPoolPreference, 2);
            out = Hex(Text(out," flags=0x"), (DWORD)description->Flags, 8);
            out = Hex(Text(out," result=0x"), (DWORD)result, 8);
            if (FAILED(result)) {
                MEMORYSTATUSEX memory = { .dwLength = sizeof(memory) };
                if (GlobalMemoryStatusEx(&memory))
                    out = Hex(Text(out, " available-commit-kib=0x"), (DWORD)(memory.ullAvailPageFile >> 10), 8);
            }
            out = Text(out,"\r\n");
            AppendTrace(line,(DWORD)(out-line));
            if (FAILED(result)) SaveRenderFailure(line, (DWORD)(out-line));
        }
        if (FAILED(result) || description->SizeInBytes >= 128ull * 1024u * 1024u) {
            TraceRemainingCommit("asset d3d12-heap-after bytes=0x", description->SizeInBytes, description->Flags, NULL);
            TraceAllocationStack("asset d3d12-heap-caller=0x");
        }
        SetLastError(error);
        return (UINT64)(DWORD)result;
    }
    const LONG thread = (LONG)GetCurrentThreadId();
    if (!InterlockedCompareExchange(&startupThread, thread, 0)) {
        char line[80];
        char *end = Hex(Text(line, "asset boot-thread=0x"), (DWORD)thread, 8);
        end = Text(end, "\r\n");
        AppendTrace(line, (DWORD)(end - line));
        PerfStart((DWORD)thread);
#if defined(HOSTED_RENDER_WIDTH) && defined(HOSTED_RENDER_HEIGHT)
        /* Report a smaller hosted monitor (the game's only display mode) so the game renders below the
           1920x1080 swap chain and upscales into it; the host presents the swap chain unchanged. */
        {
            WCHAR value[8]; char note[80], *e;
            const UINT sizes[2] = { HOSTED_RENDER_WIDTH, HOSTED_RENDER_HEIGHT };
            const WCHAR *names[2] = { L"XBOX_SDL_RENDER_WIDTH", L"XBOX_SDL_RENDER_HEIGHT" };
            for (UINT i = 0; i < 2; ++i) {
                UINT n = sizes[i], digits = 0; WCHAR tmp[8];
                do { tmp[digits++] = (WCHAR)(L'0' + n % 10); n /= 10; } while (n && digits < 7);
                for (UINT j = 0; j < digits; ++j) value[j] = tmp[digits - 1 - j];
                value[digits] = 0;
                SetEnvironmentVariableW(names[i], value);
            }
            e = CensusDec(Text(note, "asset hosted-render-size width="), HOSTED_RENDER_WIDTH);
            e = CensusDec(Text(e, " height="), HOSTED_RENDER_HEIGHT);
            e = Text(e, "\r\n");
            AppendTrace(note, (DWORD)(e - note));
        }
#endif
        {
            /* Same placement as the shared loader trace: the host's reserved processors. */
#ifndef PLACE_SPLIT
            const XboxCpuSetResult placed = XboxUseHostProcessors();
            char note[160];
            AppendTrace(note, (DWORD)(XboxFormatProcessors(note, &placed) - note));
#endif
            ProcHooksInstall();
#ifndef NO_PLACER
            PlacerStart();
#endif
#ifdef MEM_CENSUS
            { HANDLE mc = CreateThread(NULL, 0, McWatch, NULL, 0, NULL); if (mc) CloseHandle(mc); }
#endif
        }
        HANDLE watcher = CreateThread(NULL, 0, WatchLiveStartup, NULL, 0, NULL);
        if (watcher) CloseHandle(watcher);
#ifndef PLAY_BUILD
        HANDLE census = CreateThread(NULL, 0, CensusWatch, NULL, 0, NULL);
        if (census) CloseHandle(census);
        else TraceLiveThread("create-error=0x", (DWORD)thread, GetLastError(), NULL);
#endif
    }
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
        if (SUCCEEDED(result) && startupDevice) AllocInstall((ID3D12Device *)startupDevice);
        if (SUCCEEDED(result) && startupDevice) {
            xboxSacTrace = SacTrace;
            const HRESULT sac = XboxSacInstall((ID3D12Device *)startupDevice, (XboxSacPlacedFn)allocOriginal[29]);
            SacTrace("sac-install", (unsigned long long)(sac == S_OK), (unsigned long long)(DWORD)sac);
        }
#ifdef HEAP_ADDR
        if (SUCCEEDED(result) && startupDevice) HeapAddressInstall((ID3D12Device *)startupDevice);
#endif
#ifndef NO_REPLAY
        if (SUCCEEDED(result) && startupDevice) ReplayInstall((ID3D12Device *)startupDevice);
#endif
    }
    SetLastError(previous);
    return S_OK;
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

static FARPROC HostedDxgiFactory(HMODULE module, const char *name) {
    WCHAR enabled[2];
    if (!(SameName(name, "CreateDXGIFactory2") || SameName(name, "CreateDXGIFactory1") ||
            SameName(name, "CreateDXGIFactory")) || module != GetModuleHandleW(L"dxgi.dll") ||
            GetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_DXGI", enabled, ARRAYSIZE(enabled)) != 1 ||
            enabled[0] != L'1') return NULL;
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
    return GetProcAddress(adapter, name);
}

typedef struct FidelityFxCallbacks {
    void *data;
    void *(__cdecl *allocate)(void *, UINT64);
    void (__cdecl *release)(void *, void *);
} FidelityFxCallbacks;
static PVOID volatile originalFxAllocator;

static void *__cdecl TraceFidelityFxAllocation(void *data, UINT64 bytes) {
    (void)data;
    const FidelityFxCallbacks *original = InterlockedCompareExchangePointer(&originalFxAllocator, NULL, NULL);
    void *const result = original->allocate(original->data, bytes);
    const DWORD error = GetLastError();
    static volatile LONG allocations;
    if (!result || InterlockedIncrement(&allocations) <= 4) {
        TraceRemainingCommit(result ? "asset ffx-host-alloc bytes=0x" : "asset ffx-host-alloc-failed bytes=0x",
            (SIZE_T)bytes, 0, (const void *)(ULONG_PTR)original->allocate);
    }
    SetLastError(error);
    return result;
}

static void __cdecl TraceFidelityFxRelease(void *data, void *memory) {
    (void)data;
    const FidelityFxCallbacks *original = InterlockedCompareExchangePointer(&originalFxAllocator, NULL, NULL);
    original->release(original->data, memory);
}

static DWORD __cdecl TraceCreateFidelityFxContext(void **context, void *description, const void *allocator) {
    const CreateFidelityFxContext create = (CreateFidelityFxContext)InterlockedCompareExchangePointer(
        &createFidelityFxContext, NULL, NULL);
    if (!create) return 1;
    InterlockedExchange(&fidelityFxReached, 1);
    static const char standard[] = "asset ffx-create-context start allocator=default\r\n";
    static const char custom[] = "asset ffx-create-context start allocator=custom\r\n";
    if (allocator) AppendTrace(custom, sizeof(custom) - 1);
    else AppendTrace(standard, sizeof(standard) - 1);
    static const FidelityFxCallbacks traced = {NULL, TraceFidelityFxAllocation, TraceFidelityFxRelease};
    const FidelityFxCallbacks *source = allocator;
    const void *selected = allocator;
    if (source && source->allocate && source->release) {
        const PVOID first = InterlockedCompareExchangePointer(&originalFxAllocator, (PVOID)allocator, NULL);
        if (!first || first == allocator) selected = &traced;
    }
    const DWORD result = create(context, description, selected);
    const DWORD error = GetLastError();
    char line[100];
    char *out = Hex(Text(line, "asset ffx-create-context result=0x"), result, 8);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
    SetLastError(error);
    return result;
}

FARPROC WINAPI XboxGetProcAddress(HANDLE module, const char *name) {
    if (module && (ULONG_PTR)name <= 0xffffu && module == GetModuleHandleW(L"XboxWin32XInput910.dll")) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        Record(name, module, 0, FALSE, ERROR_PROC_NOT_FOUND);
        SetLastError(ERROR_PROC_NOT_FOUND);
        return NULL;
    }
    FARPROC result = GetProcAddress(module, name);
    DWORD error = GetLastError();
    Record(name, module, 0, result != 0, error);
    if (result && (ULONG_PTR)name > 0xffffu) {
        if ((name[0] == 'B' && name[1] == 'i' && name[2] == 'n' && name[3] == 'k') ||
                SameName(name, "XInputGetState") || SameName(name, "XInputGetCapabilities")) {
            char line[140];
            char *end = Token(Text(line, "asset boot-export "), name, 0);
            end = RecordImageAddress(end, __builtin_return_address(0));
            end = Text(end, "\r\n");
            AppendTrace(line, (DWORD)(end - line));
        }
        if (name[0] == 'D' && SameName(name, "D3D12CreateDevice") &&
                module == GetModuleHandleW(L"d3d12.dll")) {
            const PVOID first = InterlockedCompareExchangePointer(&d3d12CreateDevice, (PVOID)result, NULL);
            if (!first || first == (PVOID)result) result = (FARPROC)(PVOID)TraceD3D12CreateDevice;
        } else if (name[0] == 'C') {
            const FARPROC hosted = HostedDxgiFactory(module, name);
            if (hosted) {
                const char trace[] = "asset dxgi-factory=hosted\r\n";
                AppendTrace(trace, sizeof(trace) - 1);
                result = hosted;
            }
        }
    }
    if (result && module && (ULONG_PTR)name > 0xffff && SameName(name, "ffxCreateContext")) {
        InterlockedExchangePointer(&createFidelityFxContext, (PVOID)result);
        result = (FARPROC)(PVOID)TraceCreateFidelityFxContext;
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

static BOOL EqualAssetPath(const WCHAR *left, const WCHAR *right) {
    return CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
}

static const WCHAR *WithoutExtendedPrefix(const WCHAR *path) {
    return path[0] == L'\\' && path[1] == L'\\' && path[2] == L'?' && path[3] == L'\\' ? path + 4 : path;
}

static LPCWSTR RoutePreparedAsset(LPCWSTR path, WCHAR *destination, DWORD capacity, int *asset) {
    const DWORD previous = GetLastError();
    *asset = 0;
    if (!path) return path;
    const WCHAR *base = path;
    for (const WCHAR *current = path; *current; ++current)
        if (*current == L'\\' || *current == L'/') base = current + 1;
    static const WCHAR *const sources[] = {
        L"\\asset_archive\\toc",
        L"\\asset_archive\\local_mip256_hdclosed.dsar",
        L"\\asset_archive\\local_model1356_lod3_20260916.dsar"
    };
    int kind = 0;
    for (UINT i = 0; i < ARRAYSIZE(sources); ++i) {
        if (EqualAssetPath(base, sources[i] + 15)) {
            kind = (int)i + 1;
            break;
        }
    }
    if (!kind) {
        SetLastError(previous);
        return path;
    }
    WCHAR actual[768], install[768];
    const DWORD actualSize = GetFullPathNameW(path, ARRAYSIZE(actual), actual, NULL);
    const DWORD rootSize = GetEnvironmentVariableW(L"XBOX_WIN32_INSTALL_ROOT", install, ARRAYSIZE(install));
    if (!actualSize || actualSize >= ARRAYSIZE(actual) || !rootSize || rootSize >= ARRAYSIZE(install)) {
        SetLastError(previous);
        return path;
    }
    const WCHAR *actualDos = WithoutExtendedPrefix(actual);
    const WCHAR *root = WithoutExtendedPrefix(install);
    DWORD rootLength = rootSize - (DWORD)(root - install);
    while (rootLength && root[rootLength - 1] == L'\\') --rootLength;
    if (actualSize - (DWORD)(actualDos - actual) <= rootLength ||
            CompareStringOrdinal(actualDos, rootLength, root, rootLength, TRUE) != CSTR_EQUAL ||
            (!EqualAssetPath(actualDos + rootLength, sources[kind - 1]) &&
             (kind != 1 || !EqualAssetPath(actualDos + rootLength, L"\\toc")))) {
        SetLastError(previous);
        return path;
    }
    if (kind == 2) {
        static const WCHAR nesting[] = L"\\steamapps\\common\\";
        static const WCHAR file[] = L"\\Win32AssetOverlay-1817070-23986256-e64d5bc6139e.dsar";
        DWORD parent = rootLength;
        while (parent && root[parent - 1] != L'\\') --parent;
        const DWORD nestingLength = ARRAYSIZE(nesting) - 1;
        if (parent <= nestingLength ||
                CompareStringOrdinal(root + parent - nestingLength, nestingLength,
                    nesting, nestingLength, TRUE) != CSTR_EQUAL ||
                parent - nestingLength + ARRAYSIZE(file) > capacity) {
            SetLastError(previous);
            return path;
        }
        const DWORD library = parent - nestingLength;
        for (DWORD i = 0; i < library; ++i) destination[i] = root[i];
        for (DWORD i = 0; i < ARRAYSIZE(file); ++i) destination[library + i] = file[i];
        *asset = kind;
        SetLastError(previous);
        return destination;
    }
    UINT32 length = capacity;
    const WCHAR prefix[] = L"\\owned-mip-overlay\\";
    const WCHAR *name = sources[kind - 1] + 15;
    UINT32 nameLength = 0;
    while (name[nameLength]) ++nameLength;
    if (GetCurrentPackagePath(&length, destination) != ERROR_SUCCESS || !length ||
            length + ARRAYSIZE(prefix) - 1 + nameLength > capacity) {
        SetLastError(previous);
        return path;
    }
    DWORD i = length - 1;
    for (const WCHAR *p = prefix; *p;) destination[i++] = *p++;
    while (*name) destination[i++] = *name++;
    destination[i] = 0;
    *asset = kind;
    SetLastError(previous);
    return destination;
}

static PVOID volatile preparedArchiveHandles[2];
static volatile LONG preparedArchiveReads[2];

BOOL WINAPI XboxTracePreparedRead(HANDLE file, LPVOID buffer, DWORD length, LPDWORD count, LPOVERLAPPED operation) {
    UINT slot = ARRAYSIZE(preparedArchiveHandles);
    if (file) {
        for (UINT i = 0; i < ARRAYSIZE(preparedArchiveHandles); ++i) {
            if (InterlockedCompareExchangePointer(&preparedArchiveHandles[i], NULL, NULL) == file) {
                slot = i;
                break;
            }
        }
    }
    LazyPrepareWrite(buffer, length);
    const BOOL success = ReadFile(file, buffer, length, count, operation);
    const DWORD error = GetLastError();
    static volatile LONG zoneReads;
    ULARGE_INTEGER requestOffset;
    const BOOL hasOffset = slot < ARRAYSIZE(preparedArchiveHandles) && operation &&
        ReadProcessMemory(GetCurrentProcess(), &operation->Offset, &requestOffset, sizeof(requestOffset), NULL);
    const UINT64 offset = hasOffset ? requestOffset.QuadPart : 0;
    const UINT64 zoneFirst = 39839991ull;
    const UINT64 transferred = success && count ? *count : length;
    const BOOL zone = slot == 1 && hasOffset && transferred &&
        (offset >= zoneFirst || transferred > zoneFirst - offset);
    const BOOL logZone = zone && InterlockedIncrement(&zoneReads) <= 32;
    if (slot < ARRAYSIZE(preparedArchiveHandles) &&
            (logZone || InterlockedIncrement(&preparedArchiveReads[slot]) <= 24)) {
        char line[147];
        char *out = Text(line, logZone ? "asset prepared-zone-block-read=" :
            slot == 0 ? "asset prepared-archive-read=" : "asset prepared-model-read=");
        out = Text(out, success ? "ok" : error == ERROR_IO_PENDING ? "pending" : "failed");
        out = Hex(Text(out, " bytes=0x"), success && count ? *count : length, 8);
        if (hasOffset) {
            out = Hex(Text(out, " offset=0x"), requestOffset.HighPart, 8);
            out = Hex(out, requestOffset.LowPart, 8);
        }
        out = Hex(Text(out, " code=0x"), success ? 0 : error, 8);
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
    }
    SetLastError(error);
    return success;
}

BOOL WINAPI XboxTracePreparedClose(HANDLE file) {
    const DWORD error = GetLastError();
    if (file) {
        for (UINT i = 0; i < ARRAYSIZE(preparedArchiveHandles); ++i)
            InterlockedCompareExchangePointer(&preparedArchiveHandles[i], NULL, file);
    }
    SetLastError(error);
    return CloseHandle(file);
}

static void TracePreparedVolume(BOOL package, const char *event) {
    WCHAR path[1024];
    UINT32 size = ARRAYSIZE(path);
    BOOL havePath;
    if (package) {
        havePath = GetCurrentPackagePath(&size, path) == ERROR_SUCCESS;
    } else {
        size = GetEnvironmentVariableW(L"XBOX_WIN32_INSTALL_ROOT", path, size);
        havePath = size > 0 && size < ARRAYSIZE(path);
    }
    ULARGE_INTEGER available;
    if (havePath && GetDiskFreeSpaceExW(path, &available, NULL, NULL))
        TraceRemainingCommit(event, available.QuadPart, ERROR_SUCCESS, NULL);
    else
        TraceRemainingCommit(event, 0, havePath ? GetLastError() : ERROR_PATH_NOT_FOUND, NULL);
}

static void RecordPreparedAssetOpen(int kind, BOOL success, DWORD error) {
    if (kind == 1 && success) {
        static volatile LONG first;
        if (InterlockedExchange(&first, 1) == 0) {
            TracePreparedVolume(TRUE, "asset prepared-package-volume-free bytes=0x");
            TracePreparedVolume(FALSE, "asset prepared-install-volume-free bytes=0x");
        }
    }
    char line[100];
    char *out = Text(line, kind == 1 ? "asset prepared-toc-open=" :
                          kind == 2 ? "asset prepared-archive-open=" : "asset prepared-model-open=");
    out = Text(out, success ? "ok" : "failed");
    out = Hex(Text(out, " code=0x"), success ? 0 : error, 8);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

#include "pso-warmup.h"

HANDLE WINAPI XboxTraceCreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
        DWORD creation, DWORD attributes, HANDLE templateFile) {
#if PSO_SKIP_WARMUP
    WCHAR session[1024];
    LPCWSTR routed;
    if (WarmupRouteOpen(path, access, &creation, session, ARRAYSIZE(session), &routed)) {
        WarmupTrace("hidden", FALSE, 0, &warmupHiddenCount);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    if (routed) {
        const HANDLE file = CreateFileW(routed, access, share, security, creation, attributes, templateFile);
        const DWORD error = GetLastError();
        WarmupTrace("session-open", file != INVALID_HANDLE_VALUE, error, &warmupSessionCount);
        SetLastError(error);
        return file;
    }
#endif
#if PSO_WARMUP_LIST
    {
        BOOL handled;
        const HANDLE file = WarmupListOpen(path, access, share, security, creation, attributes, templateFile, &handled);
        if (handled) return file;
    }
#endif
    WCHAR local[256];
    const LPCWSTR mapped = LocalPipePath(path, local, ARRAYSIZE(local));
    WCHAR prepared[1024];
    int asset = 0;
    const DWORD readAccess = GENERIC_READ | FILE_READ_DATA | FILE_READ_EA | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
    const LPCWSTR selected = mapped ? mapped : creation == OPEN_EXISTING && !(access & ~readAccess) && !(attributes & FILE_FLAG_DELETE_ON_CLOSE)
        ? RoutePreparedAsset(path, prepared, ARRAYSIZE(prepared), &asset) : path;
    HANDLE result = CreateFileW(selected, access, share, security, creation, attributes, templateFile);
    const DWORD error = GetLastError();
    if (asset >= 2 && result != INVALID_HANDLE_VALUE)
        InterlockedExchangePointer(&preparedArchiveHandles[asset - 2], result);
    if (asset) RecordPreparedAssetOpen(asset, result != INVALID_HANDLE_VALUE, error);
    if (mapped) ObserveNativeResult("pipe-open-local", result != INVALID_HANDLE_VALUE, error);
    else ObserveAsset(path, "open", result != INVALID_HANDLE_VALUE, error);
    SetLastError(error);
    return result;
}

HANDLE WINAPI XboxTraceCreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
        DWORD creation, DWORD attributes, HANDLE templateFile) {
    HANDLE result = CreateFileA(path, access, share, security, creation, attributes, templateFile);
    ObserveAssetA(path, "open", result != INVALID_HANDLE_VALUE, GetLastError());
    return result;
}

DWORD WINAPI XboxTraceGetFileAttributesW(LPCWSTR path) {
#if PSO_SKIP_WARMUP
    if (WarmupCacheFile(path)) {
        WarmupTrace("hidden", FALSE, 0, &warmupHiddenCount);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_FILE_ATTRIBUTES;
    }
#endif
    WCHAR prepared[1024];
    int asset = 0;
    DWORD result = GetFileAttributesW(RoutePreparedAsset(path, prepared, ARRAYSIZE(prepared), &asset));
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

static char binkDirectory[768];

static void TracePathQuery(const char *query, UINT length, UINT size, const char *path, DWORD error) {
    char line[150];
    char *out = Hex(Text(Text(line, "asset path-query "), query), length, 4);
    out = Hex(Text(out, " error=0x"), error, 8);
    if (!length) {
        out = Text(out, " result=empty");
    } else if (length >= size) {
        out = Text(out, " result=truncated");
    } else if (SameName(query, "module=0x")) {
        UINT directory = length;
        while (directory && path[directory - 1] != '\\' && path[directory - 1] != ':') --directory;
        binkDirectory[0] = 0;
        if (directory > 1 && directory < ARRAYSIZE(binkDirectory)) {
            for (UINT i = 0; i < directory - 1; ++i) binkDirectory[i] = path[i];
            binkDirectory[directory - 1] = 0;
        }
        out = Text(out, directory ? " result=qualified" : " result=bare");
    } else {
        const int relation = CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, binkDirectory, -1, path, (int)length);
        out = Text(out, relation == CSTR_EQUAL ? " result=module-parent" : " result=other-directory");
    }
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
    SetLastError(error);
}

DWORD WINAPI XboxTraceGetModuleFileNameA(HMODULE module, LPSTR path, DWORD size) {
    const DWORD length = GetModuleFileNameA(module, path, size);
    TracePathQuery("module=0x", length, size, path, GetLastError());
    return length;
}

UINT WINAPI XboxTraceGetWindowsDirectoryA(LPSTR path, UINT size) {
    const UINT length = GetWindowsDirectoryA(path, size);
    TracePathQuery("windows=0x", length, size, path, GetLastError());
    return length;
}

UINT WINAPI XboxTraceGetSystemDirectoryA(LPSTR path, UINT size) {
    const UINT length = GetSystemDirectoryA(path, size);
    TracePathQuery("system=0x", length, size, path, GetLastError());
    return length;
}

int WINAPI XboxTraceMessageBoxA(HWND window, LPCSTR message, LPCSTR title, UINT type) {
    static const char checkpoint[] = "asset bink-placement-dialog\r\n";
    if (title && SameName(title, "Bink Error")) AppendTrace(checkpoint, sizeof(checkpoint) - 1);
    typedef int (WINAPI *ShowMessage)(HWND, LPCSTR, LPCSTR, UINT);
    const HMODULE user = GetModuleHandleW(L"user32.dll");
    const ShowMessage show = user ? (ShowMessage)(PVOID)GetProcAddress(user, "MessageBoxA") : NULL;
    if (!show) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return 0;
    }
    return show(window, message, title, type);
}

static void TraceBinkRuntime(const char *name, BOOL success, DWORD error, DWORD detail) {
    char line[125];
    char *out = Text(Text(line, "asset bink-crt "), name);
    out = Text(out, success ? " ok" : " failed");
    out = Hex(Text(out, " error=0x"), error, 8);
    out = Hex(Text(out, " detail=0x"), detail, 8);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
    SetLastError(error);
}

HANDLE WINAPI XboxBinkTraceHeapCreate(DWORD flags, SIZE_T initial, SIZE_T maximum) {
    const HANDLE heap = HeapCreate(flags, initial, maximum);
    TraceBinkRuntime("HeapCreate", heap != NULL, GetLastError(), flags);
    return heap;
}

LPVOID WINAPI XboxBinkTraceHeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) {
    const LPVOID memory = HeapAlloc(heap, flags, size);
    const DWORD error = GetLastError();
    static volatile LONG recorded;
    if (!memory || InterlockedIncrement(&recorded) <= 5) {
        TraceBinkRuntime("HeapAlloc", memory != NULL, error, (DWORD)size);
    }
    return memory;
}

DWORD WINAPI XboxBinkTraceFlsAlloc(PFLS_CALLBACK_FUNCTION callback) {
    const DWORD index = FlsAlloc(callback);
    TraceBinkRuntime("FlsAlloc", index != FLS_OUT_OF_INDEXES, GetLastError(), index);
    return index;
}

BOOL WINAPI XboxBinkTraceFlsSetValue(DWORD index, PVOID value) {
    const BOOL result = FlsSetValue(index, value);
    TraceBinkRuntime("FlsSetValue", result, GetLastError(), index);
    return result;
}

BOOL WINAPI XboxBinkTraceInitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION section, DWORD spin) {
    const BOOL result = InitializeCriticalSectionAndSpinCount(section, spin);
    TraceBinkRuntime("InitializeCriticalSectionAndSpinCount", result, GetLastError(), spin);
    return result;
}

VOID WINAPI XboxBinkTraceGetStartupInfoA(LPSTARTUPINFOA info) {
    if (!info) {
        GetStartupInfoA(info);
        return;
    }
    info->cb = 0;
    GetStartupInfoA(info);
    const DWORD error = GetLastError();
    TraceBinkRuntime("GetStartupInfoA", info->cb == sizeof(*info), error, info->cb);
    if (info->cb != sizeof(*info)) {
        STARTUPINFOW wide;
        wide.cb = 0;
        GetStartupInfoW(&wide);
        const DWORD wideError = GetLastError();
        const BOOL compatible = wide.cb == sizeof(wide) && !wide.lpReserved && !wide.lpDesktop && !wide.lpTitle;
        TraceBinkRuntime("GetStartupInfoW", compatible, wideError, wide.cb);
        if (compatible) {
            _Static_assert(sizeof(STARTUPINFOA) == sizeof(STARTUPINFOW), "Startup info size");
            volatile BYTE *destination = (volatile BYTE *)info;
            const BYTE *source = (const BYTE *)&wide;
            for (size_t i = 0; i < sizeof(wide); ++i) destination[i] = source[i];
        }
    }
    SetLastError(error);
}

static LONG ObserveBadStandardHandle(DWORD code) {
    TraceBinkRuntime("standard-handle-exception", FALSE, GetLastError(), code);
    return code == EXCEPTION_ACCESS_VIOLATION || code == 0xC0000008u || code == 0xC00000BBu ?
        EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

HANDLE WINAPI XboxBinkTraceGetStdHandle(DWORD stream) {
    const HANDLE handle = GetStdHandle(stream);
    const DWORD lastError = GetLastError();
    TraceBinkRuntime("GetStdHandle", handle != NULL && handle != INVALID_HANDLE_VALUE, lastError, stream);
    if (handle && handle != INVALID_HANDLE_VALUE) {
        DWORD flags = 0;
        BOOL valid;
        DWORD error;
        TraceBinkRuntime("GetHandleInformation-entry", TRUE, lastError, (DWORD)((ULONG_PTR)handle & 3));
        __try {
            valid = GetHandleInformation(handle, &flags);
            error = GetLastError();
        } __except (ObserveBadStandardHandle(GetExceptionCode())) {
            valid = FALSE;
            error = ERROR_INVALID_HANDLE;
        }
        TraceBinkRuntime("GetHandleInformation", valid, error, valid ? flags : 0);
        if (!valid && error == ERROR_INVALID_HANDLE) {
            SetLastError(lastError);
            return NULL;
        }
    }
    SetLastError(lastError);
    return handle;
}

DWORD WINAPI XboxBinkTraceGetFileType(HANDLE file) {
    TraceBinkRuntime("GetFileType-entry", TRUE, GetLastError(), 0);
    DWORD type;
    DWORD error;
    __try {
        type = GetFileType(file);
        error = GetLastError();
    } __except (ObserveBadStandardHandle(GetExceptionCode())) {
        type = FILE_TYPE_UNKNOWN;
        error = ERROR_INVALID_HANDLE;
    }
    TraceBinkRuntime("GetFileType", type != FILE_TYPE_UNKNOWN, error, type);
    return type;
}

LPSTR WINAPI XboxBinkTraceGetCommandLineA(void) {
    const LPSTR commandLine = GetCommandLineA();
    TraceBinkRuntime("GetCommandLineA", commandLine && *commandLine, GetLastError(), 0);
    return commandLine;
}

DWORD WINAPI XboxGetFileType(HANDLE file) {
    const DWORD statusInvalidHandle = 0xC0000008u;
    __try {
        return GetFileType(file);
    } __except (GetExceptionCode() == statusInvalidHandle ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        static const char message[] = "asset file-type-invalid-handle\r\n";
        AppendTrace(message, sizeof(message) - 1);
        SetLastError(ERROR_INVALID_HANDLE);
        return FILE_TYPE_UNKNOWN;
    }
}

typedef struct {
    DWORD totalBlocks;
    DWORD freeBlocks;
    DWORD freeBytes;
    DWORD largestFree;
    DWORD digest;
} AssetArenaSample;

static DWORD SampleAssetArena(HANDLE process, ULONG_PTR first, ULONG_PTR tail, AssetArenaSample *sample) {
    BOOL previousFree = FALSE;
    ULONG_PTR offset = first;
    for (DWORD index = 0; index < 32768; ++index) {
        UINT64 head;
        if (!ReadProcessMemory(process, (const void *)offset, &head, sizeof(head), NULL)) return 2;
        UINT64 size = head & ~3ull;
        BOOL free = (head & 1u) != 0;
        if (((head & 2u) != 0) != previousFree || (previousFree && free)) return 3;
        if (offset == tail) return !size && !free ? 0 : 3;
        if (size < 24 || (size & 7u) || tail - offset < 8 || size > tail - offset - 8) return 3;
        if (free) {
            ++sample->freeBlocks;
            sample->freeBytes += (DWORD)size;
            if (sample->largestFree < size) sample->largestFree = (DWORD)size;
        }
        ++sample->totalBlocks;
        sample->digest = (sample->digest ^ (DWORD)head) * 16777619u;
        offset += (ULONG_PTR)size + 8;
        previousFree = free;
    }
    return 4;
}

static void ObserveAssetArena(const BYTE *image, const void *caller, SIZE_T requested) {
    const BYTE *pool = image + 0x07785B50u;
    HANDLE process = GetCurrentProcess();
    UINT64 backing[4] = {0, 0, 0, 0};
    UINT64 alignment = 0;
    const BYTE *operations = NULL;
    const void *allocator = NULL;
    if (!ReadProcessMemory(process, pool + 0x98u, backing, sizeof(backing), NULL) ||
            !ReadProcessMemory(process, pool + 0x40u, &alignment, sizeof(alignment), NULL) ||
            !ReadProcessMemory(process, pool, &operations, sizeof(operations), NULL) ||
            !operations || !ReadProcessMemory(process, operations + 0x18u, &allocator, sizeof(allocator), NULL) ||
            allocator != image + 0x01C625A0u) {
        TraceLiveThread("menu-arena-walk=0x", 1, 0, NULL);
        return;
    }

    const ULONG_PTR arena = (ULONG_PTR)backing[0];
    const UINT64 extent = backing[3];
    TraceRemainingCommit("asset menu-arena-backing bytes=0x", extent, (DWORD)alignment, caller);
    if (!arena || arena != backing[1] || (arena & 7u) || extent < 0x19B0u ||
            extent > MAXDWORD || arena > (ULONG_PTR)-1 - extent) {
        TraceLiveThread("menu-arena-walk=0x", 1, arena != 0, NULL);
        return;
    }
    const ULONG_PTR tail = arena + (extent & ~7ull) - 8;
    MEMORY_BASIC_INFORMATION region;
    DWORD firstState = VirtualQuery((const void *)arena, &region, sizeof(region)) ? region.State : 0;
    DWORD lastState = VirtualQuery((const void *)tail, &region, sizeof(region)) ? region.State : 0;
    TraceLiveThread("menu-arena-region=0x", firstState, lastState, NULL);
    if (firstState != MEM_COMMIT || lastState != MEM_COMMIT) {
        TraceLiveThread("menu-arena-walk=0x", 2, 0, NULL);
        return;
    }

    // This exact request maps to FL 14 / SL 18 at either supported alignment.
    if ((alignment == 8 || alignment == 16) && requested == 3247528) {
        DWORD bitmaps[25];
        if (ReadProcessMemory(process, (const void *)(arena + 0x20), bitmaps, sizeof(bitmaps), NULL)) {
            const DWORD eligibleGroups = bitmaps[0] & 0x01FF8000u;
            const DWORD eligibleSubclasses = bitmaps[15] & 0xFFFC0000u;
            TraceLiveThread("menu-arena-search-bins=0x", eligibleGroups, eligibleSubclasses, NULL);
        }
    }
    AssetArenaSample first = {0, 0, 0, 0, 2166136261u};
    AssetArenaSample second = {0, 0, 0, 0, 2166136261u};
    DWORD result = SampleAssetArena(process, arena + 0x1988u, tail, &first);
    if (!result) {
        result = SampleAssetArena(process, arena + 0x1988u, tail, &second);
        if (!result && (first.digest != second.digest || first.totalBlocks != second.totalBlocks ||
                first.freeBlocks != second.freeBlocks || first.freeBytes != second.freeBytes ||
                first.largestFree != second.largestFree)) result = 5;
    }
    TraceLiveThread("menu-arena-walk=0x", result, first.totalBlocks, NULL);
    if (result) return;
    TraceLiveThread("menu-arena-free-bytes=0x", first.freeBytes, first.largestFree, NULL);
    TraceLiveThread("menu-arena-block-counts=0x", first.totalBlocks, first.freeBlocks, NULL);
}

static void ObserveAssetHeap(const BYTE *image, const void *caller) {
    const BYTE *pool = image + 0x07785B50u;
    UINT64 bytes[3] = {0, 0, 0};
    DWORD layout[4] = {0, 0, 0, 0};
    const BYTE *operations = NULL;
    const void *allocator = NULL;
    if (!ReadProcessMemory(GetCurrentProcess(), pool + 0x60u, bytes, sizeof(bytes), NULL) ||
            !ReadProcessMemory(GetCurrentProcess(), pool + 0x50u, layout, sizeof(layout), NULL) ||
            !ReadProcessMemory(GetCurrentProcess(), pool, &operations, sizeof(operations), NULL)) return;
    if (operations)
        ReadProcessMemory(GetCurrentProcess(), operations + 0x18u, &allocator, sizeof(allocator), NULL);
    TraceRemainingCommit("asset menu-heap-capacity bytes=0x", bytes[0], layout[0], caller);
    TraceRemainingCommit("asset menu-heap-used bytes=0x", bytes[1], layout[1], caller);
    TraceRemainingCommit("asset menu-heap-peak bytes=0x", bytes[2], layout[2], caller);
    TraceLiveThread("menu-heap-dispatch=0x", operations != NULL, allocator != NULL, allocator);
    const BYTE *cache = NULL;
    ReadProcessMemory(GetCurrentProcess(), pool + 0x48u, &cache, sizeof(cache), NULL);
    TraceLiveThread("menu-heap-cache=0x", cache != NULL, layout[3], NULL);
}

LPSTR WINAPI XboxObserveAssetFailure(void *manager, const BYTE *asset, DWORD status, DWORD detail) {
    const DWORD previous = GetLastError();
    const BYTE *image = (const BYTE *)GetModuleHandleW(NULL);
    const void *caller = __builtin_return_address(0);
    static const struct {
        DWORD returnRva;
        DWORD allocatorRva;
        DWORD menuBytes;
        const char *event;
    } zoneAllocations[] = {
        {0x01A1EDF5u, 0x01757E30u, 1752528, "asset menu-zone-allocation-graphics-first bytes=0x"},
        {0x01A1EE0Au, 0x01757E30u, 0, "asset menu-zone-allocation-graphics-second bytes=0x"},
        {0x01A1EE22u, 0x017570B0u, 3247528, "asset menu-zone-allocation-asset bytes=0x"},
        {0x01A1EE34u, 0x0181E860u, 4320936, "asset menu-zone-allocation-component bytes=0x"},
        {0x016253F3u, 0x01C64DF0u, 3247528, "asset menu-zone-allocation-alternate bytes=0x"},
    };
    for (unsigned i = 0; i < ARRAYSIZE(zoneAllocations); ++i) {
        if (caller != image + zoneAllocations[i].returnRva) continue;
        typedef LPSTR (WINAPI *AllocateZone)(SIZE_T, const BYTE *, DWORD);
        const AllocateZone allocate = (AllocateZone)(PVOID)(image + zoneAllocations[i].allocatorRva);
        const SIZE_T size = (SIZE_T)manager;
        if (zoneAllocations[i].allocatorRva == 0x017570B0u && size == zoneAllocations[i].menuBytes)
            ObserveAssetHeap(image, caller);
        SetLastError(previous);
        LPSTR memory = allocate(size, asset, status);
        const DWORD last = GetLastError();
        if (zoneAllocations[i].allocatorRva == 0x017570B0u &&
                size == zoneAllocations[i].menuBytes)
            ObserveAssetArena(image, caller, size);
        if (size && size == zoneAllocations[i].menuBytes)
            TraceRemainingCommit(zoneAllocations[i].event, size, memory != NULL, caller);
        SetLastError(last);
        return memory;
    }
    if (caller != image + 0x01625EE3u && caller != image + 0x0162665Fu) {
        SetLastError(previous);
        return XboxBinkTraceGetCommandLineA();
    }
    ULONG_PTR lobbyZone = 0;
    ReadProcessMemory(GetCurrentProcess(), image + 0x06C65758u, &lobbyZone, sizeof(lobbyZone), NULL);
    if ((ULONG_PTR)asset == lobbyZone && asset) {
        TraceLiveThread("menu-zone-load-error=0x", status, detail, caller);
    } else if (asset) {
        static volatile LONG nearbyFailures;
        BYTE type = 0;
        if (ReadProcessMemory(GetCurrentProcess(), asset + 2, &type, sizeof(type), NULL) &&
                (type & 0x7f) == 1 && InterlockedIncrement(&nearbyFailures) <= 12)
            TraceLiveThread("zone-type-load-error=0x", status, detail, caller);
    }
    typedef void (WINAPI *AssetFailure)(void *, const BYTE *, DWORD, DWORD);
    const AssetFailure original = (AssetFailure)(PVOID)(image + 0x01625A90u);
    SetLastError(previous);
    original(manager, asset, status, detail);
    return NULL;
}
