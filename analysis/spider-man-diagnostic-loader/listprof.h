/* Per-command-list draw, dispatch and marker accounting, joined with fence-timed GPU time per submission.
   Hooks the game's graphics command list vtable (created through the device's CreateCommandList). Included by
   gpuprof.h. Lines start with "asset listprof". */

typedef void (STDMETHODCALLTYPE *ListDrawFn)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE *ListDrawIndexedFn)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, INT, UINT);
typedef void (STDMETHODCALLTYPE *ListDispatchFn)(ID3D12GraphicsCommandList *, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *ListResetFn)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *, ID3D12PipelineState *);
typedef void (STDMETHODCALLTYPE *ListEventFn)(ID3D12GraphicsCommandList *, UINT, const void *, UINT);
typedef void (STDMETHODCALLTYPE *ListIndirectFn)(ID3D12GraphicsCommandList *, ID3D12CommandSignature *, UINT, ID3D12Resource *, UINT64, ID3D12Resource *, UINT64);
typedef HRESULT (STDMETHODCALLTYPE *DeviceCreateListFn)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *, REFIID, void **);

#define LISTPROF_SLOTS 1024u
typedef struct ListStats { volatile ULONG_PTR list; volatile LONG draws, indexed, dispatches, indirect; char name[40]; } ListStats;
static ListStats listStats[LISTPROF_SLOTS];
static void *listOriginal[64];
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

static void STDMETHODCALLTYPE ListDraw(ID3D12GraphicsCommandList *self, UINT a, UINT b, UINT c, UINT d) {
    ListStats *s = ListSlot(self); if (s) InterlockedIncrement(&s->draws);
    ((ListDrawFn)listOriginal[12])(self, a, b, c, d);
}
static void STDMETHODCALLTYPE ListDrawIndexed(ID3D12GraphicsCommandList *self, UINT a, UINT b, UINT c, INT d, UINT e) {
    ListStats *s = ListSlot(self); if (s) InterlockedIncrement(&s->indexed);
    ((ListDrawIndexedFn)listOriginal[13])(self, a, b, c, d, e);
}
static void STDMETHODCALLTYPE ListDispatch(ID3D12GraphicsCommandList *self, UINT a, UINT b, UINT c) {
    ListStats *s = ListSlot(self); if (s) InterlockedIncrement(&s->dispatches);
    ((ListDispatchFn)listOriginal[14])(self, a, b, c);
}
static HRESULT STDMETHODCALLTYPE ListReset(ID3D12GraphicsCommandList *self, ID3D12CommandAllocator *a, ID3D12PipelineState *p) {
    ListStats *s = ListSlot(self);
    if (s) { s->draws = s->indexed = s->dispatches = s->indirect = 0; s->name[0] = 0; }
    return ((ListResetFn)listOriginal[10])(self, a, p);
}
static void STDMETHODCALLTYPE ListBeginEvent(ID3D12GraphicsCommandList *self, UINT metadata, const void *data, UINT size) {
    ListStats *s = ListSlot(self);
    if (s && !s->name[0] && data && size) {
        /* PIX events: metadata 1 = ANSI string, 0 = UTF-16 string; other encodings are skipped. */
        const unsigned char *bytes = (const unsigned char *)data;
        unsigned out = 0;
        for (UINT i = 0; i < size && out + 1 < sizeof(s->name); ++i) {
            unsigned char ch = metadata == 0 ? (i % 2 ? 0 : bytes[i]) : bytes[i];
            if (metadata == 0 && i % 2) continue;
            if (!ch) break;
            s->name[out++] = (ch > 32 && ch < 127) ? (char)ch : '_';
        }
        s->name[out] = 0;
    }
    ((ListEventFn)listOriginal[57])(self, metadata, data, size);
}
static void STDMETHODCALLTYPE ListIndirect(ID3D12GraphicsCommandList *self, ID3D12CommandSignature *sig, UINT max,
        ID3D12Resource *args, UINT64 offset, ID3D12Resource *count, UINT64 countOffset) {
    ListStats *s = ListSlot(self); if (s) InterlockedIncrement(&s->indirect);
    ((ListIndirectFn)listOriginal[59])(self, sig, max, args, offset, count, countOffset);
}

/* Patches a list's vtable once; lists created through CreateCommandList1 are caught at their first submission. */
static void ListPatch(void *list) {
    void **vtable = *(void ***)list;
    if (vtable[12] == (void *)ListDraw) return;
    static const unsigned slots[] = {10, 12, 13, 14, 57, 59};
    void *const replacements[] = {(void *)ListReset, (void *)ListDraw, (void *)ListDrawIndexed, (void *)ListDispatch,
        (void *)ListBeginEvent, (void *)ListIndirect};
    for (unsigned i = 0; i < ARRAYSIZE(slots); ++i) {
        if (!listOriginal[slots[i]]) listOriginal[slots[i]] = vtable[slots[i]];
        ULONG protection = 0, ignored = 0;
        if (VirtualProtectFromApp(&vtable[slots[i]], sizeof(void *), PAGE_READWRITE, &protection)) {
            InterlockedExchangePointer(&vtable[slots[i]], replacements[i]);
            VirtualProtectFromApp(&vtable[slots[i]], sizeof(void *), protection, &ignored);
        }
    }
}

static HRESULT STDMETHODCALLTYPE ListCreate(ID3D12Device *self, UINT mask, D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator *allocator, ID3D12PipelineState *state, REFIID iid, void **out) {
    const HRESULT result = listCreateOriginal(self, mask, type, allocator, state, iid, out);
    if (SUCCEEDED(result) && out && *out && type != D3D12_COMMAND_LIST_TYPE_BUNDLE) {
        void **vtable = *(void ***)*out;
        static const unsigned slots[] = {10, 12, 13, 14, 57, 59};
        void *const replacements[] = {(void *)ListReset, (void *)ListDraw, (void *)ListDrawIndexed, (void *)ListDispatch,
            (void *)ListBeginEvent, (void *)ListIndirect};
        if (vtable[12] != (void *)ListDraw) {
            for (unsigned i = 0; i < ARRAYSIZE(slots); ++i) {
                if (!listOriginal[slots[i]]) listOriginal[slots[i]] = vtable[slots[i]];
                ULONG protection = 0, ignored = 0;
                if (VirtualProtectFromApp(&vtable[slots[i]], sizeof(void *), PAGE_READWRITE, &protection)) {
                    InterlockedExchangePointer(&vtable[slots[i]], replacements[i]);
                    VirtualProtectFromApp(&vtable[slots[i]], sizeof(void *), protection, &ignored);
                }
            }
        }
    }
    return result;
}

/* Batch signature: totals for the lists of one ExecuteCommandLists call. */
typedef struct BatchInfo { LONG draws, dispatches, indirect, lists; char name[40]; } BatchInfo;
static void ListBatch(UINT count, ID3D12CommandList *const *lists, BatchInfo *info) {
    info->draws = info->dispatches = info->indirect = 0; info->lists = (LONG)count; info->name[0] = 0;
    for (UINT i = 0; i < count; ++i) {
        if (lists[i]) ListPatch(lists[i]);
        ListStats *s = ListSlot((ID3D12GraphicsCommandList *)lists[i]);
        if (!s) continue;
        info->draws += s->draws + s->indexed; info->dispatches += s->dispatches; info->indirect += s->indirect;
        if (!info->name[0] && s->name[0]) { unsigned k = 0; while (s->name[k] && k + 1 < sizeof(info->name)) { info->name[k] = s->name[k]; ++k; } info->name[k] = 0; }
    }
}
