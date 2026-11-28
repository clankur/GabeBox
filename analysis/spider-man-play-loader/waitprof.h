/* Wait profiler (diagnostic only, -DWAIT_PROF; needs the measure build's perf-sec thread). Samples the game's main
   and render threads on every tick (Sleep(1), about 500 a second) and two of the process's other threads round-robin:
   suspend, read RIP/RSP and up to 160 stack slots, resume. Nothing that takes a lock runs while a thread is suspended.
   Each second it writes, for main and render, the eight most frequent places, and for every other thread that used at
   least 5% of a processor, its cycles, priority (+16), start address and five most frequent places:
     asset waitprof s=N qpc=Q t=main|render n=<samples> <where>/<caller><<caller2>:<count> ...
     asset waitprof s=N qpc=Q t=w<tid> mc=<Mcycles> pri=<priority+16> start=<where> n=<samples> <places> ...
   <where> is x<RVA> of the executable function holding RIP (the raw RVA for a leaf), or <module>+<offset> of the
   first sample outside the executable; <caller> and <caller2> are the first two executable return addresses on the stack (call sites). */

#define WP_KEYS 32u
#define WP_STACK 160u

typedef struct WpKey { ULONG_PTR where, caller, caller2, first; LONG count; } WpKey;
typedef struct WpThread { HANDLE handle; const char *name; WpKey keys[WP_KEYS]; LONG samples, dropped; } WpThread;
static FARPROC PerfApi(const char *name);

static ULONG_PTR wpExeBase, wpExeEnd, wpTextBase, wpTextEnd;

static BOOL WpCallBefore(ULONG_PTR ret) {
    if (ret < wpTextBase + 8 || ret >= wpTextEnd) return FALSE;
    const BYTE *p = (const BYTE *)ret;
    if (p[-5] == 0xE8) return TRUE;                                   /* call rel32 */
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return TRUE;         /* call [rip+disp32] / [reg+disp32] */
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return TRUE;         /* call reg / [reg] */
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return TRUE;         /* call [reg+disp8] */
    return FALSE;
}

static ULONG_PTR WpWhere(ULONG_PTR rip) {
    if (rip >= wpExeBase && rip < wpExeEnd) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry((DWORD64)rip, &base, NULL);
        return entry ? (ULONG_PTR)(base + entry->BeginAddress) : (rip & ~(ULONG_PTR)0x3F);   /* leaf: 64-byte granule */
    }
    HMODULE module = NULL;
    if (findImage && findImage(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)rip, &module) && module) return (ULONG_PTR)module;
    return 0;
}

static void WpSample(WpThread *t) {
    CONTEXT context;
    ULONG_PTR stack[WP_STACK];
    SIZE_T read = 0;
    __builtin_memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_CONTROL;
    if (SuspendThread(t->handle) == (DWORD)-1) { ++t->dropped; return; }
    const BOOL got = GetThreadContext(t->handle, &context);
    if (got && !ReadProcessMemory(GetCurrentProcess(), (const void *)context.Rsp, stack, sizeof(stack), &read))
        if (!ReadProcessMemory(GetCurrentProcess(), (const void *)context.Rsp, stack, 48 * sizeof(ULONG_PTR), &read)) read = 0;
    ResumeThread(t->handle);
    if (!got) { ++t->dropped; return; }
    ULONG_PTR caller = 0, caller2 = 0;
    for (unsigned i = 0; i < read / sizeof(ULONG_PTR); ++i)
        if (WpCallBefore(stack[i])) { if (!caller) caller = stack[i]; else if (stack[i] != caller) { caller2 = stack[i]; break; } }
    const ULONG_PTR rip = (ULONG_PTR)context.Rip, where = WpWhere(rip);
    ++t->samples;
    for (unsigned k = 0; k < WP_KEYS; ++k) {
        WpKey *key = &t->keys[k];
        if (key->count && key->where == where && key->caller == caller && key->caller2 == caller2) { ++key->count; return; }
        if (!key->count) { key->where = where; key->caller = caller; key->caller2 = caller2; key->first = rip; key->count = 1; return; }
    }
    ++t->dropped;
}

static char *WpPlace(char *out, ULONG_PTR where, ULONG_PTR first) {
    if (where >= wpExeBase && where < wpExeEnd) return Hex(Text(out, "x"), (unsigned long)(where - wpExeBase), 7);
    if (!where) return Text(out, "?");
    WCHAR path[260];
    const DWORD length = GetModuleFileNameW((HMODULE)where, path, ARRAYSIZE(path));
    const WCHAR *name = path + length;
    while (length && name > path && name[-1] != L'\\') --name;
    char token[40]; unsigned i = 0;
    for (; length && name[i] && i + 1 < sizeof(token); ++i) token[i] = name[i] < 128 ? (char)name[i] : '_';
    token[i] = 0;
    out = Token(out, i ? token : "mod", 0);
    return Hex(Text(out, "+"), (unsigned long)(first - where), 6);
}

static void WpFlush(WpThread *t, unsigned second, LONGLONG qpc) {
    char line[1100], *out = PerfDec(Text(line, "asset waitprof s="), second);
    out = PerfDec(Text(out, " qpc="), (unsigned long long)qpc);
    out = Text(Text(out, " t="), t->name);
    out = PerfDec(Text(out, " n="), (unsigned long long)t->samples);
    if (t->dropped) out = PerfDec(Text(out, " drop="), (unsigned long long)t->dropped);
    for (unsigned emitted = 0; emitted < 8; ++emitted) {
        unsigned best = WP_KEYS; LONG count = 0;
        for (unsigned k = 0; k < WP_KEYS; ++k) if (t->keys[k].count > count) { count = t->keys[k].count; best = k; }
        if (best == WP_KEYS) break;
        out = WpPlace(Text(out, " "), t->keys[best].where, t->keys[best].first);
        out = t->keys[best].caller ? Hex(Text(out, "/x"), (unsigned long)(t->keys[best].caller - wpExeBase), 7) : Text(out, "/-");
        out = t->keys[best].caller2 ? Hex(Text(out, "<x"), (unsigned long)(t->keys[best].caller2 - wpExeBase), 7) : out;
        out = PerfDec(Text(out, ":"), (unsigned long long)count);
        t->keys[best].count = -t->keys[best].count;   /* emitted; cleared below */
    }
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
    __builtin_memset(t->keys, 0, sizeof(t->keys));
    t->samples = t->dropped = 0;
}

#define WP_THREADS 80u
#define WP_OTHERS_PER_TICK 2u
typedef LONG (NTAPI *WpQueryThreadFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef BOOL (WINAPI *WpCycleFn)(HANDLE, PULONG64);
static WpThread wpThreads[WP_THREADS];
static DWORD wpIds[WP_THREADS];
static ULONG_PTR wpStarts[WP_THREADS];
static ULONG64 wpCycles[WP_THREADS];
static unsigned wpCount;

/* Adds this process's threads not yet tracked (workers are sampled round-robin; main and render every tick). */
static void WpDiscover(WpQueryThreadFn query) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    const DWORD self = GetCurrentProcessId(), me = GetCurrentThreadId();
    THREADENTRY32 entry = {.dwSize = sizeof(entry)};
    for (BOOL more = Thread32First(snapshot, &entry); more && wpCount < WP_THREADS; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != self || entry.th32ThreadID == me) continue;
        unsigned i = 0;
        while (i < wpCount && wpIds[i] != entry.th32ThreadID) ++i;
        if (i < wpCount) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!h) continue;
        ULONG_PTR start = 0;
        if (query) query(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), NULL);
        wpIds[wpCount] = entry.th32ThreadID;
        wpStarts[wpCount] = start;
        wpThreads[wpCount].handle = h;
        wpThreads[wpCount].name = "w";
        ++wpCount;
    }
    CloseHandle(snapshot);
}

static void WpFlushWorker(unsigned i, unsigned second, LONGLONG qpc, WpCycleFn cycles) {
    WpThread *t = &wpThreads[i];
    ULONG64 now = 0;
    if (cycles) cycles(t->handle, &now);
    const ULONG64 mc = wpCycles[i] && now >= wpCycles[i] ? (now - wpCycles[i]) / 1000000 : 0;
    wpCycles[i] = now;
    if (mc >= 87 && t->samples) {   /* at least 5% of a processor */
        char line[900], *out = PerfDec(Text(line, "asset waitprof s="), second);
        out = PerfDec(Text(out, " qpc="), (unsigned long long)qpc);
        out = Hex(Text(out, " t=w"), wpIds[i], 4);
        out = PerfDec(Text(out, " mc="), mc);
        out = PerfDec(Text(out, " pri="), (unsigned long long)(GetThreadPriority(t->handle) + 16));
        out = WpPlace(Text(out, " start="), WpWhere(wpStarts[i]), wpStarts[i]);
        out = PerfDec(Text(out, " n="), (unsigned long long)t->samples);
        for (unsigned emitted = 0; emitted < 5; ++emitted) {
            unsigned best = WP_KEYS; LONG count = 0;
            for (unsigned k = 0; k < WP_KEYS; ++k) if (t->keys[k].count > count) { count = t->keys[k].count; best = k; }
            if (best == WP_KEYS) break;
            out = WpPlace(Text(out, " "), t->keys[best].where, t->keys[best].first);
            out = t->keys[best].caller ? Hex(Text(out, "/x"), (unsigned long)(t->keys[best].caller - wpExeBase), 7) : Text(out, "/-");
            out = t->keys[best].caller2 ? Hex(Text(out, "<x"), (unsigned long)(t->keys[best].caller2 - wpExeBase), 7) : out;
            out = PerfDec(Text(out, ":"), (unsigned long long)count);
            t->keys[best].count = -t->keys[best].count;
        }
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
    }
    __builtin_memset(t->keys, 0, sizeof(t->keys));
    t->samples = t->dropped = 0;
}

static DWORD WINAPI WpRun(void *ids) {
    const DWORD *threadIds = (const DWORD *)ids;
    const WpQueryThreadFn query = (WpQueryThreadFn)(PVOID)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    const WpCycleFn cycles = (WpCycleFn)(PVOID)PerfApi("QueryThreadCycleTime");
    for (unsigned i = 0; i < 2; ++i) {
        wpThreads[i].handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
            FALSE, threadIds[i]);
        wpThreads[i].name = i ? "render" : "main";
        wpIds[i] = threadIds[i];
    }
    wpCount = 2;
    const BYTE *image = (const BYTE *)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(image + ((const IMAGE_DOS_HEADER *)image)->e_lfanew);
    wpExeBase = (ULONG_PTR)image;
    wpExeEnd = wpExeBase + nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    wpTextBase = wpExeBase + section->VirtualAddress;
    wpTextEnd = wpTextBase + section->Misc.VirtualSize;
    char note[160], *e = Hex(Text(note, "asset waitprof-start main=0x"), threadIds[0], 4);
    e = Hex(Text(e, " render=0x"), threadIds[1], 4);
    e = Hex(Text(e, " ok=0x"), (wpThreads[0].handle ? 1u : 0u) | (wpThreads[1].handle ? 2u : 0u), 1);
    e = Text(e, " workers=1\r\n");
    AppendTrace(note, (DWORD)(e - note));
    if (!wpThreads[0].handle || !wpThreads[1].handle) return 0;
    WpDiscover(query);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    unsigned cursor = 2;
    for (unsigned second = 1; second <= 3600;) {
        Sleep(1);
        WpSample(&wpThreads[0]);
        WpSample(&wpThreads[1]);
        for (unsigned k = 0; k < WP_OTHERS_PER_TICK && wpCount > 2; ++k) {
            if (cursor >= wpCount) cursor = 2;
            WpSample(&wpThreads[cursor++]);
        }
        QueryPerformanceCounter(&now);
        if (now.QuadPart - start.QuadPart >= frequency.QuadPart) {
            WpFlush(&wpThreads[0], second, now.QuadPart);
            WpFlush(&wpThreads[1], second, now.QuadPart);
            for (unsigned i = 2; i < wpCount; ++i) WpFlushWorker(i, second, now.QuadPart, cycles);
            if (second % 5 == 0) WpDiscover(query);
            start = now;
            ++second;
        }
    }
    return 0;
}

static void WpStart(DWORD mainThread, DWORD renderThread) {
    static DWORD ids[2];
    static volatile LONG started;
    if (!mainThread || !renderThread || InterlockedExchange(&started, 1)) return;
    ids[0] = mainThread;
    ids[1] = renderThread;
    HANDLE thread = CreateThread(NULL, 0, WpRun, ids, 0, NULL);
    if (thread) CloseHandle(thread);
}
