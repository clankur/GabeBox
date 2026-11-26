#pragma once
#include <windows.h>
#include <stdint.h>
#include "sdl-gpu-probe-stats.h"

/* Opt-in game-side half of the frame probe. Only when fhd-flash-probe.txt is in the package
   root does the presenter create and zero XBOX_SDL_HOST_MAILBOX + L".probe", before it writes
   the ".gpu" sidecar's magic, and then write SDL_GPU_PROBE_MAGIC here. A producer that finds
   the file copies the probe rows of the game's own renderbuffer, before its copy into the
   exchange texture, into a readback buffer on the same queue. Once that copy's ready fence
   has passed, it appends one record per sampled frame. Without the file it does nothing.
   No pixels cross this mapping. */
#define SDL_GPU_PROBE_MAGIC 0x31505053 /* "SPP1" */
#define SDL_GPU_PROBE_RING 64
#define SDL_GPU_PROBE_DEPTH 4 /* readbacks in flight on each side; a busy ring skips a frame */

typedef struct SdlGpuProbeRecord {
    volatile LONG64 sequence; /* exchange sequence; 0 while the producer rewrites the record */
    int64_t counter;          /* producer QueryPerformanceCounter when the copy was queued */
    SdlGpuProbeStats stats;
    uint32_t slot;
} SdlGpuProbeRecord;

typedef struct SdlGpuProbeMailbox {
    volatile LONG magic;
    volatile LONG producer;  /* 0 none yet, 1 sampling, otherwise the failed setup HRESULT */
    volatile LONG64 written; /* records appended; record n is ring[n % SDL_GPU_PROBE_RING] */
    volatile LONG64 skipped; /* published frames not sampled because every readback was busy */
    SdlGpuProbeRecord ring[SDL_GPU_PROBE_RING];
} SdlGpuProbeMailbox;
