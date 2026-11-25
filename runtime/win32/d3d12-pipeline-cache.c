/* Persistent Direct3D 12 pipeline-state cache, linked into the loader-trace
   adapter.

   Every ID3D12Device in a process shares one method table, including the
   devices that a hosted OpenGL runtime such as Mesa creates from a device
   factory. Once the title loads d3d12.dll, a background thread takes that
   table from a device of its own and wraps CreateRootSignature (slot 16),
   CreateGraphicsPipelineState (10), CreateComputePipelineState (11) and
   ID3D12Device2::CreatePipelineState (47).

   Timing (XBOX_WIN32_PSO_TRACE=1 or a cache): each creation of 2 ms or more
   is written as "pso kind=G|C|S utc=... us=... result=0x... cache=...".
   Root signatures (slot 16) are wrapped only when a cache mode keys them;
   passthrough patches exactly the timing slots 10, 11 and 47.

   Cache (XBOX_WIN32_PSO_CACHE=<LocalState>\D3D12PipelineCache\<app>-<build>):
   - Key: SHA-256 over the whole desc. Pointers are serialized by content: the
     root signature by the blob it was created from, every shader by its
     bytecode, semantic names by their text. A desc that cannot be keyed (an
     unknown stream subobject, an app-supplied CachedPSO, a root signature
     created before the hooks) goes to the driver unchanged.
   - Storage: <prefix>-<identity>.bin, where the identity hashes the prefix
     (AppID and game build), the DXGI adapter's vendor, device, subsystem,
     revision, description and user-mode driver version, the OS version, and
     d3d12.dll's image stamp; the adapter LUID is only compared within the
     process. A mismatched, truncated or corrupt file is deleted and rebuilt.
   - Mode: chosen by the profile, never by probing the title's device.
     XBOX_WIN32_PSO_CACHE_MODE=library uses an ID3D12PipelineLibrary, =blobs
     per-pipeline ID3D12PipelineState::GetCachedBlob records supplied back
     through CachedPSO; anything else is passthrough, which only times
     creation: the hooks forward every call and the cache makes no call on
     the device. A profile names library or blobs only with a console control
     result that proved that path in a separate process
     (probe/graphics-runtime-control --d3d12-pipeline-cache). A library that
     cannot be created, or blobs mode on a device without single-PSO support,
     leaves the process in passthrough; one mode never falls back to the
     other. The cache binds to the first device that creates a pipeline;
     other devices pass through.
   - Device safety: every cache call into the runtime (GetAdapterLuid,
     CheckFeatureSupport, QueryInterface, CreatePipelineLibrary, Store, Load,
     Serialize, GetCachedBlob, creation with CachedPSO) is followed by
     ID3D12Device::GetDeviceRemovedReason. The first call of each kind in a
     process is bracketed by "asset pso-cache call=NAME phase=before|after"
     lines and by <prefix>.guard, written before the call and deleted after
     it. A removal, seen after a cache call or reported by the title's own
     creation, writes "asset pso-cache device-removed after=NAME ..." with a
     DRED summary, leaves the guard naming that call, and stops the cache for
     the rest of the process: no further cache call and no file write. In
     passthrough a removal from the title's creation writes only
     "asset pso-cache device-removed after=game-create result=..." with the
     HRESULT the title received: no removal reason and no DRED read. A
     guard left for the same identity (a process that died inside a call, or
     a removal) selects passthrough on later launches; a driver, OS or game
     build change starts over.
   - Contract: the hooked methods return exactly what the driver returns for
     the original desc, riid and ppPipelineState. A null ppPipelineState
     (validation only) and repeated creation of one desc in a process go to
     the driver. A library load that fails, or a cached blob the driver
     rejects, falls back to the original call and writes one diagnostic line
     per failing operation. The D3D12 runtime itself compares a loaded
     pipeline's desc with the stored one, so a key collision cannot return a
     different pipeline. Loaded and stored pipelines keep a reference held by
     the library for the life of the process.
   - Writes: a writer thread serializes after creation goes quiet, during long
     bursts every few seconds, immediately after any pipeline whose creation
     took 500 ms or more (multi-second compiles were measured on the console,
     and a crash or quit right after one must not lose it) and at process
     exit, to a temporary file that then replaces the cache. Large pipelines are kept; only the 256 MiB total is bounded. Library calls are serialized
     by one lock; driver compilation runs outside it, and a second creation of
     a key already in flight goes to the driver instead of racing its store.

   Diagnostics use "asset pso-cache ..." lines so the launcher's trace filters
   keep them. */
#define WIN32_LEAN_AND_MEAN
#include "loader-trace-output.h"
#include "d3d12-pipeline-cache.h"
#include "pipeline-cache-format.h"
#include <d3d12.h>
#include <dxgi1_2.h>

DECLSPEC_IMPORT BOOL WINAPI MoveFileFromAppW(LPCWSTR, LPCWSTR);
DECLSPEC_IMPORT BOOL WINAPI ReplaceFileFromAppW(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);
DECLSPEC_IMPORT BOOL WINAPI DeleteFileFromAppW(LPCWSTR);
DECLSPEC_IMPORT HANDLE WINAPI FindFirstFileExFromAppW(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS,
    LPVOID, DWORD);

/* ---------------------------------------------------------------- helpers */

#define METHOD(object, slot, type) ((type)((*(void ***)(object))[slot]))
typedef HRESULT (STDMETHODCALLTYPE *QueryMethod)(void *, REFIID, void **);
typedef ULONG (STDMETHODCALLTYPE *ReleaseMethod)(void *);

static HRESULT Query(void *object, const GUID *iid, void **out) {
    *out = NULL;
    return METHOD(object, 0, QueryMethod)(object, iid, out);
}

static void Release(void *object) {
    if (object) METHOD(object, 2, ReleaseMethod)(object);
}

/* The adapters link without a C runtime. Clang turns copy loops and its
   __movsb/__stosb builtins into memcpy/memset calls, so use the string
   instructions directly. */
static void CopyBytes(void *to, const void *from, SIZE_T size) {
    __asm__ volatile("rep movsb" : "+D"(to), "+S"(from), "+c"(size) : : "memory");
}

static void ZeroBytes(void *to, SIZE_T size) {
    __asm__ volatile("rep stosb" : "+D"(to), "+c"(size) : "a"(0) : "memory");
}

static char *Text(char *out, const char *text) {
    while (*text) *out++ = *text++;
    return out;
}

static char *Hex32(char *out, DWORD value) {
    const char digits[] = "0123456789ABCDEF";
    out = Text(out, "0x");
    for (int shift = 28; shift >= 0; shift -= 4) *out++ = digits[(value >> shift) & 15];
    return out;
}

static char *Decimal(char *out, ULONGLONG value) {
    char digits[20];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < 20);
    while (count) *out++ = digits[--count];
    return out;
}

static char *Digits(char *out, unsigned value, int width) {
    for (int shift = width - 1; shift >= 0; --shift) {
        unsigned scale = 1;
        for (int i = 0; i < shift; ++i) scale *= 10;
        *out++ = (char)('0' + value / scale % 10);
    }
    return out;
}

static void Line(char *start, char *end) {
    *end++ = '\r';
    *end++ = '\n';
    AppendTrace(start, (DWORD)(end - start));
}

static ULONGLONG Microseconds(LONGLONG ticks) {
    static LARGE_INTEGER frequency;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart ? (ULONGLONG)ticks * 1000000 / (ULONGLONG)frequency.QuadPart : 0;
}

static LONGLONG Now(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

/* ------------------------------------------------------------------ state */

enum { KIND_GRAPHICS, KIND_COMPUTE, KIND_STREAM };
static const char kindNames[] = "GCS";

typedef HRESULT (STDMETHODCALLTYPE *CreatePipeline)(void *, const void *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *CreateRootSignatureMethod)(void *, UINT, const void *, SIZE_T, REFIID, void **);
static CreatePipeline originalPipelines[3];
static CreateRootSignatureMethod originalRootSignature;
static volatile LONG timingEvents;
static volatile LONG traceTiming;
static DWORD reentrySlot = TLS_OUT_OF_INDEXES;
static volatile LONG keyRootSignatures;

static const GUID deviceIid = {0x189819F1, 0x1DB6, 0x4B57, {0xBE, 0x54, 0x18, 0x21, 0x33, 0x9B, 0x85, 0xF7}};
static const GUID device1Iid = {0x77ACCE80, 0x638E, 0x4E65, {0x88, 0x95, 0xC1, 0xF2, 0x33, 0x86, 0x86, 0x3E}};
static const GUID libraryIid = {0xC64226A8, 0x9201, 0x46AF, {0xB4, 0xCC, 0x53, 0xFB, 0x9F, 0xF7, 0x41, 0x4F}};
static const GUID library1Iid = {0x80EABF42, 0x2568, 0x4E5E, {0xBD, 0x82, 0xC3, 0x7F, 0x86, 0x96, 0x1D, 0xC3}};
static const GUID pipelineIid = {0x765A30F3, 0xF624, 0x4C6F, {0xA8, 0x28, 0xAC, 0xE9, 0x48, 0x62, 0x24, 0x45}};
static const GUID factory1Iid = {0x770AAE78, 0xF26F, 0x4DBA, {0xA8, 0x29, 0x25, 0x3C, 0x83, 0xD1, 0xB3, 0x87}};
static const GUID dxgiDeviceIid = {0x54EC77FA, 0x1377, 0x44E6, {0x8C, 0x32, 0x88, 0xFD, 0x5F, 0x44, 0xC8, 0x4C}};
static const GUID dredIid = {0x98931D33, 0x5AE8, 0x4791, {0xAA, 0x3C, 0x1A, 0x73, 0xA2, 0x93, 0x4E, 0x71}};

/* Root signatures by object, with the SHA-256 of node mask and blob. An
   address reused by a later root signature is overwritten. */
#define ROOT_SLOTS 4096u
typedef struct RootEntry {
    void *object;
    BYTE hash[32];
} RootEntry;
static RootEntry *roots;
static SRWLOCK rootLock = SRWLOCK_INIT;

/* Pipelines by key: seen in this process, and (blob mode) cached blobs. */
#define ENTRY_SLOTS 8192u
#define ENTRY_USED 1u
#define ENTRY_SEEN 2u
#define ENTRY_BLOB 4u
#define ENTRY_HEAP 8u
#define ENTRY_INVALID 16u
typedef struct Entry {
    BYTE key[32];
    const BYTE *blob;
    UINT32 size;
    UINT32 kind;
    UINT32 flags;
} Entry;

static struct {
    SRWLOCK lock;       /* entries, the library and the mode */
    SRWLOCK writeLock;  /* one file write at a time */
    volatile LONG configured;
    volatile LONG ready;
    WCHAR prefix[400];
    WCHAR path[480];
    BYTE identity[32];
    LUID luid;
    BOOL haveLuid;
    Entry *entries;
    BYTE *file;
    XboxPsoCacheView view;
    LONG fileStatus;
    void *volatile device;
    void *library;
    void *library1;
    LONG mode;
    LONG supportFlags;
    HRESULT libraryResult;
    UINT32 pipelines;
    ULONGLONG blobBytes;
    ULONGLONG serializedBytes;
    LONG unsaved;
    LONG slowPending;
    LONG selected;      /* the profile's mode; 0 is passthrough */
    LONG writeFailures;
    ULONGLONG lastStoreTick;
    ULONGLONG lastWriteTick;
    volatile LONG exited;
    volatile LONG passthrough;  /* timing only, with stats at exit */
    HANDLE wake;
    /* Device safety: see "Device safety" above. */
    WCHAR guardPath[480];
    SRWLOCK guardLock;
    LONG guardArmed;            /* calls in flight under the guard */
    LONG guardSticky;           /* a removal: the guard stays */
    volatile LONG stopped;
    volatile LONG proven;       /* call kinds that returned with the device intact */
    volatile LONG failedCalls;  /* call kinds whose failure was traced */
    volatile LONG lastCall;
    volatile LONG removedCall;
    volatile LONG removedReason;
    LONG guardCall;             /* a guard left by an earlier launch */
    struct {
        volatile LONG hits, misses, stores, duplicates, skipped, unkeyed, bypassed, writes, failures;
        volatile LONG64 loadUs, hitUs, missUs;
    } stats;
    volatile LONG fallbackMask;
} cache; /* Zero-initialized: SRWLOCK_INIT is all zeros. */

static BOOL Reentrant(void) {
    return reentrySlot != TLS_OUT_OF_INDEXES && TlsGetValue(reentrySlot) != NULL;
}

static void SetReentrant(BOOL inside) {
    if (reentrySlot != TLS_OUT_OF_INDEXES) TlsSetValue(reentrySlot, inside ? (LPVOID)1 : NULL);
}

/* One diagnostic line per failing cache operation, then silence. */
static void Fallback(int bit, const char *operation, HRESULT result) {
    InterlockedIncrement(&cache.stats.failures);
    if (InterlockedOr(&cache.fallbackMask, 1L << bit) & (1L << bit)) return;
    char line[120];
    char *out = Text(Text(Text(line, "asset pso-cache fallback op="), operation), " result=");
    Line(line, Hex32(out, (DWORD)result));
}
enum {
    FALLBACK_LOAD, FALLBACK_BLOB, FALLBACK_STORE, FALLBACK_QUERY, FALLBACK_CACHED_BLOB, FALLBACK_OTHER_DEVICE,
    FALLBACK_EARLY, FALLBACK_FULL, FALLBACK_KEY, FALLBACK_WRITE, FALLBACK_STREAM_LIBRARY, FALLBACK_GUARD,
};

static const char *ModeName(LONG mode) {
    return mode == XBOX_PSO_CACHE_MODE_LIBRARY ? "library" : mode == XBOX_PSO_CACHE_MODE_BLOBS ? "blobs" :
        cache.passthrough ? "passthrough" : cache.stopped ? "stopped" : "disabled";
}

static void TraceStats(const char *reason) {
    char line[320];
    char *out = Text(Text(line, "asset pso-cache stats at="), reason);
    out = Text(Text(out, " mode="), ModeName(cache.mode));
    out = Decimal(Text(out, " hits="), (ULONG)cache.stats.hits);
    out = Decimal(Text(out, " misses="), (ULONG)cache.stats.misses);
    out = Decimal(Text(out, " stores="), (ULONG)cache.stats.stores);
    out = Decimal(Text(out, " dups="), (ULONG)cache.stats.duplicates);
    out = Decimal(Text(out, " skipped="), (ULONG)cache.stats.skipped);
    out = Decimal(Text(out, " unkeyed="), (ULONG)cache.stats.unkeyed);
    out = Decimal(Text(out, " bypass="), (ULONG)cache.stats.bypassed);
    out = Decimal(Text(out, " load-ms="), (ULONGLONG)cache.stats.loadUs / 1000);
    out = Decimal(Text(out, " hit-ms="), (ULONGLONG)cache.stats.hitUs / 1000);
    out = Decimal(Text(out, " miss-ms="), (ULONGLONG)cache.stats.missUs / 1000);
    out = Decimal(Text(out, " writes="), (ULONG)cache.stats.writes);
    Line(line, out);
}

/* --------------------------------------------------------- device safety */

typedef HRESULT (STDMETHODCALLTYPE *RemovedReasonMethod)(void *);

/* ID3D12Device::GetDeviceRemovedReason, a status read with no side effects. */
static HRESULT RemovedReason(void *device) {
    return device ? METHOD(device, 37, RemovedReasonMethod)(device) : S_OK;
}

static char *HexDigits(char *out, ULONGLONG value, int digits) {
    const char hex[] = "0123456789ABCDEF";
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) *out++ = hex[(value >> shift) & 15];
    return out;
}

static void CallLine(LONG call, const char *phase, HRESULT result, BOOL withResult) {
    char line[120];
    char *out = Text(Text(line, "asset pso-cache call="), XboxPsoCacheCallName((uint32_t)call));
    out = Text(Text(out, " phase="), phase);
    if (withResult) out = Hex32(Text(out, " result="), (DWORD)result);
    Line(line, out);
}

/* With guardLock held. */
static void WriteGuardLocked(LONG call) {
    if (!cache.guardPath[0]) return;
    char text[XBOX_PSO_CACHE_GUARD_MAX];
    const DWORD size = (DWORD)XboxPsoCacheFormatGuard(text, (uint32_t)call, cache.identity);
    HANDLE file = CreateFile2FromAppW(cache.guardPath, GENERIC_WRITE, 0, CREATE_ALWAYS, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        Fallback(FALLBACK_GUARD, "guard", HRESULT_FROM_WIN32(GetLastError()));
        return;
    }
    DWORD written = 0;
    if (!WriteFile(file, text, size, &written, NULL) || written != size)
        Fallback(FALLBACK_GUARD, "guard", HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_WRITE_FAULT));
    CloseHandle(file);
}

/* A compact DRED summary: whether the runtime kept removal data, the
   breadcrumb nodes with the first incomplete one, and the page fault. DRED
   is only populated when it was enabled before the device was created. */
static void TraceDred(void *device) {
    typedef HRESULT (STDMETHODCALLTYPE *Breadcrumbs)(void *, D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT *);
    typedef HRESULT (STDMETHODCALLTYPE *PageFault)(void *, D3D12_DRED_PAGE_FAULT_OUTPUT *);
    char line[240];
    void *dred = NULL;
    HRESULT result = device ? Query(device, &dredIid, &dred) : E_POINTER;
    char *out = Hex32(Text(line, "asset pso-cache dred qi="), (DWORD)result);
    if (SUCCEEDED(result) && dred) {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs;
        crumbs.pHeadAutoBreadcrumbNode = NULL;
        result = METHOD(dred, 3, Breadcrumbs)(dred, &crumbs);
        out = Hex32(Text(out, " crumbs="), (DWORD)result);
        volatile ULONG nodes = 0, incomplete = 0, done = 0, total = 0, op = 0;
        __try {
            for (const D3D12_AUTO_BREADCRUMB_NODE *node = SUCCEEDED(result) ? crumbs.pHeadAutoBreadcrumbNode : NULL;
                    node && nodes < 4096; node = node->pNext) {
                ++nodes;
                if (!node->pCommandHistory || !node->pLastBreadcrumbValue ||
                        *node->pLastBreadcrumbValue >= node->BreadcrumbCount) continue;
                if (!incomplete) {
                    done = *node->pLastBreadcrumbValue;
                    total = node->BreadcrumbCount;
                    op = (ULONG)node->pCommandHistory[done];
                }
                ++incomplete;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out = Text(out, " walk=fault");
        }
        out = Decimal(Text(out, " nodes="), nodes);
        out = Decimal(Text(out, " open="), incomplete);
        if (incomplete) {
            out = Decimal(Text(out, " done="), done);
            out = Decimal(Text(out, " of="), total);
            out = Decimal(Text(out, " op="), op);
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT fault;
        fault.PageFaultVA = 0;
        result = METHOD(dred, 4, PageFault)(dred, &fault);
        out = Hex32(Text(out, " fault="), (DWORD)result);
        if (SUCCEEDED(result)) out = HexDigits(Text(out, " va=0x"), fault.PageFaultVA, 16);
        Release(dred);
    }
    Line(line, out);
}

/* Stops the cache for this process at the first removal and keeps the guard
   naming the call that preceded it. FALSE when a removal was already seen. */
static BOOL StopForRemoval(LONG call, HRESULT reason) {
    if (InterlockedCompareExchange(&cache.stopped, 1, 0)) return FALSE;
    InterlockedExchange(&cache.removedCall, call);
    InterlockedExchange(&cache.removedReason, (LONG)reason);
    InterlockedExchange(&cache.mode, 0);
    AcquireSRWLockExclusive(&cache.guardLock);
    cache.guardSticky = 1;
    /* Passthrough made no cache call; an earlier guard keeps its call. */
    if (!cache.passthrough) WriteGuardLocked(call);
    ReleaseSRWLockExclusive(&cache.guardLock);
    return TRUE;
}

/* Passthrough: the title's own creation returned a removal. The cache makes
   no call on the device, so it records only the HRESULT the title received
   (also kept as the counters' removedReason), with no removal reason or DRED
   read. */
static void PassthroughRemoved(HRESULT result) {
    if (!StopForRemoval(XBOX_PSO_CALL_GAME_CREATE, result)) return;
    char line[120];
    char *out = Text(Text(line, "asset pso-cache device-removed after="),
        XboxPsoCacheCallName((uint32_t)XBOX_PSO_CALL_GAME_CREATE));
    Line(line, Hex32(Text(out, " result="), (DWORD)result));
    TraceStats("device-removed");
}

/* A cache mode: records the first removal with the device's reason and a
   DRED summary, and stops the cache for this process. */
static void DeviceRemoved(void *device, LONG call, HRESULT result, HRESULT reason) {
    if (!StopForRemoval(call, reason)) return;
    char line[200];
    char *out = Text(Text(line, "asset pso-cache device-removed after="), XboxPsoCacheCallName((uint32_t)call));
    out = Hex32(Text(out, " result="), (DWORD)result);
    out = Hex32(Text(out, " reason="), (DWORD)reason);
    out = Text(Text(out, " last-cache-call="), XboxPsoCacheCallName((uint32_t)cache.lastCall));
    Line(line, out);
    TraceDred(device);
    TraceStats("device-removed");
}

/* Before a cache call: the first of each kind in this process writes the
   guard and a "phase=before" line. Returns whether the guard was armed. */
static BOOL BeforeCall(LONG call) {
    InterlockedExchange(&cache.lastCall, call);
    if (cache.proven & (1L << call)) return FALSE;
    AcquireSRWLockExclusive(&cache.guardLock);
    if (!cache.guardSticky) WriteGuardLocked(call);
    ++cache.guardArmed;
    ReleaseSRWLockExclusive(&cache.guardLock);
    CallLine(call, "before", S_OK, FALSE);
    return TRUE;
}

/* After a cache call: FALSE when the device is now removed (the cache has
   stopped). Otherwise the call kind is proven for this process and the guard
   is removed once no first call is in flight. */
static BOOL AfterCall(void *device, LONG call, HRESULT result, BOOL armed) {
    const HRESULT reason = RemovedReason(device);
    const LONG bit = 1L << call;
    if (reason != S_OK) {
        if (armed) CallLine(call, "after", result, TRUE);
        DeviceRemoved(device, call, result, reason);
    } else if (armed) {
        CallLine(call, "after", result, TRUE);
        InterlockedOr(&cache.proven, bit);
    } else if (FAILED(result) && !(InterlockedOr(&cache.failedCalls, bit) & bit)) {
        CallLine(call, "after", result, TRUE);
    }
    if (armed) {
        AcquireSRWLockExclusive(&cache.guardLock);
        if (!--cache.guardArmed && !cache.guardSticky && cache.guardPath[0]) DeleteFileFromAppW(cache.guardPath);
        ReleaseSRWLockExclusive(&cache.guardLock);
    }
    return reason == S_OK;
}

/* ---------------------------------------------------------- key building */

#define FIELD(hash, value) XboxSha256Update((hash), &(value), sizeof(value))
#define MAX_ITEMS 4096u
#define MAX_BYTECODE (64u * 1024 * 1024)

/* D3D12_DEPTH_STENCIL_DESC2 (stream subobject 0x1A), absent from older headers. */
typedef struct StencilFace2 {
    D3D12_STENCIL_OP StencilFailOp, StencilDepthFailOp, StencilPassOp;
    D3D12_COMPARISON_FUNC StencilFunc;
    UINT8 StencilReadMask, StencilWriteMask;
} StencilFace2;
typedef struct DepthStencil2 {
    BOOL DepthEnable;
    D3D12_DEPTH_WRITE_MASK DepthWriteMask;
    D3D12_COMPARISON_FUNC DepthFunc;
    BOOL StencilEnable;
    StencilFace2 FrontFace, BackFace;
    BOOL DepthBoundsTestEnable;
} DepthStencil2;
_Static_assert(sizeof(DepthStencil2) == 60, "D3D12_DEPTH_STENCIL_DESC2 layout");
/* Structures hashed as raw bytes have no padding. */
_Static_assert(sizeof(D3D12_RASTERIZER_DESC) == 44, "rasterizer layout");
_Static_assert(sizeof(D3D12_DEPTH_STENCILOP_DESC) == 16, "stencil layout");
/* D3D12_RT_FORMAT_ARRAY, which some headers omit. */
typedef struct RenderTargetFormats {
    DXGI_FORMAT RTFormats[8];
    UINT NumRenderTargets;
} RenderTargetFormats;
_Static_assert(sizeof(RenderTargetFormats) == 36, "render target layout");
_Static_assert(sizeof(DXGI_SAMPLE_DESC) == 8, "sample layout");

static BOOL HashRoot(XboxSha256 *hash, void *root) {
    if (!root) {
        XboxPsoKeyU32(hash, 0);
        return TRUE;
    }
    BOOL found = FALSE;
    AcquireSRWLockShared(&rootLock);
    if (roots) {
        const UINT first = (UINT)(((ULONG_PTR)root >> 4) % ROOT_SLOTS);
        for (UINT i = 0; i < ROOT_SLOTS; ++i) {
            const RootEntry *entry = &roots[(first + i) % ROOT_SLOTS];
            if (!entry->object) break;
            if (entry->object == root) {
                XboxPsoKeyU32(hash, 1);
                XboxSha256Update(hash, entry->hash, 32);
                found = TRUE;
                break;
            }
        }
    }
    ReleaseSRWLockShared(&rootLock);
    return found;
}

static BOOL HashShader(XboxSha256 *hash, const D3D12_SHADER_BYTECODE *shader) {
    if ((shader->BytecodeLength && !shader->pShaderBytecode) || shader->BytecodeLength > MAX_BYTECODE) return FALSE;
    XboxPsoKeyBytes(hash, shader->BytecodeLength ? shader->pShaderBytecode : NULL, shader->BytecodeLength);
    return TRUE;
}

static BOOL HashStreamOutput(XboxSha256 *hash, const D3D12_STREAM_OUTPUT_DESC *output) {
    if ((output->NumEntries && !output->pSODeclaration) || output->NumEntries > MAX_ITEMS ||
            (output->NumStrides && !output->pBufferStrides) || output->NumStrides > MAX_ITEMS) return FALSE;
    FIELD(hash, output->NumEntries);
    for (UINT i = 0; i < output->NumEntries; ++i) {
        const D3D12_SO_DECLARATION_ENTRY *entry = &output->pSODeclaration[i];
        FIELD(hash, entry->Stream);
        XboxPsoKeyString(hash, entry->SemanticName);
        FIELD(hash, entry->SemanticIndex);
        FIELD(hash, entry->StartComponent);
        FIELD(hash, entry->ComponentCount);
        FIELD(hash, entry->OutputSlot);
    }
    FIELD(hash, output->NumStrides);
    if (output->NumStrides) XboxSha256Update(hash, output->pBufferStrides, output->NumStrides * sizeof(UINT));
    FIELD(hash, output->RasterizedStream);
    return TRUE;
}

static void HashBlend(XboxSha256 *hash, const D3D12_BLEND_DESC *blend) {
    FIELD(hash, blend->AlphaToCoverageEnable);
    FIELD(hash, blend->IndependentBlendEnable);
    for (UINT i = 0; i < 8; ++i) {
        const D3D12_RENDER_TARGET_BLEND_DESC *target = &blend->RenderTarget[i];
        FIELD(hash, target->BlendEnable);
        FIELD(hash, target->LogicOpEnable);
        FIELD(hash, target->SrcBlend);
        FIELD(hash, target->DestBlend);
        FIELD(hash, target->BlendOp);
        FIELD(hash, target->SrcBlendAlpha);
        FIELD(hash, target->DestBlendAlpha);
        FIELD(hash, target->BlendOpAlpha);
        FIELD(hash, target->LogicOp);
        FIELD(hash, target->RenderTargetWriteMask);
    }
}

static void HashDepthStencil(XboxSha256 *hash, const D3D12_DEPTH_STENCIL_DESC *depth) {
    FIELD(hash, depth->DepthEnable);
    FIELD(hash, depth->DepthWriteMask);
    FIELD(hash, depth->DepthFunc);
    FIELD(hash, depth->StencilEnable);
    FIELD(hash, depth->StencilReadMask);
    FIELD(hash, depth->StencilWriteMask);
    FIELD(hash, depth->FrontFace);
    FIELD(hash, depth->BackFace);
}

static void HashDepthStencil2(XboxSha256 *hash, const DepthStencil2 *depth) {
    FIELD(hash, depth->DepthEnable);
    FIELD(hash, depth->DepthWriteMask);
    FIELD(hash, depth->DepthFunc);
    FIELD(hash, depth->StencilEnable);
    const StencilFace2 *faces[2] = {&depth->FrontFace, &depth->BackFace};
    for (int i = 0; i < 2; ++i) {
        FIELD(hash, faces[i]->StencilFailOp);
        FIELD(hash, faces[i]->StencilDepthFailOp);
        FIELD(hash, faces[i]->StencilPassOp);
        FIELD(hash, faces[i]->StencilFunc);
        FIELD(hash, faces[i]->StencilReadMask);
        FIELD(hash, faces[i]->StencilWriteMask);
    }
    FIELD(hash, depth->DepthBoundsTestEnable);
}

static BOOL HashInputLayout(XboxSha256 *hash, const D3D12_INPUT_LAYOUT_DESC *layout) {
    if ((layout->NumElements && !layout->pInputElementDescs) || layout->NumElements > MAX_ITEMS) return FALSE;
    FIELD(hash, layout->NumElements);
    for (UINT i = 0; i < layout->NumElements; ++i) {
        const D3D12_INPUT_ELEMENT_DESC *element = &layout->pInputElementDescs[i];
        XboxPsoKeyString(hash, element->SemanticName);
        FIELD(hash, element->SemanticIndex);
        FIELD(hash, element->Format);
        FIELD(hash, element->InputSlot);
        FIELD(hash, element->AlignedByteOffset);
        FIELD(hash, element->InputSlotClass);
        FIELD(hash, element->InstanceDataStepRate);
    }
    return TRUE;
}

static BOOL HashGraphics(XboxSha256 *hash, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc) {
    if (desc->CachedPSO.CachedBlobSizeInBytes) return FALSE; /* The title manages its own cache. */
    if (!HashRoot(hash, desc->pRootSignature) || !HashShader(hash, &desc->VS) || !HashShader(hash, &desc->PS) ||
            !HashShader(hash, &desc->DS) || !HashShader(hash, &desc->HS) || !HashShader(hash, &desc->GS) ||
            !HashStreamOutput(hash, &desc->StreamOutput)) return FALSE;
    HashBlend(hash, &desc->BlendState);
    FIELD(hash, desc->SampleMask);
    FIELD(hash, desc->RasterizerState);
    HashDepthStencil(hash, &desc->DepthStencilState);
    if (!HashInputLayout(hash, &desc->InputLayout)) return FALSE;
    FIELD(hash, desc->IBStripCutValue);
    FIELD(hash, desc->PrimitiveTopologyType);
    FIELD(hash, desc->NumRenderTargets);
    FIELD(hash, desc->RTVFormats);
    FIELD(hash, desc->DSVFormat);
    FIELD(hash, desc->SampleDesc);
    FIELD(hash, desc->NodeMask);
    FIELD(hash, desc->Flags);
    return TRUE;
}

static BOOL HashCompute(XboxSha256 *hash, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc) {
    if (desc->CachedPSO.CachedBlobSizeInBytes || !HashRoot(hash, desc->pRootSignature) ||
            !HashShader(hash, &desc->CS)) return FALSE;
    FIELD(hash, desc->NodeMask);
    FIELD(hash, desc->Flags);
    return TRUE;
}

/* Stream subobjects: a 32-bit type, then the value at its natural alignment,
   each subobject padded to pointer size. */
static BOOL StreamValue(UINT32 type, SIZE_T *offset, SIZE_T *size) {
    SIZE_T align = 4;
    switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: *size = sizeof(void *); align = 8; break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
        *size = sizeof(D3D12_SHADER_BYTECODE); align = 8; break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: *size = sizeof(D3D12_STREAM_OUTPUT_DESC); align = 8; break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: *size = sizeof(D3D12_BLEND_DESC); break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
        *size = sizeof(UINT); break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: *size = sizeof(D3D12_RASTERIZER_DESC); break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: *size = sizeof(D3D12_DEPTH_STENCIL_DESC); break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: *size = sizeof(D3D12_INPUT_LAYOUT_DESC); align = 8; break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: *size = sizeof(RenderTargetFormats); break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: *size = sizeof(DXGI_SAMPLE_DESC); break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: *size = sizeof(D3D12_CACHED_PIPELINE_STATE); align = 8; break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: *size = sizeof(D3D12_DEPTH_STENCIL_DESC1); break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: *size = sizeof(D3D12_VIEW_INSTANCING_DESC); align = 8; break;
    case 0x1A /* DEPTH_STENCIL2 */: *size = sizeof(DepthStencil2); break;
    default: return FALSE; /* RASTERIZER1 and later: not keyed yet. */
    }
    *offset = (4 + align - 1) & ~(align - 1);
    return TRUE;
}

#define NO_CACHED_PSO ((SIZE_T)-1)

static BOOL HashStream(XboxSha256 *hash, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, SIZE_T *cachedPso) {
    const BYTE *stream = (const BYTE *)desc->pPipelineStateSubobjectStream;
    const SIZE_T size = desc->SizeInBytes;
    *cachedPso = NO_CACHED_PSO;
    if (!stream || !size || size > MAX_BYTECODE || ((ULONG_PTR)stream & 7)) return FALSE;
    XboxPsoKeyU64(hash, size);
    for (SIZE_T offset = 0; offset < size;) {
        SIZE_T valueOffset, valueSize;
        if (size - offset < 4) return FALSE;
        const UINT32 type = *(const UINT32 *)(stream + offset);
        if (!StreamValue(type, &valueOffset, &valueSize) || valueOffset + valueSize > size - offset) return FALSE;
        const BYTE *value = stream + offset + valueOffset;
        XboxPsoKeyU32(hash, type);
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
            if (!HashRoot(hash, *(void *const *)value)) return FALSE;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
            if (!HashShader(hash, (const D3D12_SHADER_BYTECODE *)value)) return FALSE;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
            if (!HashStreamOutput(hash, (const D3D12_STREAM_OUTPUT_DESC *)value)) return FALSE;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
            HashBlend(hash, (const D3D12_BLEND_DESC *)value);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
            HashDepthStencil(hash, (const D3D12_DEPTH_STENCIL_DESC *)value);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
            HashDepthStencil(hash, (const D3D12_DEPTH_STENCIL_DESC *)value);
            FIELD(hash, ((const D3D12_DEPTH_STENCIL_DESC1 *)value)->DepthBoundsTestEnable);
            break;
        case 0x1A:
            HashDepthStencil2(hash, (const DepthStencil2 *)value);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
            if (!HashInputLayout(hash, (const D3D12_INPUT_LAYOUT_DESC *)value)) return FALSE;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
            if (((const D3D12_CACHED_PIPELINE_STATE *)value)->CachedBlobSizeInBytes) return FALSE;
            *cachedPso = offset + valueOffset;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
            const D3D12_VIEW_INSTANCING_DESC *views = (const D3D12_VIEW_INSTANCING_DESC *)value;
            if ((views->ViewInstanceCount && !views->pViewInstanceLocations) || views->ViewInstanceCount > MAX_ITEMS)
                return FALSE;
            FIELD(hash, views->ViewInstanceCount);
            if (views->ViewInstanceCount)
                XboxSha256Update(hash, views->pViewInstanceLocations,
                    views->ViewInstanceCount * sizeof(D3D12_VIEW_INSTANCE_LOCATION));
            FIELD(hash, views->Flags);
            break;
        }
        default:
            /* Scalars and padding-free structures. */
            XboxSha256Update(hash, value, valueSize);
            break;
        }
        offset += (valueOffset + valueSize + 7) & ~(SIZE_T)7;
    }
    return TRUE;
}

/* The desc comes from the title. A malformed one faults here instead of in
   the driver, so it is keyed under an exception handler and then passed to
   the driver, which returns its usual error. */
static BOOL BuildKey(int kind, const void *desc, BYTE key[32], SIZE_T *cachedPso) {
    XboxSha256 hash;
    volatile BOOL keyed = FALSE; /* Read after the handler. */
    *cachedPso = NO_CACHED_PSO;
    __try {
        XboxSha256Init(&hash);
        XboxPsoKeyU32(&hash, XBOX_PSO_CACHE_KEY_VERSION);
        XboxPsoKeyU32(&hash, (UINT32)kindNames[kind]);
        keyed = kind == KIND_GRAPHICS ? HashGraphics(&hash, (const D3D12_GRAPHICS_PIPELINE_STATE_DESC *)desc) :
                kind == KIND_COMPUTE ? HashCompute(&hash, (const D3D12_COMPUTE_PIPELINE_STATE_DESC *)desc) :
                HashStream(&hash, (const D3D12_PIPELINE_STATE_STREAM_DESC *)desc, cachedPso);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        keyed = FALSE;
    }
    if (keyed) XboxSha256Final(&hash, key);
    return keyed;
}

static void RememberRoot(void *object, UINT nodeMask, const void *blob, SIZE_T size) {
    BYTE digest[32];
    __try {
        XboxSha256 hash;
        XboxSha256Init(&hash);
        XboxPsoKeyU32(&hash, nodeMask);
        XboxPsoKeyBytes(&hash, blob, size);
        XboxSha256Final(&hash, digest);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    AcquireSRWLockExclusive(&rootLock);
    if (roots) {
        const UINT first = (UINT)(((ULONG_PTR)object >> 4) % ROOT_SLOTS);
        for (UINT i = 0; i < ROOT_SLOTS; ++i) {
            RootEntry *entry = &roots[(first + i) % ROOT_SLOTS];
            if (entry->object && entry->object != object) continue;
            entry->object = object;
            CopyBytes(entry->hash, digest, 32);
            break;
        }
    }
    ReleaseSRWLockExclusive(&rootLock);
}

/* -------------------------------------------------------------- entries */

static Entry *FindEntry(const BYTE key[32], BOOL insert) {
    if (!cache.entries) return NULL;
    const UINT first = ((UINT)key[0] | (UINT)key[1] << 8 | (UINT)key[2] << 16) % ENTRY_SLOTS;
    for (UINT i = 0; i < ENTRY_SLOTS; ++i) {
        Entry *entry = &cache.entries[(first + i) % ENTRY_SLOTS];
        if (!(entry->flags & ENTRY_USED)) {
            if (!insert) return NULL;
            CopyBytes(entry->key, key, 32);
            entry->flags = ENTRY_USED;
            return entry;
        }
        if (XboxPsoCacheSameBytes(entry->key, key, 32)) return entry;
    }
    return NULL;
}

static void FreeEntries(void) {
    if (!cache.entries) return;
    for (UINT i = 0; i < ENTRY_SLOTS; ++i) {
        Entry *entry = &cache.entries[i];
        if (entry->flags & ENTRY_HEAP) HeapFree(GetProcessHeap(), 0, (void *)entry->blob);
    }
    ZeroBytes(cache.entries, ENTRY_SLOTS * sizeof(Entry));
}

/* -------------------------------------------------------------- identity */

static void HashModule(XboxSha256 *hash, const WCHAR *name) {
    const BYTE *base = (const BYTE *)GetModuleHandleW(name);
    XboxPsoKeyU32(hash, base ? 1 : 0);
    if (!base) return;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    XboxPsoKeyU32(hash, nt->FileHeader.TimeDateStamp);
    XboxPsoKeyU32(hash, nt->OptionalHeader.SizeOfImage);
    XboxPsoKeyU32(hash, nt->OptionalHeader.CheckSum);
}

/* Identity without the adapter LUID, which changes from boot to boot. */
static void ComputeIdentity(const WCHAR *prefix, BYTE identity[32], LUID *luid, BOOL *haveLuid) {
    XboxSha256 hash;
    XboxSha256Init(&hash);
    XboxPsoKeyString(&hash, "xbox-d3d12-pipeline-cache");
    XboxPsoKeyU32(&hash, XBOX_PSO_CACHE_KEY_VERSION);
    const WCHAR *leaf = prefix;
    SIZE_T length = 0;
    for (; prefix[length]; ++length)
        if (prefix[length] == L'\\' || prefix[length] == L'/') leaf = prefix + length + 1;
    XboxPsoKeyBytes(&hash, leaf, (prefix + length - leaf) * sizeof(WCHAR));

    *haveLuid = FALSE;
    typedef HRESULT (WINAPI *CreateFactory)(REFIID, void **);
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    const CreateFactory create = dxgi ? (CreateFactory)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;
    DXGI_ADAPTER_DESC1 desc;
    if (create && SUCCEEDED(create(&factory1Iid, (void **)&factory)) &&
            SUCCEEDED(factory->lpVtbl->EnumAdapters1(factory, 0, &adapter)) &&
            SUCCEEDED(adapter->lpVtbl->GetDesc1(adapter, &desc))) {
        XboxPsoKeyU32(&hash, 1);
        FIELD(&hash, desc.VendorId);
        FIELD(&hash, desc.DeviceId);
        FIELD(&hash, desc.SubSysId);
        FIELD(&hash, desc.Revision);
        FIELD(&hash, desc.Flags);
        FIELD(&hash, desc.Description);
        LARGE_INTEGER driver;
        driver.QuadPart = 0;
        const HRESULT checked = adapter->lpVtbl->CheckInterfaceSupport(adapter, &dxgiDeviceIid, &driver);
        XboxPsoKeyU32(&hash, (UINT32)checked);
        XboxPsoKeyU64(&hash, SUCCEEDED(checked) ? (UINT64)driver.QuadPart : 0);
        *luid = desc.AdapterLuid;
        *haveLuid = TRUE;
    } else {
        XboxPsoKeyU32(&hash, 0);
    }
    if (adapter) adapter->lpVtbl->Release(adapter);
    if (factory) factory->lpVtbl->Release(factory);
    if (dxgi) FreeLibrary(dxgi);

    typedef LONG (WINAPI *GetVersion)(OSVERSIONINFOW *);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const GetVersion version = ntdll ? (GetVersion)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    OSVERSIONINFOW os;
    ZeroBytes(&os, sizeof(os));
    os.dwOSVersionInfoSize = sizeof(os);
    if (version && version(&os) == 0) {
        FIELD(&hash, os.dwMajorVersion);
        FIELD(&hash, os.dwMinorVersion);
        FIELD(&hash, os.dwBuildNumber);
        FIELD(&hash, os.dwPlatformId);
    } else {
        XboxPsoKeyU32(&hash, 0);
    }
    HashModule(&hash, L"d3d12.dll");
    XboxSha256Final(&hash, identity);
}

/* ------------------------------------------------------------------ files */

static BOOL ComposePath(WCHAR *out, SIZE_T capacity, const WCHAR *first, const WCHAR *second, const WCHAR *third) {
    const WCHAR *parts[3] = {first, second, third};
    SIZE_T used = 0;
    for (int p = 0; p < 3; ++p) {
        for (const WCHAR *c = parts[p]; c && *c; ++c) {
            if (used + 1 >= capacity) return FALSE;
            out[used++] = *c;
        }
    }
    out[used] = 0;
    return TRUE;
}

static BYTE *ReadWholeFile(const WCHAR *path, ULONGLONG *size, DWORD *error) {
    *size = 0;
    HANDLE file = CreateFile2FromAppW(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return NULL;
    }
    BYTE *data = NULL;
    LARGE_INTEGER length;
    *error = ERROR_FILE_TOO_LARGE;
    if (GetFileSizeEx(file, &length) && length.QuadPart >= 0 &&
            (ULONGLONG)length.QuadPart <= XBOX_PSO_CACHE_HEADER_SIZE + XBOX_PSO_CACHE_MAX_PAYLOAD) {
        *error = ERROR_NOT_ENOUGH_MEMORY;
        data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, length.QuadPart ? (SIZE_T)length.QuadPart : 1);
        ULONGLONG done = 0;
        while (data && done < (ULONGLONG)length.QuadPart) {
            DWORD chunk = 0, want = (DWORD)((ULONGLONG)length.QuadPart - done > 0x1000000 ? 0x1000000 :
                (ULONGLONG)length.QuadPart - done);
            if (!ReadFile(file, data + done, want, &chunk, NULL) || !chunk) {
                *error = GetLastError() ? GetLastError() : ERROR_HANDLE_EOF;
                HeapFree(GetProcessHeap(), 0, data);
                data = NULL;
                break;
            }
            done += chunk;
        }
        if (data) {
            *size = done;
            *error = ERROR_SUCCESS;
        }
    }
    CloseHandle(file);
    return data;
}

static HRESULT WriteAtomically(const WCHAR *path, const BYTE *data, ULONGLONG size) {
    WCHAR temporary[520], pid[12];
    DWORD id = GetCurrentProcessId(), digits = 0;
    char decimal[12];
    char *end = Decimal(decimal, id);
    for (char *c = decimal; c < end; ++c) pid[digits++] = (WCHAR)*c;
    pid[digits] = 0;
    if (!ComposePath(temporary, ARRAYSIZE(temporary), path, L".", pid) ||
            !ComposePath(temporary, ARRAYSIZE(temporary), temporary, L".tmp", NULL))
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    HANDLE file = CreateFile2FromAppW(temporary, GENERIC_WRITE, 0, CREATE_ALWAYS, NULL);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    HRESULT result = S_OK;
    for (ULONGLONG done = 0; done < size && SUCCEEDED(result);) {
        DWORD written = 0, want = (DWORD)(size - done > 0x1000000 ? 0x1000000 : size - done);
        if (!WriteFile(file, data + done, want, &written, NULL) || written != want)
            result = HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_WRITE_FAULT);
        done += written;
    }
    if (SUCCEEDED(result) && !FlushFileBuffers(file)) result = HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(file);
    if (SUCCEEDED(result) && !MoveFileFromAppW(temporary, path)) {
        const DWORD error = GetLastError();
        if ((error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) ||
                !ReplaceFileFromAppW(path, temporary, NULL, 0, NULL, NULL))
            result = HRESULT_FROM_WIN32(error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS ?
                GetLastError() : error);
    }
    if (FAILED(result)) DeleteFileFromAppW(temporary);
    return result;
}

/* Removes this title's caches for other builds or driver identities and any
   temporary file an interrupted write left behind. */
static void RemoveStaleFiles(const WCHAR *prefix, const WCHAR *keep, const WCHAR *guard) {
    WCHAR pattern[480], stale[520];
    SIZE_T directory = 0, length = 0, app = 0;
    for (; prefix[length]; ++length)
        if (prefix[length] == L'\\') directory = length + 1;
    for (app = directory; app < length && prefix[app] != L'-'; ++app) {}
    if (!directory || app == directory || app >= ARRAYSIZE(pattern) - 3) return;
    for (SIZE_T i = 0; i < app; ++i) pattern[i] = prefix[i];
    pattern[app] = L'-';
    pattern[app + 1] = L'*';
    pattern[app + 2] = 0;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileExFromAppW(pattern, FindExInfoBasic, &found, FindExSearchNameMatch, NULL, 0);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        SIZE_T size = 0;
        while (found.cFileName[size]) ++size;
        /* Caches, interrupted writes, and guards of other game builds. */
        const BOOL ours = (size > 4 && (CompareStringOrdinal(found.cFileName + size - 4, 4, L".bin", 4, TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(found.cFileName + size - 4, 4, L".tmp", 4, TRUE) == CSTR_EQUAL)) ||
            (size > 6 && CompareStringOrdinal(found.cFileName + size - 6, 6, L".guard", 6, TRUE) == CSTR_EQUAL);
        if (!ours || (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) continue;
        pattern[directory] = 0;
        if (ComposePath(stale, ARRAYSIZE(stale), pattern, found.cFileName, NULL) &&
                CompareStringOrdinal(stale, -1, keep, -1, TRUE) != CSTR_EQUAL &&
                CompareStringOrdinal(stale, -1, guard, -1, TRUE) != CSTR_EQUAL)
            DeleteFileFromAppW(stale);
        pattern[directory] = prefix[directory];
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

/* ------------------------------------------------------------------ write */

/* At process exit a crashing thread may still hold a lock, so the exit write
   waits a bounded time instead of hanging the exit. */
static BOOL AcquireBounded(SRWLOCK *lock, BOOL bounded) {
    if (!bounded) {
        AcquireSRWLockExclusive(lock);
        return TRUE;
    }
    for (int i = 0; i < 200; ++i) {
        if (TryAcquireSRWLockExclusive(lock)) return TRUE;
        Sleep(10);
    }
    return FALSE;
}

static HRESULT WriteCache(BOOL final, const char *reason, BOOL bounded) {
    /* After a removal nothing is serialized: the guard disables the cache. */
    if (cache.stopped) return S_FALSE;
    if (!AcquireBounded(&cache.writeLock, bounded)) return HRESULT_FROM_WIN32(ERROR_BUSY);
    BYTE *buffer = NULL;
    ULONGLONG payload = 0;
    UINT32 pipelines = 0;
    LONG mode = 0, unsaved = 0;
    HRESULT result = S_FALSE;

    if (!AcquireBounded(&cache.lock, bounded)) {
        ReleaseSRWLockExclusive(&cache.writeLock);
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    mode = cache.mode;
    unsaved = cache.unsaved;
    const ULONGLONG now = GetTickCount64();
    if (XboxPsoCacheShouldWrite((uint32_t)unsaved, now - cache.lastStoreTick, now - cache.lastWriteTick, final,
            cache.slowPending)) {
        result = E_OUTOFMEMORY;
        if (cache.stopped) {
            result = S_FALSE;
        } else if (mode == XBOX_PSO_CACHE_MODE_LIBRARY && cache.library) {
            typedef SIZE_T (STDMETHODCALLTYPE *SerializedSize)(void *);
            typedef HRESULT (STDMETHODCALLTYPE *Serialize)(void *, void *, SIZE_T);
            BOOL armed = BeforeCall(XBOX_PSO_CALL_SERIALIZE);
            payload = METHOD(cache.library, 11, SerializedSize)(cache.library);
            BOOL intact = AfterCall(cache.device, XBOX_PSO_CALL_SERIALIZE, S_OK, armed);
            cache.serializedBytes = payload;
            if (!intact) {
                result = S_FALSE;
            } else if (!payload || payload > XBOX_PSO_CACHE_MAX_PAYLOAD) {
                result = payload ? HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE) : E_UNEXPECTED;
            } else if ((buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)(XBOX_PSO_CACHE_HEADER_SIZE + payload)))) {
                SetReentrant(TRUE);
                armed = BeforeCall(XBOX_PSO_CALL_SERIALIZE);
                result = METHOD(cache.library, 12, Serialize)(cache.library, buffer + XBOX_PSO_CACHE_HEADER_SIZE,
                    (SIZE_T)payload);
                if (!AfterCall(cache.device, XBOX_PSO_CALL_SERIALIZE, result, armed)) result = S_FALSE;
                SetReentrant(FALSE);
            }
            pipelines = cache.pipelines;
        } else if (mode == XBOX_PSO_CACHE_MODE_BLOBS) {
            for (UINT i = 0; i < ENTRY_SLOTS; ++i) {
                const Entry *entry = &cache.entries[i];
                if ((entry->flags & (ENTRY_BLOB | ENTRY_INVALID)) == ENTRY_BLOB) payload += XboxPsoCacheRecordSize(entry->size);
            }
            if (payload && (buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)(XBOX_PSO_CACHE_HEADER_SIZE + payload)))) {
                ULONGLONG used = XBOX_PSO_CACHE_HEADER_SIZE;
                for (UINT i = 0; i < ENTRY_SLOTS; ++i) {
                    const Entry *entry = &cache.entries[i];
                    if ((entry->flags & (ENTRY_BLOB | ENTRY_INVALID)) != ENTRY_BLOB) continue;
                    used += XboxPsoCacheWriteRecord(buffer + used, entry->key, entry->kind, entry->blob, entry->size);
                    ++pipelines;
                }
                result = S_OK;
            } else if (!payload) {
                result = E_UNEXPECTED;
            }
        } else {
            result = E_NOT_VALID_STATE;
        }
        cache.unsaved = 0;
        cache.slowPending = 0;
        cache.lastWriteTick = now;
    }
    ReleaseSRWLockExclusive(&cache.lock);

    if (result != S_FALSE) {
        if (SUCCEEDED(result)) {
            XboxPsoCacheWriteHeader(buffer, (uint32_t)mode, cache.identity, payload, pipelines);
            result = WriteAtomically(cache.path, buffer, XBOX_PSO_CACHE_HEADER_SIZE + payload);
        }
        if (SUCCEEDED(result)) {
            InterlockedIncrement(&cache.stats.writes);
        } else {
            Fallback(FALLBACK_WRITE, "write", result);
            /* Retry a few times; a persistent failure leaves the old file. */
            AcquireSRWLockExclusive(&cache.lock);
            if (++cache.writeFailures <= 3) cache.unsaved += unsaved;
            ReleaseSRWLockExclusive(&cache.lock);
        }
        char line[160];
        char *out = Text(Text(line, "asset pso-cache write at="), reason);
        out = Text(Text(out, " mode="), ModeName(mode));
        out = Decimal(Text(out, " bytes="), payload);
        out = Decimal(Text(out, " pipelines="), pipelines);
        out = Hex32(Text(out, " result="), (DWORD)result);
        Line(line, out);
        TraceStats(reason);
    }
    if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
    ReleaseSRWLockExclusive(&cache.writeLock);
    return result;
}

static DWORD WINAPI WriteInBackground(LPVOID unused) {
    (void)unused;
    for (;;) {
        WaitForSingleObject(cache.wake, 1000);
        if (!cache.ready || cache.stopped) continue;
        WriteCache(FALSE, "background", FALSE);
    }
}

/* ------------------------------------------------------------------- bind */

static HRESULT CheckSupport(void *device, LONG *flags) {
    typedef HRESULT (STDMETHODCALLTYPE *CheckFeature)(void *, D3D12_FEATURE, void *, UINT);
    D3D12_FEATURE_DATA_SHADER_CACHE support;
    support.SupportFlags = D3D12_SHADER_CACHE_SUPPORT_NONE;
    const HRESULT result = METHOD(device, 13, CheckFeature)(device, D3D12_FEATURE_SHADER_CACHE, &support, sizeof(support));
    *flags = SUCCEEDED(result) ? (LONG)support.SupportFlags : 0;
    return result;
}

static BOOL IsDriverMismatch(HRESULT result) {
    return result == D3D12_ERROR_ADAPTER_NOT_FOUND || result == D3D12_ERROR_DRIVER_VERSION_MISMATCH;
}

/* Called with the cache lock held, once, for the first device that creates a
   pipeline after the cache file was read. */
static void BindLocked(void *device) {
    const char *file = cache.fileStatus >= 0 ? XboxPsoCacheStatusName((XboxPsoCacheStatus)cache.fileStatus) : "absent";
    HRESULT result = S_OK;
    LONG mode = 0;
    BOOL intact = TRUE;
    const LONG selected = cache.selected;
    typedef LUID *(STDMETHODCALLTYPE *AdapterLuid)(void *, LUID *);
    LUID luid;
    luid.LowPart = 0;
    luid.HighPart = 0;
    const BOOL passthrough = selected != XBOX_PSO_CACHE_MODE_LIBRARY && selected != XBOX_PSO_CACHE_MODE_BLOBS;
    if (passthrough) {
        /* No call on the device at all. */
        result = S_FALSE;
        file = "passthrough";
    } else {
        BOOL armed = BeforeCall(XBOX_PSO_CALL_ADAPTER_LUID);
        METHOD(device, 43, AdapterLuid)(device, &luid);
        intact = AfterCall(device, XBOX_PSO_CALL_ADAPTER_LUID, S_OK, armed);
        if (intact) {
            armed = BeforeCall(XBOX_PSO_CALL_CHECK_SUPPORT);
            result = CheckSupport(device, &cache.supportFlags);
            intact = AfterCall(device, XBOX_PSO_CALL_CHECK_SUPPORT, result, armed);
        }
    }
    if (!intact) {
        result = (HRESULT)cache.removedReason;
        file = "device-removed";
    } else if (passthrough) {
        /* Traced below. */
    } else if (cache.haveLuid && (luid.LowPart != cache.luid.LowPart || luid.HighPart != cache.luid.HighPart)) {
        /* The identity describes the default adapter; another one is not keyed. */
        result = DXGI_ERROR_UNSUPPORTED;
        file = "other-adapter";
    } else if (selected == XBOX_PSO_CACHE_MODE_LIBRARY) {
        const BOOL libraryFile = cache.fileStatus == XBOX_PSO_CACHE_OK && cache.view.mode == XBOX_PSO_CACHE_MODE_LIBRARY;
        void *device1 = NULL, *library = NULL;
        typedef HRESULT (STDMETHODCALLTYPE *CreateLibrary)(void *, const void *, SIZE_T, REFIID, void **);
        BOOL armed = BeforeCall(XBOX_PSO_CALL_QUERY_DEVICE1);
        result = Query(device, &device1Iid, &device1);
        intact = AfterCall(device, XBOX_PSO_CALL_QUERY_DEVICE1, result, armed);
        if (intact && SUCCEEDED(result)) {
            SetReentrant(TRUE);
            /* The blob stays allocated for the library's lifetime, as the runtime requires. */
            armed = BeforeCall(XBOX_PSO_CALL_CREATE_LIBRARY);
            result = METHOD(device1, 44, CreateLibrary)(device1, libraryFile ? cache.view.payload : NULL,
                libraryFile ? (SIZE_T)cache.view.payloadSize : 0, &libraryIid, &library);
            intact = AfterCall(device, XBOX_PSO_CALL_CREATE_LIBRARY, result, armed);
            if (intact && libraryFile && FAILED(result)) {
                /* A driver or runtime update invalidates the serialized library. */
                file = result == D3D12_ERROR_DRIVER_VERSION_MISMATCH ? "driver-mismatch" :
                       result == D3D12_ERROR_ADAPTER_NOT_FOUND ? "adapter-mismatch" : "rejected";
                DeleteFileFromAppW(cache.path);
                cache.fileStatus = XBOX_PSO_CACHE_IDENTITY_MISMATCH;
                library = NULL;
                armed = BeforeCall(XBOX_PSO_CALL_CREATE_LIBRARY);
                result = METHOD(device1, 44, CreateLibrary)(device1, NULL, 0, &libraryIid, &library);
                intact = AfterCall(device, XBOX_PSO_CALL_CREATE_LIBRARY, result, armed);
                HeapFree(GetProcessHeap(), 0, cache.file);
                cache.file = NULL;
            }
            SetReentrant(FALSE);
        }
        Release(device1);
        cache.libraryResult = result;
        if (intact && SUCCEEDED(result) && library) {
            cache.library = library;
            armed = BeforeCall(XBOX_PSO_CALL_QUERY_LIBRARY1);
            const HRESULT queried = Query(library, &library1Iid, &cache.library1);
            intact = AfterCall(device, XBOX_PSO_CALL_QUERY_LIBRARY1, queried, armed);
            if (FAILED(queried)) cache.library1 = NULL;
            mode = intact ? XBOX_PSO_CACHE_MODE_LIBRARY : 0;
            cache.pipelines = libraryFile && cache.fileStatus == XBOX_PSO_CACHE_OK ? cache.view.entries : 0;
        } else {
            /* No fallback to cached blobs: that path was not selected. */
            Release(library);
            if (intact) Fallback(FALLBACK_LOAD, "create-library", result);
            else file = "device-removed";
        }
    } else if (cache.supportFlags & D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO) {
        /* Cached blobs: no further device call until the first pipeline. */
        mode = XBOX_PSO_CACHE_MODE_BLOBS;
        if (cache.fileStatus == XBOX_PSO_CACHE_OK && cache.view.mode == XBOX_PSO_CACHE_MODE_BLOBS) {
            uint64_t offset = 0;
            XboxPsoCacheRecord record;
            while (XboxPsoCacheNextRecord(cache.view.payload, cache.view.payloadSize, &offset, &record) > 0) {
                Entry *entry = FindEntry(record.key, TRUE);
                if (!entry || (entry->flags & ENTRY_BLOB)) continue;
                entry->blob = record.blob;
                entry->size = record.size;
                entry->kind = record.kind;
                entry->flags |= ENTRY_BLOB;
                cache.blobBytes += XboxPsoCacheRecordSize(record.size);
                ++cache.pipelines;
            }
        }
    } else {
        result = DXGI_ERROR_UNSUPPORTED;
        file = "no-single-pso";
    }
    cache.mode = cache.stopped ? 0 : mode;
    InterlockedExchangePointer((PVOID volatile *)&cache.device, device);

    char line[200];
    char *out = Text(Text(line, "asset pso-cache bind mode="), ModeName(cache.mode));
    out = Text(Text(out, " selected="), XboxPsoCacheModeName((uint32_t)selected));
    out = Hex32(Text(out, selected == XBOX_PSO_CACHE_MODE_LIBRARY ? " library=" : " result="), (DWORD)result);
    out = Hex32(Text(out, " support="), (DWORD)cache.supportFlags);
    out = Text(Text(out, " file="), file);
    out = Decimal(Text(out, " pipelines="), cache.pipelines);
    Line(line, out);
}

/* NULL when the cache serves this device, otherwise the bypass reason. */
static const char *Bind(void *device) {
    if (cache.stopped) return "stopped";
    void *bound = cache.device;
    if (bound == device) return cache.mode ? NULL : "disabled";
    if (bound) {
        Fallback(FALLBACK_OTHER_DEVICE, "other-device", S_FALSE);
        return "other-device";
    }
    if (!cache.ready) {
        Fallback(FALLBACK_EARLY, "before-ready", S_FALSE);
        return "early";
    }
    AcquireSRWLockExclusive(&cache.lock);
    if (!cache.device) BindLocked(device);
    bound = cache.device;
    const LONG mode = cache.mode;
    ReleaseSRWLockExclusive(&cache.lock);
    if (bound != device) return "other-device";
    return mode ? NULL : "disabled";
}

static BOOL IsRemoval(HRESULT result) {
    return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_HUNG ||
        result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

/* ------------------------------------------------------------ create path */

static HRESULT LoadFromLibrary(int kind, const WCHAR *name, const void *desc, REFIID iid, void **out) {
    typedef HRESULT (STDMETHODCALLTYPE *Load)(void *, LPCWSTR, const void *, REFIID, void **);
    if (kind == KIND_STREAM) {
        if (!cache.library1) return E_NOINTERFACE;
        return METHOD(cache.library1, 13, Load)(cache.library1, name, desc, iid, out);
    }
    return METHOD(cache.library, kind == KIND_GRAPHICS ? 9 : 10, Load)(cache.library, name, desc, iid, out);
}

/* Supplies a cached blob through the desc's CachedPSO. */
static HRESULT CreateWithBlob(int kind, void *device, const void *desc, SIZE_T cachedPso, const BYTE *blob,
        UINT32 size, REFIID iid, void **out) {
    D3D12_CACHED_PIPELINE_STATE cached;
    cached.pCachedBlob = blob;
    cached.CachedBlobSizeInBytes = size;
    if (kind == KIND_GRAPHICS) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC copy;
        CopyBytes(&copy, desc, sizeof(copy));
        copy.CachedPSO = cached;
        return originalPipelines[kind](device, &copy, iid, out);
    }
    if (kind == KIND_COMPUTE) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC copy;
        CopyBytes(&copy, desc, sizeof(copy));
        copy.CachedPSO = cached;
        return originalPipelines[kind](device, &copy, iid, out);
    }
    const D3D12_PIPELINE_STATE_STREAM_DESC *stream = (const D3D12_PIPELINE_STATE_STREAM_DESC *)desc;
    /* An appended subobject starts on the next 8-byte boundary even when the
       title's last subobject is not padded. */
    const SIZE_T append = XboxPsoCacheAppendOffset(stream->SizeInBytes);
    const SIZE_T total = cachedPso == NO_CACHED_PSO ? append + 8 + sizeof(D3D12_CACHED_PIPELINE_STATE) :
        stream->SizeInBytes;
    BYTE *copy = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
    if (!copy) return E_OUTOFMEMORY;
    CopyBytes(copy, stream->pPipelineStateSubobjectStream, stream->SizeInBytes);
    SIZE_T at = cachedPso;
    if (cachedPso == NO_CACHED_PSO) {
        *(UINT32 *)(copy + append) = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO;
        at = append + 8;
    }
    CopyBytes(copy + at, &cached, sizeof(cached));
    D3D12_PIPELINE_STATE_STREAM_DESC patched;
    patched.SizeInBytes = total;
    patched.pPipelineStateSubobjectStream = copy;
    const HRESULT result = originalPipelines[kind](device, &patched, iid, out);
    HeapFree(GetProcessHeap(), 0, copy);
    return result;
}

static void Store(int kind, void *device, const BYTE key[32], void *object, ULONGLONG createUs, const char **outcome) {
    const BOOL slow = createUs >= XBOX_PSO_CACHE_SLOW_US;
    void *pipeline = NULL;
    /* IUnknown on the title's new pipeline: an AddRef, released below. */
    HRESULT result = Query(object, &pipelineIid, &pipeline);
    if (FAILED(result) || !pipeline) {
        Fallback(FALLBACK_QUERY, "store-query", result);
        return;
    }
    if (cache.stopped) {
        /* Removed while this pipeline was created: no further cache call. */
    } else if (cache.mode == XBOX_PSO_CACHE_MODE_LIBRARY) {
        typedef HRESULT (STDMETHODCALLTYPE *StorePipeline)(void *, LPCWSTR, void *);
        WCHAR name[65];
        XboxPsoCacheHexWide(key, 32, (uint16_t *)name);
        AcquireSRWLockExclusive(&cache.lock);
        if (!cache.library || cache.stopped) {
            result = S_FALSE;
        } else if (cache.serializedBytes > XBOX_PSO_CACHE_MAX_PAYLOAD || cache.pipelines >= ENTRY_SLOTS) {
            InterlockedIncrement(&cache.stats.skipped);
            Fallback(FALLBACK_FULL, "full", S_FALSE);
            result = S_FALSE;
        } else {
            SetReentrant(TRUE);
            const BOOL armed = BeforeCall(XBOX_PSO_CALL_STORE);
            result = METHOD(cache.library, 8, StorePipeline)(cache.library, name, pipeline);
            if (!AfterCall(device, XBOX_PSO_CALL_STORE, result, armed)) result = S_FALSE;
            SetReentrant(FALSE);
            if (result == S_FALSE) {
                /* The device was removed: recorded by AfterCall. */
            } else if (SUCCEEDED(result)) {
                ++cache.pipelines;
                ++cache.unsaved;
                if (slow) cache.slowPending = 1;
                cache.lastStoreTick = GetTickCount64();
                InterlockedIncrement(&cache.stats.stores);
                *outcome = "store";
            } else if (result == E_INVALIDARG) {
                /* Already in the library under this name: stored by a racing
                   creation or loaded from a file whose desc no longer matched. */
                InterlockedIncrement(&cache.stats.duplicates);
            } else {
                Fallback(FALLBACK_STORE, "store", result);
            }
        }
        ReleaseSRWLockExclusive(&cache.lock);
    } else if (cache.mode == XBOX_PSO_CACHE_MODE_BLOBS) {
        typedef HRESULT (STDMETHODCALLTYPE *GetCachedBlob)(void *, ID3DBlob **);
        typedef void *(STDMETHODCALLTYPE *BlobPointer)(void *);
        typedef SIZE_T (STDMETHODCALLTYPE *BlobSize)(void *);
        ID3DBlob *blob = NULL;
        const BOOL armed = BeforeCall(XBOX_PSO_CALL_CACHED_BLOB);
        result = METHOD(pipeline, 8, GetCachedBlob)(pipeline, &blob);
        if (!AfterCall(device, XBOX_PSO_CALL_CACHED_BLOB, result, armed)) {
            Release(blob);
        } else if (SUCCEEDED(result) && blob) {
            const SIZE_T size = METHOD(blob, 4, BlobSize)(blob);
            const void *bytes = METHOD(blob, 3, BlobPointer)(blob);
            BYTE *copy = size && size <= XBOX_PSO_CACHE_MAX_RECORD && bytes ?
                (BYTE *)HeapAlloc(GetProcessHeap(), 0, size) : NULL;
            if (copy) CopyBytes(copy, bytes, size);
            Release(blob);
            AcquireSRWLockExclusive(&cache.lock);
            Entry *entry = FindEntry(key, FALSE);
            if (copy && entry && !(entry->flags & ENTRY_BLOB) &&
                    cache.blobBytes + XboxPsoCacheRecordSize((UINT32)size) <= XBOX_PSO_CACHE_MAX_PAYLOAD) {
                entry->blob = copy;
                entry->size = (UINT32)size;
                entry->kind = (UINT32)kind;
                entry->flags |= ENTRY_BLOB | ENTRY_HEAP;
                entry->flags &= ~ENTRY_INVALID;
                cache.blobBytes += XboxPsoCacheRecordSize((UINT32)size);
                ++cache.pipelines;
                ++cache.unsaved;
                if (slow) cache.slowPending = 1;
                cache.lastStoreTick = GetTickCount64();
                InterlockedIncrement(&cache.stats.stores);
                *outcome = "store";
                copy = NULL;
            } else if (copy) {
                InterlockedIncrement(&cache.stats.skipped);
            }
            ReleaseSRWLockExclusive(&cache.lock);
            if (copy) HeapFree(GetProcessHeap(), 0, copy);
        } else {
            Fallback(FALLBACK_BLOB, "cached-blob", result);
        }
    }
    Release(pipeline);
    if ((cache.slowPending || cache.unsaved >= (LONG)XBOX_PSO_CACHE_BURST_COUNT) && cache.wake) SetEvent(cache.wake);
}

static HRESULT CachedPipeline(int kind, void *device, const void *desc, REFIID iid, void **out,
        const char **outcome, DWORD *error) {
    BYTE key[32];
    SIZE_T cachedPso;
    if (!BuildKey(kind, desc, key, &cachedPso)) {
        InterlockedIncrement(&cache.stats.unkeyed);
        *outcome = "unkeyed";
        const HRESULT result = originalPipelines[kind](device, desc, iid, out);
        *error = GetLastError();
        return result;
    }
    const char *bypass = Bind(device);
    Entry *entry = NULL;
    HRESULT result = E_FAIL;
    if (!bypass) {
        AcquireSRWLockExclusive(&cache.lock);
        entry = cache.mode ? FindEntry(key, TRUE) : NULL;
        if (!entry) {
            bypass = cache.mode ? "full" : "disabled";
        } else if (entry->flags & ENTRY_SEEN) {
            /* Created before in this process, or in flight on another thread. */
            bypass = "repeat";
        } else {
            entry->flags |= ENTRY_SEEN;
        }
        if (!bypass && cache.mode == XBOX_PSO_CACHE_MODE_LIBRARY && (kind != KIND_STREAM || cache.library1)) {
            WCHAR name[65];
            XboxPsoCacheHexWide(key, 32, (uint16_t *)name);
            const LONGLONG start = Now();
            SetReentrant(TRUE);
            const BOOL armed = BeforeCall(XBOX_PSO_CALL_LOAD);
            result = LoadFromLibrary(kind, name, desc, iid, out);
            const BOOL intact = AfterCall(device, XBOX_PSO_CALL_LOAD, result, armed);
            SetReentrant(FALSE);
            const LONG64 loadUs = (LONG64)Microseconds(Now() - start);
            InterlockedAdd64(&cache.stats.loadUs, loadUs);
            ReleaseSRWLockExclusive(&cache.lock);
            if (!intact) {
                /* Removed: the title's own call below reports it. */
                if (SUCCEEDED(result) && out && *out) Release(*out);
                if (out) *out = NULL;
                bypass = "stopped";
            } else if (SUCCEEDED(result)) {
                InterlockedAdd64(&cache.stats.hitUs, loadUs);
                InterlockedIncrement(&cache.stats.hits);
                *outcome = "hit";
                return result;
            } else {
                if (out) *out = NULL;
                /* E_INVALIDARG: the name is not in the library. */
                if (result != E_INVALIDARG) Fallback(FALLBACK_LOAD, "load", result);
            }
        } else if (!bypass && cache.mode == XBOX_PSO_CACHE_MODE_LIBRARY) {
            ReleaseSRWLockExclusive(&cache.lock);
            Fallback(FALLBACK_STREAM_LIBRARY, "stream-library", E_NOINTERFACE);
            bypass = "stream";
        } else if (!bypass && (entry->flags & (ENTRY_BLOB | ENTRY_INVALID)) == ENTRY_BLOB && entry->kind == (UINT32)kind) {
            const BYTE *blob = entry->blob;
            const UINT32 size = entry->size;
            ReleaseSRWLockExclusive(&cache.lock);
            const LONGLONG start = Now();
            const BOOL armed = BeforeCall(XBOX_PSO_CALL_CACHED_PSO);
            result = CreateWithBlob(kind, device, desc, cachedPso, blob, size, iid, out);
            *error = GetLastError();
            const BOOL intact = AfterCall(device, XBOX_PSO_CALL_CACHED_PSO, result, armed);
            const LONG64 loadUs = (LONG64)Microseconds(Now() - start);
            InterlockedAdd64(&cache.stats.loadUs, loadUs);
            if (intact && SUCCEEDED(result)) {
                InterlockedAdd64(&cache.stats.hitUs, loadUs);
                InterlockedIncrement(&cache.stats.hits);
                *outcome = "hit";
                return result;
            }
            if (SUCCEEDED(result) && out && *out) Release(*out);
            if (out) *out = NULL;
            if (!intact) bypass = "stopped";
            else Fallback(FALLBACK_CACHED_BLOB, "cached-pso", result);
            AcquireSRWLockExclusive(&cache.lock);
            if (IsDriverMismatch(result)) {
                /* Every blob from this driver is stale: drop them all. */
                for (UINT i = 0; i < ENTRY_SLOTS; ++i) cache.entries[i].flags |= ENTRY_INVALID;
                DeleteFileFromAppW(cache.path);
            }
            entry->flags |= ENTRY_INVALID;
            entry->flags &= ~ENTRY_BLOB;
            ReleaseSRWLockExclusive(&cache.lock);
        } else {
            ReleaseSRWLockExclusive(&cache.lock);
        }
    }
    if (bypass) {
        InterlockedIncrement(&cache.stats.bypassed);
        *outcome = bypass;
        result = originalPipelines[kind](device, desc, iid, out);
        *error = GetLastError();
        return result;
    }

    const LONGLONG start = Now();
    result = originalPipelines[kind](device, desc, iid, out);
    *error = GetLastError();
    const ULONGLONG createUs = Microseconds(Now() - start);
    InterlockedAdd64(&cache.stats.missUs, (LONG64)createUs);
    InterlockedIncrement(&cache.stats.misses);
    *outcome = "miss";
    if (SUCCEEDED(result) && out && *out) Store(kind, device, key, *out, createUs, outcome);
    return result;
}

static void TraceTiming(int kind, LONGLONG ticks, HRESULT result, const char *outcome) {
    const ULONGLONG micros = Microseconds(ticks);
    if (!traceTiming || micros < 2000 || InterlockedIncrement(&timingEvents) > 2000) return;
    SYSTEMTIME now;
    GetSystemTime(&now);
    char line[128];
    char *out = Text(line, "pso kind=");
    *out++ = kindNames[kind];
    out = Text(out, " utc=");
    out = Digits(out, now.wHour, 2);
    *out++ = ':';
    out = Digits(out, now.wMinute, 2);
    *out++ = ':';
    out = Digits(out, now.wSecond, 2);
    *out++ = '.';
    out = Digits(out, now.wMilliseconds, 3);
    out = Decimal(Text(out, " us="), micros);
    out = Hex32(Text(out, " result="), (DWORD)result);
    if (outcome) out = Text(Text(out, " cache="), outcome);
    Line(line, out);
}

static HRESULT HookPipeline(int kind, void *device, const void *desc, REFIID iid, void **out) {
    const LONGLONG start = Now();
    const char *outcome = NULL;
    /* A library hit leaves the caller's last error as it was. */
    DWORD error = GetLastError();
    HRESULT result;
    if (cache.configured && desc && out && !Reentrant()) {
        result = CachedPipeline(kind, device, desc, iid, out, &outcome, &error);
    } else {
        result = originalPipelines[kind](device, desc, iid, out);
        error = GetLastError();
        if (cache.passthrough) {
            InterlockedAdd64(&cache.stats.missUs, (LONG64)Microseconds(Now() - start));
            InterlockedIncrement(&cache.stats.misses);
        }
    }
    TraceTiming(kind, Now() - start, result, outcome);
    /* The title's own creation reports a removal: record it and stop the
       cache. Passthrough records only the title's HRESULT and makes no call
       on the device. A cache mode adds the device's removal reason, the last
       cache call and a DRED summary. A creation inside a cache call (a
       library load) is attributed by that call's own check instead. */
    if (IsRemoval(result) && !cache.stopped && !Reentrant()) {
        if (cache.passthrough) PassthroughRemoved(result);
        else if (cache.configured && device == cache.device)
            DeviceRemoved(device, XBOX_PSO_CALL_GAME_CREATE, result, RemovedReason(device));
    }
    SetLastError(error);
    return result;
}

static HRESULT STDMETHODCALLTYPE HookGraphics(void *device, const void *desc, REFIID iid, void **out) {
    return HookPipeline(KIND_GRAPHICS, device, desc, iid, out);
}

static HRESULT STDMETHODCALLTYPE HookCompute(void *device, const void *desc, REFIID iid, void **out) {
    return HookPipeline(KIND_COMPUTE, device, desc, iid, out);
}

static HRESULT STDMETHODCALLTYPE HookStream(void *device, const void *desc, REFIID iid, void **out) {
    return HookPipeline(KIND_STREAM, device, desc, iid, out);
}

static HRESULT STDMETHODCALLTYPE HookRootSignature(void *device, UINT nodeMask, const void *blob, SIZE_T size,
        REFIID iid, void **out) {
    const HRESULT result = originalRootSignature(device, nodeMask, blob, size, iid, out);
    const DWORD error = GetLastError();
    /* Timing-only runs do not hash root signatures. */
    if (keyRootSignatures && SUCCEEDED(result) && out && *out && blob && size) RememberRoot(*out, nodeMask, blob, size);
    SetLastError(error);
    return result;
}

/* ------------------------------------------------------------- public API */

HRESULT XboxPipelineHooksInstall(void *device, BOOL keyRoots) {
    if (!device) return E_POINTER;
    if (keyRoots) InterlockedExchange(&keyRootSignatures, 1);
    if (reentrySlot == TLS_OUT_OF_INDEXES) {
        const DWORD slot = TlsAlloc();
        if (slot == TLS_OUT_OF_INDEXES) return HRESULT_FROM_WIN32(GetLastError());
        if (InterlockedCompareExchange((volatile LONG *)&reentrySlot, (LONG)slot, (LONG)TLS_OUT_OF_INDEXES) !=
                (LONG)TLS_OUT_OF_INDEXES)
            TlsFree(slot);
    }
    if (!roots) {
        RootEntry *table = (RootEntry *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ROOT_SLOTS * sizeof(RootEntry));
        if (!table) return E_OUTOFMEMORY;
        AcquireSRWLockExclusive(&rootLock);
        if (!roots) {
            roots = table;
            table = NULL;
        }
        ReleaseSRWLockExclusive(&rootLock);
        if (table) HeapFree(GetProcessHeap(), 0, table);
    }
    void **methods = *(void ***)device;
    static const int slots[4] = {10, 11, 47, 16};
    void *const hooks[4] = {(void *)HookGraphics, (void *)HookCompute, (void *)HookStream, (void *)HookRootSignature};
    void **originals[4] = {(void **)&originalPipelines[0], (void **)&originalPipelines[1],
        (void **)&originalPipelines[2], (void **)&originalRootSignature};
    /* Slot 16 only when keys are built: timing alone patches 10, 11 and 47. */
    const int count = keyRoots ? 4 : 3;
    ULONG previous = 0;
    if (!VirtualProtectFromApp(&methods[10], 38 * sizeof(void *), PAGE_READWRITE, &previous))
        return HRESULT_FROM_WIN32(GetLastError());
    for (int i = 0; i < count; ++i) {
        if (methods[slots[i]] == hooks[i]) continue;
        *originals[i] = methods[slots[i]];
        InterlockedExchangePointer(&methods[slots[i]], hooks[i]);
    }
    VirtualProtectFromApp(&methods[10], 38 * sizeof(void *), previous, &previous);
    return S_OK;
}

/* The call named by a guard file, or 0; *same says whether it was written
   for this identity (identity NULL: any). */
static LONG ReadGuard(const WCHAR *path, const BYTE *identity, int *same) {
    *same = 0;
    const DWORD previous = GetLastError();
    HANDLE file = CreateFile2FromAppW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, OPEN_EXISTING, NULL);
    SetLastError(previous);
    if (file == INVALID_HANDLE_VALUE) return 0;
    char text[XBOX_PSO_CACHE_GUARD_MAX];
    DWORD read = 0;
    if (!ReadFile(file, text, sizeof(text), &read, NULL)) read = 0;
    CloseHandle(file);
    static const BYTE none[32] = {0};
    const LONG call = (LONG)XboxPsoCacheParseGuard(text, read, identity ? identity : none, same);
    /* An unreadable or malformed guard still means a process died inside a call. */
    return call ? call : XBOX_PSO_CALL_UNKNOWN;
}

LONG XboxPipelineCacheTakeGuard(const WCHAR *prefix) {
    WCHAR path[480];
    int same = 0;
    if (!prefix || !ComposePath(path, ARRAYSIZE(path), prefix, L".guard", NULL)) return 0;
    const LONG call = ReadGuard(path, NULL, &same);
    if (call) DeleteFileFromAppW(path);
    return call;
}

HRESULT XboxPipelineCacheOpen(const WCHAR *prefix) {
    if (!prefix || !*prefix) return E_INVALIDARG;
    AcquireSRWLockExclusive(&cache.writeLock);
    AcquireSRWLockExclusive(&cache.lock);
    HRESULT result = S_OK;
    if (!cache.entries)
        cache.entries = (Entry *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ENTRY_SLOTS * sizeof(Entry));
    if (!cache.entries) result = E_OUTOFMEMORY;
    else if (!ComposePath(cache.prefix, ARRAYSIZE(cache.prefix), prefix, NULL, NULL) ||
             !ComposePath(cache.guardPath, ARRAYSIZE(cache.guardPath), prefix, L".guard", NULL))
        result = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    char hex[17];
    hex[0] = 0;
    if (SUCCEEDED(result)) {
        ComputeIdentity(cache.prefix, cache.identity, &cache.luid, &cache.haveLuid);
        XboxPsoCacheHex(cache.identity, 8, hex);
        WCHAR suffix[24];
        suffix[0] = L'-';
        for (int i = 0; i < 16; ++i) suffix[i + 1] = (WCHAR)hex[i];
        suffix[17] = 0;
        if (!ComposePath(cache.path, ARRAYSIZE(cache.path), cache.prefix, suffix, L".bin"))
            result = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    }
    /* A guard from an earlier launch: a process died inside a cache call or
       the device was removed. The same identity stays in passthrough; a new
       driver, OS or d3d12.dll gets a fresh start. */
    cache.guardCall = 0;
    if (SUCCEEDED(result)) {
        int same = 0;
        const LONG call = ReadGuard(cache.guardPath, cache.identity, &same);
        if (call && same) cache.guardCall = call;
        else if (call) DeleteFileFromAppW(cache.guardPath);
    }
    const BOOL usable = (cache.selected == XBOX_PSO_CACHE_MODE_LIBRARY || cache.selected == XBOX_PSO_CACHE_MODE_BLOBS) &&
        !cache.guardCall;
    ULONGLONG size = 0;
    DWORD error = ERROR_SUCCESS;
    cache.fileStatus = -1;
    if (SUCCEEDED(result) && !usable) {
        /* Passthrough: the hooks only time creation. */
        cache.configured = 0;
        cache.passthrough = 1;
        error = ERROR_CANCELLED;
    } else if (SUCCEEDED(result)) {
        RemoveStaleFiles(cache.prefix, cache.path, cache.guardPath);
        cache.file = ReadWholeFile(cache.path, &size, &error);
        if (cache.file) {
            cache.fileStatus = XboxPsoCacheValidate(cache.file, size, cache.identity, &cache.view);
            if (cache.fileStatus != XBOX_PSO_CACHE_OK) {
                HeapFree(GetProcessHeap(), 0, cache.file);
                cache.file = NULL;
                DeleteFileFromAppW(cache.path);
            }
        }
        ZeroBytes((void *)&cache.stats, sizeof(cache.stats));
        cache.lastWriteTick = cache.lastStoreTick = GetTickCount64();
        if (!cache.wake) cache.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        static volatile LONG writerStarted;
        if (cache.wake && !InterlockedExchange(&writerStarted, 1)) {
            HANDLE thread = CreateThread(NULL, 0, WriteInBackground, NULL, 0, NULL);
            if (thread) CloseHandle(thread);
        }
        cache.configured = 1;
        cache.ready = 1;
    }
    ReleaseSRWLockExclusive(&cache.lock);
    ReleaseSRWLockExclusive(&cache.writeLock);

    char line[240];
    char *out = Text(line, "asset pso-cache open file=");
    out = Text(out, cache.fileStatus >= 0 ? XboxPsoCacheStatusName((XboxPsoCacheStatus)cache.fileStatus) :
        error == ERROR_CANCELLED ? "unused" : error == ERROR_FILE_NOT_FOUND ? "absent" : "unreadable");
    if (cache.fileStatus < 0 && error != ERROR_FILE_NOT_FOUND && error != ERROR_SUCCESS && error != ERROR_CANCELLED)
        out = Hex32(Text(out, " error="), error);
    out = Text(Text(out, " mode="), usable ? XboxPsoCacheModeName((uint32_t)cache.selected) : "passthrough");
    out = Text(Text(out, " selected="), XboxPsoCacheModeName((uint32_t)cache.selected));
    if (cache.guardCall) out = Text(Text(out, " guard="), XboxPsoCacheCallName((uint32_t)cache.guardCall));
    out = Decimal(Text(out, " bytes="), size);
    if (cache.fileStatus == XBOX_PSO_CACHE_OK) out = Decimal(Text(out, " pipelines="), cache.view.entries);
    if (SUCCEEDED(result)) out = Text(Text(out, " identity="), hex);
    out = Hex32(Text(out, " result="), (DWORD)result);
    Line(line, out);
    return result;
}

HRESULT XboxPipelineCacheWriteNow(void) {
    return cache.ready ? WriteCache(TRUE, "request", FALSE) : E_NOT_VALID_STATE;
}

void XboxPipelineCacheClose(BOOL removeFile) {
    AcquireSRWLockExclusive(&cache.writeLock);
    AcquireSRWLockExclusive(&cache.lock);
    cache.ready = 0;
    Release(cache.library1);
    Release(cache.library);
    cache.library1 = cache.library = NULL;
    InterlockedExchangePointer((PVOID volatile *)&cache.device, NULL);
    cache.mode = 0;
    FreeEntries();
    if (cache.file) HeapFree(GetProcessHeap(), 0, cache.file);
    cache.file = NULL;
    cache.fileStatus = -1;
    cache.pipelines = 0;
    cache.blobBytes = 0;
    cache.serializedBytes = 0;
    cache.unsaved = 0;
    cache.writeFailures = 0;
    cache.slowPending = 0;
    cache.configured = 0;
    cache.passthrough = 0;
    cache.guardCall = 0;
    InterlockedExchange(&cache.fallbackMask, 0);
    InterlockedExchange(&cache.stopped, 0);
    InterlockedExchange(&cache.proven, 0);
    InterlockedExchange(&cache.failedCalls, 0);
    InterlockedExchange(&cache.lastCall, 0);
    InterlockedExchange(&cache.removedCall, 0);
    InterlockedExchange(&cache.removedReason, 0);
    AcquireSRWLockExclusive(&cache.guardLock);
    /* A guard on disk stays for the next Open or XboxPipelineCacheTakeGuard. */
    cache.guardArmed = 0;
    cache.guardSticky = 0;
    ReleaseSRWLockExclusive(&cache.guardLock);
    if (removeFile && cache.path[0]) DeleteFileFromAppW(cache.path);
    ReleaseSRWLockExclusive(&cache.lock);
    ReleaseSRWLockExclusive(&cache.writeLock);
}

void XboxPipelineCacheSelectMode(LONG mode) {
    AcquireSRWLockExclusive(&cache.lock);
    cache.selected = mode == XBOX_PSO_CACHE_MODE_LIBRARY || mode == XBOX_PSO_CACHE_MODE_BLOBS ? mode : 0;
    ReleaseSRWLockExclusive(&cache.lock);
}

void XboxPipelineCacheGetCounters(XboxPipelineCacheCounters *counters) {
    AcquireSRWLockShared(&cache.lock);
    counters->mode = cache.mode;
    counters->fileStatus = cache.fileStatus;
    counters->supportFlags = cache.supportFlags;
    counters->hits = cache.stats.hits;
    counters->misses = cache.stats.misses;
    counters->stores = cache.stats.stores;
    counters->duplicates = cache.stats.duplicates;
    counters->bypassed = cache.stats.bypassed + cache.stats.unkeyed;
    counters->writes = cache.stats.writes;
    counters->libraryResult = cache.libraryResult;
    counters->selected = cache.selected;
    counters->lastCall = cache.lastCall;
    counters->removedCall = cache.removedCall;
    counters->removedReason = (HRESULT)cache.removedReason;
    counters->guardCall = cache.guardCall;
    ReleaseSRWLockShared(&cache.lock);
}

void WINAPI XboxD3D12PipelineCacheFlush(void) {
    if (cache.passthrough && !cache.stopped && !InterlockedExchange(&cache.exited, 1)) {
        const DWORD error = GetLastError();
        TraceStats("exit");
        SetLastError(error);
        return;
    }
    if (!cache.ready || InterlockedExchange(&cache.exited, 1)) return;
    const DWORD error = GetLastError();
    const HRESULT result = WriteCache(TRUE, "exit", TRUE);
    if (result == S_FALSE) TraceStats("exit");
    else if (result == HRESULT_FROM_WIN32(ERROR_BUSY)) {
        char line[64];
        Line(line, Hex32(Text(line, "asset pso-cache write at=exit result="), (DWORD)result));
    }
    SetLastError(error);
}

static DWORD WINAPI StartHooks(LPVOID prefix) {
    typedef HRESULT (WINAPI *CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    const BOOL keyed = prefix && cache.selected;
    HMODULE d3d12 = NULL;
    /* Wait for the title's own runtime load, up to 60 s. A cache mode polls
       every 2 ms to keep the window before the first root signature short;
       timing alone polls every 50 ms, as the measured timing build did. */
    const DWORD interval = keyed ? 2 : 50;
    for (DWORD waited = 0; waited < 60000 && !d3d12; waited += interval) {
        d3d12 = GetModuleHandleW(L"d3d12.dll");
        if (!d3d12) Sleep(interval);
    }
    const CreateDevice create = d3d12 ? (CreateDevice)(void *)GetProcAddress(d3d12, "D3D12CreateDevice") : NULL;
    void *device = NULL;
    HRESULT result = create ? create(NULL, D3D_FEATURE_LEVEL_11_0, &deviceIid, &device) :
        HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    if (SUCCEEDED(result) && device) result = XboxPipelineHooksInstall(device, keyed);
    Release(device);
    char line[120];
    Line(line, Hex32(Text(line, "pso timing result="), (DWORD)result));
    if (prefix) {
        char *out = Hex32(Text(line, "asset pso-cache hooks result="), (DWORD)result);
        out = Text(out, keyed ? " slots=10-11-47-16" : " slots=10-11-47");
        out = Text(Text(out, " selected="), XboxPsoCacheModeName((uint32_t)cache.selected));
        Line(line, out);
        if (SUCCEEDED(result) && keyed) XboxPipelineCacheOpen((const WCHAR *)prefix);
        else if (FAILED(result)) cache.configured = 0;
        HeapFree(GetProcessHeap(), 0, prefix);
    }
    return 0;
}

/* XBOX_WIN32_PSO_CACHE_MODE from the profile: library, blobs or (anything
   else) passthrough. */
static LONG SelectedMode(void) {
    WCHAR value[16];
    const DWORD length = GetEnvironmentVariableW(L"XBOX_WIN32_PSO_CACHE_MODE", value, ARRAYSIZE(value));
    if (!length || length >= ARRAYSIZE(value)) return 0;
    if (CompareStringOrdinal(value, (int)length, L"library", -1, FALSE) == CSTR_EQUAL)
        return XBOX_PSO_CACHE_MODE_LIBRARY;
    if (CompareStringOrdinal(value, (int)length, L"blobs", -1, FALSE) == CSTR_EQUAL) return XBOX_PSO_CACHE_MODE_BLOBS;
    return 0;
}

void XboxPipelineHooksStart(void) {
    static volatile LONG started;
    WCHAR enabled[2];
    const BOOL timing = GetEnvironmentVariableW(L"XBOX_WIN32_PSO_TRACE", enabled, ARRAYSIZE(enabled)) == 1 &&
        enabled[0] == L'1';
    const DWORD length = GetEnvironmentVariableW(L"XBOX_WIN32_PSO_CACHE", NULL, 0);
    if ((!timing && length <= 1) || InterlockedExchange(&started, 1)) return;
    WCHAR *prefix = NULL;
    if (length > 1 && length < ARRAYSIZE(cache.prefix)) {
        prefix = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, length * sizeof(WCHAR));
        if (prefix && GetEnvironmentVariableW(L"XBOX_WIN32_PSO_CACHE", prefix, length) != length - 1) {
            HeapFree(GetProcessHeap(), 0, prefix);
            prefix = NULL;
        }
    }
    traceTiming = 1;
    cache.selected = prefix ? SelectedMode() : 0;
    /* Creations before the file is read are counted as early bypasses. */
    if (prefix && cache.selected) cache.configured = 1;
    else if (prefix) cache.passthrough = 1;
    HANDLE thread = CreateThread(NULL, 0, StartHooks, prefix, 0, NULL);
    if (thread) CloseHandle(thread);
    else if (prefix) HeapFree(GetProcessHeap(), 0, prefix);
}
