#pragma once
/* Demand commit for hosted Windows titles.

   A PC engine commits its fixed pools and large heap blocks in full and touches only part of them.
   On Linux that costs nothing: Wine's committed pages are anonymous memory the kernel backs on first
   touch. A UWP game on Xbox has a hard commit limit (5 GiB shared by CPU and GPU), and every
   committed page counts against it whether or not the title ever touches it. Marvel's Spider-Man
   Remastered held about 320 MiB of committed, never-touched general-heap blocks in the world and
   exhausted commit during traversal.

   This header takes over a commit the host selects: it keeps the caller's reservation, commits only
   the first and last 64 KiB, and commits each further 64 KiB chunk on first touch from a vectored
   exception handler. The caller sees the same result as a full commit: the returned base is the
   first page, and untouched pages read as zero until written.

   - Kernel writes cannot fault into the handler, so ReadFile and ReadFileEx destinations inside a
     range are committed before the call (XboxDemandCommitPrepareWrite; the wrappers below). Other
     kernel writers (ReadFileScatter, DeviceIoControl output, socket receives) are not covered, so a
     host selects only commits whose buffers those calls do not target.
   - VirtualFree drops a range when a decommit covers all of it or its reservation is released, and
     the slot is reused. A new selected commit over a dropped or stale range starts a fresh range.
   - If the console refuses a chunk, the fault continues to the normal handlers, as it would for a
     commit the title never made. If the ends cannot be committed, the host falls back to the
     title's own full commit.

   The host decides which commits to take, keeps its own trace sink, and wraps the title's VirtualFree
   and ReadFileEx imports (XboxDemandCommitWrapImports) before the first selected commit can be freed.
   Nothing here depends on an AppID, an install path or a build. */
#include <windows.h>

#define XDC_CHUNK 0x10000u
#define XDC_SLOTS 256

typedef void (*XboxDemandCommitTraceFn)(const char *line, DWORD size);

typedef struct XdcRange {
    volatile ULONG_PTR base, end;   /* end == base: free slot */
    volatile LONG64 committed, reported;
    DWORD kind;                     /* host-defined; growth of kind 0 is traced every 16 MiB */
} XdcRange;

typedef struct XdcState {
    XdcRange ranges[XDC_SLOTS];
    volatile LONG count, live, handler, failures, taken, dropped;
    SRWLOCK lock;
    XboxDemandCommitTraceFn trace;
    BOOL (WINAPI *virtualFree)(LPVOID, SIZE_T, DWORD);
    BOOL (WINAPI *readFileEx)(HANDLE, LPVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE);
    BOOL (WINAPI *readFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
} XdcState;

#define XDC_WRAP_READFILE 1u /* also wrap ReadFile; a host that already prepares ReadFile omits it */

static XdcState xdcState = {.lock = SRWLOCK_INIT};

static inline char *XdcText(char *out, const char *text) {
    while (*text) *out++ = *text++;
    return out;
}

static inline char *XdcHex(char *out, unsigned long long value, int digits) {
    static const char hex[] = "0123456789ABCDEF";
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) *out++ = hex[(value >> shift) & 15];
    return out;
}

static void XdcTrace(const char *prefix, const XdcRange *range, ULONG_PTR address) {
    if (!xdcState.trace) return;
    MEMORYSTATUSEX memory = {.dwLength = sizeof(memory)};
    GlobalMemoryStatusEx(&memory);
    char line[240], *out = XdcText(line, prefix);
    out = XdcHex(XdcText(out, " base=0x"), (UINT64)range->base, 16);
    out = XdcHex(XdcText(out, " reserved-kib=0x"), (range->end - range->base) >> 10, 8);
    out = XdcHex(XdcText(out, " committed-kib=0x"), (UINT64)range->committed >> 10, 8);
    out = XdcHex(XdcText(out, " offset-kib=0x"), (address - range->base) >> 10, 8);
    out = XdcHex(XdcText(out, " available-commit-kib=0x"), memory.ullAvailPageFile >> 10, 8);
    out = XdcHex(XdcText(out, " kind=0x"), range->kind, 1);
    out = XdcText(out, "\r\n");
    xdcState.trace(line, (DWORD)(out - line));
}

static XdcRange *XdcFindLocked(ULONG_PTR address) {
    const LONG count = xdcState.count;
    for (LONG index = 0; index < count && index < XDC_SLOTS; ++index)
        if (address >= xdcState.ranges[index].base && address < xdcState.ranges[index].end) return &xdcState.ranges[index];
    return NULL;
}

/* Commits every chunk of the range overlapping [address, address + bytes). FALSE if one was refused. */
static BOOL XdcCommit(XdcRange *range, ULONG_PTR address, SIZE_T bytes) {
    const ULONG_PTR base = range->base, end = range->end;
    ULONG_PTR chunk = address & ~(ULONG_PTR)(XDC_CHUNK - 1);
    if (chunk < base) chunk = base;
    const ULONG_PTR last = address + (bytes ? bytes : 1);
    while (chunk < last && chunk < end) {
        /* A range need not start or end on a 64 KiB boundary: chunks are clipped to it. */
        const ULONG_PTR next = (chunk & ~(ULONG_PTR)(XDC_CHUNK - 1)) + XDC_CHUNK;
        const SIZE_T length = (SIZE_T)((next < end ? next : end) - chunk);
        MEMORY_BASIC_INFORMATION region;
        const BOOL present = VirtualQuery((LPCVOID)chunk, &region, sizeof(region)) == sizeof(region) &&
            region.State == MEM_COMMIT && (ULONG_PTR)region.BaseAddress + region.RegionSize >= chunk + length;
        if (!present) {
            if (!VirtualAlloc((LPVOID)chunk, length, MEM_COMMIT, PAGE_READWRITE)) {
                if (InterlockedIncrement(&xdcState.failures) <= 8) XdcTrace("asset lazy-commit-failed", range, chunk);
                return FALSE;
            }
            const LONG64 now = InterlockedAdd64(&range->committed, (LONG64)length);
            const LONG64 step = now >> 24;
            LONG64 seen = range->reported;
            while (range->kind == 0 && step > seen) {
                if (InterlockedCompareExchange64(&range->reported, step, seen) == seen) {
                    XdcTrace("asset lazy-commit-growth", range, chunk);
                    break;
                }
                seen = range->reported;
            }
        }
        chunk = next;
    }
    return TRUE;
}

static LONG WINAPI XdcFault(EXCEPTION_POINTERS *exception) {
    const EXCEPTION_RECORD *fault = exception->ExceptionRecord;
    if (fault->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || fault->NumberParameters < 2 || !xdcState.live)
        return EXCEPTION_CONTINUE_SEARCH;
    const ULONG_PTR address = fault->ExceptionInformation[1];
    const DWORD error = GetLastError();
    AcquireSRWLockShared(&xdcState.lock);
    XdcRange *range = XdcFindLocked(address);
    const BOOL committed = range && XdcCommit(range, address, 1);
    ReleaseSRWLockShared(&xdcState.lock);
    SetLastError(error);
    return committed ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

/* Drops ranges inside [from, to); with partial, also ranges that only overlap it. Lock held exclusively. */
static void XdcDropLocked(ULONG_PTR from, ULONG_PTR to, BOOL partial) {
    const LONG count = xdcState.count;
    for (LONG index = 0; index < count && index < XDC_SLOTS; ++index) {
        XdcRange *range = &xdcState.ranges[index];
        if (range->end == range->base || range->end <= from || range->base >= to) continue;
        if (!partial && (range->base < from || range->end > to)) continue;
        range->end = range->base;
        InterlockedDecrement(&xdcState.live);
        InterlockedIncrement(&xdcState.dropped);
    }
}

/* Takes over a commit of [address, address + bytes) inside an existing reservation. Returns the
   address VirtualAlloc would return, or NULL when the host should make the title's own call. */
static LPVOID XboxDemandCommitTake(LPVOID address, SIZE_T bytes, DWORD type, DWORD protection, DWORD kind,
        XboxDemandCommitTraceFn trace) {
    if (!(type & MEM_COMMIT) || (type & ~(DWORD)MEM_COMMIT) || !address || !bytes || protection != PAGE_READWRITE)
        return NULL;
    if (trace) xdcState.trace = trace;
    if (!InterlockedCompareExchange(&xdcState.handler, 1, 0)) {
        if (!AddVectoredExceptionHandler(1, XdcFault)) {
            xdcState.handler = 2;
            if (xdcState.trace) xdcState.trace("asset lazy-handler=unavailable\r\n", 32);
        } else xdcState.handler = 3;
    }
    while (xdcState.handler == 1) SwitchToThread();
    if (xdcState.handler != 3) return NULL;
    const ULONG_PTR from = (ULONG_PTR)address & ~(ULONG_PTR)0xFFF;
    const ULONG_PTR to = ((ULONG_PTR)address + bytes + 0xFFF) & ~(ULONG_PTR)0xFFF;
    {
        /* Only inside one reservation, as a plain commit requires. */
        MEMORY_BASIC_INFORMATION first, last;
        if (VirtualQuery((LPCVOID)from, &first, sizeof(first)) != sizeof(first) || first.State == MEM_FREE ||
                VirtualQuery((LPCVOID)(to - 1), &last, sizeof(last)) != sizeof(last) || last.State == MEM_FREE ||
                first.AllocationBase != last.AllocationBase) return NULL;
    }
    AcquireSRWLockExclusive(&xdcState.lock);
    XdcDropLocked(from, to, TRUE);
    XdcRange *range = NULL;
    for (LONG index = 0; index < xdcState.count && index < XDC_SLOTS; ++index)
        if (xdcState.ranges[index].end == xdcState.ranges[index].base) { range = &xdcState.ranges[index]; break; }
    if (!range && xdcState.count < XDC_SLOTS) range = &xdcState.ranges[xdcState.count];
    if (!range) {
        ReleaseSRWLockExclusive(&xdcState.lock);
        return NULL;
    }
    XdcRange probe = {.base = from, .end = to, .kind = kind};
    if (!XdcCommit(&probe, from, 1) || !XdcCommit(&probe, to - 1, 1)) {
        ReleaseSRWLockExclusive(&xdcState.lock);
        return NULL;
    }
    range->committed = probe.committed;
    range->reported = 0;
    range->kind = kind;
    range->base = from;
    range->end = to;
    if (range == &xdcState.ranges[xdcState.count]) InterlockedIncrement(&xdcState.count);
    InterlockedIncrement(&xdcState.live);
    ReleaseSRWLockExclusive(&xdcState.lock);
    InterlockedIncrement(&xdcState.taken);
    return (LPVOID)from;
}

/* Commits a kernel-write destination inside demand ranges before the kernel writes to it. */
static void XboxDemandCommitPrepareWrite(LPVOID buffer, SIZE_T length) {
    if (!buffer || !length || !xdcState.live) return;
    const ULONG_PTR from = (ULONG_PTR)buffer, to = from + length;
    AcquireSRWLockShared(&xdcState.lock);
    const LONG count = xdcState.count;
    for (LONG index = 0; index < count && index < XDC_SLOTS; ++index) {
        XdcRange *range = &xdcState.ranges[index];
        if (range->end == range->base || range->end <= from || range->base >= to) continue;
        const ULONG_PTR start = from > range->base ? from : range->base;
        const ULONG_PTR stop = to < range->end ? to : range->end;
        XdcCommit(range, start, stop - start);
    }
    ReleaseSRWLockShared(&xdcState.lock);
}

static BOOL WINAPI XboxDemandCommitVirtualFree(LPVOID address, SIZE_T bytes, DWORD type) {
    ULONG_PTR from = (ULONG_PTR)address, to = from + bytes;
    if (xdcState.live && (type & MEM_RELEASE)) {
        /* The whole reservation goes: measure it before it is released. */
        MEMORY_BASIC_INFORMATION region;
        to = from;
        while (VirtualQuery((LPCVOID)to, &region, sizeof(region)) == sizeof(region) &&
                (ULONG_PTR)region.AllocationBase == from && region.State != MEM_FREE)
            to = (ULONG_PTR)region.BaseAddress + region.RegionSize;
    }
    const BOOL result = (xdcState.virtualFree ? xdcState.virtualFree : VirtualFree)(address, bytes, type);
    if (result && xdcState.live && to > from) {
        const DWORD error = GetLastError();
        AcquireSRWLockExclusive(&xdcState.lock);
        XdcDropLocked(from & ~(ULONG_PTR)0xFFF, (to + 0xFFF) & ~(ULONG_PTR)0xFFF, (type & MEM_RELEASE) != 0);
        ReleaseSRWLockExclusive(&xdcState.lock);
        SetLastError(error);
    }
    return result;
}

static BOOL WINAPI XboxDemandCommitReadFileEx(HANDLE file, LPVOID buffer, DWORD length, LPOVERLAPPED overlapped,
        LPOVERLAPPED_COMPLETION_ROUTINE routine) {
    XboxDemandCommitPrepareWrite(buffer, length);
    return (xdcState.readFileEx ? xdcState.readFileEx : ReadFileEx)(file, buffer, length, overlapped, routine);
}

static BOOL WINAPI XboxDemandCommitReadFile(HANDLE file, LPVOID buffer, DWORD length, LPDWORD count,
        LPOVERLAPPED overlapped) {
    XboxDemandCommitPrepareWrite(buffer, length);
    return (xdcState.readFile ? xdcState.readFile : ReadFile)(file, buffer, length, count, overlapped);
}

static BOOL XdcSameName(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

/* Points the image's VirtualFree and ReadFileEx (and with XDC_WRAP_READFILE, ReadFile) import slots at
   the wrappers above, once. The previous slot values (the platform function or another adapter) are
   kept and called. Returns the number of slots patched. */
static DWORD XboxDemandCommitWrapImports(HMODULE module, DWORD flags) {
    static volatile LONG done;
    if (!module || InterlockedExchange(&done, 1)) return 0;
    BYTE *image = (BYTE *)module;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(image + ((const IMAGE_DOS_HEADER *)image)->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    DWORD patched = 0;
    if (!dir->VirtualAddress) return 0;
    for (const IMAGE_IMPORT_DESCRIPTOR *d = (const IMAGE_IMPORT_DESCRIPTOR *)(image + dir->VirtualAddress); d->Name; ++d) {
        if (!d->OriginalFirstThunk || !d->FirstThunk) continue;
        const IMAGE_THUNK_DATA64 *names = (const IMAGE_THUNK_DATA64 *)(image + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *slots = (IMAGE_THUNK_DATA64 *)(image + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const char *name = (const char *)((const IMAGE_IMPORT_BY_NAME *)(image + names->u1.AddressOfData))->Name;
            void *hook = NULL, **original = NULL;
            if (XdcSameName(name, "VirtualFree")) {
                hook = (void *)XboxDemandCommitVirtualFree;
                original = (void **)&xdcState.virtualFree;
            } else if (XdcSameName(name, "ReadFileEx")) {
                hook = (void *)XboxDemandCommitReadFileEx;
                original = (void **)&xdcState.readFileEx;
            } else if ((flags & XDC_WRAP_READFILE) && XdcSameName(name, "ReadFile")) {
                hook = (void *)XboxDemandCommitReadFile;
                original = (void **)&xdcState.readFile;
            }
            if (!hook || (void *)(ULONG_PTR)slots->u1.Function == hook) continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, sizeof(ULONG64), PAGE_READWRITE, &old)) continue;
            *original = (void *)(ULONG_PTR)slots->u1.Function;
            slots->u1.Function = (ULONG64)(ULONG_PTR)hook;
            VirtualProtect(&slots->u1.Function, sizeof(ULONG64), old, &old);
            ++patched;
        }
    }
    return patched;
}

/* One line per live range of the given kind holding at least minimum bytes. */
static inline void XboxDemandCommitSummary(const char *prefix, DWORD kind, SIZE_T minimum) {
    for (LONG index = 0; index < xdcState.count && index < XDC_SLOTS; ++index) {
        const XdcRange *range = &xdcState.ranges[index];
        if (range->end == range->base || range->kind != kind || (SIZE_T)range->committed < minimum) continue;
        XdcTrace(prefix, range, range->base);
    }
}
