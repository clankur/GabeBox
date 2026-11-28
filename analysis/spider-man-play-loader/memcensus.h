/* Memory census for the play loader (diagnostic). Included by trace.c after lazy.h, before
   XboxTraceVirtualAlloc. Lines start with "asset mc-".

   - mc-sample: process commit every 2 s for 45 min, emitted on a change of 8 MiB or every 30 s.
   - mc-census (every 60 s while commit exceeds 4 GiB, at most 16): private commit split into
     resident and untouched pages (QueryWorkingSetEx valid bit; the console has no page file, so a
     committed page that is not resident has never been touched), by protection class, the largest
     untouched allocations, every recorded engine commit of 4 MiB or more, and each lazy range. */
#include <psapi.h>

typedef BOOL (WINAPI *McQueryWsFn)(HANDLE, PVOID, DWORD);
typedef BOOL (WINAPI *McMemoryFn)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);

typedef struct McCommit {
    ULONG_PTR base;
    SIZE_T bytes;
    DWORD frame[3];
    DWORD protect;
} McCommit;

static McCommit mcCommits[512];
static volatile LONG mcCount;

static char *McDec(char *out, unsigned long long value) {
    char digits[24];
    int count = 0;
    do digits[count++] = (char)('0' + value % 10); while (value /= 10);
    while (count) *out++ = digits[--count];
    return out;
}

static void McEmit(char *line, char *out) {
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

__declspec(noinline) static void McRecord(LPVOID allocation, SIZE_T bytes, DWORD protect) {
    if (!allocation || bytes < (4u << 20)) return;
    const LONG index = InterlockedIncrement(&mcCount) - 1;
    if (index >= (LONG)ARRAYSIZE(mcCommits)) return;
    const ULONG_PTR image = (ULONG_PTR)GetModuleHandleW(NULL);
    PVOID stack[24];
    const USHORT count = RtlCaptureStackBackTrace(1, ARRAYSIZE(stack), stack, NULL);
    DWORD frame[3] = {0, 0, 0};
    unsigned found = 0;
    for (USHORT i = 0; i < count && found < 3; ++i) {
        const ULONG_PTR a = (ULONG_PTR)stack[i];
        if (a < image || a >= image + 0x08000000u) continue;
        const DWORD rva = (DWORD)(a - image);
        if (rva >= 0x01C64000u && rva < 0x01C66000u) continue;
        frame[found++] = rva;
    }
    McCommit *entry = &mcCommits[index];
    entry->frame[0] = frame[0]; entry->frame[1] = frame[1]; entry->frame[2] = frame[2];
    entry->protect = protect;
    entry->bytes = bytes;
    entry->base = (ULONG_PTR)allocation;
}

static McQueryWsFn mcQueryWs;
static PSAPI_WORKING_SET_EX_INFORMATION mcPages[4096];

/* Committed and resident bytes of private pages in [from, to). */
static void McMeasure(ULONG_PTR from, ULONG_PTR to, UINT64 *committed, UINT64 *resident) {
    *committed = *resident = 0;
    ULONG_PTR address = from;
    while (address < to) {
        MEMORY_BASIC_INFORMATION region;
        if (VirtualQuery((LPCVOID)address, &region, sizeof(region)) != sizeof(region)) break;
        ULONG_PTR end = (ULONG_PTR)region.BaseAddress + region.RegionSize;
        if (end > to) end = to;
        if (end <= address) break;
        if (region.State == MEM_COMMIT) {
            *committed += end - address;
            if (mcQueryWs) {
                for (ULONG_PTR page = address & ~(ULONG_PTR)0xFFF; page < end;) {
                    DWORD n = 0;
                    for (; n < ARRAYSIZE(mcPages) && page < end; ++n, page += 0x1000)
                        mcPages[n].VirtualAddress = (PVOID)page;
                    if (!mcQueryWs(GetCurrentProcess(), mcPages, n * (DWORD)sizeof(mcPages[0]))) continue;
                    for (DWORD i = 0; i < n; ++i) if (mcPages[i].VirtualAttributes.Valid) *resident += 0x1000;
                }
            }
        }
        address = end;
    }
}

static void McCensus(unsigned number, unsigned seconds) {
    typedef struct { ULONG_PTR base; UINT64 committed, resident, reserved; DWORD protect; } Top;
    static Top top[24];
    for (unsigned i = 0; i < ARRAYSIZE(top); ++i) top[i].base = 0, top[i].committed = top[i].resident = 0;
    UINT64 totalCommitted = 0, totalResident = 0, wcCommitted = 0, wcResident = 0, mapped = 0, imageBytes = 0;
    ULONG_PTR address = 0, currentBase = 0;
    UINT64 curCommitted = 0, curResident = 0, curReserved = 0;
    DWORD curProtect = 0;
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    for (;;) {
        MEMORY_BASIC_INFORMATION region;
        const BOOL valid = VirtualQuery((LPCVOID)address, &region, sizeof(region)) == sizeof(region);
        const ULONG_PTR base = valid ? (ULONG_PTR)region.AllocationBase : 0;
        if (!valid || base != currentBase) {
            if (currentBase && curCommitted) {
                const UINT64 untouched = curCommitted - curResident;
                for (unsigned slot = 0; slot < ARRAYSIZE(top); ++slot) {
                    if (top[slot].base && untouched <= top[slot].committed - top[slot].resident) continue;
                    for (unsigned move = ARRAYSIZE(top) - 1; move > slot; --move) top[move] = top[move - 1];
                    top[slot].base = currentBase; top[slot].committed = curCommitted; top[slot].resident = curResident;
                    top[slot].reserved = curReserved; top[slot].protect = curProtect;
                    break;
                }
            }
            currentBase = base;
            curCommitted = curResident = curReserved = 0;
            curProtect = valid ? region.AllocationProtect : 0;
        }
        if (!valid) break;
        if (region.State != MEM_FREE) curReserved += region.RegionSize;
        if (region.State == MEM_COMMIT) {
            if (region.Type == MEM_PRIVATE) {
                UINT64 c, r;
                McMeasure((ULONG_PTR)region.BaseAddress, (ULONG_PTR)region.BaseAddress + region.RegionSize, &c, &r);
                curCommitted += c; curResident += r; totalCommitted += c; totalResident += r;
                if (region.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) { wcCommitted += c; wcResident += r; }
            } else if (region.Type == MEM_MAPPED) mapped += region.RegionSize;
            else imageBytes += region.RegionSize;
        }
        const ULONG_PTR following = (ULONG_PTR)region.BaseAddress + region.RegionSize;
        if (following <= address) break;
        address = following;
    }
    UINT64 regCommitted = 0, regResident = 0, lazyCommitted = 0, lazyResident = 0;
    const LONG commits = mcCount < (LONG)ARRAYSIZE(mcCommits) ? mcCount : (LONG)ARRAYSIZE(mcCommits);
    static UINT64 regC[ARRAYSIZE(mcCommits)], regR[ARRAYSIZE(mcCommits)];
    for (LONG i = 0; i < commits; ++i) {
        regC[i] = regR[i] = 0;
        if (!mcCommits[i].base) continue;
        McMeasure(mcCommits[i].base, mcCommits[i].base + mcCommits[i].bytes, &regC[i], &regR[i]);
        regCommitted += regC[i]; regResident += regR[i];
    }
    static UINT64 lazyC[LAZY_SLOTS], lazyR[LAZY_SLOTS];
    for (LONG i = 0; i < lazyCount && i < LAZY_SLOTS; ++i) {
        lazyC[i] = lazyR[i] = 0;
        if (lazyRanges[i].end == lazyRanges[i].base) continue;
        McMeasure(lazyRanges[i].base, lazyRanges[i].end, &lazyC[i], &lazyR[i]);
        lazyCommitted += lazyC[i]; lazyResident += lazyR[i];
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    PROCESS_MEMORY_COUNTERS_EX counters = {.cb = sizeof(counters)};
    MEMORYSTATUSEX status = {.dwLength = sizeof(status)};
    GlobalMemoryStatusEx(&status);
    const McMemoryFn memoryInfo = (McMemoryFn)(PVOID)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo");
    if (memoryInfo) memoryInfo(GetCurrentProcess(), (PPROCESS_MEMORY_COUNTERS)&counters, sizeof(counters));
    char line[400], *out = McDec(Text(line, "asset mc-census n="), number);
    out = McDec(Text(out, " s="), seconds);
    out = McDec(Text(out, " ws=" ), mcQueryWs != NULL);
    out = McDec(Text(out, " commit-kib="), (status.ullTotalPageFile - status.ullAvailPageFile) >> 10);
    out = McDec(Text(out, " private-usage-kib="), counters.PrivateUsage >> 10);
    out = McDec(Text(out, " private-committed-kib="), totalCommitted >> 10);
    out = McDec(Text(out, " private-resident-kib="), totalResident >> 10);
    out = McDec(Text(out, " wc-committed-kib="), wcCommitted >> 10);
    out = McDec(Text(out, " wc-resident-kib="), wcResident >> 10);
    out = McDec(Text(out, " recorded-committed-kib="), regCommitted >> 10);
    out = McDec(Text(out, " recorded-resident-kib="), regResident >> 10);
    out = McDec(Text(out, " lazy-committed-kib="), lazyCommitted >> 10);
    out = McDec(Text(out, " lazy-resident-kib="), lazyResident >> 10);
    out = McDec(Text(out, " mapped-kib="), mapped >> 10);
    out = McDec(Text(out, " image-kib="), imageBytes >> 10);
    out = McDec(Text(out, " ms="), (unsigned long long)(t1.QuadPart - t0.QuadPart) * 1000ull / (unsigned long long)f.QuadPart);
    McEmit(line, out);
    for (unsigned slot = 0; slot < ARRAYSIZE(top) && top[slot].base; ++slot) {
        out = McDec(Text(line, "asset mc-top n="), number);
        out = Hex(Text(out, " base=0x"), (unsigned long)((UINT64)top[slot].base >> 32), 8);
        out = Hex(out, (unsigned long)top[slot].base, 8);
        out = McDec(Text(out, " committed-kib="), top[slot].committed >> 10);
        out = McDec(Text(out, " resident-kib="), top[slot].resident >> 10);
        out = McDec(Text(out, " reserved-kib="), top[slot].reserved >> 10);
        out = Hex(Text(out, " protect=0x"), top[slot].protect, 4);
        McEmit(line, out);
    }
    for (LONG i = 0; i < commits; ++i) {
        if (!regC[i]) continue;
        out = McDec(Text(line, "asset mc-buf n="), number);
        out = Hex(Text(out, " base=0x"), (unsigned long)((UINT64)mcCommits[i].base >> 32), 8);
        out = Hex(out, (unsigned long)mcCommits[i].base, 8);
        out = McDec(Text(out, " bytes-kib="), mcCommits[i].bytes >> 10);
        out = McDec(Text(out, " committed-kib="), regC[i] >> 10);
        out = McDec(Text(out, " resident-kib="), regR[i] >> 10);
        out = Hex(Text(out, " protect=0x"), mcCommits[i].protect, 4);
        out = Hex(Text(out, " f0=0x"), mcCommits[i].frame[0], 8);
        out = Hex(Text(out, " f1=0x"), mcCommits[i].frame[1], 8);
        out = Hex(Text(out, " f2=0x"), mcCommits[i].frame[2], 8);
        McEmit(line, out);
    }
    for (LONG i = 0; i < lazyCount && i < LAZY_SLOTS; ++i) {
        if (!lazyC[i] || (lazyRanges[i].kind && lazyC[i] < (8u << 20))) continue;
        out = McDec(Text(line, "asset mc-lazy n="), number);
        out = Hex(Text(out, " base=0x"), (unsigned long)((UINT64)lazyRanges[i].base >> 32), 8);
        out = Hex(out, (unsigned long)lazyRanges[i].base, 8);
        out = McDec(Text(out, " reserved-kib="), (lazyRanges[i].end - lazyRanges[i].base) >> 10);
        out = McDec(Text(out, " committed-kib="), lazyC[i] >> 10);
        out = McDec(Text(out, " resident-kib="), lazyR[i] >> 10);
        out = McDec(Text(out, " kind="), lazyRanges[i].kind);
        McEmit(line, out);
    }
}

static DWORD WINAPI McWatch(void *unused) {
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    mcQueryWs = (McQueryWsFn)(PVOID)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32QueryWorkingSetEx");
    if (!mcQueryWs) mcQueryWs = (McQueryWsFn)(PVOID)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "QueryWorkingSetEx");
    const ULONGLONG start = GetTickCount64();
    UINT64 lastEmitted = 0, peak = 0;
    ULONGLONG lastEmitTick = 0, lastCensus = 0;
    unsigned census = 0;
    while (GetTickCount64() - start < 45ull * 60 * 1000) {
        SleepEx(2000, FALSE);
        MEMORYSTATUSEX status = {.dwLength = sizeof(status)};
        if (!GlobalMemoryStatusEx(&status)) continue;
        const UINT64 commit = status.ullTotalPageFile - status.ullAvailPageFile;
        if (commit > peak) peak = commit;
        const ULONGLONG tick = GetTickCount64();
        const UINT64 delta = commit > lastEmitted ? commit - lastEmitted : lastEmitted - commit;
        if (delta >= (8ull << 20) || tick - lastEmitTick >= 30000) {
            char line[160], *out = McDec(Text(line, "asset mc-sample s="), (tick - start) / 1000);
            out = McDec(Text(out, " commit-mib="), commit >> 20);
            out = McDec(Text(out, " peak-mib="), peak >> 20);
            out = McDec(Text(out, " avail-mib="), status.ullAvailPageFile >> 20);
            McEmit(line, out);
            lastEmitted = commit;
            lastEmitTick = tick;
        }
#ifndef MC_NO_CENSUS
        if (commit > (4096ull << 20) && census < 16 && tick - lastCensus >= 60000) {
            McCensus(++census, (unsigned)((tick - start) / 1000));
            lastCensus = GetTickCount64();
        }
#else
        (void)lastCensus; (void)census;
#endif
    }
    return 0;
}
