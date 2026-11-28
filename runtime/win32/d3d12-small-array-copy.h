/* Copies into small subresources of 2D texture arrays (Xbox D3D12 workaround).

   On the console, CopyTextureRegion into a small subresource of a texture array can write where shader views don't
   read. Small means about 1 KiB or less, for example a 32x32 BC6H face, the mips of 8x8 blocks of a larger BC
   texture, or an 8x8 RGBA16F mip. Slices 0-1 survive. Later slices sample as zero, or as a neighbouring resource's
   bytes. Copy readback returns the uploaded data, so only shaders see the fault.

   Console controls (analysis/spider-man-compatibility.md, "Rainbow sky"):
     - Copy queue: wrong for every format, with or without explicit COMMON<->COPY_DEST barriers.
     - Direct queue: right for float and UINT formats.
     - Block-compressed formats: right only when the block bytes go through a placed UINT alias of the texture
       (R32G32B32A32_UINT for 16-byte blocks, R32G32_UINT for 8-byte blocks) at the same heap offset.

   What this header does:
     - Direct lists: a copy into an affected block-compressed subresource is rewritten in place to go through the alias.
     - Copy lists: the title's copy still runs. When the title submits the list, the same copies are replayed on a private
       direct queue (through the alias for block formats), after the copy queue finishes and before the copy queue's next
       signal. The title's own upload fence therefore completes only after the corrected data is in place.
   The upload source stays valid until then, because the title can't reuse it before that fence. The alias is created
   with the texture and owned by it through SetPrivateDataInterface, so it lives exactly as long as the texture.

   Wire-up: XboxSacInstall(device) after device creation, XboxSacNotePlaced from the CreatePlacedResource hook.
   Diagnostics: the xboxSacTrace sink and the xboxSac counters. */
#pragma once
#include <windows.h>
#include <d3d12.h>

#ifndef XBOX_SAC_SMALL_BYTES
#define XBOX_SAC_SMALL_BYTES 2048u   /* subresources up to this size are corrected (1 KiB failed; 4 KiB BC6H passed) */
#endif
#define XBOX_SAC_PENDING 4096u         /* copies recorded on copy lists and not yet submitted */
#define XBOX_SAC_SLOTS 8u              /* replay command lists in flight */
#define XBOX_SAC_VTABLES 4u            /* distinct command list / queue implementations hooked */

typedef void (STDMETHODCALLTYPE *XboxSacCopyFn)(ID3D12GraphicsCommandList *, const D3D12_TEXTURE_COPY_LOCATION *, UINT, UINT, UINT,
    const D3D12_TEXTURE_COPY_LOCATION *, const D3D12_BOX *);
typedef void (STDMETHODCALLTYPE *XboxSacBarrierFn)(ID3D12GraphicsCommandList *, UINT, const D3D12_RESOURCE_BARRIER *);
typedef HRESULT (STDMETHODCALLTYPE *XboxSacResetFn)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *, ID3D12PipelineState *);
typedef void (STDMETHODCALLTYPE *XboxSacExecuteFn)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
typedef HRESULT (STDMETHODCALLTYPE *XboxSacSignalFn)(ID3D12CommandQueue *, ID3D12Fence *, UINT64);
typedef HRESULT (STDMETHODCALLTYPE *XboxSacQueueFn)(ID3D12Device *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *XboxSacListFn)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *,
    ID3D12PipelineState *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *XboxSacList1Fn)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, UINT, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *XboxSacPlacedFn)(ID3D12Device *, ID3D12Heap *, UINT64, const D3D12_RESOURCE_DESC *,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);

/* Optional trace sink supplied by the includer: (event, value a, value b). */
static void (*xboxSacTrace)(const char *event, unsigned long long a, unsigned long long b);

/* {6b1e6f0c-0a4c-4a8e-9d55-2f0c5a3e7a11}: the UINT alias attached to a block-compressed array texture */
static const GUID xboxSacAliasGuid = {0x6b1e6f0c, 0x0a4c, 0x4a8e, {0x9d, 0x55, 0x2f, 0x0c, 0x5a, 0x3e, 0x7a, 0x11}};

/* Originals per implementation: copy, direct and compute lists (or queues) may not share a vtable. */
typedef struct { void **vtable; XboxSacCopyFn copy; XboxSacResetFn reset; XboxSacBarrierFn barrier; } XboxSacListOps;
typedef struct { void **vtable; XboxSacExecuteFn execute; XboxSacSignalFn signal; } XboxSacQueueOps;

typedef struct {
    ID3D12GraphicsCommandList *list;
    ID3D12Resource *dst; UINT sub, x, y, z;
    D3D12_TEXTURE_COPY_LOCATION src;
    D3D12_BOX box; BOOL hasBox;
} XboxSacPending;

static struct {
    SRWLOCK lock;
    ID3D12Device *device;
    XboxSacListOps listOps[XBOX_SAC_VTABLES]; XboxSacQueueOps queueOps[XBOX_SAC_VTABLES];
    XboxSacQueueFn createQueue; XboxSacListFn createList; XboxSacList1Fn createList1; XboxSacPlacedFn placed;
    XboxSacPending pending[XBOX_SAC_PENDING]; UINT pendingCount;
    ID3D12CommandQueue *queue;                 /* private direct queue for replays */
    ID3D12Fence *copyDone, *replayDone; UINT64 copyValue, replayValue;
    ID3D12CommandAllocator *allocators[XBOX_SAC_SLOTS]; ID3D12GraphicsCommandList *lists[XBOX_SAC_SLOTS]; UINT64 slotValue[XBOX_SAC_SLOTS];
    UINT nextSlot;
    volatile LONG aliases, inlineRewrites, replayed, replayBatches, dropped, unsupported;
} xboxSac = {SRWLOCK_INIT};

static const XboxSacListOps *XboxSacList(void *list) {
    void **const vtable = *(void ***)list;
    for (UINT i = 0; i < XBOX_SAC_VTABLES; ++i) if (xboxSac.listOps[i].vtable == vtable) return &xboxSac.listOps[i];
    return NULL;
}

static const XboxSacQueueOps *XboxSacQueue(void *queue) {
    void **const vtable = *(void ***)queue;
    for (UINT i = 0; i < XBOX_SAC_VTABLES; ++i) if (xboxSac.queueOps[i].vtable == vtable) return &xboxSac.queueOps[i];
    return NULL;
}

static UINT XboxSacBlockBytes(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        return 8;
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
        return 16;
    default:
        return 0;
    }
}

static UINT XboxSacElementBytes(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 16;
    case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT:
        return 12;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
        return 8;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM:
        return 2;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_A8_UNORM:
        return 1;
    default:
        return 4;
    }
}

/* Bytes of subresource `sub` when it is a small subresource of a covered 2D array, else 0. */
static UINT64 XboxSacSmallBytes(const D3D12_RESOURCE_DESC *d, UINT sub) {
    if (d->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d->DepthOrArraySize < 2 || d->Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN ||
            (d->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) || d->SampleDesc.Count > 1)
        return 0;
    const UINT mips = d->MipLevels ? d->MipLevels : 1;
    const UINT mip = sub % mips;
    UINT64 w = d->Width >> mip; UINT64 h = d->Height >> mip;
    if (!w) w = 1;
    if (!h) h = 1;
    const UINT block = XboxSacBlockBytes(d->Format);
    const UINT64 bytes = block ? ((w + 3) / 4) * ((h + 3) / 4) * block : w * h * XboxSacElementBytes(d->Format);
    return bytes <= XBOX_SAC_SMALL_BYTES ? bytes : 0;
}

static ID3D12Resource *XboxSacAlias(ID3D12Resource *resource) {
    IUnknown *alias = NULL;
    UINT size = sizeof(alias);
    if (FAILED(resource->lpVtbl->GetPrivateData(resource, &xboxSacAliasGuid, &size, &alias)) || size != sizeof(alias) || !alias) return NULL;
    alias->lpVtbl->Release(alias);   /* the texture keeps its own reference */
    return (ID3D12Resource *)alias;
}

/* Called after a successful CreatePlacedResource: a block-compressed array with small mips gets its UINT alias. */
static void XboxSacNotePlaced(ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC *desc, ID3D12Resource *resource) {
    if (!xboxSac.device || !xboxSac.placed || !heap || !desc || !resource) return;
    const UINT block = XboxSacBlockBytes(desc->Format);
    const UINT mips = desc->MipLevels ? desc->MipLevels : 1;
    if (!block || !XboxSacSmallBytes(desc, mips - 1)) return;
    D3D12_RESOURCE_DESC alias = *desc;
    alias.Format = block == 16 ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32_UINT;
    alias.Width = (desc->Width + 3) / 4;
    alias.Height = (desc->Height + 3) / 4;
    alias.Alignment = 0;
    static const IID iidResource = {0x696442be, 0xa72e, 0x4059, {0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad}};
    ID3D12Resource *view = NULL;
    if (SUCCEEDED(xboxSac.placed(xboxSac.device, heap, offset, &alias, D3D12_RESOURCE_STATE_COMMON, NULL, &iidResource, (void **)&view)) && view) {
        resource->lpVtbl->SetPrivateDataInterface(resource, &xboxSacAliasGuid, (IUnknown *)view);
        view->lpVtbl->Release(view);
        const LONG n = InterlockedIncrement(&xboxSac.aliases);
        if (xboxSacTrace && (n <= 4 || !(n & (n - 1)))) xboxSacTrace("sac-alias", (unsigned long long)n, desc->Format);
    }
}

static void XboxSacTransition(ID3D12GraphicsCommandList *list, ID3D12Resource *r, UINT sub, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b;
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = r; b.Transition.Subresource = sub; b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    XboxSacList(list)->barrier(list, 1, &b);
}

static void XboxSacAliasing(ID3D12GraphicsCommandList *list, ID3D12Resource *before, ID3D12Resource *after) {
    D3D12_RESOURCE_BARRIER b;
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING; b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Aliasing.pResourceBefore = before; b.Aliasing.pResourceAfter = after;
    XboxSacList(list)->barrier(list, 1, &b);
}

/* Record the copy into `list` so that shaders read it: block formats through the alias, others directly.
   `state` is the destination's state in `list` (COPY_DEST for a title list, COMMON for a replay).
   Returns FALSE when the copy has no supported rewrite. */
static BOOL XboxSacRecordFixed(ID3D12GraphicsCommandList *list, ID3D12Resource *dstResource, UINT sub, UINT x, UINT y, UINT z,
        const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box, D3D12_RESOURCE_STATES state) {
    const XboxSacCopyFn copy = XboxSacList(list)->copy;
    D3D12_RESOURCE_DESC d;
    dstResource->lpVtbl->GetDesc(dstResource, &d);
    const UINT block = XboxSacBlockBytes(d.Format);
    D3D12_TEXTURE_COPY_LOCATION dst = {dstResource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.SubresourceIndex = sub;
    if (!block) {
        if (state != D3D12_RESOURCE_STATE_COPY_DEST) XboxSacTransition(list, dstResource, sub, state, D3D12_RESOURCE_STATE_COPY_DEST);
        copy(list, &dst, x, y, z, src, box);
        if (state != D3D12_RESOURCE_STATE_COPY_DEST) XboxSacTransition(list, dstResource, sub, D3D12_RESOURCE_STATE_COPY_DEST, state);
        return TRUE;
    }
    ID3D12Resource *alias = XboxSacAlias(dstResource);
    if (!alias || x % 4 || y % 4) return FALSE;
    D3D12_TEXTURE_COPY_LOCATION aliasDst = dst, aliasSrc = *src;
    aliasDst.pResource = alias;
    D3D12_BOX blocks; const D3D12_BOX *aliasBox = NULL;
    if (src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
        if (XboxSacBlockBytes(src->PlacedFootprint.Footprint.Format) != block) return FALSE;
        aliasSrc.PlacedFootprint.Footprint.Format = block == 16 ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32_UINT;
        aliasSrc.PlacedFootprint.Footprint.Width = (src->PlacedFootprint.Footprint.Width + 3) / 4;
        aliasSrc.PlacedFootprint.Footprint.Height = (src->PlacedFootprint.Footprint.Height + 3) / 4;
        if (box) {
            blocks.left = box->left / 4; blocks.top = box->top / 4; blocks.front = box->front;
            blocks.right = (box->right + 3) / 4; blocks.bottom = (box->bottom + 3) / 4; blocks.back = box->back;
            aliasBox = &blocks;
        }
    } else {
        if (!src->pResource) return FALSE;
        D3D12_RESOURCE_DESC s;
        src->pResource->lpVtbl->GetDesc(src->pResource, &s);
        /* a UINT intermediate laid out in blocks copies 1:1; a block-compressed source would need box rescaling */
        if (XboxSacBlockBytes(s.Format) || XboxSacElementBytes(s.Format) != block) return FALSE;
        aliasBox = box;
    }
    XboxSacAliasing(list, dstResource, alias);
    XboxSacTransition(list, alias, sub, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    copy(list, &aliasDst, x / 4, y / 4, z, &aliasSrc, aliasBox);
    XboxSacTransition(list, alias, sub, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    XboxSacAliasing(list, alias, dstResource);
    return TRUE;
}

static void STDMETHODCALLTYPE XboxSacCopyHook(ID3D12GraphicsCommandList *list, const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y,
        UINT z, const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box) {
    const XboxSacCopyFn copy = XboxSacList(list)->copy;
    if (!dst || !src || !dst->pResource || dst->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        copy(list, dst, x, y, z, src, box);
        return;
    }
    D3D12_RESOURCE_DESC d;
    dst->pResource->lpVtbl->GetDesc(dst->pResource, &d);
    if (!XboxSacSmallBytes(&d, dst->SubresourceIndex)) {
        copy(list, dst, x, y, z, src, box);
        return;
    }
    if (list->lpVtbl->GetType(list) == D3D12_COMMAND_LIST_TYPE_COPY) {
        /* keep the title's copy; replay it on the direct queue when the list is submitted */
        copy(list, dst, x, y, z, src, box);
        AcquireSRWLockExclusive(&xboxSac.lock);
        if (xboxSac.pendingCount < XBOX_SAC_PENDING) {
            XboxSacPending *p = &xboxSac.pending[xboxSac.pendingCount++];
            p->list = list; p->dst = dst->pResource; p->sub = dst->SubresourceIndex; p->x = x; p->y = y; p->z = z;
            p->src = *src; p->hasBox = box != NULL;
            if (box) p->box = *box;
        } else {
            InterlockedIncrement(&xboxSac.dropped);
        }
        ReleaseSRWLockExclusive(&xboxSac.lock);
        return;
    }
    if (XboxSacBlockBytes(d.Format) && XboxSacRecordFixed(list, dst->pResource, dst->SubresourceIndex, x, y, z, src, box,
            D3D12_RESOURCE_STATE_COPY_DEST)) {
        const LONG n = InterlockedIncrement(&xboxSac.inlineRewrites);
        if (xboxSacTrace && (n <= 4 || !(n & (n - 1)))) xboxSacTrace("sac-inline", (unsigned long long)n, d.Format);
        return;
    }
    copy(list, dst, x, y, z, src, box);
}

static HRESULT STDMETHODCALLTYPE XboxSacResetHook(ID3D12GraphicsCommandList *list, ID3D12CommandAllocator *allocator, ID3D12PipelineState *pso) {
    AcquireSRWLockExclusive(&xboxSac.lock);   /* a list reset before submission discards its recorded copies */
    UINT kept = 0;
    for (UINT i = 0; i < xboxSac.pendingCount; ++i) if (xboxSac.pending[i].list != list) xboxSac.pending[kept++] = xboxSac.pending[i];
    xboxSac.pendingCount = kept;
    ReleaseSRWLockExclusive(&xboxSac.lock);
    return XboxSacList(list)->reset(list, allocator, pso);
}

static void STDMETHODCALLTYPE XboxSacExecuteHook(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists) {
    XboxSacQueue(queue)->execute(queue, count, lists);
    if (queue == xboxSac.queue || !xboxSac.queue) return;
    D3D12_COMMAND_QUEUE_DESC qd;
    queue->lpVtbl->GetDesc(queue, &qd);
    if (qd.Type != D3D12_COMMAND_LIST_TYPE_COPY) return;
    AcquireSRWLockExclusive(&xboxSac.lock);
    UINT found = 0;
    for (UINT i = 0; i < xboxSac.pendingCount; ++i)
        for (UINT k = 0; k < count; ++k) if ((void *)xboxSac.pending[i].list == (void *)lists[k]) { ++found; break; }
    if (found) {
        /* replay slot: wait until its previous replay finished */
        const UINT slot = xboxSac.nextSlot++ % XBOX_SAC_SLOTS;
        while (xboxSac.slotValue[slot] && xboxSac.replayDone->lpVtbl->GetCompletedValue(xboxSac.replayDone) < xboxSac.slotValue[slot])
            Sleep(1);   /* the allocator can't be reset while its replay is in flight */
        ID3D12CommandAllocator *allocator = xboxSac.allocators[slot];
        ID3D12GraphicsCommandList *replay = xboxSac.lists[slot];
        allocator->lpVtbl->Reset(allocator);
        XboxSacList(replay)->reset(replay, allocator, NULL);
        UINT kept = 0, done = 0;
        for (UINT i = 0; i < xboxSac.pendingCount; ++i) {
            XboxSacPending *p = &xboxSac.pending[i];
            BOOL submitted = FALSE;
            for (UINT k = 0; k < count; ++k) if ((void *)p->list == (void *)lists[k]) { submitted = TRUE; break; }
            if (!submitted) { xboxSac.pending[kept++] = *p; continue; }
            if (XboxSacRecordFixed(replay, p->dst, p->sub, p->x, p->y, p->z, &p->src, p->hasBox ? &p->box : NULL, D3D12_RESOURCE_STATE_COMMON)) ++done;
            else InterlockedIncrement(&xboxSac.unsupported);
        }
        xboxSac.pendingCount = kept;
        replay->lpVtbl->Close(replay);
        /* copy queue -> replay on the private direct queue -> copy queue resumes (the title's next signal follows) */
        const XboxSacQueueOps *title = XboxSacQueue(queue), *own = XboxSacQueue(xboxSac.queue);
        title->signal(queue, xboxSac.copyDone, ++xboxSac.copyValue);
        xboxSac.queue->lpVtbl->Wait(xboxSac.queue, xboxSac.copyDone, xboxSac.copyValue);
        own->execute(xboxSac.queue, 1, (ID3D12CommandList *const *)&replay);
        own->signal(xboxSac.queue, xboxSac.replayDone, ++xboxSac.replayValue);
        queue->lpVtbl->Wait(queue, xboxSac.replayDone, xboxSac.replayValue);
        xboxSac.slotValue[slot] = xboxSac.replayValue;
        InterlockedAdd(&xboxSac.replayed, (LONG)done);
        const LONG batches = InterlockedIncrement(&xboxSac.replayBatches);
        if (xboxSacTrace && (batches <= 8 || !(batches & (batches - 1)))) xboxSacTrace("sac-replay", (unsigned long long)batches, done);
    }
    ReleaseSRWLockExclusive(&xboxSac.lock);
}

static void XboxSacWrite(void **entry, void *hook) {
    ULONG protection = 0, ignored = 0;
    if (VirtualProtectFromApp(entry, sizeof(void *), PAGE_READWRITE, &protection)) {
        InterlockedExchangePointer(entry, hook);
        VirtualProtectFromApp(entry, sizeof(void *), protection, &ignored);
    }
}

/* Hook copy (16), reset (10) and remember barrier (26) for this list's implementation. Called under no lock from
   list creation; the table only grows and an entry is complete before its vtable is published. */
static void XboxSacPatchList(ID3D12GraphicsCommandList *list) {
    void **const vtable = *(void ***)list;
    static SRWLOCK patchLock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&patchLock);
    if (!XboxSacList(list) && vtable[16] != (void *)XboxSacCopyHook) {
        for (UINT i = 0; i < XBOX_SAC_VTABLES; ++i) {
            XboxSacListOps *ops = &xboxSac.listOps[i];
            if (ops->vtable) continue;
            ops->copy = (XboxSacCopyFn)vtable[16]; ops->reset = (XboxSacResetFn)vtable[10]; ops->barrier = (XboxSacBarrierFn)vtable[26];
            InterlockedExchangePointer((void **)&ops->vtable, vtable);
            XboxSacWrite(&vtable[16], (void *)XboxSacCopyHook);
            XboxSacWrite(&vtable[10], (void *)XboxSacResetHook);
            if (xboxSacTrace) xboxSacTrace("sac-list-vtable", i, (unsigned long long)list->lpVtbl->GetType(list));
            break;
        }
    }
    ReleaseSRWLockExclusive(&patchLock);
}

static void XboxSacPatchQueue(ID3D12CommandQueue *queue) {
    void **const vtable = *(void ***)queue;
    static SRWLOCK patchLock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&patchLock);
    if (!XboxSacQueue(queue) && vtable[10] != (void *)XboxSacExecuteHook) {
        for (UINT i = 0; i < XBOX_SAC_VTABLES; ++i) {
            XboxSacQueueOps *ops = &xboxSac.queueOps[i];
            if (ops->vtable) continue;
            ops->execute = (XboxSacExecuteFn)vtable[10]; ops->signal = (XboxSacSignalFn)vtable[14];
            InterlockedExchangePointer((void **)&ops->vtable, vtable);
            XboxSacWrite(&vtable[10], (void *)XboxSacExecuteHook);
            if (xboxSacTrace) xboxSacTrace("sac-queue-vtable", i, 0);
            break;
        }
    }
    ReleaseSRWLockExclusive(&patchLock);
}

static void XboxSacPatchDevice(void **vtable, unsigned slot, void *hook, void **original) {
    if (vtable[slot] == hook) return;
    *original = vtable[slot];
    XboxSacWrite(&vtable[slot], hook);
}

static HRESULT STDMETHODCALLTYPE XboxSacCreateListHook(ID3D12Device *device, UINT mask, D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator *allocator, ID3D12PipelineState *pso, REFIID iid, void **out) {
    const HRESULT hr = xboxSac.createList(device, mask, type, allocator, pso, iid, out);
    if (SUCCEEDED(hr) && out && *out && type != D3D12_COMMAND_LIST_TYPE_BUNDLE) XboxSacPatchList((ID3D12GraphicsCommandList *)*out);
    return hr;
}

/* ID3D12Device4::CreateCommandList1 (slot 51) creates lists closed, without an allocator. */
static HRESULT STDMETHODCALLTYPE XboxSacCreateList1Hook(ID3D12Device *device, UINT mask, D3D12_COMMAND_LIST_TYPE type, UINT flags,
        REFIID iid, void **out) {
    const HRESULT hr = xboxSac.createList1(device, mask, type, flags, iid, out);
    if (SUCCEEDED(hr) && out && *out && type != D3D12_COMMAND_LIST_TYPE_BUNDLE) XboxSacPatchList((ID3D12GraphicsCommandList *)*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE XboxSacCreateQueueHook(ID3D12Device *device, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID iid, void **out) {
    const HRESULT hr = xboxSac.createQueue(device, desc, iid, out);
    if (SUCCEEDED(hr) && out && *out) {
        XboxSacPatchQueue((ID3D12CommandQueue *)*out);
    }
    return hr;
}

/* Install on the title's device before it creates queues and lists. `placed` is the CreatePlacedResource to use for
   aliases (the chain below any other hook). Returns S_OK or the failing HRESULT; on failure nothing is patched. */
static HRESULT XboxSacInstall(ID3D12Device *device, XboxSacPlacedFn placed) {
    static const IID iidQueue = {0x0ec870a6, 0x5d7e, 0x4c22, {0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed}};
    static const IID iidAlloc = {0x6102dee4, 0xaf59, 0x4b09, {0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24}};
    static const IID iidList = {0x5b160d0f, 0xac1b, 0x4185, {0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55}};
    static const IID iidFence = {0x0a753dcf, 0xc4d8, 0x4b91, {0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76}};
    if (!device || xboxSac.device) return E_INVALIDARG;
    void **vtable = *(void ***)device;
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    ID3D12CommandQueue *queue = NULL;
    HRESULT hr = ((XboxSacQueueFn)vtable[8])(device, &qd, &iidQueue, (void **)&queue);
    if (SUCCEEDED(hr)) hr = device->lpVtbl->CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &iidFence, (void **)&xboxSac.copyDone);
    if (SUCCEEDED(hr)) hr = device->lpVtbl->CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &iidFence, (void **)&xboxSac.replayDone);
    for (UINT i = 0; SUCCEEDED(hr) && i < XBOX_SAC_SLOTS; ++i) {
        hr = device->lpVtbl->CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &iidAlloc, (void **)&xboxSac.allocators[i]);
        if (SUCCEEDED(hr)) hr = ((XboxSacListFn)vtable[12])(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, xboxSac.allocators[i], NULL, &iidList,
            (void **)&xboxSac.lists[i]);
        if (SUCCEEDED(hr)) xboxSac.lists[i]->lpVtbl->Close(xboxSac.lists[i]);
    }
    if (FAILED(hr)) return hr;
    /* the private list's vtable is the title's: patch it now so copy/reset/barrier originals are known */
    XboxSacPatchList(xboxSac.lists[0]);
    XboxSacPatchQueue(queue);
    xboxSac.placed = placed;
    xboxSac.queue = queue;
    XboxSacPatchDevice(vtable, 8, (void *)XboxSacCreateQueueHook, (void **)&xboxSac.createQueue);
    XboxSacPatchDevice(vtable, 12, (void *)XboxSacCreateListHook, (void **)&xboxSac.createList);
    static const IID iidDevice4 = {0xe865df17, 0xa9ee, 0x46f9, {0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5}};
    IUnknown *device4 = NULL;
    if (SUCCEEDED(device->lpVtbl->QueryInterface(device, &iidDevice4, (void **)&device4)) && device4) {
        if (*(void ***)device4 == vtable) XboxSacPatchDevice(vtable, 51, (void *)XboxSacCreateList1Hook, (void **)&xboxSac.createList1);
        device4->lpVtbl->Release(device4);
    }
    xboxSac.device = device;
    return S_OK;
}
