// Included in dxgi.cpp's private namespace, after gpu-memory-budget.h.
//
// Mesa's d3d12 driver loads the system dxgi.dll itself and calls QueryVideoMemoryInfo on the native adapter, so
// a wrapped factory never sees those calls. Instead, the QueryVideoMemoryInfo entry of the native adapter's
// method table is replaced once per process. The replacement calls the original, then caps Budget with
// gpu_memory_budget::Apply for the hardware adapters recorded at install. CurrentUsage, the reservation fields,
// the return value and every other method stay native. Mesa does not call SetVideoMemoryReservation or
// RegisterVideoMemoryBudgetChangeNotificationEvent, so those stay native as well.
//
// The loader trace records the install, the first values Mesa receives, and a `dxgi gpu-memory` line every
// 60 seconds with the process's native usage and the budget it was given, so a console run shows the curve.

// IUnknown (3), IDXGIObject (4), IDXGIAdapter (3), GetDesc1, GetDesc2 and the two content-protection
// methods precede it.
constexpr unsigned queryVideoMemoryInfoSlot = 14;
constexpr ULONGLONG memoryTraceIntervalMs = 60 * 1000;

struct MemoryBudgetAdapter {
    LUID luid;
    UINT64 capacity, cap;
};

struct MemoryBudgetTable {
    void** vtable;
    void* original;
};

MemoryBudgetAdapter memoryBudgetAdapters[4];
volatile LONG memoryBudgetAdapterCount = 0;
MemoryBudgetTable memoryBudgetTables[4];
volatile LONG memoryBudgetTableCount = 0;
SRWLOCK memoryBudgetLock = SRWLOCK_INIT;
volatile LONG memoryBudgetInstalled = 0;
// Set once native usage is seen to exceed the adapter's capacity; budgets then pass through unchanged.
volatile LONG memoryBudgetPassThrough = 0;
volatile LONG memoryBudgetFirstTraced = 0;
volatile LONG64 memoryBudgetQueries = 0;
volatile LONG64 memoryBudgetPeakUsage = 0;
volatile LONG64 memoryBudgetNextTrace = 0;
ULONGLONG memoryBudgetStart = 0;

class TraceLine {
    char text_[176];
    char* out_ = text_;
    char* end_ = text_ + sizeof(text_) - 2;

public:
    TraceLine& Text(const char* value) {
        while (*value && out_ < end_) *out_++ = *value++;
        return *this;
    }
    TraceLine& Number(UINT64 value) {
        char digits[20];
        unsigned count = 0;
        do digits[count++] = static_cast<char>('0' + value % 10); while (value /= 10);
        while (count && out_ < end_) *out_++ = digits[--count];
        return *this;
    }
    TraceLine& Field(const char* label, UINT64 value) { return Text(label).Number(value); }
    TraceLine& Mib(const char* label, UINT64 bytes) { return Field(label, bytes / gpu_memory_budget::mib); }
    TraceLine& Hex(const char* label, DWORD value) {
        Text(label);
        for (int shift = 28; shift >= 0 && out_ < end_; shift -= 4) *out_++ = "0123456789ABCDEF"[(value >> shift) & 15];
        return *this;
    }
    TraceLine& Utc() {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        const auto two = [this](WORD value) { *out_++ = static_cast<char>('0' + value / 10 % 10); *out_++ = static_cast<char>('0' + value % 10); };
        Text(" utc=");
        if (end_ - out_ < 16) return *this;
        Number(now.wYear);
        two(now.wMonth);
        two(now.wDay);
        *out_++ = 'T';
        two(now.wHour);
        two(now.wMinute);
        two(now.wSecond);
        *out_++ = 'Z';
        return *this;
    }
    void Write() {
        *out_++ = '\r';
        *out_++ = '\n';
        AppendTrace(text_, static_cast<DWORD>(out_ - text_));
    }
};

void* OriginalMemoryBudgetSlot(const void* self) {
    void** const vtable = *static_cast<void** const*>(self);
    const LONG count = InterlockedCompareExchange(&memoryBudgetTableCount, 0, 0);
    for (LONG index = 0; index < count; ++index)
        if (memoryBudgetTables[index].vtable == vtable) return memoryBudgetTables[index].original;
    return nullptr;
}

const MemoryBudgetAdapter* RecordedMemoryBudgetAdapter(IDXGIAdapter3* self) {
    DXGI_ADAPTER_DESC1 description;
    if (FAILED(self->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return nullptr;
    const LONG count = InterlockedCompareExchange(&memoryBudgetAdapterCount, 0, 0);
    for (LONG index = 0; index < count; ++index) {
        const auto& adapter = memoryBudgetAdapters[index];
        if (adapter.luid.LowPart == description.AdapterLuid.LowPart &&
                adapter.luid.HighPart == description.AdapterLuid.HighPart)
            return &adapter;
    }
    return nullptr;
}

void TraceMemoryBudget(const DXGI_QUERY_VIDEO_MEMORY_INFO& local,
        const DXGI_QUERY_VIDEO_MEMORY_INFO& nonLocal, UINT64 reported, bool passThrough) {
    const UINT64 used = local.CurrentUsage + nonLocal.CurrentUsage;
    LONG64 peak = InterlockedCompareExchange64(&memoryBudgetPeakUsage, 0, 0);
    while (static_cast<UINT64>(peak) < used) {
        const LONG64 seen = InterlockedCompareExchange64(&memoryBudgetPeakUsage, static_cast<LONG64>(used), peak);
        if (seen == peak) break;
        peak = seen;
    }
    const LONG64 queries = InterlockedIncrement64(&memoryBudgetQueries);
    if (!InterlockedExchange(&memoryBudgetFirstTraced, 1)) {
        TraceLine()
            .Mib("dxgi gpu-memory-first local_budget_mib=", local.Budget)
            .Mib(" local_used_mib=", local.CurrentUsage)
            .Mib(" reservable_mib=", local.AvailableForReservation)
            .Mib(" nonlocal_budget_mib=", nonLocal.Budget)
            .Mib(" nonlocal_used_mib=", nonLocal.CurrentUsage)
            .Mib(" budget_mib=", reported)
            .Text(passThrough ? " mode=native" : " mode=capped")
            .Write();
    }
    const ULONGLONG now = GetTickCount64();
    const LONG64 due = InterlockedCompareExchange64(&memoryBudgetNextTrace, 0, 0);
    if (static_cast<LONG64>(now) < due ||
            InterlockedCompareExchange64(&memoryBudgetNextTrace, static_cast<LONG64>(now + memoryTraceIntervalMs), due) != due)
        return;
    TraceLine()
        .Mib("dxgi gpu-memory local_used_mib=", local.CurrentUsage)
        .Mib(" nonlocal_used_mib=", nonLocal.CurrentUsage)
        .Mib(" budget_mib=", reported)
        .Mib(" native_budget_mib=", local.Budget + nonLocal.Budget)
        .Mib(" peak_mib=", static_cast<UINT64>(InterlockedCompareExchange64(&memoryBudgetPeakUsage, 0, 0)))
        .Field(" queries=", static_cast<UINT64>(queries))
        .Field(" t_s=", (now - memoryBudgetStart) / 1000)
        .Utc()
        .Write();
}

HRESULT STDMETHODCALLTYPE BudgetedVideoMemoryInfo(IDXGIAdapter3* self, UINT node,
        DXGI_MEMORY_SEGMENT_GROUP group, DXGI_QUERY_VIDEO_MEMORY_INFO* info) {
    using Call = HRESULT (STDMETHODCALLTYPE*)(IDXGIAdapter3*, UINT, DXGI_MEMORY_SEGMENT_GROUP,
                                              DXGI_QUERY_VIDEO_MEMORY_INFO*);
    const auto original = reinterpret_cast<Call>(OriginalMemoryBudgetSlot(self));
    if (!original) return E_UNEXPECTED;
    const HRESULT result = original(self, node, group, info);
    if (FAILED(result) || !info || node ||
            (group != DXGI_MEMORY_SEGMENT_GROUP_LOCAL && group != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL))
        return result;
    const DWORD previous = GetLastError();
    const MemoryBudgetAdapter* adapter = RecordedMemoryBudgetAdapter(self);
    DXGI_QUERY_VIDEO_MEMORY_INFO other;
    const bool local = group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL;
    if (adapter && FAILED(original(self, node, local ? DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL :
                                                       DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &other)))
        adapter = nullptr;
    if (adapter) {
        const DXGI_QUERY_VIDEO_MEMORY_INFO& localInfo = local ? *info : other;
        const DXGI_QUERY_VIDEO_MEMORY_INFO& nonLocalInfo = local ? other : *info;
        if (!InterlockedCompareExchange(&memoryBudgetPassThrough, 0, 0) &&
                !gpu_memory_budget::UsageFitsCapacity(localInfo.CurrentUsage, nonLocalInfo.CurrentUsage,
                                                      adapter->capacity) &&
                !InterlockedExchange(&memoryBudgetPassThrough, 1)) {
            TraceLine()
                .Mib("dxgi gpu-memory-budget mode=native reason=usage-exceeds-capacity used_mib=",
                     localInfo.CurrentUsage + nonLocalInfo.CurrentUsage)
                .Mib(" capacity_mib=", adapter->capacity)
                .Write();
        }
        const bool passThrough = InterlockedCompareExchange(&memoryBudgetPassThrough, 0, 0) != 0;
        UINT64 reported = localInfo.Budget + nonLocalInfo.Budget;
        if (!passThrough) {
            const auto capped = gpu_memory_budget::Apply(adapter->cap, localInfo.Budget, nonLocalInfo.Budget);
            reported = capped.local + capped.nonLocal;
            info->Budget = local ? capped.local : capped.nonLocal;
        }
        TraceMemoryBudget(localInfo, nonLocalInfo, reported, passThrough);
    }
    SetLastError(previous);
    return result;
}

// Caller holds memoryBudgetLock exclusively.
HRESULT PatchMemoryBudgetTable(void** vtable) {
    for (LONG index = 0; index < memoryBudgetTableCount; ++index)
        if (memoryBudgetTables[index].vtable == vtable) return S_FALSE;
    if (memoryBudgetTableCount == ARRAYSIZE(memoryBudgetTables)) return E_OUTOFMEMORY;
    void** const slot = &vtable[queryVideoMemoryInfoSlot];
    memoryBudgetTables[memoryBudgetTableCount] = {vtable, *slot};
    // Publish the original before any call can route through the replacement.
    InterlockedIncrement(&memoryBudgetTableCount);
    ULONG protection = 0;
    if (!VirtualProtectFromApp(slot, sizeof(*slot), PAGE_READWRITE, &protection))
        return HRESULT_FROM_WIN32(GetLastError());
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(&BudgetedVideoMemoryInfo));
    ULONG ignored = 0;
    VirtualProtectFromApp(slot, sizeof(*slot), protection, &ignored);
    return S_OK;
}

template<class Interface>
HRESULT PatchMemoryBudgetInterface(IUnknown* adapter) {
    ComPtr<Interface> view;
    if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&view)))) return S_FALSE;
    return PatchMemoryBudgetTable(*reinterpret_cast<void***>(view.Get()));
}

// Reads the optional package-root override. Absent, it selects the default policy.
gpu_memory_budget::Override ReadMemoryBudgetOverride() {
    WCHAR path[1024];
    static constexpr WCHAR suffix[] = L"\\xbox-gpu-memory-budget.txt";
    UINT32 length = ARRAYSIZE(path);
    if (GetCurrentPackagePath(&length, path) != ERROR_SUCCESS || !length ||
            length - 1 + ARRAYSIZE(suffix) > ARRAYSIZE(path))
        return {gpu_memory_budget::Mode::Default, 0};
    for (size_t index = 0; index < ARRAYSIZE(suffix); ++index) path[length - 1 + index] = suffix[index];
    const HANDLE file = CreateFile2FromAppW(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {gpu_memory_budget::Mode::Default, 0};
    char text[33];
    DWORD read = 0;
    const BOOL ok = ReadFile(file, text, sizeof(text), &read, nullptr);
    CloseHandle(file);
    if (!ok || read == sizeof(text)) return {gpu_memory_budget::Mode::Invalid, 0};
    return gpu_memory_budget::ParseOverride(text, read);
}

HRESULT InstallMemoryBudget() {
    if (InterlockedCompareExchange(&memoryBudgetInstalled, 1, 0)) return S_FALSE;
    memoryBudgetStart = GetTickCount64();
    const auto choice = ReadMemoryBudgetOverride();
    if (choice.mode == gpu_memory_budget::Mode::Native) {
        TraceLine().Text("dxgi gpu-memory-budget mode=native source=override").Write();
        return S_FALSE;
    }
    ComPtr<IDXGIFactory1> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    UINT patched = 0;
    for (UINT index = 0; SUCCEEDED(result) && index < ARRAYSIZE(memoryBudgetAdapters); ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 description;
        if (!adapter || FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            continue;
        const UINT64 capacity = gpu_memory_budget::Capacity(description.DedicatedVideoMemory,
                                                            description.SharedSystemMemory);
        const UINT64 cap = choice.mode == gpu_memory_budget::Mode::Explicit ?
            choice.capMib * gpu_memory_budget::mib : gpu_memory_budget::DefaultCap(capacity);
        if (!cap) continue;
        AcquireSRWLockExclusive(&memoryBudgetLock);
        memoryBudgetAdapters[memoryBudgetAdapterCount] = {description.AdapterLuid, capacity, cap};
        InterlockedIncrement(&memoryBudgetAdapterCount);
        // The widest interface first, in case the narrower view uses a different table.
        result = PatchMemoryBudgetInterface<IDXGIAdapter4>(adapter.Get());
        if (SUCCEEDED(result)) result = PatchMemoryBudgetInterface<IDXGIAdapter3>(adapter.Get());
        ReleaseSRWLockExclusive(&memoryBudgetLock);
        ++patched;
        TraceLine()
            .Hex("dxgi gpu-memory-budget install result=0x", static_cast<DWORD>(result))
            .Mib(" dedicated_mib=", description.DedicatedVideoMemory)
            .Mib(" shared_mib=", description.SharedSystemMemory)
            .Mib(" capacity_mib=", capacity)
            .Mib(" cap_mib=", cap)
            .Text(choice.mode == gpu_memory_budget::Mode::Explicit ? " source=override" :
                  choice.mode == gpu_memory_budget::Mode::Invalid ? " source=default override=invalid" :
                                                                    " source=default")
            .Write();
    }
    if (FAILED(result) || !patched)
        TraceLine().Hex("dxgi gpu-memory-budget install-failed result=0x", static_cast<DWORD>(result))
            .Field(" adapters=", patched).Write();
    return FAILED(result) ? result : patched ? S_OK : S_FALSE;
}
