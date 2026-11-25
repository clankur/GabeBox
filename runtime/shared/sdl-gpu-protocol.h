#pragma once
#include <windows.h>
#include <stdint.h>

/* The presenter creates and zeroes this file at XBOX_SDL_HOST_MAILBOX + L".gpu"
   before starting the game, then writes magic. It retains the mapping throughout
   the session. Older presenters simply omit the file. This sidecar has no pixels. */
#define SDL_GPU_MAGIC 0x31475053
#define SDL_GPU_SLOTS 2
#define SDL_GPU_CURSOR_SIZE 128
typedef struct SdlGpuCursor {
    uint32_t shape; /* changes when the immutable SDL cursor image changes; 0 hidden */
    int32_t x, y;   /* top-left in top-down framebuffer pixels, hotspot already applied */
    uint32_t width, height;
    uint8_t rgba[SDL_GPU_CURSOR_SIZE * SDL_GPU_CURSOR_SIZE * 4]; /* tightly packed straight RGBA */
} SdlGpuCursor;
typedef struct SdlGpuMailbox {
    volatile LONG magic;
    volatile LONG generation; /* 0 inactive, odd being replaced, positive even stable. */
    uint64_t adapter_luid;
    uint32_t width, height, dxgi_format;
    uint32_t flip_y; /* sample V = 1-V for the GL framebuffer. */
    WCHAR names[SDL_GPU_SLOTS][3][64]; /* texture, ready fence, done fence */
    volatile LONG64 sequence[SDL_GPU_SLOTS];
    /* Presenter writes accepted_generation after opening every handle,
       refreshes heartbeat_ms with GetTickCount(), and clears acceptance on exit.
       Wait for ready[i] >= sequence[i] before reading each texture. Signal
       done[i] = sequence[i] after the last GPU read, even for skipped frames.
       The producer waits for its last copy and the last published consumer
       acknowledgement before reusing the slot. An unpublished copy needs
       only producer completion and can leave a gap in the sequence. */
    volatile LONG accepted_generation;
    volatile LONG heartbeat_ms;
    volatile LONG supports_cursor; /* presenter advertises RGBA overlay */
    volatile LONG gpu_active;      /* 0 draws the existing CPU frame, 1 uses the exchange */
    SdlGpuCursor cursor[SDL_GPU_SLOTS]; /* protected by corresponding ready/done protocol */
} SdlGpuMailbox;
