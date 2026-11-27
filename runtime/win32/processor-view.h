#pragma once
/* The processors a hosted Win32 child can actually run on.

   On Xbox the host restricts a hosted child to the title's reserved processors with a hard
   affinity mask (cpu-sets.h). Windows still reports the whole machine through
   GetLogicalProcessorInformation(Ex) and GetSystemInfo, so an engine that sizes its worker pool
   from those calls creates more busy workers than it has processors. Windows itself does the same
   (the topology calls ignore affinity), so this view is opt-in for titles whose job systems are
   measured to suffer from it.

   Each function calls the system function and then removes every processor outside the
   process affinity mask from the result: processor-core, cache, NUMA and package records keep
   only their processors in the mask and are dropped when none remain, and group records report
   the masked count. Buffer sizing follows the system contract: a short or null buffer returns
   FALSE with ERROR_INSUFFICIENT_BUFFER and the filtered length. Any failure of the system call is
   returned unchanged. */
#include <windows.h>

static inline ULONG64 XboxProcessorViewMask(void) {
    DWORD_PTR process = 0, system = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system) || !process) return ~0ull;
    return (ULONG64)process;
}

/* Filters one GROUP_AFFINITY for group 0; other groups are left unchanged. */
static inline BOOL XboxProcessorViewGroup(GROUP_AFFINITY *group, ULONG64 mask) {
    if (group->Group != 0) return TRUE;
    group->Mask &= (KAFFINITY)mask;
    return group->Mask != 0;
}

static inline BOOL XboxProcessorViewInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP relation,
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buffer, PDWORD length) {
    static BYTE scratch[65536];
    static SRWLOCK lock = SRWLOCK_INIT;
    if (!length) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const ULONG64 mask = XboxProcessorViewMask();
    AcquireSRWLockExclusive(&lock);
    DWORD size = sizeof(scratch);
    if (!GetLogicalProcessorInformationEx(relation, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)scratch, &size)) {
        const DWORD error = GetLastError();
        ReleaseSRWLockExclusive(&lock);
        /* Larger than the scratch buffer: hand the request to the system unchanged. */
        if (error == ERROR_INSUFFICIENT_BUFFER) return GetLogicalProcessorInformationEx(relation, buffer, length);
        SetLastError(error);
        return FALSE;
    }
    DWORD written = 0;
    for (DWORD offset = 0; offset + 8 <= size;) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX record = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(scratch + offset);
        if (!record->Size || offset + record->Size > size) break;
        offset += record->Size;
        BOOL keep = TRUE;
        switch (record->Relationship) {
        case RelationProcessorCore:
        case RelationProcessorPackage: {
            BOOL any = FALSE;
            for (WORD i = 0; i < record->Processor.GroupCount; ++i)
                any |= XboxProcessorViewGroup(&record->Processor.GroupMask[i], mask);
            keep = any;
            break;
        }
        case RelationCache:
            keep = XboxProcessorViewGroup(&record->Cache.GroupMask, mask);
            break;
        case RelationNumaNode:
            keep = XboxProcessorViewGroup(&record->NumaNode.GroupMask, mask);
            break;
        case RelationGroup:
            for (WORD i = 0; i < record->Group.ActiveGroupCount && i < 1; ++i) {
                record->Group.GroupInfo[i].ActiveProcessorMask &= (KAFFINITY)mask;
                record->Group.GroupInfo[i].ActiveProcessorCount =
                    (BYTE)__builtin_popcountll((ULONG64)record->Group.GroupInfo[i].ActiveProcessorMask);
            }
            break;
        default:
            break;
        }
        if (!keep) continue;
        if (buffer && written + record->Size <= *length) {
            /* Byte copy: the host may build without the C runtime. */
            const BYTE *from = (const BYTE *)record;
            BYTE *to = (BYTE *)buffer + written;
            for (DWORD i = 0; i < record->Size; ++i) to[i] = from[i];
        }
        written += record->Size;
    }
    ReleaseSRWLockExclusive(&lock);
    const BOOL fits = buffer && written <= *length;
    *length = written;
    if (!fits) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    return TRUE;
}

static inline BOOL XboxProcessorViewInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer, PDWORD length) {
    static SYSTEM_LOGICAL_PROCESSOR_INFORMATION scratch[512];
    static SRWLOCK lock = SRWLOCK_INIT;
    if (!length) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const ULONG64 mask = XboxProcessorViewMask();
    AcquireSRWLockExclusive(&lock);
    DWORD size = sizeof(scratch);
    if (!GetLogicalProcessorInformation(scratch, &size)) {
        const DWORD error = GetLastError();
        ReleaseSRWLockExclusive(&lock);
        if (error == ERROR_INSUFFICIENT_BUFFER) return GetLogicalProcessorInformation(buffer, length);
        SetLastError(error);
        return FALSE;
    }
    DWORD written = 0;
    for (DWORD i = 0; i < size / sizeof(scratch[0]); ++i) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION record = scratch[i];
        record.ProcessorMask &= (ULONG_PTR)mask;
        if (!record.ProcessorMask) continue;
        if (buffer && written + sizeof(record) <= *length) buffer[written / sizeof(record)] = record;
        written += sizeof(record);
    }
    ReleaseSRWLockExclusive(&lock);
    const BOOL fits = buffer && written <= *length;
    *length = written;
    if (!fits) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    return TRUE;
}

static inline void XboxProcessorViewSystemInfo(LPSYSTEM_INFO info, BOOL native) {
    if (native) GetNativeSystemInfo(info);
    else GetSystemInfo(info);
    const ULONG64 mask = XboxProcessorViewMask() & (ULONG64)info->dwActiveProcessorMask;
    if (!mask) return;
    info->dwActiveProcessorMask = (DWORD_PTR)mask;
    info->dwNumberOfProcessors = (DWORD)__builtin_popcountll(mask);
}

/* GetActiveProcessorCount is resolved at run time: the UWP umbrella library does not export it. */
static inline DWORD XboxProcessorViewActiveCount(WORD group) {
    typedef DWORD (WINAPI *ActiveCountFn)(WORD);
    HMODULE module = GetModuleHandleW(L"kernelbase.dll");
    ActiveCountFn active = module ? (ActiveCountFn)(PVOID)GetProcAddress(module, "GetActiveProcessorCount") : NULL;
    if (!active && (module = GetModuleHandleW(L"kernel32.dll")) != NULL)
        active = (ActiveCountFn)(PVOID)GetProcAddress(module, "GetActiveProcessorCount");
    if (!active) {
        SYSTEM_INFO info;
        XboxProcessorViewSystemInfo(&info, TRUE);
        return info.dwNumberOfProcessors;
    }
    const DWORD all = active(group);
    if (group != 0 && group != ALL_PROCESSOR_GROUPS) return all;
    const ULONG64 mask = XboxProcessorViewMask();
    const DWORD view = mask == ~0ull ? all : (DWORD)__builtin_popcountll(mask);
    return view && view < all ? view : all;
}

/* "asset proc-view cores=N logical=0x... core0=0x.. core1=0x.." for a returned EX buffer. */
static inline void XboxProcessorViewTraceEx(const void *buffer, DWORD size, void (*sink)(const char *, DWORD)) {
    static const char hex[] = "0123456789ABCDEF";
    char line[400], *out = line;
    const char *prefix = "asset proc-view";
    while (*prefix) *out++ = *prefix++;
    for (DWORD offset = 0; offset + 8 <= size && out < line + sizeof(line) - 40;) {
        const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *record =
            (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)((const BYTE *)buffer + offset);
        if (!record->Size) break;
        offset += record->Size;
        ULONG64 mask = 0;
        char kind = '?';
        if (record->Relationship == RelationProcessorCore) { kind = 'c'; mask = record->Processor.GroupMask[0].Mask; }
        else if (record->Relationship == RelationProcessorPackage) { kind = 'p'; mask = record->Processor.GroupMask[0].Mask; }
        else if (record->Relationship == RelationCache) { kind = (char)('0' + record->Cache.Level); mask = record->Cache.GroupMask.Mask; }
        else if (record->Relationship == RelationNumaNode) { kind = 'n'; mask = record->NumaNode.GroupMask.Mask; }
        else if (record->Relationship == RelationGroup) { kind = 'g'; mask = record->Group.GroupInfo[0].ActiveProcessorMask; }
        *out++ = ' ';
        *out++ = kind;
        *out++ = '=';
        for (int d = 3; d >= 0; --d) *out++ = hex[(mask >> (4 * d)) & 15];
    }
    *out++ = '\r';
    *out++ = '\n';
    sink(line, (DWORD)(out - line));
}
