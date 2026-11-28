#pragma once
/* Hosted Win32 children and the title's processors.

   On Xbox the console reserves CPU sets for the title process (the UWP host); the rest are
   the system's. A child that the host starts with CreateProcessW is given none of the
   reservation, and the scheduler keeps a thread with no explicit placement off processors
   reserved for another process, so the whole child runs on the system's two processors.
   Measured on a Series X dev kit (Spider-Man, November 26): six spinning threads in the child
   received 498 ms of CPU in 250 ms on processors 0-1; the host's reservation was 0xFC.

   Default CPU sets (SetProcessDefaultCpuSets) and per-thread selected CPU sets were accepted
   and changed nothing there. A hard affinity mask is honored: with the process affinity set
   to the host's reserved processors, the same six threads received 1500 ms on processors 2-7.
   So this header reads the processors reserved for the owner process and makes them the
   target process's affinity mask.

   The CPU-set functions are resolved at run time because the UWP umbrella library does not
   export them. Every failure leaves the target unchanged and is reported in the result. */
#include <windows.h>

typedef BOOL (WINAPI *XboxCpuSetInformationFn)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
typedef BOOL (WINAPI *XboxProcessAffinityFn)(HANDLE, DWORD_PTR);
typedef BOOL (WINAPI *XboxGetProcessAffinityFn)(HANDLE, PDWORD_PTR, PDWORD_PTR);

typedef struct XboxCpuSetResult {
    ULONG64 all;        /* group-0 logical processors with a CPU set */
    ULONG64 reserved;   /* processors reserved for some process */
    ULONG64 owner;      /* processors reserved for the owner process */
    ULONG64 applied;    /* the target's affinity mask after the call, or 0 */
    DWORD error;        /* 0 on success, otherwise the first failure */
    const char *stage;  /* the step that produced the result */
} XboxCpuSetResult;

static inline FARPROC XboxCpuSetApi(const char *name) {
    HMODULE module = GetModuleHandleW(L"kernelbase.dll");
    FARPROC found = module ? GetProcAddress(module, name) : NULL;
    if (!found && (module = GetModuleHandleW(L"kernel32.dll")) != NULL) found = GetProcAddress(module, name);
    return found;
}

static inline DWORD XboxHostProcessId(void) {
    WCHAR text[12];
    const DWORD length = GetEnvironmentVariableW(L"XBOX_STEAM_HANDOFF_PID", text, ARRAYSIZE(text));
    if (!length || length >= ARRAYSIZE(text)) return 0;
    ULONG64 value = 0;
    for (DWORD i = 0; i < length; ++i) {
        if (text[i] < L'0' || text[i] > L'9') return 0;
        value = value * 10 + (ULONG64)(text[i] - L'0');
        if (value > MAXDWORD) return 0;
    }
    return value == GetCurrentProcessId() ? 0 : (DWORD)value;
}

/* With target NULL, records every set and the reserved ones; otherwise the sets reserved for target. */
static inline BOOL XboxReadCpuSets(XboxCpuSetInformationFn query, HANDLE target, XboxCpuSetResult *result) {
    static BYTE buffer[8192];
    ULONG length = 0;
    if (!query((PSYSTEM_CPU_SET_INFORMATION)buffer, sizeof(buffer), &length,
            target ? target : GetCurrentProcess(), 0)) return FALSE;
    for (ULONG offset = 0; offset + sizeof(DWORD) * 2 <= length;) {
        const SYSTEM_CPU_SET_INFORMATION *set = (const SYSTEM_CPU_SET_INFORMATION *)(buffer + offset);
        if (!set->Size) break;
        if (set->Type == CpuSetInformation && set->CpuSet.Group == 0 && set->CpuSet.LogicalProcessorIndex < 64) {
            const ULONG64 bit = 1ull << set->CpuSet.LogicalProcessorIndex;
            if (!target) {
                result->all |= bit;
                if (set->CpuSet.Allocated) result->reserved |= bit;
            } else if (set->CpuSet.AllocatedToTargetProcess) {
                result->owner |= bit;
            }
        }
        offset += set->Size;
    }
    return TRUE;
}

/* Restricts target to the processors reserved for owner. Does nothing when no processor is
   reserved, when the owner holds none, or when the target's mask is already that set. */
static inline XboxCpuSetResult XboxShareReservedProcessors(HANDLE owner, HANDLE target) {
    XboxCpuSetResult result = {0, 0, 0, 0, 0, NULL};
    const DWORD previous = GetLastError();
    const XboxCpuSetInformationFn query =
        (XboxCpuSetInformationFn)(PVOID)XboxCpuSetApi("GetSystemCpuSetInformation");
    const XboxProcessAffinityFn assign = (XboxProcessAffinityFn)(PVOID)XboxCpuSetApi("SetProcessAffinityMask");
    const XboxGetProcessAffinityFn current =
        (XboxGetProcessAffinityFn)(PVOID)XboxCpuSetApi("GetProcessAffinityMask");
    DWORD_PTR process = 0, system = 0;
    if (!query || !assign || !current) {
        result.error = ERROR_PROC_NOT_FOUND;
        result.stage = "resolve";
    } else if (!XboxReadCpuSets(query, NULL, &result)) {
        result.error = GetLastError();
        result.stage = "query-system";
    } else if (!result.reserved) {
        result.stage = "none-reserved";
    } else if (!XboxReadCpuSets(query, owner, &result)) {
        result.error = GetLastError();
        result.stage = "query-owner";
    } else if (!result.owner) {
        result.stage = "owner-has-none";
    } else if (!current(target, &process, &system)) {
        result.error = GetLastError();
        result.stage = "query-target";
    } else if ((result.owner & system) != result.owner) {
        result.error = ERROR_INVALID_DATA;
        result.stage = "outside-system-mask";
    } else if ((ULONG64)process == result.owner) {
        result.applied = result.owner;
        result.stage = "already-applied";
    } else if (!assign(target, (DWORD_PTR)result.owner)) {
        result.error = GetLastError();
        result.stage = "assign";
    } else {
        result.applied = result.owner;
        result.stage = "applied";
    }
    SetLastError(previous);
    return result;
}

/* Child side: take the processors reserved for the host named by XBOX_STEAM_HANDOFF_PID. */
static inline XboxCpuSetResult XboxUseHostProcessors(void) {
    const DWORD previous = GetLastError();
    const DWORD pid = XboxHostProcessId();
    HANDLE host = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
    XboxCpuSetResult result = {0, 0, 0, 0, pid ? GetLastError() : ERROR_INVALID_DATA, "open-host"};
    if (host) {
        result = XboxShareReservedProcessors(host, GetCurrentProcess());
        CloseHandle(host);
    }
    SetLastError(previous);
    return result;
}

/* "asset host-processors stage=applied reserved=0x... host=0x... applied=0x... error=0x..." */
static inline char *XboxFormatProcessors(char *out, const XboxCpuSetResult *result) {
    static const char prefix[] = "asset host-processors stage=";
    static const char hex[] = "0123456789ABCDEF";
    for (const char *p = prefix; *p;) *out++ = *p++;
    for (const char *p = result->stage ? result->stage : "unknown"; *p;) *out++ = *p++;
    const char *labels[] = {" reserved=0x", " host=0x", " applied=0x", " error=0x"};
    const ULONG64 values[] = {result->reserved, result->owner, result->applied, result->error};
    for (unsigned i = 0; i < 4; ++i) {
        for (const char *p = labels[i]; *p;) *out++ = *p++;
        for (int digit = 7; digit >= 0; --digit) *out++ = hex[(values[i] >> (4 * digit)) & 15];
    }
    *out++ = '\r';
    *out++ = '\n';
    return out;
}
