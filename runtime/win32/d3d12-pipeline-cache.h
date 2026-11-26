/* Persistent Direct3D 12 pipeline-state cache for hosted Win32 titles.

   The loader-trace adapter starts it for a title whose profile carries the
   pipeline-cache marker; the console control drives the same functions
   directly. See d3d12-pipeline-cache.c for the contract. */
#pragma once
#include <windows.h>

/* Background start, once per process. Honors XBOX_WIN32_PSO_TRACE=1 (timing
   only), XBOX_WIN32_PSO_CACHE=<LocalState prefix> and
   XBOX_WIN32_PSO_CACHE_MODE=library|blobs|replay|replay-idle (any other value
   or none: passthrough, which times creation and makes no cache call on the
   device). TRUE when the hooks' thread runs, from this call or an earlier
   one: the thread creates a device of its own once d3d12.dll is loaded. */
BOOL XboxPipelineHooksStart(void);

/* Serializes writes to the shared ID3D12Device method table between these
   hooks and the residency hooks (d3d12-residency.c). Both change the page's
   protection, so one patcher restoring it while the other writes would
   fault. */
extern SRWLOCK XboxD3D12MethodTableLock;

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

/* 1 selects the pipeline library, 2 cached blobs, 3 record and replay and
   4 replay from the title's first idle period; anything else is passthrough.
   Takes effect at the next Open and bind. Select before the title's first
   root signature so the replay sees it. */
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

/* Record and replay. Configure sets the shortest creation that is recorded
   (XBOX_PSO_REPLAY_RECORD_US by default; the control records everything)
   and how long unrequested pre-created pipelines are held (0: the default). */
void XboxPipelineReplayConfigure(ULONG recordMinUs, ULONG holdMs);

/* Starts the replay on this device before the title's first root signature
   or pipeline would (the control). S_FALSE when another device was attached. */
HRESULT XboxPipelineReplayAttach(void *device);

/* TRUE once the replay has nothing it can build now (or never attached). */
BOOL XboxPipelineReplayWaitIdle(DWORD timeoutMs);

typedef struct XboxPipelineReplayCounters {
    LONG loaded;       /* records read from the file */
    LONG built;        /* pre-created */
    LONG failed;       /* creation or root signature failed */
    LONG mismatched;   /* a rebuilt desc did not reproduce its key */
    LONG hits;         /* served ready */
    LONG waitHits;     /* served after waiting for the replay's creation */
    LONG early;        /* asked for before the replay reached it */
    LONG rootMismatch; /* ready, but the title named another root object */
    LONG rootSame;     /* the runtime returned the replay's root object to the title */
    LONG rootDistinct; /* the runtime returned a new root object for the same blob */
    LONG recorded;
    LONG unrecorded;
    LONG released;     /* pre-created and never asked for */
    LONG requeued;
    LONG ownRoots;
    LONGLONG buildUs, maxBuildUs, hitUs, missUs;
    LONG attached;
} XboxPipelineReplayCounters;

void XboxPipelineReplayGetCounters(XboxPipelineReplayCounters *counters);

/* Exported for the process-exit adapters: writes unsaved pipelines once. */
void WINAPI XboxD3D12PipelineCacheFlush(void);

/* From the loader-trace adapter's process-detach notification, which runs for an ExitProcess
   made by any module. It reports the exit stats when no exit adapter flushed first, and takes
   no lock and makes no Direct3D call. */
void XboxD3D12PipelineCacheProcessExit(void);
