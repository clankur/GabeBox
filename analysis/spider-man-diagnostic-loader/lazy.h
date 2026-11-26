/* Demand-committed engine pools. A PC engine commits each fixed pool in full at startup, but under the
   Xbox 5 GiB process commit limit an untouched tail is commit no other pool can use. For a pool selected
   by name, the loader keeps the engine's reservation, commits only the first and last 64 KiB on the
   engine's full-size commit request, and commits each further 64 KiB chunk on first touch from a vectored
   exception handler. ReadFile destinations inside the range are committed before the kernel writes them.
   The pool's contract is unchanged: the engine sees a successful commit of the whole range, and pages
   read as zero until written. If the console refuses a chunk, the fault continues to the normal
   handlers. Lines start with "asset lazy-". Included by trace.c before XboxTraceVirtualAlloc. */

#define LAZY_CHUNK 0x10000u

typedef struct LazyRange {
    volatile ULONG_PTR base, end;
    volatile LONG64 committed, reported;
} LazyRange;

static LazyRange lazyRanges[16];
static volatile LONG lazyCount, lazyFailures, lazyHandler;
static volatile LONG64 lazyPendingBytes;

static LazyRange *LazyFind(ULONG_PTR address) {
    const LONG count = lazyCount;
    for (LONG index = 0; index < count && index < (LONG)ARRAYSIZE(lazyRanges); ++index)
        if (address >= lazyRanges[index].base && address < lazyRanges[index].end) return &lazyRanges[index];
    return NULL;
}

static void LazyTrace(const char *prefix, const LazyRange *range, ULONG_PTR address) {
    MEMORYSTATUSEX memory = {.dwLength = sizeof(memory)};
    GlobalMemoryStatusEx(&memory);
    char line[200], *out = Text(line, prefix);
    out = Hex(Text(out, " base=0x"), (DWORD)((UINT64)range->base >> 32), 8);
    out = Hex(out, (DWORD)range->base, 8);
    out = Hex(Text(out, " reserved-kib=0x"), (DWORD)((range->end - range->base) >> 10), 8);
    out = Hex(Text(out, " committed-kib=0x"), (DWORD)((UINT64)range->committed >> 10), 8);
    out = Hex(Text(out, " offset-kib=0x"), (DWORD)((address - range->base) >> 10), 8);
    out = Hex(Text(out, " available-commit-kib=0x"), (DWORD)(memory.ullAvailPageFile >> 10), 8);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

/* Commits every chunk overlapping [address, address + bytes). Returns FALSE if the console refused one. */
static BOOL LazyCommit(LazyRange *range, ULONG_PTR address, SIZE_T bytes) {
    ULONG_PTR chunk = address & ~(ULONG_PTR)(LAZY_CHUNK - 1);
    const ULONG_PTR last = address + (bytes ? bytes : 1);
    for (; chunk < last && chunk < range->end; chunk += LAZY_CHUNK) {
        MEMORY_BASIC_INFORMATION region;
        if (VirtualQuery((LPCVOID)chunk, &region, sizeof(region)) == sizeof(region) &&
                region.State == MEM_COMMIT && region.RegionSize >= LAZY_CHUNK) continue;
        if (!VirtualAlloc((LPVOID)chunk, LAZY_CHUNK, MEM_COMMIT, PAGE_READWRITE)) {
            if (InterlockedIncrement(&lazyFailures) <= 8) LazyTrace("asset lazy-commit-failed", range, chunk);
            return FALSE;
        }
        const LONG64 now = InterlockedAdd64(&range->committed, LAZY_CHUNK);
        /* Report each further 16 MiB of growth. */
        const LONG64 step = now >> 24;
        LONG64 seen = range->reported;
        while (step > seen) {
            if (InterlockedCompareExchange64(&range->reported, step, seen) == seen) {
                LazyTrace("asset lazy-commit-growth", range, chunk);
                break;
            }
            seen = range->reported;
        }
    }
    return TRUE;
}

static LONG WINAPI LazyFault(EXCEPTION_POINTERS *exception) {
    const EXCEPTION_RECORD *fault = exception->ExceptionRecord;
    if (fault->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || fault->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    const ULONG_PTR address = fault->ExceptionInformation[1];
    LazyRange *range = LazyFind(address);
    if (!range) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD error = GetLastError();
    const BOOL committed = LazyCommit(range, address, 1);
    SetLastError(error);
    return committed ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

/* Called from the pool-size hook: the next full commit of exactly this many bytes becomes lazy. */
static void LazyExpect(UINT64 bytes) {
    InterlockedExchange64(&lazyPendingBytes, (LONG64)bytes);
}

/* Returns the address when the commit request was taken over, NULL otherwise. */
/* pool is TRUE for a full-size commit from the engine's fixed-pool constructor. */
static LPVOID LazyTakeCommit(LPVOID address, SIZE_T bytes, DWORD type, DWORD protection, BOOL pool) {
    if (!(type & MEM_COMMIT) || !address || protection != PAGE_READWRITE ||
            lazyCount >= (LONG)ARRAYSIZE(lazyRanges)) return NULL;
    const BOOL expected = lazyPendingBytes && (LONG64)bytes == lazyPendingBytes &&
        InterlockedCompareExchange64(&lazyPendingBytes, 0, (LONG64)bytes) == (LONG64)bytes;
    if (!expected && !(pool && bytes >= (16u << 20))) return NULL;
    if (!InterlockedExchange(&lazyHandler, 1)) {
        typedef PVOID (WINAPI *RegisterHandler)(ULONG, PVECTORED_EXCEPTION_HANDLER);
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        const RegisterHandler registerHandler = kernel ?
            (RegisterHandler)(void *)GetProcAddress(kernel, "AddVectoredExceptionHandler") : NULL;
        if (!registerHandler || !registerHandler(1, LazyFault)) {
            AppendTrace("asset lazy-handler=unavailable\r\n", 32);
            return NULL;
        }
    }
    LazyRange *range = &lazyRanges[lazyCount];
    range->base = (ULONG_PTR)address;
    range->end = (ULONG_PTR)address + bytes;
    range->committed = 0;
    range->reported = 0;
    InterlockedIncrement(&lazyCount);
    if (!LazyCommit(range, (ULONG_PTR)address, LAZY_CHUNK) ||
            !LazyCommit(range, range->end - LAZY_CHUNK, LAZY_CHUNK)) {
        /* Not enough commit even for the ends: fall back to the engine's own full request. */
        range->end = range->base;
        return NULL;
    }
    LazyTrace("asset lazy-range", range, (ULONG_PTR)address);
    return address;
}

/* Commits a ReadFile destination inside a lazy range before the kernel writes to it. */
static void LazyPrepareWrite(LPVOID buffer, DWORD length) {
    LazyRange *range = buffer ? LazyFind((ULONG_PTR)buffer) : NULL;
    if (range) LazyCommit(range, (ULONG_PTR)buffer, length);
}

static void LazySummary(unsigned sample) {
    for (LONG index = 0; index < lazyCount && index < (LONG)ARRAYSIZE(lazyRanges); ++index) {
        char prefix[48], *out = Hex(Text(prefix, "asset lazy-summary sample=0x"), sample, 4);
        *out = 0;
        LazyTrace(prefix, &lazyRanges[index], lazyRanges[index].base);
    }
}
