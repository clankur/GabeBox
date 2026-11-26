/* Direct3D 12 fence-wait trace, linked into the loader-trace adapter.

   Mesa's d3d12 driver (mesa-uwp 34732bb) recycles a batch once its fence
   has passed: d3d12_reset_batch waits with d3d12_fence_finish, which for an
   infinite timeout calls ID3D12Fence::SetEventOnCompletion(value, NULL) and
   trusts its HRESULT (d3d12_fence.cpp L162-L163), then clears the batch's
   shader-visible view and sampler heaps (d3d12_batch.cpp L212-L213). A NULL
   event means "block until the fence reaches value". If that call returned
   success early on Xbox, Mesa would rewrite descriptors the GPU is still
   reading. This trace checks, after each such call, whether the fence has
   reached the value, without changing what the caller sees.

   Every ID3D12Fence and every ID3D12CommandQueue in a process normally
   shares its class's method table, Mesa's included, as the devices do
   (d3d12-residency.c). Once d3d12.dll is loaded, a background thread creates
   a device, a plain and a shared fence (Mesa's is shared) and a direct queue
   of its own, and patches through them:
   - ID3D12Fence::SetEventOnCompletion (slot 9);
   - ID3D12CommandQueue::ExecuteCommandLists (slot 10);
   - ID3D12Device::CreateCommandQueue (8) and CreateFence (36), which patch
     the table of each queue or fence created later if it is a new one.
   Mesa's own fence and queue may already exist by then; the shared tables
   cover them. A run whose trace shows exec > 0 and null_waits = 0 for long
   has a fence the hooks do not reach.

   Modes (xbox-d3d12-fence-trace.txt in the package root, see
   d3d12-fence-policy.h). Without the file nothing here runs.
   - trace: SetEventOnCompletion(value, NULL) is timed and, after it returned
     success, GetCompletedValue() is compared with value. A call with an
     event, or one that failed, is passed through and counted.
   - wait: as trace, and after an early return the hook polls
     GetCompletedValue() (spin, yield, then 1 ms sleeps) until it reaches
     value or 2 s pass, so the caller cannot run ahead of the GPU.
   - off: no hooks.

   Contract: each hook calls the driver with the caller's arguments
   unchanged, returns exactly the driver's HRESULT and keeps the driver's
   last-error value. CreateFence and CreateCommandQueue take and release one
   QueryInterface reference on the new object, so its count is unchanged.

   Writes to a method table take XboxD3D12MethodTableLock, shared with the
   pipeline and residency hooks, because the tables may share a page.

   Trace (each per-event line carries a millisecond UTC stamp):
   - "asset d3d12-fence mode=trace|wait|off source=package-file hooks=..."
     at start;
   - "asset d3d12-fence install result=... tables=... mode=... utc=";
   - "asset d3d12-fence selftest result=... wait_us=... utc=": a GPU signal
     of 1 on the adapter's own queue and a NULL-event wait for 1 through the
     original method (not counted); 0x00000001 (S_FALSE) means the wait
     returned before the fence reached 1;
   - each second with any call, "asset d3d12-fence null_waits= early=
     max_wait_us= exec= [event_waits=] [settled= max_settle_us=]
     [timeouts=] [failed=] utc=";
   - "asset d3d12-fence first-early value= completed= wait_us= ..." once;
   - "asset d3d12-fence timeout ..." for the first wait-mode timeouts;
   - "asset d3d12-fence create kind=fence|queue ..." for the first creations
     after the install;
   - every 60 s, "asset d3d12-fence-total ...". */
#define WIN32_LEAN_AND_MEAN
#include "loader-trace-output.h"
#include "d3d12-pipeline-cache.h"
#include "d3d12-fence-trace.h"
#include "d3d12-fence-policy.h"
#include <appmodel.h>
#include <d3d12.h>

#define METHOD(object, slot, type) ((type)((*(void ***)(object))[slot]))
typedef HRESULT (STDMETHODCALLTYPE *QueryMethod)(void *, REFIID, void **);
typedef ULONG (STDMETHODCALLTYPE *ReleaseMethod)(void *);
typedef UINT64 (STDMETHODCALLTYPE *CompletedMethod)(void *);
typedef HRESULT (STDMETHODCALLTYPE *SetEventMethod)(void *, UINT64, HANDLE);
typedef void (STDMETHODCALLTYPE *ExecuteMethod)(void *, UINT, void *const *);
typedef HRESULT (STDMETHODCALLTYPE *QueueSignalMethod)(void *, void *, UINT64);
typedef HRESULT (STDMETHODCALLTYPE *CreateFenceMethod)(void *, UINT64, D3D12_FENCE_FLAGS, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *CreateQueueMethod)(void *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **);

static const GUID deviceIid = {0x189819F1, 0x1DB6, 0x4B57, {0xBE, 0x54, 0x18, 0x21, 0x33, 0x9B, 0x85, 0xF7}};
static const GUID fenceIid = {0x0A753DCF, 0xC4D8, 0x4B91, {0xAD, 0xF6, 0xBE, 0x5A, 0x60, 0xD9, 0x5A, 0x76}};
static const GUID queueIid = {0x0EC870A6, 0x5D7E, 0x4C22, {0x8C, 0xFC, 0x5B, 0xAA, 0xE0, 0x76, 0x16, 0xED}};

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

static char *Utc(char *out) {
    SYSTEMTIME now;
    GetSystemTime(&now);
    return XboxFenceUtc(out, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        now.wMilliseconds);
}

static void Line(char *start, char *end) {
    *end++ = '\r';
    *end++ = '\n';
    AppendTrace(start, (DWORD)(end - start));
}

#define LINE_BUFFER (XBOX_FENCE_LINE_MAX + 4)
#define LINE_END(line) ((line) + XBOX_FENCE_LINE_MAX - XBOX_FENCE_UTC_CHARS)

/* ------------------------------------------------------------- state */

static volatile LONG fenceMode = XBOX_FENCE_TRACE;
static LONGLONG ticksPerSecond;

static ULONGLONG Microseconds(LONGLONG ticks) {
    return ticksPerSecond > 0 && ticks > 0 ? (ULONGLONG)ticks * 1000000u / (ULONGLONG)ticksPerSecond : 0;
}

static LONGLONG Ticks(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

enum {
    COUNT_NULL_WAITS, COUNT_EARLY, COUNT_MAX_WAIT_US, COUNT_EXEC, COUNT_EVENT_WAITS, COUNT_SETTLED,
    COUNT_MAX_SETTLE_US, COUNT_TIMEOUTS, COUNT_FAILED, COUNTERS
};
static volatile LONG64 window[COUNTERS];

static void Add(int counter, UINT64 value) {
    if (value) InterlockedAdd64(&window[counter], (LONG64)value);
}

static void Max(int counter, UINT64 value) {
    LONG64 seen = window[counter];
    while ((UINT64)seen < value) {
        const LONG64 previous = InterlockedCompareExchange64(&window[counter], (LONG64)value, seen);
        if (previous == seen) break;
        seen = previous;
    }
}

/* Original methods by table. One table per class normally serves every
   object; a class with a table of its own gets its own originals. */
enum { HOOK_CREATE_QUEUE, HOOK_CREATE_FENCE, HOOK_SET_EVENT, HOOK_EXECUTE, HOOKS };
typedef struct PatchedTable {
    void **methods;
    void *volatile originals[HOOKS];
} PatchedTable;
static PatchedTable tables[8];
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

static BOOL Patched(void **methods, int hook) {
    const LONG count = InterlockedCompareExchange(&tableCount, 0, 0);
    for (LONG i = 0; i < count; ++i)
        if (tables[i].methods == methods && tables[i].originals[hook]) return TRUE;
    return FALSE;
}

/* ------------------------------------------------------------- hooks */

typedef struct WaitContext {
    void *fence;
    CompletedMethod completed;
} WaitContext;

static uint64_t FenceCompleted(void *context) {
    const WaitContext *wait = (const WaitContext *)context;
    return wait->completed(wait->fence);
}

static uint64_t FenceNowUs(void *context) {
    (void)context;
    return Microseconds(Ticks());
}

static void FencePause(void *context, uint32_t round) {
    (void)context;
    if (round < 64) YieldProcessor();
    else if (round < 128) SwitchToThread();
    else Sleep(1);
}

static volatile LONG firstEarly, timeoutLines, createLines;

static HRESULT STDMETHODCALLTYPE HookSetEventOnCompletion(void *fence, UINT64 value, HANDLE event) {
    const SetEventMethod original = (SetEventMethod)Original(fence, HOOK_SET_EVENT);
    if (!original) return E_UNEXPECTED;
    if (event) {
        const HRESULT result = original(fence, value, event);
        const DWORD error = GetLastError();
        Add(COUNT_EVENT_WAITS, 1);
        if (FAILED(result)) Add(COUNT_FAILED, 1);
        SetLastError(error);
        return result;
    }
    const LONGLONG start = Ticks();
    const HRESULT result = original(fence, value, NULL);
    const LONGLONG returned = Ticks();
    const DWORD error = GetLastError();
    const ULONGLONG waitUs = Microseconds(returned - start);
    Add(COUNT_NULL_WAITS, 1);
    Max(COUNT_MAX_WAIT_US, waitUs);
    if (FAILED(result)) {
        Add(COUNT_FAILED, 1);
    } else {
        const uint32_t mode = (uint32_t)InterlockedCompareExchange(&fenceMode, 0, 0);
        WaitContext context = {fence, METHOD(fence, XBOX_FENCE_SLOT_GET_COMPLETED_VALUE, CompletedMethod)};
        const XboxFenceOps ops = {FenceCompleted, FenceNowUs, FencePause, &context};
        XboxFenceCheck check;
        XboxFenceCheckWait(&ops, value, mode, XBOX_FENCE_SETTLE_TIMEOUT_US, &check);
        if (check.early) {
            Add(COUNT_EARLY, 1);
            if (check.settled) {
                Add(COUNT_SETTLED, 1);
                Max(COUNT_MAX_SETTLE_US, check.settleUs);
            }
            if (check.timedOut) Add(COUNT_TIMEOUTS, 1);
            char line[LINE_BUFFER];
            if (!InterlockedExchange(&firstEarly, 1))
                Line(line, Utc(XboxFenceFormatEvent(line, "first-early", value, check.completedAfterWait, waitUs,
                    &check, mode)));
            if (check.timedOut && InterlockedIncrement(&timeoutLines) <= 16)
                Line(line, Utc(XboxFenceFormatEvent(line, "timeout", value, check.completedAfterWait, waitUs,
                    &check, mode)));
        }
    }
    SetLastError(error);
    return result;
}

static void STDMETHODCALLTYPE HookExecuteCommandLists(void *queue, UINT count, void *const *lists) {
    const ExecuteMethod original = (ExecuteMethod)Original(queue, HOOK_EXECUTE);
    Add(COUNT_EXEC, 1);
    if (original) original(queue, count, lists);
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

/* Patches one slot in the table behind object's iid view. S_OK for a table
   patched now, S_FALSE for one already patched. */
static HRESULT PatchObject(void *object, const GUID *iid, int hook, unsigned slot, void *replacement) {
    if (!object) return E_POINTER;
    void *view = NULL;
    HRESULT result = Query(object, iid, &view);
    if (SUCCEEDED(result) && !view) result = E_NOINTERFACE;
    if (FAILED(result)) return result;
    void **const methods = *(void ***)view;
    if (Patched(methods, hook)) {
        result = S_FALSE;
    } else {
        AcquireSRWLockExclusive(&XboxD3D12MethodTableLock);
        PatchedTable *table = TableFor(methods);
        result = table ? PatchSlot(table, hook, slot, replacement) : E_OUTOFMEMORY;
        ReleaseSRWLockExclusive(&XboxD3D12MethodTableLock);
    }
    Release(view);
    return result;
}

static void NoteCreate(const char *kind, DWORD detail, HRESULT result, HRESULT patched) {
    if (InterlockedIncrement(&createLines) > 16) return;
    char line[LINE_BUFFER];
    const char *end = LINE_END(line);
    char *out = XboxResidencyText(XboxResidencyText(line, end, "asset d3d12-fence create kind="), end, kind);
    out = Hex32(XboxResidencyText(out, end, " detail="), detail);
    out = Hex32(XboxResidencyText(out, end, " result="), (DWORD)result);
    out = XboxResidencyText(out, end, FAILED(result) ? "" : patched == S_OK ? " table=new" :
        patched == S_FALSE ? " table=known" : " table=failed");
    Line(line, Utc(XboxResidencyText(out, end, " utc=")));
}

static HRESULT STDMETHODCALLTYPE HookCreateFence(void *device, UINT64 initial, D3D12_FENCE_FLAGS flags,
        REFIID iid, void **out) {
    const CreateFenceMethod original = (CreateFenceMethod)Original(device, HOOK_CREATE_FENCE);
    if (!original) return E_UNEXPECTED;
    const HRESULT result = original(device, initial, flags, iid, out);
    const DWORD error = GetLastError();
    HRESULT patched = E_POINTER;
    if (SUCCEEDED(result) && out && *out)
        patched = PatchObject(*out, &fenceIid, HOOK_SET_EVENT, XBOX_FENCE_SLOT_SET_EVENT_ON_COMPLETION,
            (void *)HookSetEventOnCompletion);
    NoteCreate("fence", (DWORD)flags, result, patched);
    SetLastError(error);
    return result;
}

static HRESULT STDMETHODCALLTYPE HookCreateCommandQueue(void *device, const D3D12_COMMAND_QUEUE_DESC *desc,
        REFIID iid, void **out) {
    const CreateQueueMethod original = (CreateQueueMethod)Original(device, HOOK_CREATE_QUEUE);
    if (!original) return E_UNEXPECTED;
    const HRESULT result = original(device, desc, iid, out);
    const DWORD error = GetLastError();
    HRESULT patched = E_POINTER;
    if (SUCCEEDED(result) && out && *out)
        patched = PatchObject(*out, &queueIid, HOOK_EXECUTE, XBOX_FENCE_SLOT_QUEUE_EXECUTE,
            (void *)HookExecuteCommandLists);
    NoteCreate("queue", desc ? (DWORD)desc->Type : 0xFFFFFFFFu, result, patched);
    SetLastError(error);
    return result;
}

static HRESULT Combine(HRESULT first, HRESULT next) {
    return FAILED(first) ? first : FAILED(next) ? next : S_OK;
}

static void InitClock(void) {
    if (!ticksPerSecond) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        ticksPerSecond = frequency.QuadPart;
    }
}

static HRESULT CreateDirectQueue(CreateQueueMethod create, void *device, void **queue) {
    D3D12_COMMAND_QUEUE_DESC desc;
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = 0;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;
    const HRESULT result = create(device, &desc, &queueIid, queue);
    return SUCCEEDED(result) && !*queue ? E_POINTER : result;
}

HRESULT XboxD3D12FenceTraceInstall(void *device) {
    if (!device) return E_POINTER;
    InitClock();
    const CreateFenceMethod createFence = METHOD(device, XBOX_FENCE_SLOT_CREATE_FENCE, CreateFenceMethod);
    const CreateQueueMethod createQueue = METHOD(device, XBOX_FENCE_SLOT_CREATE_COMMAND_QUEUE, CreateQueueMethod);
    if ((void *)createFence == (void *)HookCreateFence || (void *)createQueue == (void *)HookCreateCommandQueue)
        return S_FALSE;
    /* The adapter's own objects, created through the unpatched methods. */
    void *plain = NULL, *shared = NULL, *queue = NULL;
    HRESULT result = createFence(device, 0, D3D12_FENCE_FLAG_NONE, &fenceIid, &plain);
    if (SUCCEEDED(result) && !plain) result = E_POINTER;
    if (SUCCEEDED(result))
        result = PatchObject(plain, &fenceIid, HOOK_SET_EVENT, XBOX_FENCE_SLOT_SET_EVENT_ON_COMPLETION,
            (void *)HookSetEventOnCompletion);
    /* Mesa creates its fence with D3D12_FENCE_FLAG_SHARED. */
    if (SUCCEEDED(createFence(device, 0, D3D12_FENCE_FLAG_SHARED, &fenceIid, &shared)) && shared)
        result = Combine(result, PatchObject(shared, &fenceIid, HOOK_SET_EVENT,
            XBOX_FENCE_SLOT_SET_EVENT_ON_COMPLETION, (void *)HookSetEventOnCompletion));
    HRESULT queued = CreateDirectQueue(createQueue, device, &queue);
    if (SUCCEEDED(queued))
        queued = PatchObject(queue, &queueIid, HOOK_EXECUTE, XBOX_FENCE_SLOT_QUEUE_EXECUTE,
            (void *)HookExecuteCommandLists);
    result = Combine(result, queued);
    Release(plain);
    Release(shared);
    Release(queue);
    /* Later creations: patch any new table. */
    AcquireSRWLockExclusive(&XboxD3D12MethodTableLock);
    PatchedTable *table = TableFor(*(void ***)device);
    HRESULT hooked = table ? PatchSlot(table, HOOK_CREATE_FENCE, XBOX_FENCE_SLOT_CREATE_FENCE,
        (void *)HookCreateFence) : E_OUTOFMEMORY;
    if (SUCCEEDED(hooked))
        hooked = PatchSlot(table, HOOK_CREATE_QUEUE, XBOX_FENCE_SLOT_CREATE_COMMAND_QUEUE,
            (void *)HookCreateCommandQueue);
    ReleaseSRWLockExclusive(&XboxD3D12MethodTableLock);
    return Combine(result, hooked);
}

HRESULT XboxD3D12FenceSelfTest(void *device, ULONGLONG *waitUs) {
    *waitUs = 0;
    if (!device) return E_POINTER;
    InitClock();
    const CreateFenceMethod createFence = (CreateFenceMethod)Original(device, HOOK_CREATE_FENCE);
    const CreateQueueMethod createQueue = (CreateQueueMethod)Original(device, HOOK_CREATE_QUEUE);
    if (!createFence || !createQueue) return E_UNEXPECTED;
    void *fence = NULL, *queue = NULL;
    HRESULT result = createFence(device, 0, D3D12_FENCE_FLAG_NONE, &fenceIid, &fence);
    if (SUCCEEDED(result) && !fence) result = E_POINTER;
    if (SUCCEEDED(result)) result = CreateDirectQueue(createQueue, device, &queue);
    if (SUCCEEDED(result))
        result = METHOD(queue, XBOX_FENCE_SLOT_QUEUE_SIGNAL, QueueSignalMethod)(queue, fence, 1);
    if (SUCCEEDED(result)) {
        const SetEventMethod wait = (SetEventMethod)Original(fence, HOOK_SET_EVENT);
        const LONGLONG start = Ticks();
        result = wait ? wait(fence, 1, NULL) : E_UNEXPECTED;
        *waitUs = Microseconds(Ticks() - start);
        if (SUCCEEDED(result) && METHOD(fence, XBOX_FENCE_SLOT_GET_COMPLETED_VALUE, CompletedMethod)(fence) < 1)
            result = S_FALSE;
    }
    Release(fence);
    Release(queue);
    return result;
}

/* ------------------------------------------------------------- reporter */

static void TakeWindow(XboxFenceWindow *taken, XboxFenceWindow *total) {
    LONG64 values[COUNTERS];
    for (int i = 0; i < COUNTERS; ++i) values[i] = InterlockedExchange64(&window[i], 0);
    taken->nullWaits = (uint64_t)values[COUNT_NULL_WAITS];
    taken->early = (uint64_t)values[COUNT_EARLY];
    taken->maxWaitUs = (uint64_t)values[COUNT_MAX_WAIT_US];
    taken->exec = (uint64_t)values[COUNT_EXEC];
    taken->eventWaits = (uint64_t)values[COUNT_EVENT_WAITS];
    taken->settled = (uint64_t)values[COUNT_SETTLED];
    taken->maxSettleUs = (uint64_t)values[COUNT_MAX_SETTLE_US];
    taken->timeouts = (uint64_t)values[COUNT_TIMEOUTS];
    taken->failed = (uint64_t)values[COUNT_FAILED];
    total->nullWaits += taken->nullWaits;
    total->early += taken->early;
    if (taken->maxWaitUs > total->maxWaitUs) total->maxWaitUs = taken->maxWaitUs;
    total->exec += taken->exec;
    total->eventWaits += taken->eventWaits;
    total->settled += taken->settled;
    if (taken->maxSettleUs > total->maxSettleUs) total->maxSettleUs = taken->maxSettleUs;
    total->timeouts += taken->timeouts;
    total->failed += taken->failed;
}

static XboxFenceWindow totals;

static void Report(void) {
    const ULONGLONG start = GetTickCount64();
    ULONGLONG nextTotal = start + 60000;
    for (;;) {
        Sleep(1000);
        XboxFenceWindow taken;
        TakeWindow(&taken, &totals);
        char line[LINE_BUFFER];
        if (XboxFenceWindowActive(&taken)) Line(line, Utc(XboxFenceFormatWindow(line, &taken)));
        const ULONGLONG now = GetTickCount64();
        if (now >= nextTotal) {
            nextTotal = now + 60000;
            Line(line, Utc(XboxFenceFormatTotal(line, &totals, (uint32_t)fenceMode, (now - start) / 1000)));
        }
    }
}

static DWORD WINAPI FenceThread(LPVOID unused) {
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
    if (SUCCEEDED(result)) result = XboxD3D12FenceTraceInstall(device);
    char line[LINE_BUFFER];
    const char *end = LINE_END(line);
    char *out = Hex32(XboxResidencyText(line, end, "asset d3d12-fence install result="), (DWORD)result);
    out = XboxResidencyField(out, end, " tables=", (uint64_t)tableCount);
    out = XboxResidencyText(XboxResidencyText(out, end, " mode="), end, XboxFenceModeName((uint32_t)fenceMode));
    Line(line, Utc(XboxResidencyText(out, end, " utc=")));
    if (SUCCEEDED(result)) {
        /* Written after the install line, so a wait that never returns shows
           as an install line with no self-test line. */
        ULONGLONG waitUs = 0;
        const HRESULT test = XboxD3D12FenceSelfTest(device, &waitUs);
        out = Hex32(XboxResidencyText(line, end, "asset d3d12-fence selftest result="), (DWORD)test);
        out = XboxResidencyField(out, end, " wait_us=", waitUs);
        Line(line, Utc(XboxResidencyText(out, end, " utc=")));
    }
    Release(device);
    if (SUCCEEDED(result)) Report();
    return 0;
}

/* The package-root file, or XBOX_FENCE_ABSENT without one. */
static uint32_t ReadModeFile(void) {
    WCHAR path[1024];
    static const WCHAR suffix[] = L"\\xbox-d3d12-fence-trace.txt";
    UINT32 length = ARRAYSIZE(path);
    if (GetCurrentPackagePath(&length, path) != ERROR_SUCCESS || !length ||
            length - 1 + ARRAYSIZE(suffix) > ARRAYSIZE(path))
        return XBOX_FENCE_ABSENT;
    for (UINT32 i = 0; i < ARRAYSIZE(suffix); ++i) path[length - 1 + i] = suffix[i];
    const HANDLE file = CreateFile2FromAppW(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL);
    if (file == INVALID_HANDLE_VALUE) return XBOX_FENCE_ABSENT;
    char text[32];
    DWORD read = 0;
    const BOOL ok = ReadFile(file, text, sizeof(text), &read, NULL);
    CloseHandle(file);
    if (!ok || read == sizeof(text)) return XBOX_FENCE_INVALID;
    return XboxFenceParseMode(text, read);
}

void XboxD3D12FenceTraceStart(void) {
    static volatile LONG started;
    if (InterlockedExchange(&started, 1)) return;
    const DWORD error = GetLastError();
    const uint32_t parsed = ReadModeFile();
    if (parsed == XBOX_FENCE_ABSENT) {
        SetLastError(error);
        return;
    }
    const uint32_t mode = XboxFenceEffectiveMode(parsed);
    InterlockedExchange(&fenceMode, (LONG)mode);
    HANDLE thread = mode != XBOX_FENCE_OFF ? CreateThread(NULL, 0, FenceThread, NULL, 0, NULL) : NULL;
    char line[LINE_BUFFER];
    const char *end = LINE_END(line);
    char *out = XboxResidencyText(XboxResidencyText(line, end, "asset d3d12-fence mode="), end,
        XboxFenceModeName(mode));
    out = XboxResidencyText(XboxResidencyText(out, end, " source="), end, XboxFenceSourceName(parsed));
    out = XboxResidencyText(out, end, mode == XBOX_FENCE_OFF ? " hooks=off" : thread ? " hooks=pending" :
        " hooks=thread-failed");
    Line(line, out);
    if (thread) CloseHandle(thread);
    SetLastError(error);
}
