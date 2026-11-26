/* Direct3D 12 residency counters and eviction switch, linked into the
   loader-trace adapter.

   Mesa's d3d12 driver manages residency itself (d3d12_residency.cpp at
   mesa-uwp 34732bb): above 70% of the DXGI budget it evicts least recently
   used allocations with ID3D12Device::Evict, it evicts on
   ID3D12Device15 trim notifications when the runtime has them, and it makes
   evicted allocations resident again with ID3D12Device3::EnqueueMakeResident
   before the batch that uses them. Its textures and buffers are also created
   with D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT when the device implements
   ID3D12Device8, so their first EnqueueMakeResident is what makes them
   resident at all.

   Every ID3D12Device in a process shares one method table, including Mesa's.
   Once d3d12.dll is loaded, a background thread takes that table from a
   device of its own and wraps MakeResident (slot 34), Evict (35),
   EnqueueMakeResident (50) and, where QueryInterface finds ID3D12Device15,
   RegisterTrimNotificationCallback (83). Slots are counted from d3d12.idl in
   microsoft/DirectX-Headers (d3d12-residency-policy.h).

   Modes (xbox-d3d12-residency.txt in the package root):
   - native (the default): every call goes to the driver unchanged. The hooks
     count calls, objects and bytes; nothing else changes.
   - pin: Evict returns S_OK without calling the driver, so no allocation is
     ever evicted. MakeResident and EnqueueMakeResident still go to the
     driver: a CREATE_NOT_RESIDENT allocation needs that call to become
     resident, and the driver signals EnqueueMakeResident's fence with the
     requested value itself, so Mesa's GPU wait on that fence completes as
     before. On an allocation that a pinned Evict left resident, the driver
     call only raises its residency count; nothing is paged. Nothing in pin
     mode changes a COM reference count, and a malformed Evict (objects NULL
     with a nonzero count) still goes to the driver for its error.
   - off: no hooks.
   The default installs the counters only when the pipeline hooks already
   create a device of their own (a cache or timing profile), so a default
   launch makes no Direct3D call it did not make before.

   Contract: each hook returns exactly what the driver returned (pin's Evict:
   S_OK), keeps the driver's last-error value, and passes the caller's
   arguments through untouched. Byte counts come from QueryInterface for
   ID3D12Resource or ID3D12Heap, GetDesc and GetResourceAllocationInfo after
   the driver call; each QueryInterface is released before the hook returns.
   A trim callback is registered through a forwarding callback that counts
   the notification and calls the title's callback with its own context; the
   cookie the runtime returns is written back to the caller's structure.

   Writes to the method table take XboxD3D12MethodTableLock, shared with the
   pipeline hooks, because both change the page's protection.

   Trace:
   - "asset residency mode=... source=... hooks=pending|none|off" at start;
   - "asset residency install result=... slots=34-35-50[-83] device3=.
     device15=. mode=...";
   - each second with any call, "asset residency evict=calls makeresident=calls
     enqueue=calls evict_obj= evict_kib= res_obj= res_kib= restored= [trim=]
     [failed=] utc=", where restored counts objects made resident again after
     an Evict of the same address (in pin mode: round trips that pin kept
     resident);
   - every 60 s, "asset residency-total ..." with the process totals;
   - the first trim notifications and registrations. */
#define WIN32_LEAN_AND_MEAN
#include "loader-trace-output.h"
#include "d3d12-pipeline-cache.h"
#include "d3d12-residency.h"
#include "d3d12-residency-policy.h"
#include <appmodel.h>
#include <d3d12.h>

#define METHOD(object, slot, type) ((type)((*(void ***)(object))[slot]))
typedef HRESULT (STDMETHODCALLTYPE *QueryMethod)(void *, REFIID, void **);
typedef ULONG (STDMETHODCALLTYPE *ReleaseMethod)(void *);
typedef HRESULT (STDMETHODCALLTYPE *ResidencyMethod)(void *, UINT, void *const *);
typedef HRESULT (STDMETHODCALLTYPE *EnqueueMethod)(void *, UINT, UINT, void *const *, void *, UINT64);
typedef D3D12_RESOURCE_DESC *(STDMETHODCALLTYPE *ResourceDescMethod)(void *, D3D12_RESOURCE_DESC *);
typedef D3D12_HEAP_DESC *(STDMETHODCALLTYPE *HeapDescMethod)(void *, D3D12_HEAP_DESC *);
typedef D3D12_RESOURCE_ALLOCATION_INFO *(STDMETHODCALLTYPE *AllocationInfoMethod)(void *,
    D3D12_RESOURCE_ALLOCATION_INFO *, UINT, UINT, const D3D12_RESOURCE_DESC *);

/* ID3D12Device15 trim notification types (Agility SDK headers only). */
typedef struct TrimNotification {
    void *context;
    UINT flags;
    UINT64 bytesToTrim;
} TrimNotification;
typedef void (STDMETHODCALLTYPE *TrimCallback)(const TrimNotification *);
typedef struct RegisterTrim {
    TrimCallback callback;
    void *context;
    DWORD cookie;
} RegisterTrim;
typedef HRESULT (STDMETHODCALLTYPE *RegisterTrimMethod)(void *, RegisterTrim *);

static const GUID deviceIid = {0x189819F1, 0x1DB6, 0x4B57, {0xBE, 0x54, 0x18, 0x21, 0x33, 0x9B, 0x85, 0xF7}};
static const GUID device3Iid = {0x81DADC15, 0x2BAD, 0x4392, {0x93, 0xC5, 0x10, 0x13, 0x45, 0xC4, 0xAA, 0x98}};
static const GUID device15Iid = {0x76CFF76F, 0x1E9B, 0x4450, {0x8C, 0xDC, 0x34, 0xF1, 0xAF, 0x78, 0x8E, 0x5B}};
static const GUID resourceIid = {0x696442BE, 0xA72E, 0x4059, {0xBC, 0x79, 0x5B, 0x5C, 0x98, 0x04, 0x0F, 0xAD}};
static const GUID heapIid = {0x6B3B2502, 0x6E51, 0x45B3, {0x90, 0xEE, 0x98, 0x84, 0x26, 0x5E, 0x8D, 0xF3}};

static HRESULT Query(void *object, const GUID *iid, void **out) {
    *out = NULL;
    return METHOD(object, 0, QueryMethod)(object, iid, out);
}

static void Release(void *object) {
    if (object) METHOD(object, 2, ReleaseMethod)(object);
}

/* ------------------------------------------------------------- trace */

static char *Hex32(char *out, DWORD value) {
    const char digits[] = "0123456789ABCDEF";
    *out++ = '0';
    *out++ = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) *out++ = digits[(value >> shift) & 15];
    return out;
}

static char *Two(char *out, WORD value) {
    *out++ = (char)('0' + value / 10 % 10);
    *out++ = (char)('0' + value % 10);
    return out;
}

/* YYYYMMDDTHHMMSSZ, 16 characters. */
static char *Utc(char *out) {
    SYSTEMTIME now;
    GetSystemTime(&now);
    out = Two(Two(out, (WORD)(now.wYear / 100)), (WORD)(now.wYear % 100));
    out = Two(Two(out, now.wMonth), now.wDay);
    *out++ = 'T';
    out = Two(Two(Two(out, now.wHour), now.wMinute), now.wSecond);
    *out++ = 'Z';
    return out;
}

static void Line(char *start, char *end) {
    *end++ = '\r';
    *end++ = '\n';
    AppendTrace(start, (DWORD)(end - start));
}

#define LINE_BUFFER (XBOX_RESIDENCY_LINE_MAX + 4)

/* ------------------------------------------------------------- state */

static volatile LONG residencyMode = XBOX_RESIDENCY_NATIVE;

enum {
    COUNT_EVICT_CALLS, COUNT_MAKE_RESIDENT_CALLS, COUNT_ENQUEUE_CALLS, COUNT_EVICT_OBJECTS, COUNT_EVICT_BYTES,
    COUNT_RESIDENT_OBJECTS, COUNT_RESIDENT_BYTES, COUNT_RESTORED, COUNT_TRIMS, COUNT_TRIM_BYTES, COUNT_FAILURES,
    COUNTERS
};
static volatile LONG64 window[COUNTERS];

static void Add(int counter, UINT64 value) {
    if (value) InterlockedAdd64(&window[counter], (LONG64)value);
}

/* Original methods by table. Every view of the device normally shares one
   table; a runtime that gave a view its own table gets its own originals. */
enum { HOOK_MAKE_RESIDENT, HOOK_EVICT, HOOK_ENQUEUE, HOOK_REGISTER_TRIM, HOOKS };
typedef struct PatchedTable {
    void **methods;
    void *volatile originals[HOOKS];
} PatchedTable;
static PatchedTable tables[4];
static volatile LONG tableCount;

static void *Original(void *self, int hook) {
    void **const methods = *(void ***)self;
    const LONG count = InterlockedCompareExchange(&tableCount, 0, 0);
    for (LONG i = 0; i < count; ++i)
        if (tables[i].methods == methods && tables[i].originals[hook]) return tables[i].originals[hook];
    for (LONG i = 0; i < count; ++i)
        if (tables[i].originals[hook]) return tables[i].originals[hook];
    return NULL;
}

/* Addresses of recently evicted objects (XboxResidencyEvictedSlot). */
static void *volatile evicted[XBOX_RESIDENCY_EVICTED_SLOTS];

static void NoteEvicted(void *object) {
    InterlockedExchangePointer(&evicted[XboxResidencyEvictedSlot((UINT64)(ULONG_PTR)object)], object);
}

static BOOL TakeEvicted(void *object) {
    void *volatile *slot = &evicted[XboxResidencyEvictedSlot((UINT64)(ULONG_PTR)object)];
    return InterlockedCompareExchangePointer(slot, NULL, object) == object;
}

/* The allocation size behind a pageable, or 0 for one that is neither a
   resource nor a heap (descriptor heaps, query heaps, pipeline states). */
static UINT64 PageableBytes(void *device, void *object) {
    if (!object) return 0;
    void *view = NULL;
    UINT64 bytes = 0;
    if (SUCCEEDED(Query(object, &resourceIid, &view)) && view) {
        D3D12_RESOURCE_DESC desc;
        D3D12_RESOURCE_ALLOCATION_INFO info;
        METHOD(view, 10, ResourceDescMethod)(view, &desc);
        METHOD(device, XBOX_RESIDENCY_SLOT_ALLOCATION_INFO, AllocationInfoMethod)(device, &info, 0, 1, &desc);
        bytes = info.SizeInBytes == ~(UINT64)0 ? 0 : info.SizeInBytes;
    } else if (SUCCEEDED(Query(object, &heapIid, &view)) && view) {
        D3D12_HEAP_DESC desc;
        METHOD(view, 8, HeapDescMethod)(view, &desc);
        bytes = desc.SizeInBytes;
    }
    Release(view);
    return bytes;
}

/* Objects in a call Mesa never batches past 128; a larger call from another
   title is counted without sizes. */
#define SIZED_OBJECTS_MAX 1024u

static void CountResident(void *device, UINT count, void *const *objects) {
    UINT64 bytes = 0, restored = 0;
    for (UINT i = 0; objects && i < count; ++i) {
        if (TakeEvicted(objects[i])) ++restored;
        if (count <= SIZED_OBJECTS_MAX) bytes += PageableBytes(device, objects[i]);
    }
    Add(COUNT_RESIDENT_OBJECTS, count);
    Add(COUNT_RESIDENT_BYTES, bytes);
    Add(COUNT_RESTORED, restored);
}

/* ------------------------------------------------------------- hooks */

static HRESULT STDMETHODCALLTYPE HookMakeResident(void *device, UINT count, void *const *objects) {
    const ResidencyMethod original = (ResidencyMethod)Original(device, HOOK_MAKE_RESIDENT);
    if (!original) return E_UNEXPECTED;
    const HRESULT result = original(device, count, objects);
    const DWORD error = GetLastError();
    Add(COUNT_MAKE_RESIDENT_CALLS, 1);
    if (SUCCEEDED(result)) CountResident(device, count, objects);
    else Add(COUNT_FAILURES, 1);
    SetLastError(error);
    return result;
}

static HRESULT STDMETHODCALLTYPE HookEvict(void *device, UINT count, void *const *objects) {
    const BOOL pin = InterlockedCompareExchange(&residencyMode, 0, 0) == (LONG)XBOX_RESIDENCY_PIN &&
        (objects || !count);
    HRESULT result = S_OK;
    if (!pin) {
        const ResidencyMethod original = (ResidencyMethod)Original(device, HOOK_EVICT);
        if (!original) return E_UNEXPECTED;
        result = original(device, count, objects);
    }
    const DWORD error = GetLastError();
    Add(COUNT_EVICT_CALLS, 1);
    if (SUCCEEDED(result)) {
        UINT64 bytes = 0;
        for (UINT i = 0; objects && i < count; ++i) {
            NoteEvicted(objects[i]);
            if (count <= SIZED_OBJECTS_MAX) bytes += PageableBytes(device, objects[i]);
        }
        Add(COUNT_EVICT_OBJECTS, count);
        Add(COUNT_EVICT_BYTES, bytes);
    } else {
        Add(COUNT_FAILURES, 1);
    }
    SetLastError(error);
    return result;
}

static HRESULT STDMETHODCALLTYPE HookEnqueueMakeResident(void *device, UINT flags, UINT count,
        void *const *objects, void *fence, UINT64 value) {
    const EnqueueMethod original = (EnqueueMethod)Original(device, HOOK_ENQUEUE);
    if (!original) return E_UNEXPECTED;
    const HRESULT result = original(device, flags, count, objects, fence, value);
    const DWORD error = GetLastError();
    Add(COUNT_ENQUEUE_CALLS, 1);
    if (SUCCEEDED(result)) CountResident(device, count, objects);
    else Add(COUNT_FAILURES, 1);
    SetLastError(error);
    return result;
}

/* Trim notifications, forwarded to the registered callback with its own
   context. Registrations past the table are passed through unwrapped. */
typedef struct TrimTarget {
    TrimCallback callback;
    void *context;
} TrimTarget;
static TrimTarget trimTargets[8];
static volatile LONG trimTargetCount;
static volatile LONG trimLines;

static void STDMETHODCALLTYPE ForwardTrim(const TrimNotification *notification) {
    const TrimTarget *target = (const TrimTarget *)notification->context;
    const DWORD error = GetLastError();
    Add(COUNT_TRIMS, 1);
    Add(COUNT_TRIM_BYTES, notification->bytesToTrim);
    if (InterlockedIncrement(&trimLines) <= 16) {
        char line[LINE_BUFFER];
        char *out = Hex32(XboxResidencyText(line, line + XBOX_RESIDENCY_LINE_MAX, "asset residency trim flags="),
            notification->flags);
        out = XboxResidencyField(out, line + XBOX_RESIDENCY_LINE_MAX, " bytes_kib=",
            XboxResidencyKib(notification->bytesToTrim));
        Line(line, Utc(XboxResidencyText(out, line + XBOX_RESIDENCY_LINE_MAX, " utc=")));
    }
    SetLastError(error);
    TrimNotification forwarded;
    forwarded.context = target->context;
    forwarded.flags = notification->flags;
    forwarded.bytesToTrim = notification->bytesToTrim;
    target->callback(&forwarded);
}

static HRESULT STDMETHODCALLTYPE HookRegisterTrim(void *device, RegisterTrim *data) {
    const RegisterTrimMethod original = (RegisterTrimMethod)Original(device, HOOK_REGISTER_TRIM);
    if (!original) return E_UNEXPECTED;
    LONG index = -1;
    if (data && data->callback) {
        index = InterlockedIncrement(&trimTargetCount) - 1;
        if (index >= (LONG)ARRAYSIZE(trimTargets)) index = -1;
    }
    HRESULT result;
    DWORD error;
    if (index < 0) {
        result = original(device, data);
        error = GetLastError();
    } else {
        trimTargets[index].callback = data->callback;
        trimTargets[index].context = data->context;
        RegisterTrim wrapped;
        wrapped.callback = ForwardTrim;
        wrapped.context = &trimTargets[index];
        wrapped.cookie = data->cookie;
        result = original(device, &wrapped);
        error = GetLastError();
        data->cookie = wrapped.cookie;
    }
    char line[LINE_BUFFER];
    char *out = Hex32(XboxResidencyText(line, line + XBOX_RESIDENCY_LINE_MAX, "asset residency trim-register result="),
        (DWORD)result);
    out = XboxResidencyText(out, line + XBOX_RESIDENCY_LINE_MAX, index < 0 ? " forwarded=0" : " forwarded=1");
    Line(line, out);
    SetLastError(error);
    return result;
}

/* ------------------------------------------------------------- install */

/* With XboxD3D12MethodTableLock held exclusively. */
static PatchedTable *TableFor(void **methods) {
    const LONG count = tableCount;
    for (LONG i = 0; i < count; ++i)
        if (tables[i].methods == methods) return &tables[i];
    if (count == (LONG)ARRAYSIZE(tables)) return NULL;
    tables[count].methods = methods;
    InterlockedIncrement(&tableCount);
    return &tables[count];
}

/* With XboxD3D12MethodTableLock held exclusively. The original is published
   before the slot is swapped, so the hook always finds it. */
static HRESULT PatchSlot(PatchedTable *table, int hook, unsigned slot, void *replacement) {
    void **const entry = &table->methods[slot];
    if (*entry == replacement) return S_FALSE;
    InterlockedExchangePointer(&table->originals[hook], *entry);
    ULONG previous = 0;
    if (!VirtualProtectFromApp(entry, sizeof(*entry), PAGE_READWRITE, &previous))
        return HRESULT_FROM_WIN32(GetLastError());
    InterlockedExchangePointer(entry, replacement);
    VirtualProtectFromApp(entry, sizeof(*entry), previous, &previous);
    return S_OK;
}

/* level: 0 for ID3D12Device, 3 for ID3D12Device3, 15 for ID3D12Device15. */
static HRESULT PatchView(void *view, int level) {
    PatchedTable *table = TableFor(*(void ***)view);
    if (!table) return E_OUTOFMEMORY;
    HRESULT result = PatchSlot(table, HOOK_MAKE_RESIDENT, XBOX_RESIDENCY_SLOT_MAKE_RESIDENT, (void *)HookMakeResident);
    if (SUCCEEDED(result)) result = PatchSlot(table, HOOK_EVICT, XBOX_RESIDENCY_SLOT_EVICT, (void *)HookEvict);
    if (SUCCEEDED(result) && level >= 3)
        result = PatchSlot(table, HOOK_ENQUEUE, XBOX_RESIDENCY_SLOT_ENQUEUE_MAKE_RESIDENT,
            (void *)HookEnqueueMakeResident);
    if (SUCCEEDED(result) && level >= 15)
        result = PatchSlot(table, HOOK_REGISTER_TRIM, XBOX_RESIDENCY_SLOT_REGISTER_TRIM, (void *)HookRegisterTrim);
    return FAILED(result) ? result : S_OK;
}

static volatile LONG device3Found, device15Found;

HRESULT XboxD3D12ResidencyInstall(void *device) {
    if (!device) return E_POINTER;
    void *device3 = NULL, *device15 = NULL;
    if (FAILED(Query(device, &device3Iid, &device3))) device3 = NULL;
    if (FAILED(Query(device, &device15Iid, &device15))) device15 = NULL;
    if (device3) InterlockedExchange(&device3Found, 1);
    if (device15) InterlockedExchange(&device15Found, 1);
    AcquireSRWLockExclusive(&XboxD3D12MethodTableLock);
    HRESULT result = PatchView(device, 0);
    if (SUCCEEDED(result) && device3) result = PatchView(device3, 3);
    if (SUCCEEDED(result) && device15) result = PatchView(device15, 15);
    ReleaseSRWLockExclusive(&XboxD3D12MethodTableLock);
    Release(device3);
    Release(device15);
    return result;
}

/* ------------------------------------------------------------- reporter */

static void TakeWindow(XboxResidencyWindow *taken, XboxResidencyWindow *total) {
    LONG64 values[COUNTERS];
    for (int i = 0; i < COUNTERS; ++i) values[i] = InterlockedExchange64(&window[i], 0);
    taken->evictCalls = (uint64_t)values[COUNT_EVICT_CALLS];
    taken->makeResidentCalls = (uint64_t)values[COUNT_MAKE_RESIDENT_CALLS];
    taken->enqueueCalls = (uint64_t)values[COUNT_ENQUEUE_CALLS];
    taken->evictObjects = (uint64_t)values[COUNT_EVICT_OBJECTS];
    taken->evictBytes = (uint64_t)values[COUNT_EVICT_BYTES];
    taken->residentObjects = (uint64_t)values[COUNT_RESIDENT_OBJECTS];
    taken->residentBytes = (uint64_t)values[COUNT_RESIDENT_BYTES];
    taken->restored = (uint64_t)values[COUNT_RESTORED];
    taken->trims = (uint64_t)values[COUNT_TRIMS];
    taken->trimBytes = (uint64_t)values[COUNT_TRIM_BYTES];
    taken->failures = (uint64_t)values[COUNT_FAILURES];
    total->evictCalls += taken->evictCalls;
    total->makeResidentCalls += taken->makeResidentCalls;
    total->enqueueCalls += taken->enqueueCalls;
    total->evictObjects += taken->evictObjects;
    total->evictBytes += taken->evictBytes;
    total->residentObjects += taken->residentObjects;
    total->residentBytes += taken->residentBytes;
    total->restored += taken->restored;
    total->trims += taken->trims;
    total->trimBytes += taken->trimBytes;
    total->failures += taken->failures;
}

static XboxResidencyWindow totals;

static void Report(void) {
    const ULONGLONG start = GetTickCount64();
    ULONGLONG nextTotal = start + 60000;
    for (;;) {
        Sleep(1000);
        XboxResidencyWindow taken;
        TakeWindow(&taken, &totals);
        char line[LINE_BUFFER];
        if (XboxResidencyWindowActive(&taken)) Line(line, Utc(XboxResidencyFormatWindow(line, &taken)));
        const ULONGLONG now = GetTickCount64();
        if (now >= nextTotal) {
            nextTotal = now + 60000;
            Line(line, Utc(XboxResidencyFormatTotal(line, &totals, (uint32_t)residencyMode, (now - start) / 1000)));
        }
    }
}

static DWORD WINAPI ResidencyThread(LPVOID unused) {
    (void)unused;
    typedef HRESULT (WINAPI *CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    /* Wait for the title's own runtime load, up to 120 s. */
    HMODULE d3d12 = NULL;
    for (DWORD waited = 0; waited < 120000 && !d3d12; waited += 20) {
        d3d12 = GetModuleHandleW(L"d3d12.dll");
        if (!d3d12) Sleep(20);
    }
    const CreateDevice create = d3d12 ? (CreateDevice)(void *)GetProcAddress(d3d12, "D3D12CreateDevice") : NULL;
    void *device = NULL;
    HRESULT result = create ? create(NULL, D3D_FEATURE_LEVEL_11_0, &deviceIid, &device) :
        HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    if (SUCCEEDED(result) && !device) result = E_POINTER;
    if (SUCCEEDED(result)) result = XboxD3D12ResidencyInstall(device);
    Release(device);
    char line[LINE_BUFFER];
    const char *end = line + XBOX_RESIDENCY_LINE_MAX;
    char *out = Hex32(XboxResidencyText(line, end, "asset residency install result="), (DWORD)result);
    out = XboxResidencyText(out, end, device3Found ? " slots=34-35-50" : " slots=34-35");
    if (device15Found) out = XboxResidencyText(out, end, "-83");
    out = XboxResidencyField(out, end, " device3=", (uint64_t)device3Found);
    out = XboxResidencyField(out, end, " device15=", (uint64_t)device15Found);
    out = XboxResidencyText(XboxResidencyText(out, end, " mode="), end, XboxResidencyModeName((uint32_t)residencyMode));
    Line(line, out);
    if (SUCCEEDED(result)) Report();
    return 0;
}

/* The package-root file, or XBOX_RESIDENCY_DEFAULT without one. */
static uint32_t ReadModeFile(int *found) {
    *found = 0;
    WCHAR path[1024];
    static const WCHAR suffix[] = L"\\xbox-d3d12-residency.txt";
    UINT32 length = ARRAYSIZE(path);
    if (GetCurrentPackagePath(&length, path) != ERROR_SUCCESS || !length ||
            length - 1 + ARRAYSIZE(suffix) > ARRAYSIZE(path))
        return XBOX_RESIDENCY_DEFAULT;
    for (UINT32 i = 0; i < ARRAYSIZE(suffix); ++i) path[length - 1 + i] = suffix[i];
    const HANDLE file = CreateFile2FromAppW(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL);
    if (file == INVALID_HANDLE_VALUE) return XBOX_RESIDENCY_DEFAULT;
    *found = 1;
    char text[32];
    DWORD read = 0;
    const BOOL ok = ReadFile(file, text, sizeof(text), &read, NULL);
    CloseHandle(file);
    if (!ok || read == sizeof(text)) return XBOX_RESIDENCY_INVALID;
    return XboxResidencyParseMode(text, read);
}

void XboxD3D12ResidencyStart(BOOL pipelineHooks) {
    static volatile LONG started;
    if (InterlockedExchange(&started, 1)) return;
    const DWORD error = GetLastError();
    int found = 0;
    const uint32_t parsed = ReadModeFile(&found);
    const uint32_t mode = XboxResidencyEffectiveMode(parsed);
    InterlockedExchange(&residencyMode, (LONG)mode);
    BOOL start = XboxResidencyShouldStart(parsed, pipelineHooks);
    HANDLE thread = start ? CreateThread(NULL, 0, ResidencyThread, NULL, 0, NULL) : NULL;
    char line[LINE_BUFFER];
    const char *end = line + XBOX_RESIDENCY_LINE_MAX;
    char *out = XboxResidencyText(XboxResidencyText(line, end, "asset residency mode="), end,
        XboxResidencyModeName(mode));
    out = XboxResidencyText(XboxResidencyText(out, end, " source="), end, XboxResidencySourceName(parsed, found));
    out = XboxResidencyText(out, end, mode == XBOX_RESIDENCY_OFF ? " hooks=off" : thread ? " hooks=pending" :
        start ? " hooks=thread-failed" : " hooks=none");
    Line(line, out);
    if (thread) CloseHandle(thread);
    SetLastError(error);
}
