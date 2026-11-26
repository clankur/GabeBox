/* Console control for the persistent Direct3D 12 pipeline cache.

   Links the same d3d12-pipeline-cache.c that the loader-trace adapter uses and
   checks, path by path, whether the console's D3D12 runtime can persist
   pipelines across processes without losing the device. The top process
   makes no D3D12 call. For each path, the pipeline library and then the
   GetCachedBlob/CachedPSO path, it starts a round process, so a device that
   one path removes (or a process the runtime ends) cannot affect the other.
   The round creates its device as the pinned Mesa does (an ID3D12DeviceFactory
   from D3D12GetInterface with ALLOW_RETURNING_EXISTING_DEVICE and
   ALLOW_RETURNING_INCOMPATIBLE_EXISTING_DEVICE, feature level 1_0_GENERIC and
   then 11_0, ID3D12Device3; D3D12CreateDevice without a factory), installs
   the shared hooks, selects the path as a profile would, creates a root
   signature and a trivial compute pipeline (a miss that the cache stores
   through the exact calls the title path makes: bind, store, serialize),
   writes the cache to TempState and starts a reload process. The reload
   process loads the file and creates the same pipeline again, which must be
   a hit. The cache checks GetDeviceRemovedReason after each of its calls.

   Exit code 0x5CCXLBFF: L and B are the library and cached-blob outcomes, FF
   is D3D12_FEATURE_DATA_SHADER_CACHE.SupportFlags and X is the XboxPsoCacheCall
   that preceded the first device removal (0 none; 4 CreatePipelineLibrary,
   5 StorePipeline, 6 Load, 7 Serialize, 8 GetCachedBlob, 9 CachedPSO creation,
   14 the control's own creation, 15 unknown). Outcomes:
     1 a pipeline was stored, serialized, reloaded in a second process and hit
     2 CreatePipelineLibrary returned DXGI_ERROR_UNSUPPORTED
     3 CreatePipelineLibrary returned E_NOTIMPL
     4 the device has no ID3D12Device1
     5 CreatePipelineLibrary failed otherwise
     6 the device does not report single-PSO cache support
     7 the device was removed (DXGI_ERROR_DEVICE_REMOVED), 8 hung, 9 reset,
       10 DXGI_ERROR_DRIVER_INTERNAL_ERROR
     11 a round or reload process exited with another code (X from its guard)
   Any other code is a failure 0x5CSSMRRR: stage SS, path M (1 library,
   2 blobs, 0 setup) and the low 12 bits RRR of the HRESULT or detail.
   Stages: 01 load d3d12.dll, 02 create the device, 03 install hooks,
   04 D3D12SerializeRootSignature, 05 CreateRootSignature, 06 open the cache,
   07 create the pipeline, 08 counters after the miss (RRR = misses << 4 |
   stores), 09 write, 0A start a process, 0B a process did not finish,
   0C the reload process failed (RRR = its stage << 4 | its low nibble).
   A round process exits 0x5EXMO0FF. The reload process exits 0x5DC0M00H on
   success (H hits), 0x5D0DMXO0 after a removal, or 0x5DSSMRRR with stage 06
   reporting the cache file status + 1 and 08 RRR = hits << 4 | misses.

   Built with -DXBOX_PSO_REPLAY_CONTROL (build.sh --d3d12-pipeline-replay),
   the top process runs the record-and-replay rounds instead; see
   ReplayControl below for its 0x5AXODRMM code. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include "../../runtime/win32/d3d12-pipeline-cache.h"
#include "../../runtime/win32/pipeline-cache-format.h"
#include "pipeline-cache-shader.h"

DECLSPEC_IMPORT DECLSPEC_NORETURN void WINAPI ExitProcess(UINT code);

#define METHOD(object, slot, type) ((type)((*(void ***)(object))[slot]))
typedef ULONG (STDMETHODCALLTYPE *ReleaseMethod)(void *);
static const GUID device3Iid = {0x81DADC15, 0x2BAD, 0x4392, {0x93, 0xC5, 0x10, 0x13, 0x45, 0xC4, 0xAA, 0x98}};
static const GUID deviceFactoryClass = {0x114863BF, 0xC386, 0x4AEE, {0xB3, 0x9D, 0x8F, 0x0B, 0xBB, 0x06, 0x29, 0x55}};
static const GUID deviceFactoryIid = {0x61F307D3, 0xD34E, 0x4E7C, {0x83, 0x74, 0x3B, 0xA4, 0xDE, 0x23, 0xCC, 0xCB}};
static const GUID rootIid = {0xC54A6B66, 0x72DF, 0x4EE8, {0x8B, 0xE5, 0xA9, 0x46, 0xA1, 0x42, 0x92, 0x14}};
static const GUID pipelineIid = {0x765A30F3, 0xF624, 0x4C6F, {0xA8, 0x28, 0xAC, 0xE9, 0x48, 0x62, 0x24, 0x45}};

#define FEATURE_LEVEL_1_0_GENERIC ((D3D_FEATURE_LEVEL)0x100)

static void Release(void *object) {
    if (object) METHOD(object, 2, ReleaseMethod)(object);
}

static UINT Code(UINT base, UINT stage, UINT mode, UINT detail) {
    return base | (stage & 0xFF) << 16 | (mode & 0xF) << 12 | (detail & 0xFFF);
}

static UINT RoundCode(UINT call, UINT mode, UINT outcome, UINT support) {
    return 0x5E000000u | (call & 15) << 20 | (mode & 15) << 16 | (outcome & 15) << 12 | (support & 0xFF);
}

typedef struct Setup {
    HMODULE d3d12;
    void *device;
    void *root;
} Setup;

/* Mesa's create_device: a device factory when the runtime provides one,
   otherwise D3D12CreateDevice; 1_0_GENERIC first, then 11_0; ID3D12Device3. */
static HRESULT CreateDeviceLikeMesa(HMODULE d3d12, void **device) {
    typedef HRESULT (WINAPI *GetInterface)(REFCLSID, REFIID, void **);
    typedef HRESULT (WINAPI *CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    typedef HRESULT (STDMETHODCALLTYPE *SetFlags)(void *, UINT);
    typedef HRESULT (STDMETHODCALLTYPE *FactoryCreate)(void *, IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    *device = NULL;
    const GetInterface get = (GetInterface)(void *)GetProcAddress(d3d12, "D3D12GetInterface");
    void *factory = NULL;
    if (get && SUCCEEDED(get(&deviceFactoryClass, &deviceFactoryIid, &factory)) && factory) {
        METHOD(factory, 5, SetFlags)(factory, 0x1 | 0x2);
        HRESULT result = METHOD(factory, 9, FactoryCreate)(factory, NULL, FEATURE_LEVEL_1_0_GENERIC, &device3Iid, device);
        if (FAILED(result))
            result = METHOD(factory, 9, FactoryCreate)(factory, NULL, D3D_FEATURE_LEVEL_11_0, &device3Iid, device);
        Release(factory);
        return result;
    }
    const CreateDevice create = (CreateDevice)(void *)GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!create) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    HRESULT result = create(NULL, FEATURE_LEVEL_1_0_GENERIC, &device3Iid, device);
    if (FAILED(result)) result = create(NULL, D3D_FEATURE_LEVEL_11_0, &device3Iid, device);
    return result;
}

/* Loads d3d12.dll, creates the device and installs the shared hooks.
   Returns 0, or the failing stage with *result set. */
static UINT PrepareDevice(Setup *setup, HRESULT *result) {
    setup->device = setup->root = NULL;
    setup->d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!setup->d3d12) {
        *result = HRESULT_FROM_WIN32(GetLastError());
        return 1;
    }
    if (FAILED(*result = CreateDeviceLikeMesa(setup->d3d12, &setup->device)) || !setup->device) return 2;
    if (FAILED(*result = XboxPipelineHooksInstall(setup->device, TRUE))) return 3;
    return 0;
}

/* The control's root signature, created through the device's method table
   as a title's would be. Returns 0, or the failing stage. */
static UINT CreateRoot(Setup *setup, HRESULT *result) {
    typedef HRESULT (WINAPI *SerializeRoot)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **,
        ID3DBlob **);
    typedef HRESULT (STDMETHODCALLTYPE *CreateRoot)(void *, UINT, const void *, SIZE_T, REFIID, void **);
    typedef void *(STDMETHODCALLTYPE *BlobPointer)(void *);
    typedef SIZE_T (STDMETHODCALLTYPE *BlobSize)(void *);
    const SerializeRoot serialize = (SerializeRoot)(void *)GetProcAddress(setup->d3d12, "D3D12SerializeRootSignature");
    D3D12_ROOT_SIGNATURE_DESC desc;
    desc.NumParameters = 0;
    desc.pParameters = NULL;
    desc.NumStaticSamplers = 0;
    desc.pStaticSamplers = NULL;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ID3DBlob *blob = NULL, *errors = NULL;
    *result = serialize ? serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors) :
        HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    Release(errors);
    if (FAILED(*result) || !blob) return 4;
    /* Through the device's method table, so the cache records the blob. */
    *result = METHOD(setup->device, 16, CreateRoot)(setup->device, 0, METHOD(blob, 3, BlobPointer)(blob),
        METHOD(blob, 4, BlobSize)(blob), &rootIid, &setup->root);
    Release(blob);
    return FAILED(*result) ? 5 : 0;
}

/* Returns 0, or the failing stage with *result set. */
static UINT Prepare(Setup *setup, HRESULT *result) {
    const UINT stage = PrepareDevice(setup, result);
    return stage ? stage : CreateRoot(setup, result);
}

static HRESULT CreatePipeline(const Setup *setup, void **pipeline) {
    typedef HRESULT (STDMETHODCALLTYPE *CreateCompute)(void *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **);
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
    desc.pRootSignature = (ID3D12RootSignature *)setup->root;
    desc.CS.pShaderBytecode = controlComputeShader;
    desc.CS.BytecodeLength = sizeof(controlComputeShader);
    desc.NodeMask = 0;
    desc.CachedPSO.pCachedBlob = NULL;
    desc.CachedPSO.CachedBlobSizeInBytes = 0;
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    *pipeline = NULL;
    return METHOD(setup->device, 11, CreateCompute)(setup->device, &desc, &pipelineIid, pipeline);
}

static UINT Unavailable(const XboxPipelineCacheCounters *counters, UINT mode) {
    if (mode == XBOX_PSO_CACHE_MODE_BLOBS) return 6;
    if (counters->libraryResult == DXGI_ERROR_UNSUPPORTED) return 2;
    if (counters->libraryResult == E_NOTIMPL) return 3;
    if (counters->libraryResult == E_NOINTERFACE) return 4;
    return 5;
}

static UINT Append(WCHAR *out, UINT used, UINT capacity, const WCHAR *text) {
    while (*text && used + 1 < capacity) out[used++] = *text++;
    out[used] = 0;
    return used;
}

/* Starts this image as "<verb> <mode> <prefix>" and waits for it. Returns 0
   with *exit set, or a failure stage (0A, 0B) with *exit the detail. */
static UINT RunSelf(const WCHAR *verb, UINT mode, const WCHAR *prefix, DWORD *exit) {
    static WCHAR command[1200], image[600];
    const DWORD length = GetModuleFileNameW(NULL, image, ARRAYSIZE(image));
    UINT used = 0;
    used = Append(command, used, ARRAYSIZE(command), L"\"");
    used = Append(command, used, ARRAYSIZE(command), image);
    used = Append(command, used, ARRAYSIZE(command), L"\" ");
    used = Append(command, used, ARRAYSIZE(command), verb);
    const WCHAR selected[4] = {L' ', (WCHAR)(L'0' + mode), L' ', 0};
    used = Append(command, used, ARRAYSIZE(command), selected);
    Append(command, used, ARRAYSIZE(command), prefix);
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    for (UINT i = 0; i < sizeof(startup); ++i) ((volatile BYTE *)&startup)[i] = 0;
    startup.cb = sizeof(startup);
    if (!length || length >= ARRAYSIZE(image) ||
            !CreateProcessW(image, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        *exit = GetLastError();
        return 10;
    }
    CloseHandle(process.hThread);
    const DWORD waited = WaitForSingleObject(process.hProcess, 90000);
    const BOOL finished = waited == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, exit);
    if (!finished) TerminateProcess(process.hProcess, ERROR_TIMEOUT);
    CloseHandle(process.hProcess);
    if (!finished) {
        *exit = waited;
        return 11;
    }
    return 0;
}

/* A removal the cache recorded in this process, as (call, outcome). */
static BOOL Removed(UINT *call, UINT *outcome) {
    XboxPipelineCacheCounters counters;
    XboxPipelineCacheGetCounters(&counters);
    if (!counters.removedCall) return FALSE;
    *call = (UINT)counters.removedCall;
    *outcome = XboxPsoCacheRemovalOutcome((uint32_t)counters.removedReason);
    return TRUE;
}

/* The reload process: load the file and expect a hit. */
static UINT Reload(UINT mode, const WCHAR *prefix) {
    const UINT base = 0x5D000000u;
    Setup setup;
    HRESULT result;
    UINT stage = Prepare(&setup, &result);
    if (stage) return Code(base, stage, mode, (UINT)result);
    XboxPipelineCacheSelectMode((LONG)mode);
    if (FAILED(result = XboxPipelineCacheOpen(prefix))) return Code(base, 6, mode, (UINT)result);
    XboxPipelineCacheCounters counters;
    XboxPipelineCacheGetCounters(&counters);
    if (counters.fileStatus != 0) return Code(base, 6, mode, (UINT)(counters.fileStatus + 1));
    void *pipeline = NULL;
    result = CreatePipeline(&setup, &pipeline);
    UINT call = 0, outcome = 0;
    if (Removed(&call, &outcome)) return Code(base, 13, mode, call << 8 | outcome << 4);
    if (FAILED(result)) return Code(base, 7, mode, (UINT)result);
    XboxPipelineCacheGetCounters(&counters);
    Release(pipeline);
    XboxPipelineCacheClose(FALSE);
    Release(setup.root);
    Release(setup.device);
    if (counters.hits != 1 || counters.misses != 0)
        return Code(base, 8, mode, (UINT)(counters.hits << 4 | (counters.misses & 15)));
    return 0x5DC00000u | mode << 12 | (UINT)counters.hits;
}

/* One path in this process, then the reload in another. Returns the round
   code 0x5EXMO0FF, or a failure code 0x5CSSMRRR. */
static UINT Round(UINT mode, const WCHAR *prefix) {
    const UINT base = 0x5C000000u;
    Setup setup;
    HRESULT result;
    const UINT stage = Prepare(&setup, &result);
    if (stage) return Code(base, stage, mode, (UINT)result);
    XboxPipelineCacheSelectMode((LONG)mode);
    if (FAILED(result = XboxPipelineCacheOpen(prefix))) return Code(base, 6, mode, (UINT)result);
    void *pipeline = NULL;
    result = CreatePipeline(&setup, &pipeline);
    XboxPipelineCacheCounters counters;
    XboxPipelineCacheGetCounters(&counters);
    const UINT support = (UINT)counters.supportFlags;
    UINT call = 0, outcome = 0;
    if (Removed(&call, &outcome)) return RoundCode(call, mode, outcome, support);
    if (FAILED(result)) {
        XboxPipelineCacheClose(TRUE);
        return Code(base, 7, mode, (UINT)result);
    }
    if (counters.mode != (LONG)mode) {
        Release(pipeline);
        XboxPipelineCacheClose(TRUE);
        return RoundCode(0, mode, Unavailable(&counters, mode), support);
    }
    if (counters.misses != 1 || counters.stores != 1) {
        Release(pipeline);
        XboxPipelineCacheClose(TRUE);
        return Code(base, 8, mode, (UINT)((counters.misses & 0xFF) << 4 | (counters.stores & 15)));
    }
    result = XboxPipelineCacheWriteNow();
    if (Removed(&call, &outcome)) return RoundCode(call, mode, outcome, support);
    XboxPipelineCacheGetCounters(&counters);
    Release(pipeline);
    XboxPipelineCacheClose(FALSE);
    if (FAILED(result) || counters.writes < 1) {
        XboxPipelineCacheClose(TRUE);
        return Code(base, 9, mode, (UINT)result);
    }
    Release(setup.root);
    Release(setup.device);

    DWORD exit = 0;
    const UINT failed = RunSelf(L"reload", mode, prefix, &exit);
    XboxPipelineCacheClose(TRUE);
    if (failed) return Code(base, failed, mode, exit);
    if ((exit & 0xFFFF0000u) == 0x5DC00000u) return RoundCode(0, mode, XBOX_PSO_OUTCOME_HIT, support);
    if ((exit & 0xFFFF0000u) == 0x5D0D0000u) return RoundCode((exit >> 8) & 15, mode, (exit >> 4) & 15, support);
    if ((exit & 0xFF000000u) == 0x5D000000u) return Code(base, 12, mode, ((exit >> 16) & 0xFF) << 4 | (exit & 15));
    /* The reload process died: the guard names the call it was in. */
    const LONG guard = XboxPipelineCacheTakeGuard(prefix);
    return RoundCode(guard ? (UINT)guard : XBOX_PSO_CALL_UNKNOWN, mode, XboxPsoCacheRemovalOutcome(exit), support);
}

/* Builds TempState\psoctl-<tick>-<mode>. */
static BOOL Prefix(WCHAR *prefix, UINT capacity, UINT mode) {
    WCHAR number[24];
    UINT used = GetTempPathW(300, prefix);
    if (!used || used >= 300 || capacity < 400) return FALSE;
    ULONGLONG tick = GetTickCount64();
    UINT digits = 0;
    do {
        number[digits++] = (WCHAR)(L'0' + tick % 10);
        tick /= 10;
    } while (tick && digits < 20);
    used = Append(prefix, used, capacity, L"psoctl-");
    while (digits) {
        const WCHAR digit[2] = {number[--digits], 0};
        used = Append(prefix, used, capacity, digit);
    }
    const WCHAR suffix[3] = {L'-', (WCHAR)(L'0' + mode), 0};
    Append(prefix, used, capacity, suffix);
    return TRUE;
}

/* The top process: no D3D12 call, one round process per path. */
static UINT Control(void) {
    static WCHAR prefix[400];
    UINT outcomes[3] = {0, 0, 0};
    UINT removal = 0, support = 0;
    BOOL haveSupport = FALSE;
    for (UINT mode = XBOX_PSO_CACHE_MODE_LIBRARY; mode <= XBOX_PSO_CACHE_MODE_BLOBS; ++mode) {
        if (!Prefix(prefix, ARRAYSIZE(prefix), mode)) return Code(0x5C000000u, 6, mode, ERROR_PATH_NOT_FOUND);
        DWORD exit = 0;
        const UINT failed = RunSelf(L"round", mode, prefix, &exit);
        if (failed) return Code(0x5C000000u, failed, mode, exit);
        UINT call = 0;
        if ((exit & 0xFF000000u) == 0x5E000000u) {
            call = (exit >> 20) & 15;
            outcomes[mode] = (exit >> 12) & 15;
            if (!haveSupport) {
                support = exit & 0xFF;
                haveSupport = TRUE;
            }
        } else if ((exit & 0xFF000000u) == 0x5C000000u) {
            XboxPipelineCacheTakeGuard(prefix);
            return exit;
        } else {
            /* The round process died, for example because the runtime ended
               it at a device removal; its guard names the call. */
            const LONG guard = XboxPipelineCacheTakeGuard(prefix);
            call = guard ? (UINT)guard : XBOX_PSO_CALL_UNKNOWN;
            outcomes[mode] = XboxPsoCacheRemovalOutcome(exit);
        }
        XboxPipelineCacheTakeGuard(prefix);
        if (!removal) removal = call;
    }
    return 0x5CC00000u | (removal & 15) << 16 | outcomes[1] << 12 | outcomes[2] << 8 | (support & 0xFF);
}

#ifdef XBOX_PSO_REPLAY_CONTROL
/* Record and replay (build.sh --d3d12-pipeline-replay). The top process makes
   no D3D12 call and starts one record process. That process selects replay
   before its first root signature, as a profile would, records a compute
   pipeline (CreateComputePipelineState) and the same shader as a subobject
   stream (ID3D12Device2::CreatePipelineState), times both creations, writes
   the file and starts a replay process. The replay process reads the file,
   attaches the replay to its device before creating any root signature, so
   the replay creates its own root signature from the recorded blob and
   pre-creates both pipelines; it then creates its root signature through the
   method table (which tells whether the runtime returned the replay's object
   for the same blob) and the two pipelines, which must be served from the
   replay.

   Exit code 0x5AXODRMM:
     X  the XboxPsoCacheCall before the first device removal (0 none,
        B replay-root, C replay-create, E the control's own creation)
     O  1 both pipelines served from the replay and faster than the record
          process created them
        2 both served, not faster
        3 one served
        4 neither served
        5 the replay process found no valid file
        6 the replay built fewer than two pipelines
        7-A the device was removed (removed, hung, reset, internal error)
        B  a process exited with another code (X from its guard)
        C  the replay did not finish within 30 s
     D  root-signature identity: 1 the runtime returned the replay's object
        for the same blob, 2 a new object (the replay rebuilt with the title's),
        0 not observed
     R  floor(log2(record time / serve time)), at most 15
     MM the record process's creation time for both, in 10 us units (FF: 2.55 ms+)
   Any other code is a failure 0x5CSS3RRR (stages as above; 0E replay idle
   timeout, 0F built count, 10 serve counts), or from the replay process
   0x5DSS3RRR. */

typedef struct ComputeStream {
    struct {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        void *root;
    } root;
    struct {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        D3D12_SHADER_BYTECODE cs;
    } cs;
} ComputeStream;

static LONGLONG Ticks(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

static ULONGLONG ElapsedUs(LONGLONG start) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart ? (ULONGLONG)(Ticks() - start) * 1000000 / (ULONGLONG)frequency.QuadPart : 0;
}

static HRESULT CreateStreamPipeline(const Setup *setup, void **pipeline) {
    typedef HRESULT (STDMETHODCALLTYPE *CreateState)(void *, const D3D12_PIPELINE_STATE_STREAM_DESC *, REFIID, void **);
    ComputeStream stream;
    for (UINT i = 0; i < sizeof(stream); ++i) ((volatile BYTE *)&stream)[i] = 0;
    stream.root.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
    stream.root.root = setup->root;
    stream.cs.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
    stream.cs.cs.pShaderBytecode = controlComputeShader;
    stream.cs.cs.BytecodeLength = sizeof(controlComputeShader);
    D3D12_PIPELINE_STATE_STREAM_DESC desc;
    desc.SizeInBytes = sizeof(stream);
    desc.pPipelineStateSubobjectStream = &stream;
    *pipeline = NULL;
    /* ID3D12Device2::CreatePipelineState, slot 47 of the shared table. */
    return METHOD(setup->device, 47, CreateState)(setup->device, &desc, &pipelineIid, pipeline);
}

/* Creates both pipelines, timing them. Returns 0 or the failing stage. */
static UINT CreateBoth(const Setup *setup, ULONGLONG *us, HRESULT *result) {
    void *compute = NULL, *stream = NULL;
    const LONGLONG start = Ticks();
    *result = CreatePipeline(setup, &compute);
    if (SUCCEEDED(*result)) *result = CreateStreamPipeline(setup, &stream);
    *us = ElapsedUs(start);
    Release(compute);
    Release(stream);
    return FAILED(*result) ? 7 : 0;
}

static UINT ReplayCode(UINT call, UINT outcome, UINT identity, UINT ratio, UINT units) {
    return 0x5A000000u | (call & 15) << 20 | (outcome & 15) << 16 | (identity & 15) << 12 | (ratio & 15) << 8 |
        (units > 255 ? 255 : units);
}

/* The replay process: exits 0x5D8DHUUU on success (D identity, H pipelines
   served, UUU the serve time in us, at most 0xFFF), 0x5D0D3XO0 after a
   removal, or 0x5DSS3RRR. */
static UINT Replay(const WCHAR *prefix) {
    const UINT base = 0x5D000000u, mode = XBOX_PSO_CACHE_MODE_REPLAY;
    Setup setup;
    HRESULT result;
    UINT stage = PrepareDevice(&setup, &result);
    if (stage) return Code(base, stage, mode, (UINT)result);
    XboxPipelineCacheSelectMode((LONG)mode);
    XboxPipelineReplayConfigure(0, 0);
    if (FAILED(result = XboxPipelineCacheOpen(prefix))) return Code(base, 6, mode, (UINT)result);
    XboxPipelineCacheCounters counters;
    XboxPipelineCacheGetCounters(&counters);
    if (counters.fileStatus != 0) return Code(base, 6, mode, (UINT)(counters.fileStatus + 1));
    /* Before any root signature of the control's: the replay creates its own. */
    if (FAILED(result = XboxPipelineReplayAttach(setup.device))) return Code(base, 6, mode, (UINT)result);
    UINT call = 0, outcome = 0;
    if (!XboxPipelineReplayWaitIdle(30000)) return Code(base, 14, mode, 0);
    if (Removed(&call, &outcome)) return Code(base, 13, mode, call << 8 | outcome << 4);
    XboxPipelineReplayCounters replay;
    XboxPipelineReplayGetCounters(&replay);
    if (replay.built != 2) return Code(base, 15, mode, (UINT)((replay.built & 15) << 8 | (replay.failed & 15) << 4 |
        (replay.mismatched & 15)));
    if ((stage = CreateRoot(&setup, &result))) return Code(base, stage, mode, (UINT)result);
    /* A new root object for the blob: the replay rebuilds with the control's. */
    if (!XboxPipelineReplayWaitIdle(30000)) return Code(base, 14, mode, 1);
    ULONGLONG us = 0;
    stage = CreateBoth(&setup, &us, &result);
    if (Removed(&call, &outcome)) return Code(base, 13, mode, call << 8 | outcome << 4);
    if (stage) return Code(base, stage, mode, (UINT)result);
    XboxPipelineReplayGetCounters(&replay);
    const UINT served = (UINT)(replay.hits + replay.waitHits);
    const UINT identity = replay.rootSame ? 1 : replay.rootDistinct ? 2 : 0;
    XboxPipelineCacheClose(FALSE);
    Release(setup.root);
    Release(setup.device);
    if (served < 2) return Code(base, 16, mode, served << 8 | (UINT)(replay.rootMismatch & 15) << 4 | (UINT)(replay.early & 15));
    return 0x5D800000u | identity << 16 | served << 12 | (UINT)(us > 0xFFF ? 0xFFF : us);
}

/* The record process: records both pipelines, then runs the replay process. */
static UINT Record(const WCHAR *prefix) {
    const UINT base = 0x5C000000u, mode = XBOX_PSO_CACHE_MODE_REPLAY;
    Setup setup;
    HRESULT result;
    UINT stage = PrepareDevice(&setup, &result);
    if (stage) return Code(base, stage, mode, (UINT)result);
    /* As a profile selects it: before the first root signature. */
    XboxPipelineCacheSelectMode((LONG)mode);
    XboxPipelineReplayConfigure(0, 0);
    if (FAILED(result = XboxPipelineCacheOpen(prefix))) return Code(base, 6, mode, (UINT)result);
    if ((stage = CreateRoot(&setup, &result))) return Code(base, stage, mode, (UINT)result);
    ULONGLONG recordUs = 0;
    stage = CreateBoth(&setup, &recordUs, &result);
    UINT call = 0, outcome = 0;
    if (Removed(&call, &outcome)) return ReplayCode(call, outcome, 0, 0, 0);
    if (stage) {
        XboxPipelineCacheClose(TRUE);
        return Code(base, stage, mode, (UINT)result);
    }
    XboxPipelineReplayCounters replay;
    XboxPipelineReplayGetCounters(&replay);
    if (replay.recorded != 2) {
        XboxPipelineCacheClose(TRUE);
        return Code(base, 8, mode, (UINT)((replay.recorded & 15) << 4 | (replay.unrecorded & 15)));
    }
    result = XboxPipelineCacheWriteNow();
    XboxPipelineCacheCounters counters;
    XboxPipelineCacheGetCounters(&counters);
    XboxPipelineCacheClose(FALSE);
    Release(setup.root);
    Release(setup.device);
    if (FAILED(result) || counters.writes < 1) {
        XboxPipelineCacheClose(TRUE);
        return Code(base, 9, mode, (UINT)result);
    }
    DWORD exit = 0;
    const UINT failed = RunSelf(L"reload", mode, prefix, &exit);
    XboxPipelineCacheClose(TRUE);
    if (failed) return Code(base, failed, mode, exit);
    const UINT units = (UINT)(recordUs / 10);
    if ((exit & 0xFFF00000u) == 0x5D800000u) {
        const UINT identity = (exit >> 16) & 15, served = (exit >> 12) & 15, serveUs = exit & 0xFFF;
        UINT ratio = 0;
        for (ULONGLONG quotient = recordUs / (serveUs ? serveUs : 1); quotient > 1 && ratio < 15; quotient >>= 1) ++ratio;
        const UINT verdict = served == 2 ? (recordUs > serveUs ? 1 : 2) : served == 1 ? 3 : 4;
        return ReplayCode(0, verdict, identity, ratio, units);
    }
    if ((exit & 0xFFFF0000u) == 0x5D0D0000u) return ReplayCode((exit >> 8) & 15, (exit >> 4) & 15, 0, 0, units);
    if ((exit & 0xFF000000u) == 0x5D000000u) {
        const UINT replayStage = (exit >> 16) & 0xFF;
        if (replayStage == 6) return ReplayCode(0, 5, 0, 0, units);
        if (replayStage == 15) return ReplayCode(0, 6, 0, 0, units);
        if (replayStage == 14) return ReplayCode(0, 12, 0, 0, units);
        if (replayStage == 16) return ReplayCode(0, ((exit >> 8) & 15) ? 3 : 4, 0, 0, units);
        return Code(base, 12, mode, replayStage << 4 | (exit & 15));
    }
    /* The replay process died: the guard names the call it was in. */
    const LONG guard = XboxPipelineCacheTakeGuard(prefix);
    return ReplayCode(guard ? (UINT)guard : XBOX_PSO_CALL_UNKNOWN, XboxPsoCacheRemovalOutcome(exit), 0, 0, units);
}

static UINT ReplayControl(void) {
    static WCHAR prefix[400];
    const UINT mode = XBOX_PSO_CACHE_MODE_REPLAY;
    if (!Prefix(prefix, ARRAYSIZE(prefix), mode)) return Code(0x5C000000u, 6, mode, ERROR_PATH_NOT_FOUND);
    DWORD exit = 0;
    const UINT failed = RunSelf(L"round", mode, prefix, &exit);
    if (failed) return Code(0x5C000000u, failed, mode, exit);
    const LONG guard = XboxPipelineCacheTakeGuard(prefix);
    if ((exit & 0xFF000000u) == 0x5A000000u || (exit & 0xFF000000u) == 0x5C000000u) return exit;
    /* The record process died: its guard names the call. */
    return ReplayCode(guard ? (UINT)guard : XBOX_PSO_CALL_UNKNOWN, XboxPsoCacheRemovalOutcome(exit), 0, 0, 0);
}
#endif

/* "<verb> <mode> <prefix>" after the image name, or NULL. */
static const WCHAR *Argument(const WCHAR *line, const WCHAR *verb, UINT *mode) {
    for (const WCHAR *p = line; p && *p; ++p) {
        if (*p != L' ') continue;
        UINT i = 0;
        while (verb[i] && p[1 + i] == verb[i]) ++i;
        if (verb[i] || p[1 + i] != L' ') continue;
        const WCHAR *selected = p + 2 + i;
        if ((*selected != L'1' && *selected != L'2' && *selected != L'3') || selected[1] != L' ' || !selected[2])
            return NULL;
        *mode = (UINT)(*selected - L'0');
        return selected + 2;
    }
    return NULL;
}

void WINAPI ChildEntry(void) {
    const WCHAR *line = GetCommandLineW();
    UINT mode = 0;
    const WCHAR *prefix = Argument(line, L"reload", &mode);
#ifdef XBOX_PSO_REPLAY_CONTROL
    if (prefix && mode == XBOX_PSO_CACHE_MODE_REPLAY) ExitProcess(Replay(prefix));
    if (prefix) ExitProcess(Reload(mode, prefix));
    prefix = Argument(line, L"round", &mode);
    if (prefix && mode == XBOX_PSO_CACHE_MODE_REPLAY) ExitProcess(Record(prefix));
    if (prefix) ExitProcess(Round(mode, prefix));
    /* The library and blob rounds stay reachable by verb; the top process runs replay only. */
    (void)Control;
    ExitProcess(ReplayControl());
#else
    if (prefix && mode == XBOX_PSO_CACHE_MODE_REPLAY) prefix = NULL;
    if (prefix) ExitProcess(Reload(mode, prefix));
    prefix = Argument(line, L"round", &mode);
    if (prefix && mode == XBOX_PSO_CACHE_MODE_REPLAY) prefix = NULL;
    if (prefix) ExitProcess(Round(mode, prefix));
    ExitProcess(Control());
#endif
}
