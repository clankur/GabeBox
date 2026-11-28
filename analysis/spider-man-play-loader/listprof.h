/* Per-command-list state accounting, joined with fence-timed GPU time per submission.
   Hooks the game's graphics command list vtable (created through the device's CreateCommandList, or caught at first
   submission). Counts draws, dispatches, ExecuteIndirect calls and their MaxCommandCount, barriers by type, pipeline,
   descriptor-heap, root-argument and render-target changes, clears, bundles and copies. Included by gpuprof.h.
   Lines start with "asset listprof". */

enum {
    LP_DRAW, LP_INDEXED, LP_DISPATCH, LP_INDIRECT, LP_INDIRECT_MAX, LP_BARRIER_CALL, LP_BARRIER_TRANSITION,
    LP_BARRIER_UAV, LP_BARRIER_ALIAS, LP_BARRIER_SPLIT, LP_PSO, LP_HEAPS, LP_ROOTSIG, LP_ROOTARG, LP_RTS, LP_CLEAR,
    LP_BUNDLE, LP_COPY, LP_COUNT
};
static const char *const lpNames[LP_COUNT] = {
    "draw", "indexed", "dispatch", "indirect", "indirect-max", "barrier-calls", "transitions", "uav", "alias", "split",
    "pso", "heaps", "rootsig", "rootarg", "rts", "clears", "bundles", "copies"
};

#define LISTPROF_SLOTS 1024u
typedef struct ListStats { volatile ULONG_PTR list; volatile LONG c[LP_COUNT]; char name[40]; volatile LONG tslot; ULONG_PTR pso; } ListStats;

/* Per-pipeline GPU timing. A sample of direct and compute lists get a timestamp at Reset, before every
   SetPipelineState and at Close; Close resolves them into a readback slot. When the submission's fence completes, each
   span's GPU time is credited to the pipeline that was bound during it. */
#define PT_SLOTS 64u
#define PT_STAMPS 1024u
typedef struct PtSlot { volatile LONG state; UINT n; ULONGLONG since; ULONG_PTR key[PT_STAMPS]; LONG work[PT_STAMPS]; } PtSlot;
static PtSlot ptSlots[PT_SLOTS];
static ID3D12QueryHeap *ptHeap;
static ID3D12Resource *ptReadback;
static volatile UINT64 *ptStamps;
static volatile LONG ptReady, ptOverflow, ptNoSlot, ptStarted, ptFinished, ptBad;
#define PT_PSOS 8192u
typedef struct PtPso { volatile ULONG_PTR pso; DWORD ordinal; LONG graphics; UINT64 vs, ps; DWORD vsBytes, psBytes;
    volatile LONG64 ticks, spans, work; volatile ULONG_PTR caller; } PtPso;
static PtPso ptPsos[PT_PSOS];
static PtPso *PtPsoSlot(ULONG_PTR key) {
    const unsigned start = (unsigned)((key >> 4) * 2654435761u) % PT_PSOS;
    for (unsigned probe = 0; probe < PT_PSOS; ++probe) {
        PtPso *slot = &ptPsos[(start + probe) % PT_PSOS];
        if (slot->pso == key) return slot;
        if (!slot->pso && !InterlockedCompareExchangePointer((PVOID volatile *)&slot->pso, (PVOID)key, NULL)) return slot;
    }
    return NULL;
}
static UINT64 PtDigest(const D3D12_SHADER_BYTECODE *code) {
    if (!code || !code->pShaderBytecode || code->BytecodeLength < 20) return 0;
    UINT64 v; __builtin_memcpy(&v, (const char *)code->pShaderBytecode + 4, sizeof(v)); return v;
}
/* Called after a pipeline is created: remembers its ordinal and shader digests. */
static void PtRegister(void *pso, DWORD ordinal, BOOL graphics, const D3D12_SHADER_BYTECODE *first, const D3D12_SHADER_BYTECODE *second) {
    if (!pso) return;
    PtPso *e = PtPsoSlot((ULONG_PTR)pso); if (!e) return;
    e->ordinal = ordinal; e->graphics = graphics;
    e->vs = PtDigest(first); e->vsBytes = first ? (DWORD)first->BytecodeLength : 0;
    e->ps = PtDigest(second); e->psBytes = second ? (DWORD)second->BytecodeLength : 0;
}
typedef void (STDMETHODCALLTYPE *LpEndQueryFn)(ID3D12GraphicsCommandList *, ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT);
typedef void (STDMETHODCALLTYPE *LpResolveFn)(ID3D12GraphicsCommandList *, ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource *, UINT64);
typedef D3D12_COMMAND_LIST_TYPE (STDMETHODCALLTYPE *LpTypeFn)(ID3D12GraphicsCommandList *);
static void PtStamp(ID3D12GraphicsCommandList *self, UINT index) {
    ((LpEndQueryFn)(*(void ***)self)[53])(self, ptHeap, D3D12_QUERY_TYPE_TIMESTAMP, index);
}
static void PtBegin(ID3D12GraphicsCommandList *self, struct ListStats *s) {
    const D3D12_COMMAND_LIST_TYPE type = ((LpTypeFn)(*(void ***)self)[8])(self);
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return;
    if (InterlockedIncrement(&ptStarted) & 1) return; /* time every other list */
    const ULONGLONG now = GetTickCount64();
    for (unsigned i = 0; i < PT_SLOTS; ++i) {
        PtSlot *slot = &ptSlots[i];
        if (slot->state && now - slot->since > 4000) InterlockedExchange(&slot->state, 0); /* abandoned */
        if (InterlockedCompareExchange(&slot->state, 1, 0)) continue;
        slot->since = now; slot->n = 1; slot->key[0] = s->pso; slot->work[0] = 0;
        PtStamp(self, i * PT_STAMPS);
        s->tslot = (LONG)i + 1;
        return;
    }
    InterlockedIncrement(&ptNoSlot);
}
static void PtWork(struct ListStats *s) {
    if (s && s->tslot) { PtSlot *slot = &ptSlots[s->tslot - 1]; slot->work[slot->n - 1] += 1; }
}
static ListStats listStats[LISTPROF_SLOTS];
static void *listOriginal[64];

typedef HRESULT (STDMETHODCALLTYPE *DeviceCreateListFn)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *, REFIID, void **);
static DeviceCreateListFn listCreateOriginal;

static ListStats *ListSlot(ID3D12GraphicsCommandList *list) {
    const ULONG_PTR key = (ULONG_PTR)list;
    const unsigned start = (unsigned)((key >> 4) * 2654435761u) % LISTPROF_SLOTS;
    for (unsigned probe = 0; probe < LISTPROF_SLOTS; ++probe) {
        ListStats *slot = &listStats[(start + probe) % LISTPROF_SLOTS];
        if (slot->list == key) return slot;
        if (!slot->list && !InterlockedCompareExchangePointer((PVOID volatile *)&slot->list, (PVOID)key, NULL)) return slot;
    }
    return NULL;
}
/* A list is recorded by one thread at a time, so plain increments are enough. */
static ListStats *ListAdd(ID3D12GraphicsCommandList *self, unsigned counter, LONG amount) {
    ListStats *s = ListSlot(self); if (s) s->c[counter] += amount;
    return s;
}
#define LP_CALL(slot, type, ...) ((type)listOriginal[slot])(__VA_ARGS__)

typedef void (STDMETHODCALLTYPE *LpDrawFn)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, UINT);
static void STDMETHODCALLTYPE ListDraw(ID3D12GraphicsCommandList *self, UINT a, UINT b, UINT c, UINT d) {
    PtWork(ListAdd(self, LP_DRAW, 1)); LP_CALL(12, LpDrawFn, self, a, b, c, d);
}
typedef void (STDMETHODCALLTYPE *LpDrawIndexedFn)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, INT, UINT);
static void STDMETHODCALLTYPE ListDrawIndexed(ID3D12GraphicsCommandList *self, UINT a, UINT b, UINT c, INT d, UINT e) {
    PtWork(ListAdd(self, LP_INDEXED, 1)); LP_CALL(13, LpDrawIndexedFn, self, a, b, c, d, e);
}
typedef void (STDMETHODCALLTYPE *LpDispatchFn)(ID3D12GraphicsCommandList *, UINT, UINT, UINT);
static void STDMETHODCALLTYPE ListDispatch(ID3D12GraphicsCommandList *self, UINT a, UINT b, UINT c) {
    PtWork(ListAdd(self, LP_DISPATCH, 1)); LP_CALL(14, LpDispatchFn, self, a, b, c);
}
typedef HRESULT (STDMETHODCALLTYPE *LpResetFn)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *, ID3D12PipelineState *);
static HRESULT STDMETHODCALLTYPE ListReset(ID3D12GraphicsCommandList *self, ID3D12CommandAllocator *a, ID3D12PipelineState *p) {
    ListStats *s = ListSlot(self);
    if (s) {
        for (unsigned i = 0; i < LP_COUNT; ++i) s->c[i] = 0;
        s->name[0] = 0;
        if (s->tslot) { InterlockedCompareExchange(&ptSlots[s->tslot - 1].state, 0, 1); s->tslot = 0; }
        s->pso = (ULONG_PTR)p;
    }
    const HRESULT hr = LP_CALL(10, LpResetFn, self, a, p);
    if (SUCCEEDED(hr) && s && ptReady > 0) PtBegin(self, s);
    return hr;
}
typedef void (STDMETHODCALLTYPE *LpBufferCopyFn)(ID3D12GraphicsCommandList *, ID3D12Resource *, UINT64, ID3D12Resource *, UINT64, UINT64);
static void STDMETHODCALLTYPE ListCopyBuffer(ID3D12GraphicsCommandList *self, ID3D12Resource *d, UINT64 o, ID3D12Resource *s, UINT64 so, UINT64 n) {
    ListAdd(self, LP_COPY, 1); LP_CALL(15, LpBufferCopyFn, self, d, o, s, so, n);
}
typedef void (STDMETHODCALLTYPE *LpTextureCopyFn)(ID3D12GraphicsCommandList *, const D3D12_TEXTURE_COPY_LOCATION *, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION *, const D3D12_BOX *);
static void STDMETHODCALLTYPE ListCopyTexture(ID3D12GraphicsCommandList *self, const D3D12_TEXTURE_COPY_LOCATION *d, UINT x, UINT y, UINT z,
        const D3D12_TEXTURE_COPY_LOCATION *s, const D3D12_BOX *box) {
    ListAdd(self, LP_COPY, 1); LP_CALL(16, LpTextureCopyFn, self, d, x, y, z, s, box);
}
typedef void (STDMETHODCALLTYPE *LpResourceCopyFn)(ID3D12GraphicsCommandList *, ID3D12Resource *, ID3D12Resource *);
static void STDMETHODCALLTYPE ListCopyResource(ID3D12GraphicsCommandList *self, ID3D12Resource *d, ID3D12Resource *s) {
    ListAdd(self, LP_COPY, 1); LP_CALL(17, LpResourceCopyFn, self, d, s);
}
typedef void (STDMETHODCALLTYPE *LpPsoFn)(ID3D12GraphicsCommandList *, ID3D12PipelineState *);
static void STDMETHODCALLTYPE ListSetPipeline(ID3D12GraphicsCommandList *self, ID3D12PipelineState *p) {
    ListStats *s = ListAdd(self, LP_PSO, 1);
    if (s) {
        if (s->tslot) {
            PtSlot *slot = &ptSlots[s->tslot - 1];
            if (slot->n + 1 < PT_STAMPS) {
                PtStamp(self, (UINT)(s->tslot - 1) * PT_STAMPS + slot->n);
                slot->key[slot->n] = (ULONG_PTR)p; slot->work[slot->n] = 0; slot->n += 1;
            } else InterlockedIncrement(&ptOverflow);
        }
        s->pso = (ULONG_PTR)p;
    }
    LP_CALL(25, LpPsoFn, self, p);
}
typedef void (STDMETHODCALLTYPE *LpBarrierFn)(ID3D12GraphicsCommandList *, UINT, const D3D12_RESOURCE_BARRIER *);
static void STDMETHODCALLTYPE ListBarrier(ID3D12GraphicsCommandList *self, UINT n, const D3D12_RESOURCE_BARRIER *b) {
    ListStats *s = ListSlot(self);
    if (s && b) {
        s->c[LP_BARRIER_CALL] += 1;
        for (UINT i = 0; i < n; ++i) {
            if (b[i].Flags) s->c[LP_BARRIER_SPLIT] += 1;
            s->c[b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV ? LP_BARRIER_UAV
                 : b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING ? LP_BARRIER_ALIAS : LP_BARRIER_TRANSITION] += 1;
        }
    }
    LP_CALL(26, LpBarrierFn, self, n, b);
}
typedef void (STDMETHODCALLTYPE *LpBundleFn)(ID3D12GraphicsCommandList *, ID3D12GraphicsCommandList *);
static void STDMETHODCALLTYPE ListBundle(ID3D12GraphicsCommandList *self, ID3D12GraphicsCommandList *bundle) {
    ListAdd(self, LP_BUNDLE, 1); LP_CALL(27, LpBundleFn, self, bundle);
}
typedef void (STDMETHODCALLTYPE *LpHeapsFn)(ID3D12GraphicsCommandList *, UINT, ID3D12DescriptorHeap *const *);
static void STDMETHODCALLTYPE ListHeaps(ID3D12GraphicsCommandList *self, UINT n, ID3D12DescriptorHeap *const *h) {
    ListAdd(self, LP_HEAPS, 1); LP_CALL(28, LpHeapsFn, self, n, h);
}
typedef void (STDMETHODCALLTYPE *LpRootSigFn)(ID3D12GraphicsCommandList *, ID3D12RootSignature *);
static void STDMETHODCALLTYPE ListComputeRootSig(ID3D12GraphicsCommandList *self, ID3D12RootSignature *r) {
    ListAdd(self, LP_ROOTSIG, 1); LP_CALL(29, LpRootSigFn, self, r);
}
static void STDMETHODCALLTYPE ListGraphicsRootSig(ID3D12GraphicsCommandList *self, ID3D12RootSignature *r) {
    ListAdd(self, LP_ROOTSIG, 1); LP_CALL(30, LpRootSigFn, self, r);
}
/* Root descriptor tables (31, 32) and root CBV/SRV/UAV (37-42) all pass (UINT, UINT64). */
typedef void (STDMETHODCALLTYPE *LpRootArgFn)(ID3D12GraphicsCommandList *, UINT, UINT64);
#define LP_ROOTARG_HOOK(slot) static void STDMETHODCALLTYPE ListRootArg##slot(ID3D12GraphicsCommandList *self, UINT i, UINT64 v) { \
    ListAdd(self, LP_ROOTARG, 1); LP_CALL(slot, LpRootArgFn, self, i, v); }
LP_ROOTARG_HOOK(31) LP_ROOTARG_HOOK(32) LP_ROOTARG_HOOK(37) LP_ROOTARG_HOOK(38) LP_ROOTARG_HOOK(39)
LP_ROOTARG_HOOK(40) LP_ROOTARG_HOOK(41) LP_ROOTARG_HOOK(42)
typedef void (STDMETHODCALLTYPE *LpRtsFn)(ID3D12GraphicsCommandList *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE *);
static void STDMETHODCALLTYPE ListRenderTargets(ID3D12GraphicsCommandList *self, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *r, BOOL single,
        const D3D12_CPU_DESCRIPTOR_HANDLE *ds) {
    ListAdd(self, LP_RTS, 1); LP_CALL(46, LpRtsFn, self, n, r, single, ds);
}
typedef void (STDMETHODCALLTYPE *LpClearDsvFn)(ID3D12GraphicsCommandList *, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT *);
static void STDMETHODCALLTYPE ListClearDepth(ID3D12GraphicsCommandList *self, D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS f, FLOAT d, UINT8 s,
        UINT n, const D3D12_RECT *r) {
    ListAdd(self, LP_CLEAR, 1); LP_CALL(47, LpClearDsvFn, self, h, f, d, s, n, r);
}
typedef void (STDMETHODCALLTYPE *LpClearRtvFn)(ID3D12GraphicsCommandList *, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT *, UINT, const D3D12_RECT *);
static void STDMETHODCALLTYPE ListClearTarget(ID3D12GraphicsCommandList *self, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT *c, UINT n, const D3D12_RECT *r) {
    ListAdd(self, LP_CLEAR, 1); LP_CALL(48, LpClearRtvFn, self, h, c, n, r);
}
typedef void (STDMETHODCALLTYPE *LpClearUavFn)(ID3D12GraphicsCommandList *, D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource *, const void *, UINT, const D3D12_RECT *);
static void STDMETHODCALLTYPE ListClearUavUint(ID3D12GraphicsCommandList *self, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c,
        ID3D12Resource *res, const void *v, UINT n, const D3D12_RECT *r) {
    ListAdd(self, LP_CLEAR, 1); LP_CALL(49, LpClearUavFn, self, g, c, res, v, n, r);
}
static void STDMETHODCALLTYPE ListClearUavFloat(ID3D12GraphicsCommandList *self, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c,
        ID3D12Resource *res, const void *v, UINT n, const D3D12_RECT *r) {
    ListAdd(self, LP_CLEAR, 1); LP_CALL(50, LpClearUavFn, self, g, c, res, v, n, r);
}
typedef void (STDMETHODCALLTYPE *LpEventFn)(ID3D12GraphicsCommandList *, UINT, const void *, UINT);
static void STDMETHODCALLTYPE ListBeginEvent(ID3D12GraphicsCommandList *self, UINT metadata, const void *data, UINT size) {
    ListStats *s = ListSlot(self);
    if (s && !s->name[0] && data && size) {
        /* PIX events: metadata 1 = ANSI string, 0 = UTF-16 string; other encodings are skipped. */
        const unsigned char *bytes = (const unsigned char *)data;
        unsigned out = 0;
        for (UINT i = 0; i < size && out + 1 < sizeof(s->name); ++i) {
            if (metadata == 0 && i % 2) continue;
            const unsigned char ch = bytes[i];
            if (!ch) break;
            s->name[out++] = (ch > 32 && ch < 127) ? (char)ch : '_';
        }
        s->name[out] = 0;
    }
    LP_CALL(57, LpEventFn, self, metadata, data, size);
}
typedef void (STDMETHODCALLTYPE *LpIndirectFn)(ID3D12GraphicsCommandList *, ID3D12CommandSignature *, UINT, ID3D12Resource *, UINT64, ID3D12Resource *, UINT64);
static void STDMETHODCALLTYPE ListIndirect(ID3D12GraphicsCommandList *self, ID3D12CommandSignature *sig, UINT max,
        ID3D12Resource *args, UINT64 offset, ID3D12Resource *count, UINT64 countOffset) {
    ListStats *s = ListSlot(self); if (s) { s->c[LP_INDIRECT] += 1; s->c[LP_INDIRECT_MAX] += (LONG)max; PtWork(s); }
    LP_CALL(59, LpIndirectFn, self, sig, max, args, offset, count, countOffset);
}

typedef HRESULT (STDMETHODCALLTYPE *LpCloseFn)(ID3D12GraphicsCommandList *);
static HRESULT STDMETHODCALLTYPE ListClose(ID3D12GraphicsCommandList *self) {
    ListStats *s = ListSlot(self);
    if (s && s->tslot) {
        const UINT base = (UINT)(s->tslot - 1) * PT_STAMPS;
        PtSlot *slot = &ptSlots[s->tslot - 1];
        PtStamp(self, base + slot->n);
        ((LpResolveFn)(*(void ***)self)[54])(self, ptHeap, D3D12_QUERY_TYPE_TIMESTAMP, base, slot->n + 1, ptReadback, (UINT64)base * 8);
    }
    return LP_CALL(9, LpCloseFn, self);
}

static const unsigned listSlots[] = {9, 10, 12, 13, 14, 15, 16, 17, 25, 26, 27, 28, 29, 30, 31, 32, 37, 38, 39, 40, 41, 42,
    46, 47, 48, 49, 50, 57, 59};
static void *const listHooks[] = {(void *)ListClose, (void *)ListReset, (void *)ListDraw, (void *)ListDrawIndexed, (void *)ListDispatch,
    (void *)ListCopyBuffer, (void *)ListCopyTexture, (void *)ListCopyResource, (void *)ListSetPipeline, (void *)ListBarrier,
    (void *)ListBundle, (void *)ListHeaps, (void *)ListComputeRootSig, (void *)ListGraphicsRootSig, (void *)ListRootArg31,
    (void *)ListRootArg32, (void *)ListRootArg37, (void *)ListRootArg38, (void *)ListRootArg39, (void *)ListRootArg40,
    (void *)ListRootArg41, (void *)ListRootArg42, (void *)ListRenderTargets, (void *)ListClearDepth, (void *)ListClearTarget,
    (void *)ListClearUavUint, (void *)ListClearUavFloat, (void *)ListBeginEvent, (void *)ListIndirect};
_Static_assert(ARRAYSIZE(listSlots) == ARRAYSIZE(listHooks), "list hook table");

/* Patches a list's (shared) vtable once. */
static void ListPatch(void *list) {
    void **vtable = *(void ***)list;
    if (vtable[12] == (void *)ListDraw) return;
    for (unsigned i = 0; i < ARRAYSIZE(listSlots); ++i) {
        const unsigned slot = listSlots[i];
        if (vtable[slot] == listHooks[i]) continue;
        if (!listOriginal[slot]) listOriginal[slot] = vtable[slot];
        ULONG protection = 0, ignored = 0;
        if (VirtualProtectFromApp(&vtable[slot], sizeof(void *), PAGE_READWRITE, &protection)) {
            InterlockedExchangePointer(&vtable[slot], listHooks[i]);
            VirtualProtectFromApp(&vtable[slot], sizeof(void *), protection, &ignored);
        }
    }
}

static HRESULT STDMETHODCALLTYPE ListCreate(ID3D12Device *self, UINT mask, D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator *allocator, ID3D12PipelineState *state, REFIID iid, void **out) {
    const HRESULT result = listCreateOriginal(self, mask, type, allocator, state, iid, out);
    if (SUCCEEDED(result) && out && *out && type != D3D12_COMMAND_LIST_TYPE_BUNDLE) ListPatch(*out);
    return result;
}

/* Batch signature: totals for the lists of one ExecuteCommandLists call. */
typedef struct BatchInfo { LONG c[LP_COUNT]; LONG lists; char name[40]; LONG tslot[8]; LONG tcount; } BatchInfo;
static void ListBatch(UINT count, ID3D12CommandList *const *lists, BatchInfo *info) {
    for (unsigned k = 0; k < LP_COUNT; ++k) info->c[k] = 0;
    info->lists = (LONG)count; info->name[0] = 0; info->tcount = 0;
    for (UINT i = 0; i < count; ++i) {
        if (lists[i]) ListPatch(lists[i]);
        ListStats *s = ListSlot((ID3D12GraphicsCommandList *)lists[i]);
        if (!s) continue;
        if (s->tslot) {
            const LONG t = s->tslot - 1;
            s->tslot = 0;
            if (info->tcount < 8 && InterlockedCompareExchange(&ptSlots[t].state, 2, 1) == 1) {
                ptSlots[t].since = GetTickCount64();
                info->tslot[info->tcount++] = t;
            } else InterlockedCompareExchange(&ptSlots[t].state, 0, 1);
        }
        for (unsigned k = 0; k < LP_COUNT; ++k) info->c[k] += s->c[k];
        if (!info->name[0] && s->name[0]) { unsigned k = 0; while (s->name[k] && k + 1 < sizeof(info->name)) { info->name[k] = s->name[k]; ++k; } info->name[k] = 0; }
    }
}
