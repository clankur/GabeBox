// Included in dxgi.cpp's private namespace.
//
// Xbox's unified-memory GPU reports a fixed 512 MB carve-out as DedicatedVideoMemory, so games that size
// their texture settings from the adapter description treat the console as a low-end integrated GPU.
// When a profile opts in, hardware adapter descriptions report the process's local video memory budget
// instead. The budget comes from the console at call time, so each console model reports its own value.
//
// A D3D11 device returns dxgi.dll's own adapter object from IDXGIDevice::GetAdapter, so wrapping only the
// factory's adapters would miss games that read the description through the device. The override instead
// replaces the description methods in the native adapter's vtable once per process. Every other method,
// including QueryVideoMemoryInfo, stays native.

bool AdapterBudgetEnabled() {
    WCHAR value[2];
    const DWORD previous = GetLastError();
    const bool enabled = GetEnvironmentVariableW(L"XBOX_WIN32_DXGI_ADAPTER_BUDGET", value, ARRAYSIZE(value)) == 1 &&
        value[0] == L'1';
    SetLastError(previous);
    return enabled;
}

// IUnknown (3) + IDXGIObject (4) precede IDXGIAdapter's methods.
constexpr unsigned getDescSlot = 8, getDesc1Slot = 10, getDesc2Slot = 11, getDesc3Slot = 18, budgetSlots = 19;

struct BudgetTable {
    void** vtable;
    void* original[budgetSlots];
};

BudgetTable budgetTables[4];
volatile LONG budgetTableCount = 0;
SRWLOCK budgetLock = SRWLOCK_INIT;
volatile LONG budgetTraced = 0;

void* OriginalBudgetSlot(const void* self, unsigned slot) {
    void** const vtable = *static_cast<void** const*>(self);
    const LONG count = InterlockedCompareExchange(&budgetTableCount, 0, 0);
    for (LONG index = 0; index < count; ++index)
        if (budgetTables[index].vtable == vtable) return budgetTables[index].original[slot];
    return nullptr;
}

void TraceBudget(UINT64 reported, UINT64 budget) {
    if (InterlockedExchange(&budgetTraced, 1)) return;
    char line[96], *out = line;
    const auto text = [&](const char* value) { while (*value) *out++ = *value++; };
    const auto number = [&](UINT64 value) {
        char digits[20];
        unsigned count = 0;
        do digits[count++] = static_cast<char>('0' + value % 10); while (value /= 10);
        while (count) *out++ = digits[--count];
    };
    text("dxgi adapter-budget reported-mib=");
    number(reported >> 20);
    text(" budget-mib=");
    number(budget >> 20);
    text("\r\n");
    AppendTrace(line, static_cast<DWORD>(out - line));
}

// Returns the local segment budget, or the reported size when the budget is unavailable or smaller.
SIZE_T BudgetedMemory(IDXGIAdapter* self, SIZE_T reported, bool software) {
    if (software) return reported;
    const DWORD previous = GetLastError();
    ComPtr<IDXGIAdapter3> adapter;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    const bool queried = SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&adapter))) &&
        SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info));
    SetLastError(previous);
    if (!queried || info.Budget <= reported) return reported;
    TraceBudget(reported, info.Budget);
    return static_cast<SIZE_T>(info.Budget);
}

template<class Description, unsigned Slot>
HRESULT STDMETHODCALLTYPE BudgetedDescription(IDXGIAdapter* self, Description* out) {
    using Call = HRESULT (STDMETHODCALLTYPE*)(IDXGIAdapter*, Description*);
    const auto original = reinterpret_cast<Call>(OriginalBudgetSlot(self, Slot));
    if (!original) return E_UNEXPECTED;
    const HRESULT result = original(self, out);
    if (FAILED(result) || !out) return result;
    bool software;
    if constexpr (Slot == getDescSlot) {
        // DXGI_ADAPTER_DESC has no flags field.
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 flags{};
        const DWORD previous = GetLastError();
        software = FAILED(self->QueryInterface(IID_PPV_ARGS(&adapter))) || FAILED(adapter->GetDesc1(&flags)) ||
            (flags.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
        SetLastError(previous);
    } else {
        software = (static_cast<UINT>(out->Flags) & static_cast<UINT>(DXGI_ADAPTER_FLAG_SOFTWARE)) != 0;
    }
    out->DedicatedVideoMemory = BudgetedMemory(self, out->DedicatedVideoMemory, software);
    return result;
}

void* BudgetReplacement(unsigned slot) {
    switch (slot) {
    case getDescSlot: return reinterpret_cast<void*>(&BudgetedDescription<DXGI_ADAPTER_DESC, getDescSlot>);
    case getDesc1Slot: return reinterpret_cast<void*>(&BudgetedDescription<DXGI_ADAPTER_DESC1, getDesc1Slot>);
    case getDesc2Slot: return reinterpret_cast<void*>(&BudgetedDescription<DXGI_ADAPTER_DESC2, getDesc2Slot>);
    case getDesc3Slot: return reinterpret_cast<void*>(&BudgetedDescription<DXGI_ADAPTER_DESC3, getDesc3Slot>);
    default: return nullptr;
    }
}

// Caller holds budgetLock exclusively.
HRESULT PatchBudgetTable(void** vtable, const unsigned* slots, unsigned slotCount) {
    for (LONG index = 0; index < budgetTableCount; ++index)
        if (budgetTables[index].vtable == vtable) return S_FALSE;
    if (budgetTableCount == ARRAYSIZE(budgetTables)) return E_OUTOFMEMORY;
    BudgetTable& table = budgetTables[budgetTableCount];
    table = {vtable, {}};
    for (unsigned index = 0; index < slotCount; ++index) table.original[slots[index]] = vtable[slots[index]];
    // Publish the originals before any slot can route a call through the replacement.
    InterlockedIncrement(&budgetTableCount);
    for (unsigned index = 0; index < slotCount; ++index) {
        void** const slot = &vtable[slots[index]];
        DWORD protection = 0;
        if (!VirtualProtectFromApp(slot, sizeof(*slot), PAGE_READWRITE, reinterpret_cast<PULONG>(&protection)))
            return HRESULT_FROM_WIN32(GetLastError());
        InterlockedExchangePointer(slot, BudgetReplacement(slots[index]));
        DWORD ignored = 0;
        VirtualProtectFromApp(slot, sizeof(*slot), protection, reinterpret_cast<PULONG>(&ignored));
    }
    return S_OK;
}

template<class Interface>
HRESULT PatchBudgetInterface(IUnknown* adapter, const unsigned* slots, unsigned slotCount) {
    ComPtr<Interface> view;
    if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&view)))) return S_FALSE;
    return PatchBudgetTable(*reinterpret_cast<void***>(view.Get()), slots, slotCount);
}

// Installs the override for the hardware adapter class behind this adapter.
void InstallAdapterBudget(IDXGIAdapter1* adapter) {
    if (!adapter || !AdapterBudgetEnabled()) return;
    const DWORD previous = GetLastError();
    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        SetLastError(previous);
        return;
    }
    static const unsigned adapter0[] = {getDescSlot};
    static const unsigned adapter1[] = {getDescSlot, getDesc1Slot};
    static const unsigned adapter2[] = {getDescSlot, getDesc1Slot, getDesc2Slot};
    static const unsigned adapter4[] = {getDescSlot, getDesc1Slot, getDesc2Slot, getDesc3Slot};
    AcquireSRWLockExclusive(&budgetLock);
    // Patch the widest interface first so a shared vtable records every slot it exposes.
    HRESULT result = PatchBudgetInterface<IDXGIAdapter4>(adapter, adapter4, ARRAYSIZE(adapter4));
    if (SUCCEEDED(result)) result = PatchBudgetInterface<IDXGIAdapter2>(adapter, adapter2, ARRAYSIZE(adapter2));
    if (SUCCEEDED(result)) result = PatchBudgetInterface<IDXGIAdapter1>(adapter, adapter1, ARRAYSIZE(adapter1));
    if (SUCCEEDED(result)) result = PatchBudgetInterface<IDXGIAdapter>(adapter, adapter0, ARRAYSIZE(adapter0));
    ReleaseSRWLockExclusive(&budgetLock);
    static volatile LONG events = 0;
    if (InterlockedIncrement(&events) <= 4) Trace("adapter-budget-install", result);
    SetLastError(previous);
}
