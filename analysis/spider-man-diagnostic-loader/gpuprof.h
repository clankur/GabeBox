/* GPU batch profiler. For each ExecuteCommandLists call on a direct queue, a one-command list writes a
   timestamp before and after the game's lists and resolves both into a readback buffer. The census thread
   aggregates finished pairs by the submitting call site. Lines start with "asset gpuprof". Included by
   allocs.h; uses allocDevice and the device vtable hooks. */

#define GPUPROF_SETS 96u
#define GPUPROF_PAIRS 4096u

typedef void (STDMETHODCALLTYPE *GpuExecuteFn)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
typedef HRESULT (STDMETHODCALLTYPE *GpuCreateQueueFn)(ID3D12Device *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **);

typedef struct GpuStampSet {
    ID3D12CommandAllocator *allocator[2];
    ID3D12GraphicsCommandList *list[2];
    volatile UINT64 fenceValue;
} GpuStampSet;

static GpuExecuteFn gpuExecuteOriginal;
static GpuCreateQueueFn gpuCreateQueueOriginal;
static ID3D12CommandQueue *volatile gpuQueues[8];
static volatile LONG gpuQueueKind[8];
static ID3D12QueryHeap *gpuHeap;
static ID3D12Resource *gpuReadback;
static UINT64 *gpuStamps;
static ID3D12Fence *gpuFence;
static volatile LONG64 gpuFenceNext;
static GpuStampSet gpuSets[3][GPUPROF_SETS];
static ID3D12QueryHeap *gpuCopyHeap;
static volatile LONG gpuSetCursor, gpuPairCursor, gpuReady, gpuSkipped;
static struct { volatile ULONG_PTR caller; volatile UINT64 fence; volatile LONG lists, kind; } gpuPairs[GPUPROF_PAIRS];
static UINT64 gpuFrequency;
static SRWLOCK gpuLock = SRWLOCK_INIT;

/* Returns 0 for a direct queue, 1 for a compute queue, 2 for a copy queue, -1 otherwise. */
static LONG GpuQueueKind(ID3D12CommandQueue *queue) {
    for (unsigned i = 0; i < ARRAYSIZE(gpuQueues); ++i) if (gpuQueues[i] == queue) return gpuQueueKind[i];
    return -1;
}

/* Copy queues: no timestamp support, so each submission signals a fence and a watcher records when the CPU
   sees it complete. Busy time assumes the copy queue executes its submissions in order. */
#define GPUCOPY_RING 8192u
static ID3D12Fence *gpuCopyFence;
static volatile LONG64 gpuCopyNext;
static volatile LONG64 gpuCopySubmit[GPUCOPY_RING];
static volatile LONG64 gpuCopyBusy, gpuCopyBatches, gpuCopyLists, gpuCopyLatency, gpuCopyMaxLatency;
static DWORD WINAPI GpuCopyWatch(void *unused) {
    (void)unused;
    UINT64 seen = 0;
    LONG64 previousEnd = 0;
    for (;;) {
        const UINT64 completed = ID3D12Fence_GetCompletedValue(gpuCopyFence);
        if (completed == ~0ull) return 0;
        if (completed > seen) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            for (UINT64 v = seen + 1; v <= completed; ++v) {
                const LONG64 submit = gpuCopySubmit[v % GPUCOPY_RING];
                if (!submit) continue;
                const LONG64 start = submit > previousEnd ? submit : previousEnd;
                if (now.QuadPart > start) InterlockedAdd64(&gpuCopyBusy, now.QuadPart - start);
                const LONG64 latency = now.QuadPart - submit;
                InterlockedAdd64(&gpuCopyLatency, latency);
                if (latency > gpuCopyMaxLatency) gpuCopyMaxLatency = latency;
                InterlockedIncrement64(&gpuCopyBatches);
                previousEnd = now.QuadPart;
            }
            seen = completed;
        }
        YieldProcessor();
        Sleep(0);
    }
}

static BOOL GpuSetup(ID3D12CommandQueue *queue) {
    if (gpuReady) return gpuReady > 0;
    AcquireSRWLockExclusive(&gpuLock);
    if (!gpuReady) {
        static const IID heapIid = {0x0d9658ae, 0xed45, 0x469e, {0xa6, 0x1d, 0x97, 0x0e, 0xc5, 0x83, 0xca, 0xb4}};
        static const IID resourceIid = {0x696442be, 0xa72e, 0x4059, {0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad}};
        static const IID fenceIid = {0x0a753dcf, 0xc4d8, 0x4b91, {0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76}};
        static const IID allocatorIid = {0x6102dee4, 0xaf59, 0x4b09, {0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24}};
        static const IID listIid = {0x5b160d0f, 0xac1b, 0x4185, {0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55}};
        HRESULT hr = ID3D12CommandQueue_GetTimestampFrequency(queue, &gpuFrequency);
        D3D12_QUERY_HEAP_DESC heapDesc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, GPUPROF_PAIRS * 2, 0};
        if (SUCCEEDED(hr)) hr = ID3D12Device_CreateQueryHeap(allocDevice, &heapDesc, &heapIid, (void **)&gpuHeap);
        D3D12_HEAP_PROPERTIES props = {D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
        D3D12_RESOURCE_DESC desc = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, GPUPROF_PAIRS * 16, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
            D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        /* Through the original slot so the allocation hooks do not count it. */
        if (SUCCEEDED(hr)) hr = ((AllocCommittedFn)allocOriginal[27])(allocDevice, &props, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &resourceIid, (void **)&gpuReadback);
        D3D12_RANGE none = {0, 0};
        if (SUCCEEDED(hr)) hr = ID3D12Resource_Map(gpuReadback, 0, &none, (void **)&gpuStamps);
        if (SUCCEEDED(hr)) hr = ID3D12Device_CreateFence(allocDevice, 0, D3D12_FENCE_FLAG_NONE, &fenceIid, (void **)&gpuFence);
        if (SUCCEEDED(hr) && SUCCEEDED(ID3D12Device_CreateFence(allocDevice, 0, D3D12_FENCE_FLAG_NONE, &fenceIid, (void **)&gpuCopyFence))) {
            HANDLE watcher = CreateThread(NULL, 0, GpuCopyWatch, NULL, 0, NULL);
            if (watcher) CloseHandle(watcher); else gpuCopyFence = NULL;
        }
        if (SUCCEEDED(hr)) {
            D3D12_QUERY_HEAP_DESC copyDesc = {(D3D12_QUERY_HEAP_TYPE)5 /* COPY_QUEUE_TIMESTAMP */, GPUPROF_PAIRS * 2, 0};
            if (FAILED(ID3D12Device_CreateQueryHeap(allocDevice, &copyDesc, &heapIid, (void **)&gpuCopyHeap))) gpuCopyHeap = NULL;
        }
        for (unsigned t = 0; SUCCEEDED(hr) && t < 3; ++t)
        for (unsigned s = 0; SUCCEEDED(hr) && s < GPUPROF_SETS; ++s)
            for (unsigned k = 0; SUCCEEDED(hr) && k < 2; ++k) {
                const D3D12_COMMAND_LIST_TYPE type = t == 2 ? D3D12_COMMAND_LIST_TYPE_COPY : t ? D3D12_COMMAND_LIST_TYPE_COMPUTE : D3D12_COMMAND_LIST_TYPE_DIRECT;
                hr = ID3D12Device_CreateCommandAllocator(allocDevice, type, &allocatorIid,
                    (void **)&gpuSets[t][s].allocator[k]);
                if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandList(allocDevice, 0, type,
                    gpuSets[t][s].allocator[k], NULL, &listIid, (void **)&gpuSets[t][s].list[k]);
                if (SUCCEEDED(hr)) hr = ID3D12GraphicsCommandList_Close(gpuSets[t][s].list[k]);
            }
        gpuReady = SUCCEEDED(hr) ? 1 : -1;
        char line[96], *out = Hex(Text(line, "asset gpuprof-setup hr=0x"), (unsigned long)hr, 8);
        out = CensusDec(Text(out, " frequency="), gpuFrequency);
        out = Text(out, gpuCopyHeap ? " copy-heap=ok build=7" : " copy-heap=none build=7");
        CensusEmit(line, out);
    }
    ReleaseSRWLockExclusive(&gpuLock);
    return gpuReady > 0;
}

static void STDMETHODCALLTYPE GpuExecute(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists) {
    const ULONG_PTR caller = (ULONG_PTR)__builtin_return_address(0);
    const LONG kind = GpuQueueKind(queue);
    if (kind == 2 && GpuSetup(queue) && gpuCopyFence) {
        gpuExecuteOriginal(queue, count, lists);
        const UINT64 value = (UINT64)InterlockedIncrement64(&gpuCopyNext);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        gpuCopySubmit[value % GPUCOPY_RING] = now.QuadPart;
        InterlockedAdd64(&gpuCopyLists, count);
        ID3D12CommandQueue_Signal(queue, gpuCopyFence, value);
        return;
    }
    if (kind < 0 || !GpuSetup(queue) || (kind == 2 && !gpuCopyHeap)) { gpuExecuteOriginal(queue, count, lists); return; }
    ID3D12QueryHeap *heap = kind == 2 ? gpuCopyHeap : gpuHeap;
    const LONG setIndex = (LONG)((ULONG)InterlockedIncrement(&gpuSetCursor) % GPUPROF_SETS);
    GpuStampSet *set = &gpuSets[kind][setIndex];
    if (ID3D12Fence_GetCompletedValue(gpuFence) < set->fenceValue) {
        InterlockedIncrement(&gpuSkipped);
        gpuExecuteOriginal(queue, count, lists);
        return;
    }
    const UINT pair = (UINT)((ULONG)InterlockedIncrement(&gpuPairCursor) % GPUPROF_PAIRS);
    for (unsigned k = 0; k < 2; ++k) {
        ID3D12CommandAllocator_Reset(set->allocator[k]);
        ID3D12GraphicsCommandList_Reset(set->list[k], set->allocator[k], NULL);
        ID3D12GraphicsCommandList_EndQuery(set->list[k], heap, D3D12_QUERY_TYPE_TIMESTAMP, pair * 2 + k);
        if (k == 1) ID3D12GraphicsCommandList_ResolveQueryData(set->list[k], heap, D3D12_QUERY_TYPE_TIMESTAMP,
            pair * 2, 2, gpuReadback, (UINT64)pair * 16);
        ID3D12GraphicsCommandList_Close(set->list[k]);
    }
    gpuPairs[pair].fence = 0;
    ID3D12CommandList *before = (ID3D12CommandList *)set->list[0], *after = (ID3D12CommandList *)set->list[1];
    gpuExecuteOriginal(queue, 1, &before);
    gpuExecuteOriginal(queue, count, lists);
    gpuExecuteOriginal(queue, 1, &after);
    const UINT64 value = (UINT64)InterlockedIncrement64(&gpuFenceNext);
    ID3D12CommandQueue_Signal(queue, gpuFence, value);
    set->fenceValue = value;
    gpuPairs[pair].caller = caller;
    gpuPairs[pair].lists = (LONG)count;
    gpuPairs[pair].kind = kind;
    gpuPairs[pair].fence = value;
}

static HRESULT STDMETHODCALLTYPE GpuCreateQueue(ID3D12Device *self, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID iid, void **out) {
    const HRESULT result = gpuCreateQueueOriginal(self, desc, iid, out);
    if (SUCCEEDED(result) && out && *out && desc &&
            (desc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT || desc->Type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
             desc->Type == D3D12_COMMAND_LIST_TYPE_COPY)) {
        ID3D12CommandQueue *queue = (ID3D12CommandQueue *)*out;
        for (unsigned i = 0; i < ARRAYSIZE(gpuQueues); ++i)
            if (!InterlockedCompareExchangePointer((PVOID volatile *)&gpuQueues[i], queue, NULL)) {
                gpuQueueKind[i] = desc->Type == D3D12_COMMAND_LIST_TYPE_COPY ? 2 : desc->Type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? 1 : 0;
                break;
            }
        void **vtable = *(void ***)queue;
        if (vtable[10] != (void *)GpuExecute) {
            ULONG protection = 0, ignored = 0;
            if (!gpuExecuteOriginal) gpuExecuteOriginal = (GpuExecuteFn)vtable[10];
            if (VirtualProtectFromApp(&vtable[10], sizeof(void *), PAGE_READWRITE, &protection)) {
                InterlockedExchangePointer(&vtable[10], (void *)GpuExecute);
                VirtualProtectFromApp(&vtable[10], sizeof(void *), protection, &ignored);
            }
        }
        char line[96], *out2 = Hex(Text(line, "asset gpuprof-queue type=0x"), (unsigned long)desc->Type, 2);
        out2 = Hex(Text(out2, " priority=0x"), (unsigned long)desc->Priority, 8);
        CensusEmit(line, out2);
    }
    return result;
}

/* Called from the census loop: GPU milliseconds per submitting call site for pairs finished since the last report. */
static void GpuReport(unsigned sample) {
    if (gpuReady <= 0 || !gpuFrequency) return;
    static UINT64 reported;
    const UINT64 completed = ID3D12Fence_GetCompletedValue(gpuFence);
    struct { ULONG_PTR caller; UINT64 ticks; unsigned calls, lists; LONG kind; } sites[48];
    unsigned siteCount = 0, pairs = 0;
    UINT64 first = ~0ull, last = 0, busy = 0, busyKind[3] = {0, 0, 0};
    for (unsigned p = 0; p < GPUPROF_PAIRS; ++p) {
        const UINT64 fence = gpuPairs[p].fence;
        if (!fence || fence <= reported || fence > completed) continue;
        const UINT64 begin = gpuStamps[p * 2], end = gpuStamps[p * 2 + 1];
        if (end <= begin) continue;
        ++pairs;
        busy += end - begin;
        busyKind[gpuPairs[p].kind % 3] += end - begin;
        if (begin < first) first = begin;
        if (end > last) last = end;
        unsigned s = 0;
        while (s < siteCount && (sites[s].caller != gpuPairs[p].caller || sites[s].kind != gpuPairs[p].kind)) ++s;
        if (s == siteCount) {
            if (siteCount == ARRAYSIZE(sites)) continue;
            sites[siteCount].caller = gpuPairs[p].caller;
            sites[siteCount].kind = gpuPairs[p].kind;
            sites[siteCount].ticks = 0;
            sites[siteCount].calls = sites[siteCount].lists = 0;
            ++siteCount;
        }
        sites[s].ticks += end - begin;
        sites[s].calls += 1;
        sites[s].lists += (unsigned)gpuPairs[p].lists;
    }
    reported = completed;
    {
        static LONG64 lastBusy, lastBatches, lastLists, lastLatency;
        static LARGE_INTEGER lastQpc;
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const LONG64 busyNow = gpuCopyBusy, batchesNow = gpuCopyBatches, listsNow = gpuCopyLists, latencyNow = gpuCopyLatency;
        if (lastQpc.QuadPart && frequency.QuadPart) {
            const LONG64 batches = batchesNow - lastBatches;
            char copyLine[200], *c = CensusDec(Text(copyLine, "asset gpuprof-copy sample="), sample);
            c = CensusDec(Text(c, " wall-ms="), (unsigned long long)((now.QuadPart - lastQpc.QuadPart) * 1000 / frequency.QuadPart));
            c = CensusDec(Text(c, " busy-ms="), (unsigned long long)((busyNow - lastBusy) * 1000 / frequency.QuadPart));
            c = CensusDec(Text(c, " batches="), (unsigned long long)batches);
            c = CensusDec(Text(c, " lists="), (unsigned long long)(listsNow - lastLists));
            c = CensusDec(Text(c, " mean-latency-us="), batches ? (unsigned long long)((latencyNow - lastLatency) * 1000000 / frequency.QuadPart / batches) : 0);
            c = CensusDec(Text(c, " max-latency-us="), (unsigned long long)(gpuCopyMaxLatency * 1000000 / frequency.QuadPart));
            CensusEmit(copyLine, c);
            gpuCopyMaxLatency = 0;
        }
        lastQpc = now; lastBusy = busyNow; lastBatches = batchesNow; lastLists = listsNow; lastLatency = latencyNow;
    }
    if (!pairs || last <= first) return;
    const UINT64 spanUs = (last - first) * 1000000ull / gpuFrequency;
    char line[220], *out = CensusDec(Text(line, "asset gpuprof sample="), sample);
    out = CensusDec(Text(out, " pairs="), pairs);
    out = CensusDec(Text(out, " span-ms="), spanUs / 1000);
    out = CensusDec(Text(out, " busy-ms="), busy * 1000ull / gpuFrequency);
    out = CensusDec(Text(out, " direct-ms="), busyKind[0] * 1000ull / gpuFrequency);
    out = CensusDec(Text(out, " compute-ms="), busyKind[1] * 1000ull / gpuFrequency);
    out = CensusDec(Text(out, " copy-ms="), busyKind[2] * 1000ull / gpuFrequency);
    out = CensusDec(Text(out, " skipped="), (unsigned long long)gpuSkipped);
    CensusEmit(line, out);
    for (unsigned emitted = 0; emitted < siteCount && emitted < 20; ++emitted) {
        unsigned best = emitted;
        for (unsigned s = emitted + 1; s < siteCount; ++s) if (sites[s].ticks > sites[best].ticks) best = s;
        const ULONG_PTR caller = sites[best].caller; const UINT64 ticks = sites[best].ticks;
        const unsigned calls = sites[best].calls, lists = sites[best].lists;
        const LONG kind = sites[best].kind;
        sites[best] = sites[emitted];
        out = CensusDec(Text(line, "asset gpuprof-site sample="), sample);
        out = Text(out, kind == 2 ? " queue=copy" : kind ? " queue=compute" : " queue=direct");
        out = CensusDec(Text(out, " us="), ticks * 1000000ull / gpuFrequency);
        out = CensusDec(Text(out, " calls="), calls);
        out = CensusDec(Text(out, " lists="), lists);
        out = RecordImageAddress(out, (const void *)caller);
        CensusEmit(line, out);
    }
}
