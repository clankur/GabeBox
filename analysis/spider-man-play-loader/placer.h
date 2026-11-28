/* Thread placer for the play loader (A/B experiments), included after hooks.h.

   PLACE_SPLIT: the process affinity stays at the system default and every thread gets a hard
   thread affinity instead: the host's reserved processors (0xFC), or the unreserved (shared)
   processors for threads whose priority is below normal. The scheduler keeps a thread whose mask
   includes unreserved processors on those only, so a process mask of 0xFF is not usable; the
   split is the only way to run on all eight.
   PLACE_HOT: every 2 s the two busiest threads get ideal processors on different physical cores.
   Always: every 5 s, traces the 16 busiest threads (tid, start, priority, cores, class). */

#define PL_MAX 512
typedef BOOL (WINAPI *PlCycleFn)(HANDLE, PULONG64);
typedef LONG (NTAPI *PlQueryThreadFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef struct PlThread {
    DWORD tid;
    HANDLE handle;
    ULONG_PTR start;
    ULONG64 last, window, log;
    int priority;
    BYTE cls; /* 0 unplaced, 1 reserved, 2 shared, 3 failed */
    BYTE alive;
} PlThread;
static PlThread plThreads[PL_MAX];
static DWORD plCount;
static ULONG64 plReserved, plShared;

static void PlLine(const char *prefix, ULONG64 a, ULONG64 b, ULONG64 c) {
    char line[200], *out = Text(line, prefix);
    out = Hex(Text(out, " a=0x"), (unsigned long)(a >> 32), 8); out = Hex(out, (unsigned long)a, 8);
    out = Hex(Text(out, " b=0x"), (unsigned long)(b >> 32), 8); out = Hex(out, (unsigned long)b, 8);
    out = Hex(Text(out, " c=0x"), (unsigned long)(c >> 32), 8); out = Hex(out, (unsigned long)c, 8);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

static BOOL PlMasks(void) {
    const XboxCpuSetInformationFn query =
        (XboxCpuSetInformationFn)(PVOID)XboxCpuSetApi("GetSystemCpuSetInformation");
    XboxCpuSetResult r = {0, 0, 0, 0, 0, NULL};
    const DWORD pid = XboxHostProcessId();
    HANDLE host = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
    BOOL ok = query && host && XboxReadCpuSets(query, NULL, &r) && XboxReadCpuSets(query, host, &r);
    if (host) CloseHandle(host);
    plReserved = r.owner;
    plShared = r.all & ~r.reserved;
    PlLine("asset place-masks", plReserved, plShared, ok);
    return ok && plReserved && plShared;
}

static void PlPlace(PlThread *t) {
#ifdef PLACE_SPLIT
    const BYTE want = t->priority < THREAD_PRIORITY_NORMAL ? 2 : 1;
    if (t->cls == want || t->cls == 3) return;
    if (SetThreadAffinityMask(t->handle, (DWORD_PTR)(want == 2 ? plShared : plReserved))) t->cls = want;
    else t->cls = 3;
#else
    (void)t;
#endif
}

static DWORD WINAPI PlacerThread(void *unused) {
    (void)unused;
    const PlCycleFn cycles = (PlCycleFn)(PVOID)XboxCpuSetApi("QueryThreadCycleTime");
    const PlQueryThreadFn query = (PlQueryThreadFn)(PVOID)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
        "NtQueryInformationThread");
    if (!PlMasks() || !cycles) return 0;
#ifdef PLACE_SPLIT
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)plShared);
#endif
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    const ULONG_PTR image = (ULONG_PTR)GetModuleHandleW(NULL);
    LARGE_INTEGER frequency, now, lastWindow, lastLog;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&lastWindow);
    lastLog = lastWindow;
    for (DWORD round = 0;; ++round) {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry;
            entry.dwSize = sizeof(entry);
            for (DWORD i = 0; i < plCount; ++i) plThreads[i].alive = 0;
            if (Thread32First(snapshot, &entry)) do {
                if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) goto next;
                DWORD i = 0;
                while (i < plCount && plThreads[i].tid != entry.th32ThreadID) ++i;
                if (i == plCount) {
                    if (plCount == PL_MAX) goto next;
                    HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                    if (!h) goto next;
                    PlThread *t = &plThreads[plCount++];
                    t->tid = entry.th32ThreadID;
                    t->handle = h;
                    if (query) query(h, 9, &t->start, sizeof(t->start), NULL);
                    cycles(h, &t->last);
                    t->priority = GetThreadPriority(h);
                    PlPlace(t);
                }
                plThreads[i].alive = 1;
            next:
                entry.dwSize = sizeof(entry);
            } while (Thread32Next(snapshot, &entry));
            CloseHandle(snapshot);
        }
        QueryPerformanceCounter(&now);
        const BOOL windowDue = now.QuadPart - lastWindow.QuadPart >= 2 * frequency.QuadPart;
        for (DWORD i = 0; i < plCount; ++i) {
            PlThread *t = &plThreads[i];
            if (!t->handle) continue;
            if (!t->alive) { CloseHandle(t->handle); t->handle = NULL; continue; }
            if (!(round & 7)) {
                const int priority = GetThreadPriority(t->handle);
                if (priority != THREAD_PRIORITY_ERROR_RETURN && priority != t->priority) { t->priority = priority; PlPlace(t); }
            }
            if (windowDue) {
                ULONG64 value = 0;
                if (cycles(t->handle, &value)) { t->window = value - t->last; t->log += t->window; t->last = value; }
            }
        }
        if (windowDue) {
            lastWindow = now;
#ifdef PLACE_HOT
            PlThread *a = NULL, *b = NULL;
            for (DWORD i = 0; i < plCount; ++i) {
                PlThread *t = &plThreads[i];
                if (!t->handle) continue;
                if (!a || t->window > a->window) { b = a; a = t; }
                else if (!b || t->window > b->window) b = t;
            }
            /* Lowest reserved processor of two different cores (pairs are 2n/2n+1). */
            ULONG64 cores = plReserved & 0x5555555555555555ull;
            const DWORD first = cores ? (DWORD)__builtin_ctzll(cores) : 0;
            cores &= cores - 1;
            const DWORD second = cores ? (DWORD)__builtin_ctzll(cores) : first;
            if (a) SetThreadIdealProcessor(a->handle, first);
            if (b) SetThreadIdealProcessor(b->handle, second);
#endif
        }
        if (now.QuadPart - lastLog.QuadPart >= 5 * frequency.QuadPart) {
            const double seconds = (double)(now.QuadPart - lastLog.QuadPart) / (double)frequency.QuadPart;
            lastLog = now;
            for (int n = 0; n < 16; ++n) {
                PlThread *best = NULL;
                for (DWORD i = 0; i < plCount; ++i)
                    if (plThreads[i].log && (!best || plThreads[i].log > best->log)) best = &plThreads[i];
                if (!best) break;
                /* cores x1000 at the 1,745.5 MHz cycle clock of the console */
                const ULONG64 milli = (ULONG64)((double)best->log / 1745.5e6 / seconds * 1000.0);
                const ULONG_PTR start = best->start >= image && best->start < image + 0x10000000 ? best->start - image : best->start;
                PlLine("asset place-thread", (ULONG64)best->tid << 32 | (ULONG64)(BYTE)best->priority << 8 | best->cls,
                    start, milli);
                best->log = 0;
            }
            for (DWORD i = 0; i < plCount; ++i) plThreads[i].log = 0;
        }
        Sleep(round < 3000 ? 20 : 100);
    }
}

static void PlacerStart(void) {
    HANDLE thread = CreateThread(NULL, 0, PlacerThread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
}
