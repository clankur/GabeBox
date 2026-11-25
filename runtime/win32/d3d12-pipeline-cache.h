/* Persistent Direct3D 12 pipeline-state cache for hosted Win32 titles.

   The loader-trace adapter starts it for a title whose profile carries the
   pipeline-cache marker; the console control drives the same functions
   directly. See d3d12-pipeline-cache.c for the contract. */
#pragma once
#include <windows.h>

/* Background start, once per process. Honors XBOX_WIN32_PSO_TRACE=1 (timing
   only), XBOX_WIN32_PSO_CACHE=<LocalState prefix> and
   XBOX_WIN32_PSO_CACHE_MODE=library|blobs (any other value or none:
   passthrough, which times creation and makes no cache call on the device). */
void XboxPipelineHooksStart(void);

/* Patches the shared ID3D12Device method table through this device: slots 10,
   11 and 47, and with keyRoots also slot 16, so root signatures created from
   now on are hashed for cache keys. */
HRESULT XboxPipelineHooksInstall(void *device, BOOL keyRoots);

/* Computes the driver identity, reads <prefix>.guard and, in a cache mode
   without a guard for this identity, preloads <prefix>-<identity>.bin. */
HRESULT XboxPipelineCacheOpen(const WCHAR *prefix);

/* Serializes unsaved pipelines now. S_FALSE when nothing was unsaved or the
   cache stopped after a device removal. */
HRESULT XboxPipelineCacheWriteNow(void);

/* Releases the library and every cached record; optionally deletes the cache
   file. A guard file is kept. */
void XboxPipelineCacheClose(BOOL removeFile);

/* 1 selects the pipeline library and 2 cached blobs; anything else is
   passthrough. Takes effect at the next Open and bind. */
void XboxPipelineCacheSelectMode(LONG mode);

/* The XboxPsoCacheCall recorded in <prefix>.guard (15 when unreadable), or 0
   without a guard; the guard is deleted. For the console control. */
LONG XboxPipelineCacheTakeGuard(const WCHAR *prefix);

typedef struct XboxPipelineCacheCounters {
    LONG mode;        /* 0 passthrough or stopped, 1 pipeline library, 2 cached blobs */
    LONG fileStatus;  /* XboxPsoCacheStatus of the preloaded file, -1 when absent */
    LONG supportFlags;
    LONG hits;
    LONG misses;
    LONG stores;
    LONG duplicates;
    LONG bypassed;
    LONG writes;
    HRESULT libraryResult;
    LONG selected;       /* the requested mode */
    LONG lastCall;       /* XboxPsoCacheCall of the latest cache call */
    LONG removedCall;    /* the call that preceded a device removal, 0 without one */
    HRESULT removedReason;
    LONG guardCall;      /* a guard for this identity found at Open */
} XboxPipelineCacheCounters;

void XboxPipelineCacheGetCounters(XboxPipelineCacheCounters *counters);

/* Exported for the process-exit adapters: writes unsaved pipelines once. */
void WINAPI XboxD3D12PipelineCacheFlush(void);
