/* Wait profiler (diagnostic only, -DWAIT_PROF; needs the measure build's perf-sec thread). Samples the game's main
   and render threads about every millisecond: suspend, read RIP/RSP and 48 stack slots, resume. Nothing that takes a
   lock runs while a thread is suspended. Each second it writes, per thread, the six most frequent places:
     asset waitprof s=N qpc=Q t=main|render n=<samples> <where>/<caller>:<count> ...
   <where> is x<RVA> of the executable function holding RIP (the raw RVA for a leaf), or <module>+<offset> of the
   first sample outside the executable; <caller> is the first executable return address on the stack (call site). */

#define WP_KEYS 32u
#define WP_STACK 160u

typedef struct WpKey { ULONG_PTR where, caller, caller2, first; LONG count; } WpKey;
typedef struct WpThread { HANDLE handle; const char *name; WpKey keys[WP_KEYS]; LONG samples, dropped; } WpThread;

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

static DWORD WINAPI WpRun(void *ids) {
    const DWORD *threadIds = (const DWORD *)ids;
    static WpThread threads[2] = {{NULL, "main"}, {NULL, "render"}};
    for (unsigned i = 0; i < 2; ++i)
        threads[i].handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
            FALSE, threadIds[i]);
    const BYTE *image = (const BYTE *)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(image + ((const IMAGE_DOS_HEADER *)image)->e_lfanew);
    wpExeBase = (ULONG_PTR)image;
    wpExeEnd = wpExeBase + nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    wpTextBase = wpExeBase + section->VirtualAddress;
    wpTextEnd = wpTextBase + section->Misc.VirtualSize;
    char note[160], *e = Hex(Text(note, "asset waitprof-start main=0x"), threadIds[0], 4);
    e = Hex(Text(e, " render=0x"), threadIds[1], 4);
    e = Hex(Text(e, " ok=0x"), (threads[0].handle ? 1u : 0u) | (threads[1].handle ? 2u : 0u), 1);
    e = Text(e, "\r\n");
    AppendTrace(note, (DWORD)(e - note));
    if (!threads[0].handle || !threads[1].handle) return 0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    for (unsigned second = 1; second <= 3600;) {
        Sleep(1);
        WpSample(&threads[0]);
        WpSample(&threads[1]);
        QueryPerformanceCounter(&now);
        if (now.QuadPart - start.QuadPart >= frequency.QuadPart) {
            WpFlush(&threads[0], second, now.QuadPart);
            WpFlush(&threads[1], second, now.QuadPart);
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
