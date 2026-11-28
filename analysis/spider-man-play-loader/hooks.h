/* Processor-query hooks for the play loader (A/B experiments). Included by trace.c after Text/Hex,
   CensusDec and AppendTrace exist.

   - Always: patches the main image's import slots for the processor APIs and traces each call
     (caller RVA, arguments, result), bounded.
   - PROC_VIEW: GetLogicalProcessorInformation(Ex), GetSystemInfo, GetNativeSystemInfo and
     GetActiveProcessorCount report only the processors in the process affinity mask. */
#include "processor-view.h"

static volatile LONG procLines;
static ULONG_PTR procImage;

static void ProcTrace(const char *what, const void *caller, ULONG64 a, ULONG64 b, ULONG64 r, DWORD error) {
    if (InterlockedIncrement(&procLines) > 400) return;
    char line[200], *out = Text(Text(line, "asset proc-call "), what);
    out = Hex(Text(out, " caller=exe+0x"), (unsigned long)((ULONG_PTR)caller - procImage), 8);
    out = Hex(Text(out, " a=0x"), (unsigned long)(a >> 32), 8); out = Hex(out, (unsigned long)a, 8);
    out = Hex(Text(out, " b=0x"), (unsigned long)(b >> 32), 8); out = Hex(out, (unsigned long)b, 8);
    out = Hex(Text(out, " r=0x"), (unsigned long)(r >> 32), 8); out = Hex(out, (unsigned long)r, 8);
    out = Hex(Text(out, " err=0x"), error, 8);
    out = Hex(Text(out, " tid=0x"), GetCurrentThreadId(), 4);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

typedef DWORD_PTR (WINAPI *ProcSetThreadAffinityFn)(HANDLE, DWORD_PTR);
typedef DWORD (WINAPI *ProcSetIdealFn)(HANDLE, DWORD);
typedef BOOL (WINAPI *ProcSetProcessAffinityFn)(HANDLE, DWORD_PTR);
typedef BOOL (WINAPI *ProcGetProcessAffinityFn)(HANDLE, PDWORD_PTR, PDWORD_PTR);
typedef BOOL (WINAPI *ProcSetPriorityFn)(HANDLE, int);
static ProcSetThreadAffinityFn procSetThreadAffinity;
static ProcSetIdealFn procSetIdeal;
static ProcSetProcessAffinityFn procSetProcessAffinity;
static ProcGetProcessAffinityFn procGetProcessAffinity;
static ProcSetPriorityFn procSetPriority;

static DWORD_PTR WINAPI ProcSetThreadAffinityMask(HANDLE thread, DWORD_PTR mask) {
    const DWORD_PTR result = procSetThreadAffinity(thread, mask);
    const DWORD error = GetLastError();
    ProcTrace("SetThreadAffinityMask", __builtin_return_address(0), mask, GetThreadId(thread), result, result ? 0 : error);
    SetLastError(error);
    return result;
}

static DWORD WINAPI ProcSetThreadIdealProcessor(HANDLE thread, DWORD ideal) {
    const DWORD result = procSetIdeal(thread, ideal);
    const DWORD error = GetLastError();
    ProcTrace("SetThreadIdealProcessor", __builtin_return_address(0), ideal, GetThreadId(thread), result,
        result == (DWORD)-1 ? error : 0);
    SetLastError(error);
    return result;
}

static BOOL WINAPI ProcSetProcessAffinityMask(HANDLE process, DWORD_PTR mask) {
    const BOOL result = procSetProcessAffinity(process, mask);
    const DWORD error = GetLastError();
    ProcTrace("SetProcessAffinityMask", __builtin_return_address(0), mask, 0, result, result ? 0 : error);
    SetLastError(error);
    return result;
}

static BOOL WINAPI ProcGetProcessAffinityMask(HANDLE process, PDWORD_PTR mask, PDWORD_PTR system) {
    const BOOL result = procGetProcessAffinity(process, mask, system);
    const DWORD error = GetLastError();
    ProcTrace("GetProcessAffinityMask", __builtin_return_address(0), result && mask ? *mask : 0,
        result && system ? *system : 0, result, result ? 0 : error);
    SetLastError(error);
    return result;
}

static BOOL WINAPI ProcSetThreadPriority(HANDLE thread, int priority) {
    const BOOL result = procSetPriority(thread, priority);
    const DWORD error = GetLastError();
    ProcTrace("SetThreadPriority", __builtin_return_address(0), (ULONG64)(LONG64)priority, GetThreadId(thread), result,
        result ? 0 : error);
    SetLastError(error);
    return result;
}

static BOOL WINAPI ProcGetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP relation,
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buffer, PDWORD length) {
    const DWORD requested = length ? *length : 0;
#ifdef PROC_VIEW
    const BOOL result = XboxProcessorViewInformationEx(relation, buffer, length);
#else
    const BOOL result = GetLogicalProcessorInformationEx(relation, buffer, length);
#endif
    const DWORD error = GetLastError();
    ProcTrace("GetLogicalProcessorInformationEx", __builtin_return_address(0), (ULONG64)relation | (ULONG64)requested << 32,
        length ? *length : 0, result, result ? 0 : error);
    if (result && buffer && length) XboxProcessorViewTraceEx(buffer, *length, AppendTrace);
    SetLastError(error);
    return result;
}

static BOOL WINAPI ProcGetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer, PDWORD length) {
    const DWORD requested = length ? *length : 0;
#ifdef PROC_VIEW
    const BOOL result = XboxProcessorViewInformation(buffer, length);
#else
    const BOOL result = GetLogicalProcessorInformation(buffer, length);
#endif
    const DWORD error = GetLastError();
    ProcTrace("GetLogicalProcessorInformation", __builtin_return_address(0), requested, length ? *length : 0, result,
        result ? 0 : error);
    SetLastError(error);
    return result;
}

static void WINAPI ProcGetSystemInfo(LPSYSTEM_INFO info) {
#ifdef PROC_VIEW
    XboxProcessorViewSystemInfo(info, FALSE);
#else
    GetSystemInfo(info);
#endif
    ProcTrace("GetSystemInfo", __builtin_return_address(0), info->dwNumberOfProcessors, info->dwActiveProcessorMask, 0, 0);
}

static void WINAPI ProcGetNativeSystemInfo(LPSYSTEM_INFO info) {
#ifdef PROC_VIEW
    XboxProcessorViewSystemInfo(info, TRUE);
#else
    GetNativeSystemInfo(info);
#endif
    ProcTrace("GetNativeSystemInfo", __builtin_return_address(0), info->dwNumberOfProcessors, info->dwActiveProcessorMask, 0, 0);
}

static BOOL ProcSameName(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

/* Replaces the main image's import slots for the named functions. Returns the number patched. */
static DWORD ProcHooksInstall(void) {
    static const struct { const char *name; void *hook; void **original; } table[] = {
        { "SetThreadAffinityMask", (void *)ProcSetThreadAffinityMask, (void **)&procSetThreadAffinity },
        { "SetThreadIdealProcessor", (void *)ProcSetThreadIdealProcessor, (void **)&procSetIdeal },
        { "SetProcessAffinityMask", (void *)ProcSetProcessAffinityMask, (void **)&procSetProcessAffinity },
        { "GetProcessAffinityMask", (void *)ProcGetProcessAffinityMask, (void **)&procGetProcessAffinity },
        { "SetThreadPriority", (void *)ProcSetThreadPriority, (void **)&procSetPriority },
        { "GetLogicalProcessorInformationEx", (void *)ProcGetLogicalProcessorInformationEx, NULL },
        { "GetLogicalProcessorInformation", (void *)ProcGetLogicalProcessorInformation, NULL },
        { "GetSystemInfo", (void *)ProcGetSystemInfo, NULL },
        { "GetNativeSystemInfo", (void *)ProcGetNativeSystemInfo, NULL },
    };
    BYTE *image = (BYTE *)GetModuleHandleW(NULL);
    procImage = (ULONG_PTR)image;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(image + ((IMAGE_DOS_HEADER *)image)->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    DWORD patched = 0, failed = 0;
    if (!dir->VirtualAddress) return 0;
    for (const IMAGE_IMPORT_DESCRIPTOR *d = (const IMAGE_IMPORT_DESCRIPTOR *)(image + dir->VirtualAddress); d->Name; ++d) {
        if (!d->OriginalFirstThunk || !d->FirstThunk) continue;
        const IMAGE_THUNK_DATA64 *names = (const IMAGE_THUNK_DATA64 *)(image + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *slots = (IMAGE_THUNK_DATA64 *)(image + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const char *name = (const char *)((const IMAGE_IMPORT_BY_NAME *)(image + names->u1.AddressOfData))->Name;
            for (UINT i = 0; i < ARRAYSIZE(table); ++i) {
                if (!ProcSameName(name, table[i].name)) continue;
                DWORD old;
                if (!VirtualProtect(&slots->u1.Function, sizeof(ULONG64), PAGE_READWRITE, &old)) { ++failed; break; }
                if (table[i].original) *table[i].original = (void *)(ULONG_PTR)slots->u1.Function;
                slots->u1.Function = (ULONG64)(ULONG_PTR)table[i].hook;
                VirtualProtect(&slots->u1.Function, sizeof(ULONG64), old, &old);
                ++patched;
                break;
            }
        }
    }
    char line[120], *out = CensusDec(Text(line, "asset proc-hooks patched="), patched);
    out = CensusDec(Text(out, " failed="), failed);
#ifdef PROC_VIEW
    out = Text(out, " view=affinity");
#else
    out = Text(out, " view=system");
#endif
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
    return patched;
}
