/* Diagnostic census for a hosted Win32 game: timer granularity, CPU set and per-thread CPU use, and a
   committed-memory breakdown. Included once by trace.c after Text/Hex/AppendTrace are defined.
   Every line starts with "asset census-" so the filtered loader trace keeps it. */
#include <tlhelp32.h>
#include <psapi.h>
#include <dxgi1_4.h>

/* The DLL links without a C runtime; zero-initialized locals lower to memset. */
void *memset(void *target, int value, size_t count) {
    volatile unsigned char *out = (volatile unsigned char *)target;
    while (count--) *out++ = (unsigned char)value;
    return target;
}

static FARPROC CensusApi(const char *name) {
    FARPROC found = GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), name);
    return found ? found : GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name);
}
#define CENSUS_API(type, name) ((type)(PVOID)CensusApi(name))
typedef BOOL (WINAPI *CensusAffinityFn)(HANDLE, PDWORD_PTR, PDWORD_PTR);
typedef BOOL (WINAPI *CensusCpuSetFn)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
typedef BOOL (WINAPI *CensusMemoryFn)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
typedef BOOL (WINAPI *CensusIdealFn)(HANDLE, PPROCESSOR_NUMBER);
typedef BOOL (WINAPI *CensusTimesFn)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME, LPFILETIME);
typedef HANDLE (WINAPI *CensusTimerFn)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);

static char *CensusDec(char *out, unsigned long long value) {
    char digits[24];
    int count = 0;
    do digits[count++] = (char)('0' + value % 10); while (value /= 10);
    while (count) *out++ = digits[--count];
    return out;
}

static void CensusEmit(char *line, char *end) {
    end = Text(end, "\r\n");
    AppendTrace(line, (DWORD)(end - line));
}

#include "profile.h"

static unsigned long long CensusMicros(LARGE_INTEGER from, LARGE_INTEGER to, LARGE_INTEGER frequency) {
    return (unsigned long long)(to.QuadPart - from.QuadPart) * 1000000ull / (unsigned long long)frequency.QuadPart;
}

/* Average and worst duration of twenty Sleep(1) calls and twenty 1 ms high-resolution timer waits. */
static void CensusSleep(const char *phase) {
    LARGE_INTEGER frequency, before, after;
    QueryPerformanceFrequency(&frequency);
    unsigned long long total = 0, worst = 0, timerTotal = 0, timerWorst = 0;
    for (int i = 0; i < 20; ++i) {
        QueryPerformanceCounter(&before);
        Sleep(1);
        QueryPerformanceCounter(&after);
        const unsigned long long spent = CensusMicros(before, after, frequency);
        total += spent;
        if (spent > worst) worst = spent;
    }
    const CensusTimerFn createTimer = CENSUS_API(CensusTimerFn, "CreateWaitableTimerExW");
    HANDLE timer = !createTimer ? NULL : createTimer(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    const DWORD timerError = timer ? 0 : GetLastError();
    for (int i = 0; timer && i < 20; ++i) {
        LARGE_INTEGER due = {.QuadPart = -10000};
        QueryPerformanceCounter(&before);
        if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) break;
        WaitForSingleObject(timer, INFINITE);
        QueryPerformanceCounter(&after);
        const unsigned long long spent = CensusMicros(before, after, frequency);
        timerTotal += spent;
        if (spent > timerWorst) timerWorst = spent;
    }
    if (timer) CloseHandle(timer);
    ULONG minimum = 0, maximum = 0, current = 0;
    typedef LONG (NTAPI *QueryResolution)(PULONG, PULONG, PULONG);
    const QueryResolution query = (QueryResolution)(PVOID)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
        "NtQueryTimerResolution");
    const LONG status = query ? query(&minimum, &maximum, &current) : -1;
    char line[320], *out = Text(Text(line, "asset census-timer phase="), phase);
    out = CensusDec(Text(out, " sleep1-avg-us="), total / 20);
    out = CensusDec(Text(out, " sleep1-max-us="), worst);
    out = CensusDec(Text(out, " hires-avg-us="), timerTotal / 20);
    out = CensusDec(Text(out, " hires-max-us="), timerWorst);
    out = Hex(Text(out, " hires-error=0x"), timerError, 8);
    out = Hex(Text(out, " query-status=0x"), (unsigned long)status, 8);
    out = CensusDec(Text(out, " res-coarsest-100ns="), minimum);
    out = CensusDec(Text(out, " res-finest-100ns="), maximum);
    out = CensusDec(Text(out, " res-current-100ns="), current);
    CensusEmit(line, out);
}

static void CensusTimer(void) {
    CensusSleep("initial");
    typedef UINT (WINAPI *BeginPeriod)(UINT);
    HMODULE winmm = LoadLibraryW(L"winmm.dll");
    const BeginPeriod begin = winmm ? (BeginPeriod)(PVOID)GetProcAddress(winmm, "timeBeginPeriod") : NULL;
    const UINT result = begin ? begin(1) : 0xFFFFFFFFu;
    char line[96], *out = Hex(Text(line, "asset census-timer-begin-period result=0x"), result, 8);
    CensusEmit(line, out);
    CensusSleep("after-begin-period-1");
}

static void CensusCpuSets(void) {
    DWORD_PTR process = 0, system = 0;
    const CensusAffinityFn affinity = CENSUS_API(CensusAffinityFn, "GetProcessAffinityMask");
    if (affinity) affinity(GetCurrentProcess(), &process, &system);
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    static BYTE buffer[4096];
    ULONG length = 0;
    unsigned long long allocated = 0, allocatedToProcess = 0, all = 0, parked = 0;
    unsigned sets = 0;
    const CensusCpuSetFn cpuSets = CENSUS_API(CensusCpuSetFn, "GetSystemCpuSetInformation");
    if (cpuSets && cpuSets((PSYSTEM_CPU_SET_INFORMATION)buffer, sizeof(buffer), &length,
            GetCurrentProcess(), 0)) {
        for (ULONG offset = 0; offset < length;) {
            const SYSTEM_CPU_SET_INFORMATION *set = (const SYSTEM_CPU_SET_INFORMATION *)(buffer + offset);
            if (!set->Size) break;
            if (set->Type == CpuSetInformation) {
                const unsigned index = set->CpuSet.LogicalProcessorIndex;
                ++sets;
                if (index < 64) {
                    all |= 1ull << index;
                    if (set->CpuSet.Allocated) allocated |= 1ull << index;
                    if (set->CpuSet.AllocatedToTargetProcess) allocatedToProcess |= 1ull << index;
                    if (set->CpuSet.Parked) parked |= 1ull << index;
                }
            }
            offset += set->Size;
        }
    }
    char line[320], *out = Hex(Text(line, "asset census-cpu process-mask=0x"), (unsigned long)process, 8);
    out = Hex(Text(out, " system-mask=0x"), (unsigned long)system, 8);
    out = CensusDec(Text(out, " processors="), info.dwNumberOfProcessors);
    out = CensusDec(Text(out, " cpu-sets="), sets);
    out = Hex(Text(out, " set-mask=0x"), (unsigned long)all, 8);
    out = Hex(Text(out, " allocated=0x"), (unsigned long)allocated, 8);
    out = Hex(Text(out, " allocated-to-process=0x"), (unsigned long)allocatedToProcess, 8);
    out = Hex(Text(out, " parked=0x"), (unsigned long)parked, 8);
    CensusEmit(line, out);
}

typedef struct CensusThread {
    DWORD id;
    unsigned long long cpu;
} CensusThread;

static CensusThread censusThreads[512];
static unsigned censusThreadCount;

static unsigned long long CensusThreadCpu(HANDLE thread) {
    FILETIME created, exited, kernel, user;
    const CensusTimesFn times = CENSUS_API(CensusTimesFn, "GetThreadTimes");
    if (!times || !times(thread, &created, &exited, &kernel, &user)) return 0;
    return ((unsigned long long)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
        ((unsigned long long)user.dwHighDateTime << 32 | user.dwLowDateTime);
}

/* Top threads by CPU share over the interval since the previous call, with start address and name. */
static void CensusThreads(unsigned long long wallMicros, unsigned sample) {
    typedef LONG (NTAPI *QueryThread)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    typedef HRESULT (WINAPI *Describe)(HANDLE, PWSTR *);
    const QueryThread queryThread = (QueryThread)(PVOID)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
        "NtQueryInformationThread");
    const Describe describe = (Describe)(PVOID)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
        "GetThreadDescription");
    const DWORD self = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    struct { DWORD id; unsigned long long delta; ULONG_PTR start; DWORD ideal; } top[10] = {{0}};
    static CensusThread next[512];
    unsigned nextCount = 0, threads = 0;
    unsigned long long busy = 0;
    THREADENTRY32 entry = {.dwSize = sizeof(entry)};
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != self) continue;
        /* Widen every thread's affinity to the whole process mask and record masks the game had narrowed. */
        {
            static unsigned widened;
            typedef BOOL (WINAPI *GetMaskFn)(HANDLE, PDWORD_PTR, PDWORD_PTR);
            typedef DWORD_PTR (WINAPI *SetMaskFn)(HANDLE, DWORD_PTR);
            const GetMaskFn getMask = CENSUS_API(GetMaskFn, "GetProcessAffinityMask");
            const SetMaskFn setMask = CENSUS_API(SetMaskFn, "SetThreadAffinityMask");
            DWORD_PTR processMask = 0, systemMask = 0;
            if (getMask) getMask(GetCurrentProcess(), &processMask, &systemMask);
            HANDLE setter = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (setter && processMask && setMask) {
                const DWORD_PTR previous = setMask(setter, processMask);
                if (previous && previous != processMask && widened < 64) {
                    ++widened;
                    char note[120], *end = Hex(Text(note, "asset affinity-widen tid=0x"), entry.th32ThreadID, 4);
                    end = Hex(Text(end, " previous=0x"), (unsigned long)previous, 8);
                    end = Hex(Text(end, " process=0x"), (unsigned long)processMask, 8);
                    end = CensusDec(Text(end, " sample="), sample);
                    CensusEmit(note, end);
                }
            }
            if (setter) CloseHandle(setter);
        }
        HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        ++threads;
        const unsigned long long cpu = CensusThreadCpu(thread);
        unsigned long long previous = cpu;
        for (unsigned i = 0; i < censusThreadCount; ++i)
            if (censusThreads[i].id == entry.th32ThreadID) { previous = censusThreads[i].cpu; break; }
        if (nextCount < ARRAYSIZE(next)) next[nextCount++] = (CensusThread){entry.th32ThreadID, cpu};
        const unsigned long long delta = cpu - previous;
        busy += delta;
        for (unsigned slot = 0; slot < ARRAYSIZE(top); ++slot) {
            if (delta <= top[slot].delta) continue;
            for (unsigned move = ARRAYSIZE(top) - 1; move > slot; --move) top[move] = top[move - 1];
            ULONG_PTR start = 0;
            if (queryThread) queryThread(thread, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), NULL);
            PROCESSOR_NUMBER ideal = {0};
            const CensusIdealFn idealFn = CENSUS_API(CensusIdealFn, "GetThreadIdealProcessorEx");
            if (idealFn) idealFn(thread, &ideal);
            top[slot].id = entry.th32ThreadID;
            top[slot].delta = delta;
            top[slot].start = start;
            top[slot].ideal = ideal.Number;
            break;
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    for (unsigned i = 0; i < nextCount; ++i) censusThreads[i] = next[i];
    censusThreadCount = nextCount;
    if (!wallMicros) return;
    char line[512], *out = CensusDec(Text(line, "asset census-threads sample="), sample);
    out = CensusDec(Text(out, " count="), threads);
    out = CensusDec(Text(out, " wall-ms="), wallMicros / 1000);
    /* Percent of one core, summed across the process. */
    out = CensusDec(Text(out, " process-core-pct="), busy / 10 * 100 / wallMicros);
    CensusEmit(line, out);
    const ULONG_PTR image = (ULONG_PTR)GetModuleHandleW(NULL);
    for (unsigned slot = 0; slot < ARRAYSIZE(top) && top[slot].delta; ++slot) {
        out = CensusDec(Text(line, "asset census-thread sample="), sample);
        out = Hex(Text(out, " tid=0x"), top[slot].id, 4);
        out = CensusDec(Text(out, " core-pct="), top[slot].delta / 10 * 100 / wallMicros);
        HMODULE owner = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)top[slot].start, &owner);
        if ((ULONG_PTR)owner == image) out = Hex(Text(out, " start=exe+0x"), (unsigned long)(top[slot].start - image), 8);
        else {
            out = Hex(Text(out, " start-module=0x"), (unsigned long)((ULONG_PTR)owner >> 16), 8);
            out = Hex(Text(out, " start-offset=0x"), (unsigned long)(top[slot].start - (ULONG_PTR)owner), 8);
        }
        out = CensusDec(Text(out, " ideal="), top[slot].ideal);
        if (describe) {
            HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, top[slot].id);
            PWSTR name = NULL;
            if (thread && SUCCEEDED(describe(thread, &name)) && name) {
                out = Text(out, " name=");
                for (unsigned i = 0; name[i] && i < 40; ++i) *out++ = name[i] > 32 && name[i] < 127 ? (char)name[i] : '_';
                LocalFree(name);
            }
            if (thread) CloseHandle(thread);
        }
        CensusEmit(line, out);
    }
    /* Once a minute after the first two minutes, profile the two busiest threads for 2 s each. */
    if (sample >= 12 && sample % 6 == 0)
        for (unsigned slot = 0; slot < 4 && top[slot].delta; ++slot) ProfileThread(top[slot].id, sample, 300, 5);
}

/* Committed bytes by region type, the largest private allocations, and the DXGI local and
   non-local usage, so a 5 GiB process limit can be attributed. */
static void CensusMemory(unsigned sample) {
    struct { ULONG_PTR base; SIZE_T committed, reserved; DWORD protect; } top[16] = {{0}};
    SIZE_T byType[3] = {0};
    ULONG_PTR address = 0, currentBase = 0;
    SIZE_T currentCommitted = 0, currentReserved = 0;
    DWORD currentProtect = 0;
    MEMORY_BASIC_INFORMATION region;
    for (;;) {
        const BOOL valid = VirtualQuery((LPCVOID)address, &region, sizeof(region)) == sizeof(region);
        const ULONG_PTR base = valid ? (ULONG_PTR)region.AllocationBase : 0;
        if (!valid || base != currentBase) {
            if (currentBase && currentCommitted) {
                for (unsigned slot = 0; slot < ARRAYSIZE(top); ++slot) {
                    if (currentCommitted <= top[slot].committed) continue;
                    for (unsigned move = ARRAYSIZE(top) - 1; move > slot; --move) top[move] = top[move - 1];
                    top[slot].base = currentBase;
                    top[slot].committed = currentCommitted;
                    top[slot].reserved = currentReserved;
                    top[slot].protect = currentProtect;
                    break;
                }
            }
            currentBase = base;
            currentCommitted = currentReserved = 0;
            currentProtect = valid ? region.AllocationProtect : 0;
        }
        if (!valid) break;
        if (region.State != MEM_FREE) currentReserved += region.RegionSize;
        if (region.State == MEM_COMMIT) {
            if (region.Type == MEM_PRIVATE) { byType[0] += region.RegionSize; currentCommitted += region.RegionSize; }
            else if (region.Type == MEM_MAPPED) byType[1] += region.RegionSize;
            else if (region.Type == MEM_IMAGE) byType[2] += region.RegionSize;
        }
        const ULONG_PTR following = (ULONG_PTR)region.BaseAddress + region.RegionSize;
        if (following <= address) break;
        address = following;
    }
    PROCESS_MEMORY_COUNTERS_EX counters = {.cb = sizeof(counters)};
    { const CensusMemoryFn memoryInfo = CENSUS_API(CensusMemoryFn, "K32GetProcessMemoryInfo"); if (memoryInfo) memoryInfo(GetCurrentProcess(), (PPROCESS_MEMORY_COUNTERS)&counters, sizeof(counters)); }
    MEMORYSTATUSEX status = {.dwLength = sizeof(status)};
    GlobalMemoryStatusEx(&status);
    DXGI_QUERY_VIDEO_MEMORY_INFO local = {0}, nonlocal = {0};
    typedef HRESULT (WINAPI *CreateFactory)(REFIID, void **);
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    const CreateFactory create = dxgi ? (CreateFactory)(PVOID)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    IDXGIFactory1 *factory = NULL;
    HRESULT video = E_NOINTERFACE;
    static const IID factoryIid = {0x770AAE78, 0xF26F, 0x4DBA, {0xA8,0x29,0x25,0x3C,0x83,0xD1,0xB3,0x87}};
    static const IID adapter3Iid = {0x645967A4, 0x1392, 0x4310, {0xA7,0x98,0x80,0x53,0xCE,0x3E,0x93,0xFD}};
    if (create && SUCCEEDED(create(&factoryIid, (void **)&factory))) {
        IDXGIAdapter1 *adapter = NULL;
        if (SUCCEEDED(IDXGIFactory1_EnumAdapters1(factory, 0, &adapter))) {
            IDXGIAdapter3 *adapter3 = NULL;
            if (SUCCEEDED(IDXGIAdapter1_QueryInterface(adapter, &adapter3Iid, (void **)&adapter3))) {
                video = IDXGIAdapter3_QueryVideoMemoryInfo(adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
                IDXGIAdapter3_QueryVideoMemoryInfo(adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal);
                IDXGIAdapter3_Release(adapter3);
            }
            IDXGIAdapter1_Release(adapter);
        }
        IDXGIFactory1_Release(factory);
    }
    char line[400], *out = CensusDec(Text(line, "asset census-memory sample="), sample);
    out = CensusDec(Text(out, " private-commit-kib="), counters.PrivateUsage >> 10);
    out = CensusDec(Text(out, " vq-private-kib="), byType[0] >> 10);
    out = CensusDec(Text(out, " vq-mapped-kib="), byType[1] >> 10);
    out = CensusDec(Text(out, " vq-image-kib="), byType[2] >> 10);
    out = CensusDec(Text(out, " working-set-kib="), counters.WorkingSetSize >> 10);
    out = CensusDec(Text(out, " avail-commit-kib="), status.ullAvailPageFile >> 10);
    out = Hex(Text(out, " video=0x"), (unsigned long)video, 8);
    out = CensusDec(Text(out, " local-usage-kib="), local.CurrentUsage >> 10);
    out = CensusDec(Text(out, " local-budget-kib="), local.Budget >> 10);
    out = CensusDec(Text(out, " nonlocal-usage-kib="), nonlocal.CurrentUsage >> 10);
    CensusEmit(line, out);
    for (unsigned slot = 0; slot < ARRAYSIZE(top) && top[slot].committed; ++slot) {
        out = CensusDec(Text(line, "asset census-allocation sample="), sample);
        out = Hex(Text(out, " base=0x"), (unsigned long)(top[slot].base >> 32), 8);
        out = Hex(out, (unsigned long)top[slot].base, 8);
        out = CensusDec(Text(out, " committed-kib="), top[slot].committed >> 10);
        out = CensusDec(Text(out, " reserved-kib="), top[slot].reserved >> 10);
        out = Hex(Text(out, " protect=0x"), top[slot].protect, 4);
        CensusEmit(line, out);
    }
    /* Owner tables at the first memory sample and every fourth after it, so world entry is covered. */
    LazySummary(sample);
    static unsigned memoryReports;
    if (memoryReports++ % 4 == 0 && memoryReports <= 17) OwnerEmit(sample);
}

static void AllocSummaryHook(unsigned sample);
static void GpuReportHook(unsigned sample);
/* Keeps the engine's ray-tracing gate byte at 0 during startup so RayTracingManager::Initialize is skipped,
   the engine's own path for a GPU without ray tracing. Logs the first value the engine wrote. */
static DWORD WINAPI RtGateWatch(void *unused) {
    (void)unused;
    volatile BYTE *flag = (volatile BYTE *)GetModuleHandleW(NULL) + 0x07A89150u;
    unsigned cleared = 0;
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 180000) {
        const BYTE value = *flag;
        if (value) {
            *flag = 0;
            if (cleared++ < 4) {
                char note[96], *end = Hex(Text(note, "asset rt-gate-cleared value=0x"), value, 2);
                end = CensusDec(Text(end, " ms="), GetTickCount64() - start);
                CensusEmit(note, end);
            }
        }
        Sleep(1);
    }
    char note[64], *end = CensusDec(Text(note, "asset rt-gate-watch-done cleared="), cleared);
    CensusEmit(note, end);
    return 0;
}

static DWORD WINAPI CensusWatch(void *unused) {
    (void)unused;
    CensusCpuSets();
    CensusTimer();
    LARGE_INTEGER frequency, previous, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&previous);
    CensusThreads(0, 0);
    unsigned memorySamples = 0;
    ULONGLONG lastMemory = 0;
    /* Ten-second thread windows for 25 minutes; memory once a minute while commit exceeds 3.5 GiB. */
    for (unsigned sample = 1; sample <= 150; ++sample) {
        Sleep(10000);
        QueryPerformanceCounter(&now);
        CensusThreads(CensusMicros(previous, now, frequency), sample);
        previous = now;
        PROCESS_MEMORY_COUNTERS_EX counters = {.cb = sizeof(counters)};
        { const CensusMemoryFn memoryInfo = CENSUS_API(CensusMemoryFn, "K32GetProcessMemoryInfo"); if (memoryInfo) memoryInfo(GetCurrentProcess(), (PPROCESS_MEMORY_COUNTERS)&counters, sizeof(counters)); }
        const ULONGLONG tick = GetTickCount64();
        if (counters.PrivateUsage > (3584ull << 20) && memorySamples < 20 && tick - lastMemory >= 60000) {
            CensusMemory(sample);
            ++memorySamples;
            lastMemory = tick;
        }
        if (sample % 3 == 0) AllocSummaryHook(sample);
        if (sample % 6 == 0) {
            /* Ray-tracing gate bytes read at exe+0x0188A2B3/0x0188A2D2, and the manager pointer field it also checks. */
            const BYTE *image = (const BYTE *)GetModuleHandleW(NULL);
            const ULONG_PTR manager = *(const ULONG_PTR *)(image + 0x07A2CF00u);
            char note[160], *end = CensusDec(Text(note, "asset rt-gate sample="), sample);
            end = Hex(Text(end, " flag=0x"), image[0x07A89150u], 2);
            end = Hex(Text(end, " flag2=0x"), image[0x07A89151u], 2);
            end = Hex(Text(end, " device-d0=0x"), manager ? (unsigned long)(*(const ULONG_PTR *)(manager + 0xD0) != 0) : 0xFF, 2);
            CensusEmit(note, end);
        }
        if (sample >= 12 && sample % 3 == 0) GpuReportHook(sample);
        if (sample % 30 == 0) CensusSleep("periodic");
    }
    return 0;
}
