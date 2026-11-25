/* Persistent Direct3D 12 pipeline-state cache for hosted Win32 titles.

   The loader-trace adapter starts it for a title whose profile carries the
   pipeline-cache marker; the console control drives the same functions
   directly. See d3d12-pipeline-cache.c for the contract. */
#pragma once
#include <windows.h>

/* Background start, once per process. Honors XBOX_WIN32_PSO_TRACE=1 (timing
   only) and XBOX_WIN32_PSO_CACHE=<LocalState prefix> (timing and cache). */
void XboxPipelineHooksStart(void);

/* Patches the shared ID3D12Device method table through this device. With
   keyRoots, root signatures created from now on are hashed for cache keys. */
HRESULT XboxPipelineHooksInstall(void *device, BOOL keyRoots);

/* Computes the driver identity and preloads <prefix>-<identity>.bin. */
HRESULT XboxPipelineCacheOpen(const WCHAR *prefix);

/* Serializes unsaved pipelines now. S_FALSE when nothing was unsaved. */
HRESULT XboxPipelineCacheWriteNow(void);

/* Releases the library and every cached record; optionally deletes the file. */
void XboxPipelineCacheClose(BOOL removeFile);

/* 0 selects automatically (library, then cached blobs); 1 or 2 forces the
   pipeline library or the cached-blob path. Takes effect at the next bind. */
void XboxPipelineCacheSelectMode(LONG mode);

typedef struct XboxPipelineCacheCounters {
    LONG mode;        /* 0 disabled, 1 pipeline library, 2 cached blobs */
    LONG fileStatus;  /* XboxPsoCacheStatus of the preloaded file, -1 when absent */
    LONG supportFlags;
    LONG hits;
    LONG misses;
    LONG stores;
    LONG duplicates;
    LONG bypassed;
    LONG writes;
    HRESULT libraryResult;
} XboxPipelineCacheCounters;

void XboxPipelineCacheGetCounters(XboxPipelineCacheCounters *counters);

/* Exported for the process-exit adapters: writes unsaved pipelines once. */
void WINAPI XboxD3D12PipelineCacheFlush(void);
