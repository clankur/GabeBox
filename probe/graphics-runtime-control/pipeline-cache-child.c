/* Console control for the persistent Direct3D 12 pipeline cache.

   Links the same d3d12-pipeline-cache.c that the loader-trace adapter uses and
   checks whether the console's D3D12 runtime can persist pipelines across
   processes. For each path, the pipeline library and then the
   GetCachedBlob/CachedPSO fallback, this process installs the shared hooks,
   creates a root signature and a trivial compute pipeline (a miss that the
   cache stores), serializes the cache to TempState and starts a second copy
   of itself. The second process loads the file and creates the same pipeline
   again, which must be a hit.

   Exit code 0x5CC0LBFF: L and B are the library and cached-blob outcomes and
   FF is D3D12_FEATURE_DATA_SHADER_CACHE.SupportFlags. Outcomes:
     1 a pipeline was stored, serialized, reloaded in a second process and hit
     2 CreatePipelineLibrary returned DXGI_ERROR_UNSUPPORTED
     3 CreatePipelineLibrary returned E_NOTIMPL
     4 the device has no ID3D12Device1
     5 CreatePipelineLibrary failed otherwise
     6 the device does not report single-PSO cache support
   Any other code is a failure 0x5CSSMRRR: stage SS, path M (1 library,
   2 blobs, 0 setup) and the low 12 bits RRR of the HRESULT or detail.
   Stages: 01 load d3d12.dll, 02 D3D12CreateDevice, 03 install hooks,
   04 D3D12SerializeRootSignature, 05 CreateRootSignature, 06 open cache,
   07 create pipeline, 08 counters after the miss (RRR = misses << 4 | stores),
   09 write, 0A start the second process, 0B second process did not finish,
   0C second process failed (RRR = its stage << 4 | its low nibble).
   The second process exits 0x5DC0M00H on success (H hits) or 0x5DSSMRRR with
   stage 06 reporting the cache file status + 1 and 08 RRR = hits << 4 | misses. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include "../../runtime/win32/d3d12-pipeline-cache.h"
#include "pipeline-cache-shader.h"

DECLSPEC_IMPORT DECLSPEC_NORETURN void WINAPI ExitProcess(UINT code);

#define METHOD(object, slot, type) ((type)((*(void ***)(object))[slot]))
typedef ULONG (STDMETHODCALLTYPE *ReleaseMethod)(void *);
static const GUID deviceIid = {0x189819F1, 0x1DB6, 0x4B57, {0xBE, 0x54, 0x18, 0x21, 0x33, 0x9B, 0x85, 0xF7}};
static const GUID rootIid = {0xC54A6B66, 0x72DF, 0x4EE8, {0x8B, 0xE5, 0xA9, 0x46, 0xA1, 0x42, 0x92, 0x14}};
static const GUID pipelineIid = {0x765A30F3, 0xF624, 0x4C6F, {0xA8, 0x28, 0xAC, 0xE9, 0x48, 0x62, 0x24, 0x45}};

static void Release(void *object) {
    if (object) METHOD(object, 2, ReleaseMethod)(object);
}

static UINT Code(UINT base, UINT stage, UINT mode, UINT detail) {
    return base | (stage & 0xFF) << 16 | (mode & 0xF) << 12 | (detail & 0xFFF);
}

typedef struct Setup {
    void *device;
    void *root;
} Setup;

/* Returns 0, or the failing stage with *result set. */
static UINT Prepare(Setup *setup, HRESULT *result) {
    typedef HRESULT (WINAPI *CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    typedef HRESULT (WINAPI *SerializeRoot)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **,
        ID3DBlob **);
    typedef HRESULT (STDMETHODCALLTYPE *CreateRoot)(void *, UINT, const void *, SIZE_T, REFIID, void **);
    typedef void *(STDMETHODCALLTYPE *BlobPointer)(void *);
    typedef SIZE_T (STDMETHODCALLTYPE *BlobSize)(void *);
    setup->device = setup->root = NULL;
    const HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!d3d12) {
        *result = HRESULT_FROM_WIN32(GetLastError());
        return 1;
    }
    const CreateDevice create = (CreateDevice)(void *)GetProcAddress(d3d12, "D3D12CreateDevice");
    *result = create ? create(NULL, D3D_FEATURE_LEVEL_11_0, &deviceIid, &setup->device) :
        HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    if (FAILED(*result)) return 2;
    if (FAILED(*result = XboxPipelineHooksInstall(setup->device, TRUE))) return 3;
    const SerializeRoot serialize = (SerializeRoot)(void *)GetProcAddress(d3d12, "D3D12SerializeRootSignature");
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
    if (mode == 2) return 6;
    if (counters->libraryResult == DXGI_ERROR_UNSUPPORTED) return 2;
    if (counters->libraryResult == E_NOTIMPL) return 3;
    if (counters->libraryResult == E_NOINTERFACE) return 4;
    return 5;
}

/* The second process: load the file and expect a hit. */
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
    if (FAILED(result = CreatePipeline(&setup, &pipeline))) return Code(base, 7, mode, (UINT)result);
    XboxPipelineCacheGetCounters(&counters);
    Release(pipeline);
    XboxPipelineCacheClose(FALSE);
    Release(setup.root);
    Release(setup.device);
    if (counters.hits != 1 || counters.misses != 0)
        return Code(base, 8, mode, (UINT)(counters.hits << 4 | (counters.misses & 15)));
    return 0x5DC00000u | mode << 12 | (UINT)counters.hits;
}

static UINT Append(WCHAR *out, UINT used, UINT capacity, const WCHAR *text) {
    while (*text && used + 1 < capacity) out[used++] = *text++;
    out[used] = 0;
    return used;
}

/* One path in this process, then the reload in a second one. Returns the
   outcome nibble, or a full failure code in *failure. */
static UINT Round(const Setup *setup, UINT mode, LONG *support, UINT *failure) {
    const UINT base = 0x5C000000u;
    /* Static: the control is single-threaded and links without stack probes. */
    static WCHAR prefix[400], command[1200], image[600];
    WCHAR number[24];
    UINT used = GetTempPathW(300, prefix);
    if (!used || used >= 300) {
        *failure = Code(base, 6, mode, ERROR_PATH_NOT_FOUND);
        return 0;
    }
    ULONGLONG tick = GetTickCount64();
    UINT digits = 0;
    do {
        number[digits++] = (WCHAR)(L'0' + tick % 10);
        tick /= 10;
    } while (tick && digits < 20);
    used = Append(prefix, used, ARRAYSIZE(prefix), L"psoctl-");
    while (digits) {
        const WCHAR digit[2] = {number[--digits], 0};
        used = Append(prefix, used, ARRAYSIZE(prefix), digit);
    }
    const WCHAR suffix[3] = {L'-', (WCHAR)(L'0' + mode), 0};
    Append(prefix, used, ARRAYSIZE(prefix), suffix);

    XboxPipelineCacheSelectMode((LONG)mode);
    HRESULT result = XboxPipelineCacheOpen(prefix);
    if (FAILED(result)) {
        *failure = Code(base, 6, mode, (UINT)result);
        return 0;
    }
    void *pipeline = NULL;
    result = CreatePipeline(setup, &pipeline);
    XboxPipelineCacheCounters counters;
    XboxPipelineCacheGetCounters(&counters);
    *support = counters.supportFlags;
    if (FAILED(result)) {
        XboxPipelineCacheClose(TRUE);
        *failure = Code(base, 7, mode, (UINT)result);
        return 0;
    }
    if (counters.mode != (LONG)mode) {
        Release(pipeline);
        XboxPipelineCacheClose(TRUE);
        return Unavailable(&counters, mode);
    }
    if (counters.misses != 1 || counters.stores != 1) {
        Release(pipeline);
        XboxPipelineCacheClose(TRUE);
        *failure = Code(base, 8, mode, (UINT)((counters.misses & 0xFF) << 4 | (counters.stores & 15)));
        return 0;
    }
    result = XboxPipelineCacheWriteNow();
    XboxPipelineCacheGetCounters(&counters);
    Release(pipeline);
    XboxPipelineCacheClose(FALSE);
    if (FAILED(result) || counters.writes < 1) {
        XboxPipelineCacheClose(TRUE);
        *failure = Code(base, 9, mode, (UINT)result);
        return 0;
    }

    const DWORD length = GetModuleFileNameW(NULL, image, ARRAYSIZE(image));
    used = 0;
    used = Append(command, used, ARRAYSIZE(command), L"\"");
    used = Append(command, used, ARRAYSIZE(command), image);
    used = Append(command, used, ARRAYSIZE(command), L"\" reload ");
    const WCHAR selected[3] = {(WCHAR)(L'0' + mode), L' ', 0};
    used = Append(command, used, ARRAYSIZE(command), selected);
    used = Append(command, used, ARRAYSIZE(command), prefix);
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    for (UINT i = 0; i < sizeof(startup); ++i) ((volatile BYTE *)&startup)[i] = 0;
    startup.cb = sizeof(startup);
    if (!length || length >= ARRAYSIZE(image) ||
            !CreateProcessW(image, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        XboxPipelineCacheClose(TRUE);
        *failure = Code(base, 10, mode, GetLastError());
        return 0;
    }
    CloseHandle(process.hThread);
    DWORD exit = 0;
    const DWORD waited = WaitForSingleObject(process.hProcess, 60000);
    const BOOL finished = waited == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exit);
    if (!finished) TerminateProcess(process.hProcess, ERROR_TIMEOUT);
    CloseHandle(process.hProcess);
    XboxPipelineCacheClose(TRUE);
    if (!finished) {
        *failure = Code(base, 11, mode, waited);
        return 0;
    }
    if ((exit & 0xFFFF0000u) != 0x5DC00000u) {
        *failure = Code(base, 12, mode, ((exit >> 16) & 0xFF) << 4 | (exit & 15));
        return 0;
    }
    return 1;
}

void WINAPI ChildEntry(void) {
    const WCHAR *line = GetCommandLineW();
    for (const WCHAR *p = line; p && *p; ++p) {
        static const WCHAR marker[] = L" reload ";
        UINT i = 0;
        while (marker[i] && p[i] == marker[i]) ++i;
        if (marker[i]) continue;
        const WCHAR *mode = p + i;
        if ((*mode == L'1' || *mode == L'2') && mode[1] == L' ' && mode[2])
            ExitProcess(Reload((UINT)(*mode - L'0'), mode + 2));
        ExitProcess(Code(0x5D000000u, 6, 0, ERROR_INVALID_PARAMETER));
    }

    Setup setup;
    HRESULT result;
    const UINT stage = Prepare(&setup, &result);
    if (stage) ExitProcess(Code(0x5C000000u, stage, 0, (UINT)result));
    LONG support = 0;
    UINT failure = 0;
    const UINT library = Round(&setup, 1, &support, &failure);
    if (failure) ExitProcess(failure);
    const UINT blobs = Round(&setup, 2, &support, &failure);
    if (failure) ExitProcess(failure);
    Release(setup.root);
    Release(setup.device);
    ExitProcess(0x5CC00000u | library << 12 | blobs << 8 | ((UINT)support & 0xFF));
}
