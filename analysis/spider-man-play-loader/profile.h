/* Sampling profiler for the busiest game threads. Suspends one thread at a time for a few microseconds,
   reads its instruction pointer, unwinds two frames while it is still suspended, and aggregates by the
   containing function and its caller. Lines start with "asset profile". Included by census.h. */

typedef struct ProfileEntry { ULONG_PTR function, caller, site; LONG hits; } ProfileEntry;

static ULONG_PTR ProfileFunction(ULONG_PTR pc) {
    DWORD64 base = 0;
    const PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(pc, &base, NULL);
    return entry ? (ULONG_PTR)(base + entry->BeginAddress) : pc;
}

static void ProfileThread(DWORD id, unsigned sample, unsigned count, DWORD intervalMs) {
    static ProfileEntry table[512];
    for (unsigned i = 0; i < ARRAYSIZE(table); ++i) table[i] = (ProfileEntry){0, 0, 0, 0};
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
    if (!thread) return;
    ULONG_PTR low = 0, high = 0;
    unsigned taken = 0, dropped = 0;
    for (unsigned n = 0; n < count; ++n) {
        Sleep(intervalMs);
        if (SuspendThread(thread) == (DWORD)-1) break;
        CONTEXT context;
        memset(&context, 0, sizeof(context));
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        ULONG_PTR pc = 0, callerPc = 0;
        if (GetThreadContext(thread, &context)) {
            pc = (ULONG_PTR)context.Rip;
            DWORD64 base = 0, establisher = 0;
            PVOID data = NULL;
            /* Unwind while the thread is suspended. The caller is the first game-image frame above the
               sample, or the direct caller when the sample is already in the game image. */
            const ULONG_PTR image = (ULONG_PTR)GetModuleHandleW(NULL);
            const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(image + ((const IMAGE_DOS_HEADER *)image)->e_lfanew);
            const ULONG_PTR imageEnd = image + nt->OptionalHeader.SizeOfImage;
            const BOOL inImage = pc >= image && pc < imageEnd;
            for (unsigned depth = 0; depth < 16; ++depth) {
                const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &base, NULL);
                if (!function) break;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context, &data, &establisher, NULL);
                if (!context.Rip) break;
                const ULONG_PTR frame = (ULONG_PTR)context.Rip;
                if (inImage ? depth == 0 : (frame >= image && frame < imageEnd)) { callerPc = frame; break; }
            }
        }
        ResumeThread(thread);
        if (!pc) continue;
        ++taken;
        const ULONG_PTR function = ProfileFunction(pc), caller = callerPc;
        const ULONG_PTR hash = (function ^ (caller * 31)) >> 4;
        unsigned probe = 0;
        for (; probe < ARRAYSIZE(table); ++probe) {
            ProfileEntry *e = &table[(hash + probe) % ARRAYSIZE(table)];
            if (!e->hits) { e->function = function; e->caller = caller; e->site = pc; e->hits = 1; break; }
            if (e->function == function && e->caller == caller) { ++e->hits; break; }
        }
        if (probe == ARRAYSIZE(table)) ++dropped;
    }
    CloseHandle(thread);
    (void)low; (void)high;
    char line[220], *out = Hex(Text(line, "asset profile-thread sample=0x"), sample, 4);
    out = Hex(Text(out, " tid=0x"), id, 4);
    out = Hex(Text(out, " samples=0x"), taken, 4);
    out = Hex(Text(out, " dropped=0x"), dropped, 4);
    CensusEmit(line, out);
    for (unsigned emitted = 0; emitted < 24; ++emitted) {
        unsigned best = ARRAYSIZE(table);
        for (unsigned i = 0; i < ARRAYSIZE(table); ++i)
            if (table[i].hits && (best == ARRAYSIZE(table) || table[i].hits > table[best].hits)) best = i;
        if (best == ARRAYSIZE(table)) break;
        out = Hex(Text(line, "asset profile tid=0x"), id, 4);
        out = Hex(Text(out, " hits=0x"), (DWORD)table[best].hits, 4);
        out = Text(out, " fn");
        out = RecordImageAddress(out, (const void *)table[best].function);
        out = Text(out, " pc");
        out = RecordImageAddress(out, (const void *)table[best].site);
        if (table[best].caller) {
            out = Text(out, " caller");
            out = RecordImageAddress(out, (const void *)table[best].caller);
        }
        CensusEmit(line, out);
        table[best].hits = 0;
    }
}
