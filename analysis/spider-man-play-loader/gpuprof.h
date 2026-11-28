/* GPU batch profiler. For each ExecuteCommandLists call on a direct queue, a one-command list writes a
   timestamp before and after the game's lists and resolves both into a readback buffer. The census thread
   aggregates finished pairs by the submitting call site. Lines start with "asset gpuprof". Included by
   allocs.h; uses allocDevice and the device vtable hooks. */

#include "listprof.h"

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

/* Fence-based GPU timing: each submission on a watched queue is followed by a Signal on that queue's own fence. A
   watcher records when the CPU sees each value complete; a submission's GPU time is its completion minus the later
   of its submit time and the previous completion on the same queue. */
#define GPUQ_SLOTS 8u
#define GPUQ_RING 4096u
static ID3D12Fence *gpuQFence[GPUQ_SLOTS];
static volatile LONG64 gpuQNext[GPUQ_SLOTS];
static volatile LONG64 gpuQSubmit[GPUQ_SLOTS][GPUQ_RING];
static volatile ULONG_PTR gpuQCaller[GPUQ_SLOTS][GPUQ_RING];
static BatchInfo gpuQBatch[GPUQ_SLOTS][GPUQ_RING];
static ID3D12Fence *gpuCopyFence; /* non-null once fence timing is ready */
static volatile LONG64 gpuCopyBusy, gpuCopyBatches, gpuCopyLists, gpuCopyLatency, gpuCopyMaxLatency;
#define GPUFENCE_SITES 256
static struct { volatile ULONG_PTR caller; volatile LONG kind; volatile LONG64 busy, count, c[LP_COUNT]; char name[40]; } gpuFenceSites[GPUFENCE_SITES];
static volatile LONG gpuQKind[GPUQ_SLOTS];
static int GpuNameEqual(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static void GpuFenceCredit(ULONG_PTR caller, LONG kind, LONG64 busy, const BatchInfo *info) {
    for (unsigned i = 0; i < GPUFENCE_SITES; ++i) {
        if (gpuFenceSites[i].caller == caller && gpuFenceSites[i].kind == kind && GpuNameEqual(gpuFenceSites[i].name, info->name)) {
            InterlockedAdd64(&gpuFenceSites[i].busy, busy); InterlockedIncrement64(&gpuFenceSites[i].count);
            for (unsigned k = 0; k < LP_COUNT; ++k) InterlockedAdd64(&gpuFenceSites[i].c[k], info->c[k]);
            return;
        }
        if (!gpuFenceSites[i].caller) {
            unsigned k = 0; while (info->name[k] && k + 1 < sizeof(gpuFenceSites[i].name)) { gpuFenceSites[i].name[k] = info->name[k]; ++k; }
            gpuFenceSites[i].name[k] = 0;
            gpuFenceSites[i].kind = kind;
            if (InterlockedCompareExchangePointer((PVOID volatile *)&gpuFenceSites[i].caller, (PVOID)caller, NULL)) { --i; continue; }
            InterlockedAdd64(&gpuFenceSites[i].busy, busy); InterlockedIncrement64(&gpuFenceSites[i].count);
            for (unsigned k = 0; k < LP_COUNT; ++k) InterlockedAdd64(&gpuFenceSites[i].c[k], info->c[k]);
            return;
        }
    }
}
/* Queue timeline: submissions, queue and CPU fence signals and waits, and completions, in a ring. Dumped once for
   the last 2.5 s of a settled in-world window. Lines start with "asset qtl". */
#define QTL_EVENTS 32768u
typedef struct QtlEvent { volatile LONG64 qpc; ULONG_PTR fence; UINT64 value; ULONG_PTR caller; USHORT tid; char kind; signed char queue; } QtlEvent;
static QtlEvent qtlRing[QTL_EVENTS];
static volatile LONG qtlCursor, qtlArmed, qtlDumped;
static signed char QtlQueue(ID3D12CommandQueue *queue) {
    for (unsigned i = 0; i < ARRAYSIZE(gpuQueues); ++i) if (gpuQueues[i] == queue) return (signed char)i;
    return -1;
}
static BOOL QtlOwnFence(const void *fence) {
    if (!fence || fence == gpuFence) return TRUE;
    for (unsigned q = 0; q < GPUQ_SLOTS; ++q) if (gpuQFence[q] == fence) return TRUE;
    return FALSE;
}
static void QtlLog(char kind, signed char queue, const void *fence, UINT64 value, const void *caller) {
    if (qtlDumped) return;
    const LONG i = (LONG)((ULONG)InterlockedIncrement(&qtlCursor) % QTL_EVENTS);
    QtlEvent *e = &qtlRing[i];
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    e->qpc = 0; e->kind = kind; e->queue = queue; e->fence = (ULONG_PTR)fence; e->value = value; e->caller = (ULONG_PTR)caller;
    e->tid = (USHORT)GetCurrentThreadId();
    InterlockedExchange64(&e->qpc, now.QuadPart);
}
/* Fence polling: which code polls which fence, the last completed value it saw, and the highest value any
   queue or CPU signal requested on that fence. Lines start with "asset fencepoll". */
#define FP_SLOTS 512u
typedef struct FencePoll { volatile ULONG_PTR fence, caller; volatile LONG64 calls; volatile UINT64 last; } FencePoll;
static FencePoll fencePolls[FP_SLOTS];
typedef struct FenceSignal { volatile ULONG_PTR fence; volatile UINT64 max; volatile LONG64 count; } FenceSignal;
static FenceSignal fenceSignals[FP_SLOTS];
static void FenceSignalNote(const void *fence, UINT64 value) {
    const unsigned h = (unsigned)(((ULONG_PTR)fence >> 4) * 2654435761u) % FP_SLOTS;
    for (unsigned k = 0; k < 16; ++k) {
        FenceSignal *e = &fenceSignals[(h + k) % FP_SLOTS];
        if (e->fence != (ULONG_PTR)fence && (e->fence || InterlockedCompareExchangePointer((PVOID volatile *)&e->fence, (PVOID)fence, NULL))) {
            if (e->fence != (ULONG_PTR)fence) continue;
        }
        if (value > e->max) e->max = value;
        InterlockedIncrement64(&e->count);
        return;
    }
}
static FenceSignal *FenceSignalFind(ULONG_PTR fence) {
    const unsigned h = (unsigned)((fence >> 4) * 2654435761u) % FP_SLOTS;
    for (unsigned k = 0; k < 16; ++k) if (fenceSignals[(h + k) % FP_SLOTS].fence == fence) return &fenceSignals[(h + k) % FP_SLOTS];
    return NULL;
}
typedef UINT64 (STDMETHODCALLTYPE *QtlFenceValueFn)(ID3D12Fence *);
static QtlFenceValueFn qtlFenceValueOriginal;
static UINT64 STDMETHODCALLTYPE QtlFenceValue(ID3D12Fence *fence) {
    const UINT64 value = qtlFenceValueOriginal(fence);
    const ULONG_PTR caller = (ULONG_PTR)__builtin_return_address(0);
    const unsigned h = (unsigned)((((ULONG_PTR)fence >> 4) ^ (caller >> 1)) * 2654435761u) % FP_SLOTS;
    for (unsigned k = 0; k < 16; ++k) {
        FencePoll *e = &fencePolls[(h + k) % FP_SLOTS];
        if (e->fence == (ULONG_PTR)fence && e->caller == caller) { e->calls += 1; e->last = value; return value; }
        if (!e->fence && !InterlockedCompareExchangePointer((PVOID volatile *)&e->fence, (PVOID)fence, NULL)) {
            e->caller = caller; e->last = value; e->calls = 1; return value;
        }
    }
    return value;
}
typedef HRESULT (STDMETHODCALLTYPE *QtlFenceSignalFn)(ID3D12Fence *, UINT64);
typedef HRESULT (STDMETHODCALLTYPE *QtlFenceEventFn)(ID3D12Fence *, UINT64, HANDLE);
static QtlFenceSignalFn qtlFenceSignalOriginal;
static QtlFenceEventFn qtlFenceEventOriginal;
static HRESULT STDMETHODCALLTYPE QtlFenceSignal(ID3D12Fence *fence, UINT64 value) {
    if (!QtlOwnFence(fence)) { QtlLog('C', -1, fence, value, __builtin_return_address(0)); FenceSignalNote(fence, value); }
    return qtlFenceSignalOriginal(fence, value);
}
static HRESULT STDMETHODCALLTYPE QtlFenceEvent(ID3D12Fence *fence, UINT64 value, HANDLE event) {
    if (!QtlOwnFence(fence)) QtlLog('V', -1, fence, value, __builtin_return_address(0));
    const HRESULT hr = qtlFenceEventOriginal(fence, value, event);
    if (!QtlOwnFence(fence)) QtlLog('v', -1, fence, value, __builtin_return_address(0));
    return hr;
}
static void QtlPatchFence(ID3D12Fence *fence) {
    if (!fence) return;
    void **vtable = *(void ***)fence;
    if (vtable[10] == (void *)QtlFenceSignal) return;
    ULONG protection = 0, ignored = 0;
    if (!qtlFenceSignalOriginal) qtlFenceSignalOriginal = (QtlFenceSignalFn)vtable[10];
    if (!qtlFenceEventOriginal) qtlFenceEventOriginal = (QtlFenceEventFn)vtable[9];
    if (!qtlFenceValueOriginal) qtlFenceValueOriginal = (QtlFenceValueFn)vtable[8];
    if (VirtualProtectFromApp(&vtable[8], 3 * sizeof(void *), PAGE_READWRITE, &protection)) {
        InterlockedExchangePointer(&vtable[8], (void *)QtlFenceValue);
        InterlockedExchangePointer(&vtable[9], (void *)QtlFenceEvent);
        InterlockedExchangePointer(&vtable[10], (void *)QtlFenceSignal);
        VirtualProtectFromApp(&vtable[8], 3 * sizeof(void *), protection, &ignored);
    }
}
static void QtlDump(void) {
    if (InterlockedExchange(&qtlDumped, 1)) return;
    LARGE_INTEGER now, frequency; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
    const LONG64 from = now.QuadPart - frequency.QuadPart * 5 / 2;
    static char buffer[65536];
    char *out = buffer;
    unsigned lines = 0;
    const ULONG last = (ULONG)qtlCursor;
    for (ULONG k = 0; k < QTL_EVENTS; ++k) {
        const QtlEvent *e = &qtlRing[(last + 1 + k) % QTL_EVENTS];
        if (!e->qpc || e->qpc < from) continue;
        if (out - buffer > (long)sizeof(buffer) - 256) { AppendTrace(buffer, (DWORD)(out - buffer)); out = buffer; }
        out = CensusDec(Text(out, "asset qtl us="), (unsigned long long)((e->qpc - from) * 1000000 / frequency.QuadPart));
        char kind[2] = {e->kind, 0};
        out = Text(Text(out, " k="), kind);
        out = CensusDec(Text(out, " q="), (unsigned long long)(e->queue + 1));
        out = Hex(Text(out, " f=0x"), (unsigned long)(e->fence & 0xFFFFFFFFu), 8);
        out = CensusDec(Text(out, " v="), (unsigned long long)e->value);
        out = Hex(Text(out, " tid=0x"), e->tid, 4);
        out = e->caller ? RecordImageAddress(out, (const void *)e->caller) : Text(out, " -");
        out = Text(out, "\r\n");
        ++lines;
    }
    if (out > buffer) AppendTrace(buffer, (DWORD)(out - buffer));
    char line[96], *t = CensusDec(Text(line, "asset qtl-end lines="), lines);
    CensusEmit(line, t);
}

static void PtFinish(const BatchInfo *info, ULONG_PTR caller) {
    for (LONG t = 0; t < info->tcount; ++t) {
        const LONG index = info->tslot[t];
        PtSlot *slot = &ptSlots[index];
        if (slot->state != 2) continue;
        const UINT base = (UINT)index * PT_STAMPS;
        for (UINT i = 0; i < slot->n; ++i) {
            const UINT64 begin = ptStamps[base + i], end = ptStamps[base + i + 1];
            if (!begin || end < begin || end - begin > gpuFrequency) { InterlockedIncrement(&ptBad); continue; }
            PtPso *e = PtPsoSlot(slot->key[i]);
            if (!e) continue;
            InterlockedAdd64(&e->ticks, (LONG64)(end - begin)); InterlockedIncrement64(&e->spans);
            InterlockedAdd64(&e->work, slot->work[i]); e->caller = caller;
        }
        for (UINT i = 0; i <= slot->n; ++i) ptStamps[base + i] = 0;
        InterlockedIncrement(&ptFinished);
        InterlockedExchange(&slot->state, 0);
    }
}
static DWORD WINAPI GpuCopyWatch(void *unused) {
    (void)unused;
    UINT64 seen[GPUQ_SLOTS] = {0};
    LONG64 previousEnd[GPUQ_SLOTS] = {0};
    for (;;) {
        for (unsigned q = 0; q < GPUQ_SLOTS; ++q) {
            if (!gpuQFence[q]) continue;
            const UINT64 completed = ID3D12Fence_GetCompletedValue(gpuQFence[q]);
            if (completed == ~0ull) return 0;
            if (completed <= seen[q]) continue;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            for (UINT64 v = seen[q] + 1; v <= completed; ++v) {
                const LONG64 submit = gpuQSubmit[q][v % GPUQ_RING];
                if (!submit) continue;
                const LONG64 begin = submit > previousEnd[q] ? submit : previousEnd[q];
                if (now.QuadPart > begin) {
                    InterlockedAdd64(&gpuCopyBusy, now.QuadPart - begin);
                    GpuFenceCredit(gpuQCaller[q][v % GPUQ_RING], gpuQKind[q] * 16 + (LONG)q, now.QuadPart - begin, &gpuQBatch[q][v % GPUQ_RING]);
                }
                PtFinish(&gpuQBatch[q][v % GPUQ_RING], gpuQCaller[q][v % GPUQ_RING]);
                QtlLog('X', (signed char)q, NULL, v, NULL);
                InterlockedAdd64(&gpuCopyLatency, now.QuadPart - submit);
                if (now.QuadPart - submit > gpuCopyMaxLatency) gpuCopyMaxLatency = now.QuadPart - submit;
                InterlockedIncrement64(&gpuCopyBatches);
                previousEnd[q] = now.QuadPart;
            }
            seen[q] = completed;
        }
#ifdef GPU_PROF
        Sleep(1);   /* play-build profiling: do not spin one of the game's six processors */
#else
        YieldProcessor();
        Sleep(0);
#endif
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
        for (unsigned q = 0; SUCCEEDED(hr) && q < GPUQ_SLOTS; ++q)
            if (FAILED(ID3D12Device_CreateFence(allocDevice, 0, D3D12_FENCE_FLAG_NONE, &fenceIid, (void **)&gpuQFence[q]))) gpuQFence[q] = NULL;
        if (SUCCEEDED(hr)) {
            D3D12_QUERY_HEAP_DESC ptDesc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, PT_SLOTS * PT_STAMPS, 0};
            D3D12_RESOURCE_DESC ptBuffer = desc; ptBuffer.Width = (UINT64)PT_SLOTS * PT_STAMPS * 8;
            void *mapped = NULL;
            if (SUCCEEDED(ID3D12Device_CreateQueryHeap(allocDevice, &ptDesc, &heapIid, (void **)&ptHeap)) &&
                SUCCEEDED(((AllocCommittedFn)allocOriginal[27])(allocDevice, &props, D3D12_HEAP_FLAG_NONE, &ptBuffer,
                    D3D12_RESOURCE_STATE_COPY_DEST, NULL, &resourceIid, (void **)&ptReadback)) &&
                SUCCEEDED(ID3D12Resource_Map(ptReadback, 0, &none, &mapped))) {
                ptStamps = (volatile UINT64 *)mapped;
                for (UINT64 i = 0; i < (UINT64)PT_SLOTS * PT_STAMPS; ++i) ptStamps[i] = 0;
                ptReady = 1;
            } else ptReady = -1;
        }
        if (SUCCEEDED(hr) && gpuQFence[0]) {
            HANDLE watcher = CreateThread(NULL, 0, GpuCopyWatch, NULL, 0, NULL);
            if (watcher) { CloseHandle(watcher); gpuCopyFence = gpuQFence[0]; }
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
        out = Text(out, gpuCopyHeap ? " copy-heap=ok build=9" : " copy-heap=none build=9");
        CensusEmit(line, out);
    }
    ReleaseSRWLockExclusive(&gpuLock);
    return gpuReady > 0;
}

static void STDMETHODCALLTYPE GpuExecute(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists) {
    const ULONG_PTR caller = (ULONG_PTR)__builtin_return_address(0);
    const LONG kind = GpuQueueKind(queue);
    if (kind >= 0 && GpuSetup(queue) && gpuCopyFence) {
        unsigned q = 0;
        while (q < GPUQ_SLOTS && gpuQueues[q] != queue) ++q;
        gpuExecuteOriginal(queue, count, lists);
        if (q < GPUQ_SLOTS && gpuQFence[q]) {
            gpuQKind[q] = kind;
            AcquireSRWLockExclusive(&gpuLock);
            const UINT64 value = (UINT64)InterlockedIncrement64(&gpuQNext[q]);
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            gpuQSubmit[q][value % GPUQ_RING] = now.QuadPart;
            gpuQCaller[q][value % GPUQ_RING] = caller;
#ifndef NO_LISTPROF
            ListBatch(count, lists, &gpuQBatch[q][value % GPUQ_RING]);
#else
            gpuQBatch[q][value % GPUQ_RING].tcount = 0; gpuQBatch[q][value % GPUQ_RING].lists = (LONG)count;
#endif
            QtlLog('E', (signed char)q, NULL, value, (const void *)caller);
            InterlockedAdd64(&gpuCopyLists, count);
            ID3D12CommandQueue_Signal(queue, gpuQFence[q], value);
            ReleaseSRWLockExclusive(&gpuLock);
        }
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

typedef HRESULT (STDMETHODCALLTYPE *GpuWaitFn)(ID3D12CommandQueue *, ID3D12Fence *, UINT64);
static GpuWaitFn gpuWaitOriginal;
static volatile LONG64 gpuWaits[3];
static HRESULT STDMETHODCALLTYPE GpuWait(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value) {
    const LONG kind = GpuQueueKind(queue);
    if (kind >= 0 && kind < 3) InterlockedIncrement64(&gpuWaits[kind]);
    QtlPatchFence(fence);
    QtlLog('W', QtlQueue(queue), fence, value, __builtin_return_address(0));
    return gpuWaitOriginal(queue, fence, value);
}
typedef HRESULT (STDMETHODCALLTYPE *GpuSignalFn)(ID3D12CommandQueue *, ID3D12Fence *, UINT64);
static GpuSignalFn gpuSignalOriginal;
static HRESULT STDMETHODCALLTYPE GpuSignal(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value) {
    if (!QtlOwnFence(fence)) {
        QtlPatchFence(fence);
        QtlLog('S', QtlQueue(queue), fence, value, __builtin_return_address(0));
        FenceSignalNote(fence, value);
    }
    return gpuSignalOriginal(queue, fence, value);
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
        if (vtable[14] != (void *)GpuSignal) {
            ULONG protection = 0, ignored = 0;
            if (!gpuSignalOriginal) gpuSignalOriginal = (GpuSignalFn)vtable[14];
            if (VirtualProtectFromApp(&vtable[14], sizeof(void *), PAGE_READWRITE, &protection)) {
                InterlockedExchangePointer(&vtable[14], (void *)GpuSignal);
                VirtualProtectFromApp(&vtable[14], sizeof(void *), protection, &ignored);
            }
        }
        if (vtable[15] != (void *)GpuWait) {
            ULONG protection = 0, ignored = 0;
            if (!gpuWaitOriginal) gpuWaitOriginal = (GpuWaitFn)vtable[15];
            if (VirtualProtectFromApp(&vtable[15], sizeof(void *), PAGE_READWRITE, &protection)) {
                InterlockedExchangePointer(&vtable[15], (void *)GpuWait);
                VirtualProtectFromApp(&vtable[15], sizeof(void *), protection, &ignored);
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
            char copyLine[200], *c = CensusDec(Text(copyLine, "asset gpufence sample="), sample);
            c = CensusDec(Text(c, " wall-ms="), (unsigned long long)((now.QuadPart - lastQpc.QuadPart) * 1000 / frequency.QuadPart));
            c = CensusDec(Text(c, " busy-ms="), (unsigned long long)((busyNow - lastBusy) * 1000 / frequency.QuadPart));
            c = CensusDec(Text(c, " batches="), (unsigned long long)batches);
            c = CensusDec(Text(c, " lists="), (unsigned long long)(listsNow - lastLists));
            c = CensusDec(Text(c, " mean-latency-us="), batches ? (unsigned long long)((latencyNow - lastLatency) * 1000000 / frequency.QuadPart / batches) : 0);
            c = CensusDec(Text(c, " max-latency-us="), (unsigned long long)(gpuCopyMaxLatency * 1000000 / frequency.QuadPart));
            CensusEmit(copyLine, c);
            gpuCopyMaxLatency = 0;
            /* Two consecutive in-world windows (over 8000 batches each) arm and then fire the timeline dump. */
            #ifdef GPU_PROF
            (void)batches;   /* keep the ring running for qtlframes */
#else
            if (batches > 8000) { if (qtlArmed >= 1) QtlDump(); else ++qtlArmed; } else if (!qtlDumped) qtlArmed = 0;
#endif
            for (unsigned emitted = 0; emitted < 24; ++emitted) {
                unsigned best = GPUFENCE_SITES; LONG64 bestBusy = 0;
                for (unsigned i = 0; i < GPUFENCE_SITES && gpuFenceSites[i].caller; ++i)
                    if (gpuFenceSites[i].busy > bestBusy) { bestBusy = gpuFenceSites[i].busy; best = i; }
                if (best == GPUFENCE_SITES) break;
                const LONG64 busy = InterlockedExchange64(&gpuFenceSites[best].busy, 0), n = InterlockedExchange64(&gpuFenceSites[best].count, 0);
                char site[640], *e = CensusDec(Text(site, "asset listprof sample="), sample);
                e = CensusDec(Text(e, " busy-ms="), (unsigned long long)(busy * 1000 / frequency.QuadPart));
                e = CensusDec(Text(e, " batches="), (unsigned long long)n);
                for (unsigned k = 0; k < LP_COUNT; ++k) {
                    const LONG64 v = InterlockedExchange64(&gpuFenceSites[best].c[k], 0);
                    e = CensusDec(Text(Text(Text(e, " "), lpNames[k]), "="), (unsigned long long)v);
                }
                e = CensusDec(Text(e, " q="), (unsigned long long)gpuFenceSites[best].kind);
                e = Text(Text(e, " name="), gpuFenceSites[best].name[0] ? gpuFenceSites[best].name : "-");
                e = RecordImageAddress(e, (const void *)gpuFenceSites[best].caller);
                CensusEmit(site, e);
            }
            for (unsigned i = 0; i < GPUFENCE_SITES && gpuFenceSites[i].caller; ++i) {
                gpuFenceSites[i].busy = gpuFenceSites[i].count = 0; for (unsigned k = 0; k < LP_COUNT; ++k) gpuFenceSites[i].c[k] = 0;
            }
        }
        if (lastQpc.QuadPart && ptReady > 0) {
            static LONG lastStarted, lastFinished, lastNoSlot, lastOverflow, lastBad;
            UINT64 total = 0;
            for (unsigned i = 0; i < PT_PSOS; ++i) total += (UINT64)ptPsos[i].ticks;
            char pl[320], *t = CensusDec(Text(pl, "asset psotime-total sample="), sample);
            t = CensusDec(Text(t, " us="), total * 1000000ull / gpuFrequency);
            t = CensusDec(Text(t, " lists-started="), (unsigned long long)(ptStarted - lastStarted));
            t = CensusDec(Text(t, " lists-timed="), (unsigned long long)(ptFinished - lastFinished));
            t = CensusDec(Text(t, " no-slot="), (unsigned long long)(ptNoSlot - lastNoSlot));
            t = CensusDec(Text(t, " overflow="), (unsigned long long)(ptOverflow - lastOverflow));
            t = CensusDec(Text(t, " bad-spans="), (unsigned long long)(ptBad - lastBad));
            static LONG64 lastWaits[3];
            t = CensusDec(Text(t, " waits-direct="), (unsigned long long)(gpuWaits[0] - lastWaits[0]));
            t = CensusDec(Text(t, " waits-compute="), (unsigned long long)(gpuWaits[1] - lastWaits[1]));
            t = CensusDec(Text(t, " waits-copy="), (unsigned long long)(gpuWaits[2] - lastWaits[2]));
            for (unsigned k = 0; k < 3; ++k) lastWaits[k] = gpuWaits[k];
            CensusEmit(pl, t);
            lastStarted = ptStarted; lastFinished = ptFinished; lastNoSlot = ptNoSlot; lastOverflow = ptOverflow; lastBad = ptBad;
            for (unsigned emitted = 0; emitted < 40; ++emitted) {
                unsigned best = PT_PSOS; LONG64 bestTicks = 0;
                for (unsigned i = 0; i < PT_PSOS; ++i) if (ptPsos[i].ticks > bestTicks) { bestTicks = ptPsos[i].ticks; best = i; }
                if (best == PT_PSOS) break;
                PtPso *e = &ptPsos[best];
                const LONG64 ticks = InterlockedExchange64(&e->ticks, 0), spans = InterlockedExchange64(&e->spans, 0), work = InterlockedExchange64(&e->work, 0);
                char line[320], *o = CensusDec(Text(line, "asset psotime sample="), sample);
                o = CensusDec(Text(o, " us="), (unsigned long long)ticks * 1000000ull / gpuFrequency);
                o = CensusDec(Text(o, " spans="), (unsigned long long)spans);
                o = CensusDec(Text(o, " work="), (unsigned long long)work);
                o = CensusDec(Text(o, " n="), (unsigned long long)e->ordinal);
                o = Text(o, e->ordinal ? (e->graphics ? " kind=g" : " kind=c") : " kind=?");
                o = Hex(Text(o, " pso=0x"), (unsigned long)(e->pso & 0xFFFFFFFFu), 8);
                o = Hex(Hex(Text(o, " s0=0x"), (unsigned long)(e->vs >> 32), 8), (unsigned long)(e->vs & 0xFFFFFFFFu), 8);
                o = CensusDec(Text(o, " s0b="), (unsigned long long)e->vsBytes);
                o = Hex(Hex(Text(o, " s1=0x"), (unsigned long)(e->ps >> 32), 8), (unsigned long)(e->ps & 0xFFFFFFFFu), 8);
                o = CensusDec(Text(o, " s1b="), (unsigned long long)e->psBytes);
#ifdef GPU_PROF
                o = Text(o, (e->forms & 1) ? " f0=dxbc" : (e->forms & 2) ? " f0=dxil" : " f0=-");
                o = Text(o, (e->forms & 4) ? " f1=dxbc" : (e->forms & 8) ? " f1=dxil" : " f1=-");
#endif
                o = RecordImageAddress(o, (const void *)e->caller);
                CensusEmit(line, o);
            }
            for (unsigned i = 0; i < PT_PSOS; ++i) ptPsos[i].ticks = ptPsos[i].spans = ptPsos[i].work = 0;
        }
        for (unsigned emitted = 0; emitted < 16; ++emitted) {
            unsigned best = FP_SLOTS; LONG64 bestCalls = 0;
            for (unsigned i = 0; i < FP_SLOTS; ++i) if (fencePolls[i].calls > bestCalls) { bestCalls = fencePolls[i].calls; best = i; }
            if (best == FP_SLOTS) break;
            FencePoll *e = &fencePolls[best];
            const LONG64 calls = InterlockedExchange64(&e->calls, 0);
            const FenceSignal *sig = FenceSignalFind(e->fence);
            char line[300], *o = CensusDec(Text(line, "asset fencepoll sample="), sample);
            o = CensusDec(Text(o, " calls="), (unsigned long long)calls);
            o = Hex(Text(o, " fence=0x"), (unsigned long)(e->fence & 0xFFFFFFFFu), 8);
            o = CensusDec(Text(o, " last="), (unsigned long long)e->last);
            o = CensusDec(Text(o, " signaled-max="), sig ? (unsigned long long)sig->max : 0ull);
            o = CensusDec(Text(o, " signals="), sig ? (unsigned long long)sig->count : 0ull);
            o = Text(o, QtlOwnFence((const void *)e->fence) ? " own=1" : " own=0");
            o = RecordImageAddress(o, (const void *)e->caller);
            CensusEmit(line, o);
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
