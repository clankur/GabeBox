/* Diagnostic hooks on the primed D3D12 device's resource and heap creation methods. A failure is logged
   with its arguments and the remaining process commit; successes are only counted. Included by trace.c
   after census.h. Lines start with "asset d3d12-" so the filtered loader trace keeps them. */

typedef HRESULT (STDMETHODCALLTYPE *AllocCommittedFn)(ID3D12Device *, const D3D12_HEAP_PROPERTIES *,
    D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *,
    REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *AllocHeapFn)(ID3D12Device *, const D3D12_HEAP_DESC *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *AllocPlacedFn)(ID3D12Device *, ID3D12Heap *, UINT64,
    const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *AllocCommitted1Fn)(ID3D12Device *, const D3D12_HEAP_PROPERTIES *,
    D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *,
    void *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *AllocHeap1Fn)(ID3D12Device *, const D3D12_HEAP_DESC *, void *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *AllocRemovedFn)(ID3D12Device *);

static void *allocOriginal[57];
static ID3D12Device *allocDevice;
static volatile LONG allocCalls[5], allocFailures;
static volatile LONG64 allocHeapBytes;

static void AllocTrace(const char *api, HRESULT result, const D3D12_RESOURCE_DESC *desc, UINT64 heapBytes,
        UINT heapType, UINT flags) {
    if (InterlockedIncrement(&allocFailures) > 200) return;
    MEMORYSTATUSEX memory = {.dwLength = sizeof(memory)};
    GlobalMemoryStatusEx(&memory);
    char line[320], *out = Text(Text(line, "asset d3d12-alloc-fail api="), api);
    out = Hex(Text(out, " hr=0x"), (unsigned long)result, 8);
    if (desc) {
        out = CensusDec(Text(out, " dim="), desc->Dimension);
        out = CensusDec(Text(out, " width="), desc->Width);
        out = CensusDec(Text(out, " height="), desc->Height);
        out = CensusDec(Text(out, " depth="), desc->DepthOrArraySize);
        out = CensusDec(Text(out, " mips="), desc->MipLevels);
        out = CensusDec(Text(out, " format="), desc->Format);
        out = Hex(Text(out, " res-flags=0x"), desc->Flags, 4);
    }
    out = CensusDec(Text(out, " heap-bytes="), heapBytes);
    out = CensusDec(Text(out, " heap-type="), heapType);
    out = Hex(Text(out, " heap-flags=0x"), flags, 4);
    out = CensusDec(Text(out, " avail-commit-kib="), memory.ullAvailPageFile >> 10);
    out = Hex(Text(out, " caller=exe+0x"), (unsigned long)((ULONG_PTR)__builtin_return_address(0) -
        (ULONG_PTR)GetModuleHandleW(NULL)), 8);
    CensusEmit(line, out);
}

static HRESULT STDMETHODCALLTYPE AllocCommitted(ID3D12Device *self, const D3D12_HEAP_PROPERTIES *props,
        D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
        const D3D12_CLEAR_VALUE *clear, REFIID iid, void **out) {
    InterlockedIncrement(&allocCalls[0]);
    const HRESULT result = ((AllocCommittedFn)allocOriginal[27])(self, props, flags, desc, state, clear, iid, out);
    if (FAILED(result)) {
        const DWORD error = GetLastError();
        AllocTrace("committed", result, desc, 0, props ? props->Type : 0, flags);
        SetLastError(error);
    }
    return result;
}

static HRESULT STDMETHODCALLTYPE AllocHeap(ID3D12Device *self, const D3D12_HEAP_DESC *desc, REFIID iid, void **out) {
    InterlockedIncrement(&allocCalls[1]);
    const HRESULT result = ((AllocHeapFn)allocOriginal[28])(self, desc, iid, out);
    if (SUCCEEDED(result) && desc && out) {
        InterlockedAdd64(&allocHeapBytes, (LONG64)desc->SizeInBytes);
        OwnerHeap(desc->Properties.Type, desc->Flags, desc->SizeInBytes);
    }
    if (FAILED(result)) {
        const DWORD error = GetLastError();
        AllocTrace("heap", result, NULL, desc ? desc->SizeInBytes : 0, desc ? desc->Properties.Type : 0,
            desc ? desc->Flags : 0);
        SetLastError(error);
    }
    return result;
}

static HRESULT STDMETHODCALLTYPE AllocPlaced(ID3D12Device *self, ID3D12Heap *heap, UINT64 offset,
        const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear,
        REFIID iid, void **out) {
    InterlockedIncrement(&allocCalls[2]);
    const HRESULT result = ((AllocPlacedFn)allocOriginal[29])(self, heap, offset, desc, state, clear, iid, out);
    if (FAILED(result)) {
        const DWORD error = GetLastError();
        AllocTrace("placed", result, desc, offset, 0, 0);
        SetLastError(error);
    } else if (out && *out) {
        const DWORD error = GetLastError();
        XboxSacNotePlaced(heap, offset, desc, (ID3D12Resource *)*out);   /* small-array copy workaround */
        SetLastError(error);
    }
    return result;
}

static HRESULT STDMETHODCALLTYPE AllocCommitted1(ID3D12Device *self, const D3D12_HEAP_PROPERTIES *props,
        D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
        const D3D12_CLEAR_VALUE *clear, void *session, REFIID iid, void **out) {
    InterlockedIncrement(&allocCalls[3]);
    const HRESULT result = ((AllocCommitted1Fn)allocOriginal[53])(self, props, flags, desc, state, clear,
        session, iid, out);
    if (FAILED(result)) {
        const DWORD error = GetLastError();
        AllocTrace("committed1", result, desc, 0, props ? props->Type : 0, flags);
        SetLastError(error);
    }
    return result;
}

static HRESULT STDMETHODCALLTYPE AllocHeap1(ID3D12Device *self, const D3D12_HEAP_DESC *desc, void *session,
        REFIID iid, void **out) {
    InterlockedIncrement(&allocCalls[4]);
    const HRESULT result = ((AllocHeap1Fn)allocOriginal[54])(self, desc, session, iid, out);
    if (SUCCEEDED(result) && desc && out) {
        InterlockedAdd64(&allocHeapBytes, (LONG64)desc->SizeInBytes);
        OwnerHeap(desc->Properties.Type, desc->Flags, desc->SizeInBytes);
    }
    if (FAILED(result)) {
        const DWORD error = GetLastError();
        AllocTrace("heap1", result, NULL, desc ? desc->SizeInBytes : 0, desc ? desc->Properties.Type : 0,
            desc ? desc->Flags : 0);
        SetLastError(error);
    }
    return result;
}

/* ID3D12Device::GetDeviceRemovedReason reaches the kernel (D3DKMT device state) on every call. The engine
   checks it around each GPU wait, so it can cost a large share of the render thread. A successful result
   is reused for up to 8 ms; any failure is returned immediately and every later call queries again, so a
   removal is reported at most 8 ms late. */
static volatile LONG removedCalls, removedQueries;
static volatile LONG64 removedTicks, removedLastOk;
static LARGE_INTEGER removedFrequency;
static HRESULT STDMETHODCALLTYPE AllocRemovedCached(ID3D12Device *self) {
    InterlockedIncrement(&removedCalls);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONG64 last = removedLastOk;
    if (last && now.QuadPart - last < removedFrequency.QuadPart / 125) return S_OK;
    InterlockedIncrement(&removedQueries);
    const HRESULT result = ((AllocRemovedFn)allocOriginal[37])(self);
    LARGE_INTEGER after;
    QueryPerformanceCounter(&after);
    InterlockedAdd64(&removedTicks, after.QuadPart - now.QuadPart);
    InterlockedExchange64(&removedLastOk, result == S_OK ? after.QuadPart : 0);
    return result;
}

#include "gpuprof.h"

static void AllocInstall(ID3D12Device *device) {
#ifdef PLAY_BUILD
    static const unsigned slots[] = {27, 28, 29, 53, 54, 37};
#else
    static const unsigned slots[] = {27, 28, 29, 53, 54, 37, 8, 12};
#endif
    void *const replacements[] = {(void *)AllocCommitted, (void *)AllocHeap, (void *)AllocPlaced,
        (void *)AllocCommitted1, (void *)AllocHeap1, (void *)AllocRemovedCached, (void *)GpuCreateQueue, (void *)ListCreate};
    gpuCreateQueueOriginal = (GpuCreateQueueFn)(*(void ***)device)[8];
    listCreateOriginal = (DeviceCreateListFn)(*(void ***)device)[12];
    QueryPerformanceFrequency(&removedFrequency);
    void **const vtable = *(void ***)device;
    HRESULT status = S_OK;
    for (unsigned i = 0; i < ARRAYSIZE(slots); ++i) allocOriginal[slots[i]] = vtable[slots[i]];
    for (unsigned i = 0; i < ARRAYSIZE(slots); ++i) {
        ULONG protection = 0, ignored = 0;
        if (!VirtualProtectFromApp(&vtable[slots[i]], sizeof(void *), PAGE_READWRITE, &protection)) {
            status = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        InterlockedExchangePointer(&vtable[slots[i]], replacements[i]);
        VirtualProtectFromApp(&vtable[slots[i]], sizeof(void *), protection, &ignored);
    }
    if (SUCCEEDED(status)) allocDevice = device;
    char line[96], *out = Hex(Text(line, "asset d3d12-alloc-hooks status=0x"), (unsigned long)status, 8);
    CensusEmit(line, out);
}

/* Called from the census loop. */
static void AllocSummary(unsigned sample) {
    if (!allocDevice) return;
    const HRESULT removed = ((AllocRemovedFn)allocOriginal[37])(allocDevice);
    char line[256], *out = CensusDec(Text(line, "asset d3d12-alloc-summary sample="), sample);
    out = CensusDec(Text(out, " committed="), (unsigned long long)allocCalls[0]);
    out = CensusDec(Text(out, " heaps="), (unsigned long long)allocCalls[1]);
    out = CensusDec(Text(out, " placed="), (unsigned long long)allocCalls[2]);
    out = CensusDec(Text(out, " committed1="), (unsigned long long)allocCalls[3]);
    out = CensusDec(Text(out, " heaps1="), (unsigned long long)allocCalls[4]);
    out = CensusDec(Text(out, " heap-mib="), (unsigned long long)(allocHeapBytes >> 20));
    out = CensusDec(Text(out, " failures="), (unsigned long long)allocFailures);
    out = Hex(Text(out, " removed=0x"), (unsigned long)removed, 8);
    out = CensusDec(Text(out, " removed-calls="), (unsigned long long)removedCalls);
    out = CensusDec(Text(out, " removed-queries="), (unsigned long long)removedQueries);
    out = CensusDec(Text(out, " removed-query-ms="),
        removedFrequency.QuadPart ? (unsigned long long)(removedTicks * 1000 / removedFrequency.QuadPart) : 0);
    CensusEmit(line, out);
}

static void AllocSummaryHook(unsigned sample) { AllocSummary(sample); }
static void GpuReportHook(unsigned sample) { GpuReport(sample); }
