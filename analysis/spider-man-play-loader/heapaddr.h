/* Diagnostic + guard: ID3D12Device3::OpenExistingHeapFromAddress / FromFileMapping hand CPU memory
   to the GPU. A demand-committed range has uncommitted pages the GPU cannot fault in, so the range
   is committed in full before the call. Every call is traced (first 64) with whether the address
   was inside a demand range. Included by trace.c after allocs.h. Lines start with "asset d3d12-". */

static const IID heapDevice3Iid = {0x81dadc15, 0x2bad, 0x4392, {0x93,0xc5,0x10,0x13,0x45,0xc4,0xaa,0x98}};
typedef HRESULT (STDMETHODCALLTYPE *HeapFromAddressFn)(ID3D12Device *, const void *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *HeapFromMappingFn)(ID3D12Device *, HANDLE, REFIID, void **);
static HeapFromAddressFn heapFromAddressOriginal;
static HeapFromMappingFn heapFromMappingOriginal;
static volatile LONG heapAddressCalls, heapMappingCalls, heapAddressLazy;

static HRESULT STDMETHODCALLTYPE HeapFromAddress(ID3D12Device *self, const void *address, REFIID iid, void **out) {
    const LONG call = InterlockedIncrement(&heapAddressCalls);
    ULONG_PTR lazyBase = 0, lazyEnd = 0;
    AcquireSRWLockShared(&xdcState.lock);
    const XdcRange *range = xdcState.live ? XdcFindLocked((ULONG_PTR)address) : NULL;
    if (range) { lazyBase = range->base; lazyEnd = range->end; }
    ReleaseSRWLockShared(&xdcState.lock);
    if (lazyEnd > lazyBase) {
        InterlockedIncrement(&heapAddressLazy);
        XboxDemandCommitPrepareWrite((LPVOID)lazyBase, (SIZE_T)(lazyEnd - lazyBase));
    }
    MEMORY_BASIC_INFORMATION info = {0};
    VirtualQuery(address, &info, sizeof(info));
    const HRESULT result = heapFromAddressOriginal(self, address, iid, out);
    if (call <= 64) {
        const DWORD error = GetLastError();
        char line[240], *o = Hex(Text(line, "asset d3d12-heap-from-address n=0x"), (DWORD)call, 4);
        o = Hex(Text(o, " hr=0x"), (unsigned long)result, 8);
        o = Hex(Text(o, " region-kib=0x"), (DWORD)(info.RegionSize >> 10), 8);
        o = Hex(Text(o, " state=0x"), info.State, 5);
        o = Hex(Text(o, " protect=0x"), info.Protect, 4);
        o = Hex(Text(o, " lazy-kib=0x"), (DWORD)((lazyEnd - lazyBase) >> 10), 8);
        o = Hex(Text(o, " caller=exe+0x"), (unsigned long)((ULONG_PTR)__builtin_return_address(0) -
            (ULONG_PTR)GetModuleHandleW(NULL)), 8);
        CensusEmit(line, o);
        SetLastError(error);
    }
    return result;
}

static HRESULT STDMETHODCALLTYPE HeapFromMapping(ID3D12Device *self, HANDLE mapping, REFIID iid, void **out) {
    const LONG call = InterlockedIncrement(&heapMappingCalls);
    const HRESULT result = heapFromMappingOriginal(self, mapping, iid, out);
    if (call <= 16) {
        const DWORD error = GetLastError();
        char line[120], *o = Hex(Text(line, "asset d3d12-heap-from-mapping n=0x"), (DWORD)call, 4);
        o = Hex(Text(o, " hr=0x"), (unsigned long)result, 8);
        CensusEmit(line, o);
        SetLastError(error);
    }
    return result;
}

static DWORD WINAPI HeapAddressSummary(void *unused);
static void HeapAddressInstall(ID3D12Device *device) {
    void *view = NULL;
    HRESULT status = ID3D12Device_QueryInterface(device, &heapDevice3Iid, &view);
    if (SUCCEEDED(status) && view) {
        void **const vtable = *(void ***)view;
        const unsigned slots[2] = {
            (unsigned)(__builtin_offsetof(ID3D12Device3Vtbl, OpenExistingHeapFromAddress) / sizeof(void *)),
            (unsigned)(__builtin_offsetof(ID3D12Device3Vtbl, OpenExistingHeapFromFileMapping) / sizeof(void *))};
        void *const replacements[2] = {(void *)HeapFromAddress, (void *)HeapFromMapping};
        heapFromAddressOriginal = (HeapFromAddressFn)vtable[slots[0]];
        heapFromMappingOriginal = (HeapFromMappingFn)vtable[slots[1]];
        for (unsigned i = 0; i < 2 && SUCCEEDED(status); ++i) {
            ULONG protection = 0, ignored = 0;
            if (!VirtualProtectFromApp(&vtable[slots[i]], sizeof(void *), PAGE_READWRITE, &protection)) {
                status = HRESULT_FROM_WIN32(GetLastError());
                break;
            }
            InterlockedExchangePointer(&vtable[slots[i]], replacements[i]);
            VirtualProtectFromApp(&vtable[slots[i]], sizeof(void *), protection, &ignored);
        }
        ((IUnknown *)view)->lpVtbl->Release((IUnknown *)view);
    }
    char line[96], *out = Hex(Text(line, "asset d3d12-heap-address-hooks status=0x"), (unsigned long)status, 8);
    CensusEmit(line, out);
    HANDLE thread = CreateThread(NULL, 0, HeapAddressSummary, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
}

/* Every 30 s for 45 min: cumulative D3D12 heap bytes and call counts from allocs.h, plus these hooks. */
static DWORD WINAPI HeapAddressSummary(void *unused) {
    (void)unused;
    for (unsigned sample = 0; sample < 90; ++sample) {
        SleepEx(30000, FALSE);
        char line[240], *out = CensusDec(Text(line, "asset d3d12-summary heap-mib="), (unsigned long long)allocHeapBytes >> 20);
        out = CensusDec(Text(out, " committed-calls="), (unsigned long long)(allocCalls[0] + allocCalls[3]));
        out = CensusDec(Text(out, " heap-calls="), (unsigned long long)(allocCalls[1] + allocCalls[4]));
        out = CensusDec(Text(out, " placed-calls="), (unsigned long long)allocCalls[2]);
        out = CensusDec(Text(out, " from-address="), (unsigned long long)heapAddressCalls);
        out = CensusDec(Text(out, " from-address-lazy="), (unsigned long long)heapAddressLazy);
        out = CensusDec(Text(out, " from-mapping="), (unsigned long long)heapMappingCalls);
        CensusEmit(line, out);
    }
    return 0;
}
