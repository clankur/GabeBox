/* Per-second performance counters for stall attribution (diagnostic; included by trace.c after the
   pipeline hook helpers). One line per second:
     asset perf-sec s=N qpc=Q cpu-ms=<process user+kernel ms> main-mc=<main thread Mcycles>
       render-mc=<render thread Mcycles> faults=<demand-commit faults> pso-n=<game compiles>
       pso-ms=<game compile ms> commit-mib=<process commit> avail-mib=<commit available>
       xa-lat=<XAudio2 latency samples> xa-rate=<mastering rate> xa-glitch=<glitches since start>
       xa-src=<active source voices> xa-pct=<audio thread % of a core, x10>
   XAudio2 2.7 GetPerformanceData is documented callable from any thread. */
volatile LONG perfFaults;          /* incremented by the demand-commit fault handler */
static volatile LONG perfPsoCount;
static volatile LONG perfReads;
static LARGE_INTEGER perfQpcFrequency = {.QuadPart = 1};
static volatile LONG64 perfReadBytes, perfReadTicks;
static volatile LONG64 perfPsoMicros;
static void *volatile perfXAudio;  /* IXAudio2 (2.7), referenced */
static volatile LONG perfXAudioRate;

typedef struct PerfXa27 {           /* XAUDIO2_PERFORMANCE_DATA, XAudio2 2.7 */
    UINT64 audioCycles, totalCycles;
    UINT32 minCycles, maxCycles, memory, latency, glitches, activeSources, totalSources,
        activeSubmix, resamplers, matrixMixes, xmaSources, xmaStreams;
    UINT32 pad[8];
} PerfXa27;

static FARPROC PerfApi(const char *name) {
    static const WCHAR *modules[] = {L"kernelbase.dll", L"kernel32.dll", L"api-ms-win-core-processthreads-l1-1-0.dll"};
    for (unsigned i = 0; i < 3; ++i) {
        HMODULE module = GetModuleHandleW(modules[i]);
        FARPROC proc = module ? GetProcAddress(module, name) : NULL;
        if (proc) return proc;
    }
    return NULL;
}

static char *PerfDec(char *out, unsigned long long value) {
    char digits[24]; int n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) *out++ = digits[--n];
    return out;
}

static void PerfXAudioCreated(void *xaudio, void *masteringVoice) {
    if (!xaudio || InterlockedCompareExchangePointer((PVOID volatile *)&perfXAudio, NULL, NULL)) return;
    if (masteringVoice) {
        typedef void (WINAPI *Details)(void *, UINT32 *);
        UINT32 details[4] = {0};
        ((Details)(*(void ***)masteringVoice)[0])(masteringVoice, details);
        InterlockedExchange(&perfXAudioRate, (LONG)details[2]);
    }
    typedef ULONG (WINAPI *AddRefFn)(void *);
    ((AddRefFn)(*(void ***)xaudio)[1])(xaudio);
    InterlockedExchangePointer((PVOID volatile *)&perfXAudio, xaudio);
}

static DWORD PerfRenderThread(void) {
    struct { DWORD state, flags; UINT64 threadId; } info = {0};
    SIZE_T read = 0;
    const BYTE *image = (const BYTE *)GetModuleHandleW(NULL);
    if (!ReadProcessMemory(GetCurrentProcess(), image + 0x07A2FD68u, &info, sizeof(info), &read) ||
            read != sizeof(info) || info.threadId > MAXDWORD) return 0;
    return (DWORD)info.threadId;
}

static DWORD WINAPI PerfSecond(void *mainThread) {
    typedef BOOL (WINAPI *TimesFn)(HANDLE, FILETIME *, FILETIME *, FILETIME *, FILETIME *);
    typedef BOOL (WINAPI *CycleFn)(HANDLE, PULONG64);
    const TimesFn times = (TimesFn)(PVOID)PerfApi("GetProcessTimes");
    const CycleFn cycles = (CycleFn)(PVOID)PerfApi("QueryThreadCycleTime");
    HANDLE main = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)(ULONG_PTR)mainThread);
    HANDLE render = NULL;
    DWORD renderId = 0;
    UINT64 lastCpu = 0, lastMain = 0, lastRender = 0, lastAudio = 0, lastTotal = 0;
    LONG lastFaults = 0, lastPso = 0;
    LONG64 lastPsoMicros = 0, lastReadBytes = 0, lastReadTicks = 0;
    LONG lastReads = 0;
    QueryPerformanceFrequency(&perfQpcFrequency);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (unsigned long long second = 0; second < 6 * 3600; ++second) {
        Sleep(1000);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!render) {
            renderId = PerfRenderThread();
            if (renderId) render = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, renderId);
            if (render && cycles) cycles(render, &lastRender);
        }
        UINT64 cpu = 0, mainCycles = 0, renderCycles = 0;
        FILETIME created, exited, kernel, user;
        if (times && times(GetCurrentProcess(), &created, &exited, &kernel, &user))
            cpu = (((UINT64)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
                (((UINT64)user.dwHighDateTime << 32) | user.dwLowDateTime);
        if (cycles && main) cycles(main, &mainCycles);
        if (cycles && render) cycles(render, &renderCycles);
        const LONG faults = perfFaults, pso = perfPsoCount;
        const LONG reads = perfReads;
        const LONG64 readBytes = perfReadBytes, readTicks = perfReadTicks;
#ifdef GPU_PROF
        if (second && second % 10 == 0) GpuReport((unsigned)(second + 1));
        if (second && second % 5 == 0) QtlFrames((unsigned)(second + 1));
#endif
#ifdef WAIT_PROF
        if (renderId) WpStart((DWORD)(ULONG_PTR)mainThread, renderId);
#endif
        const LONG64 psoMicros = perfPsoMicros;
        MEMORYSTATUSEX memory = {.dwLength = sizeof(memory)};
        GlobalMemoryStatusEx(&memory);
        PerfXa27 xa = {0};
        void *xaudio = InterlockedCompareExchangePointer((PVOID volatile *)&perfXAudio, NULL, NULL);
        if (xaudio) {
            typedef void (WINAPI *PerfFn)(void *, PerfXa27 *);
            ((PerfFn)(*(void ***)xaudio)[14])(xaudio, &xa);
        }
        char line[400], *out = Text(line, "asset perf-sec s=");
        out = PerfDec(out, second + 1);
        out = PerfDec(Text(out, " qpc="), (unsigned long long)now.QuadPart);
        out = PerfDec(Text(out, " cpu-ms="), second ? (cpu - lastCpu) / 10000 : 0);
        out = PerfDec(Text(out, " main-mc="), second ? (mainCycles - lastMain) / 1000000 : 0);
        out = PerfDec(Text(out, " render-mc="), lastRender ? (renderCycles - lastRender) / 1000000 : 0);
        out = Hex(Text(out, " render-tid=0x"), renderId, 4);
        out = PerfDec(Text(out, " faults="), (unsigned long long)(faults - lastFaults));
        out = PerfDec(Text(out, " pso-n="), (unsigned long long)(pso - lastPso));
        out = PerfDec(Text(out, " pso-ms="), (unsigned long long)((psoMicros - lastPsoMicros) / 1000));
        out = PerfDec(Text(out, " commit-mib="), (memory.ullTotalPageFile - memory.ullAvailPageFile) >> 20);
        out = PerfDec(Text(out, " avail-mib="), memory.ullAvailPageFile >> 20);
        out = PerfDec(Text(out, " rd-n="), (unsigned long long)(reads - lastReads));
        out = PerfDec(Text(out, " rd-kib="), (unsigned long long)((readBytes - lastReadBytes) >> 10));
        out = PerfDec(Text(out, " rd-ms="), (unsigned long long)((readTicks - lastReadTicks) * 1000 / perfQpcFrequency.QuadPart));
        if (xaudio) {
            out = PerfDec(Text(out, " xa-lat="), xa.latency);
            out = PerfDec(Text(out, " xa-rate="), (unsigned long long)perfXAudioRate);
            out = PerfDec(Text(out, " xa-glitch="), xa.glitches);
            out = PerfDec(Text(out, " xa-src="), xa.activeSources);
            out = PerfDec(Text(out, " xa-pct10="), xa.totalCycles ? xa.audioCycles * 1000 / xa.totalCycles : 0);
            out = PerfDec(Text(out, " xa-maxq="), xa.maxCycles);
        }
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
        lastCpu = cpu; lastMain = mainCycles; lastRender = renderCycles;
        lastFaults = faults; lastPso = pso; lastPsoMicros = psoMicros;
        lastReads = reads; lastReadBytes = readBytes; lastReadTicks = readTicks;
        (void)lastAudio; (void)lastTotal;
    }
    return 0;
}

static void PerfStart(DWORD mainThread) {
#ifdef NO_PERF_SEC
    (void)mainThread;
    return;
#endif
    static volatile LONG started;
    if (InterlockedExchange(&started, 1)) return;
    HANDLE thread = CreateThread(NULL, 0, PerfSecond, (void *)(ULONG_PTR)mainThread, 0, NULL);
    if (thread) CloseHandle(thread);
}

/* A 64-bit identity of a pipeline's shaders, for pairing compiles across runs. */
static UINT64 PerfShaderKey(BOOL graphics, const void *desc) {
    if (!graphics) {
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *c = (const D3D12_COMPUTE_PIPELINE_STATE_DESC *)desc;
        return c->CS.pShaderBytecode ? XprHash(c->CS.pShaderBytecode, c->CS.BytecodeLength) : 0;
    }
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *g = (const D3D12_GRAPHICS_PIPELINE_STATE_DESC *)desc;
    UINT64 key = g->VS.pShaderBytecode ? XprHash(g->VS.pShaderBytecode, g->VS.BytecodeLength) : 0;
    if (g->PS.pShaderBytecode) key ^= XprRotate(XprHash(g->PS.pShaderBytecode, g->PS.BytecodeLength), 17);
    return key;
}
