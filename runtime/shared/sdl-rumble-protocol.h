#pragma once
#include <windows.h>

/* Optional sidecar. The presenter owns readiness and the generation. XInput
   publishes both motor values with that generation in one atomic word. */
#define SDL_RUMBLE_MAGIC 0x53445231
#define SDL_RUMBLE_SUFFIX L".rumble"

typedef struct SdlRumbleMailbox {
    volatile LONG magic;
    volatile LONG active;
    volatile LONG generation;
    volatile LONG reserved;
    volatile LONG64 command;
} SdlRumbleMailbox;

static inline LONG64 SdlRumbleCommand(LONG generation, DWORD motors) {
    return (LONG64)(((UINT64)(DWORD)generation << 32) | motors);
}

static inline void SdlRumbleDisarm(SdlRumbleMailbox *box) {
    InterlockedExchange(&box->active, 0);
    InterlockedExchange64(&box->command, 0);
}

static inline void SdlRumbleArm(SdlRumbleMailbox *box) {
    LONG generation = InterlockedIncrement(&box->generation);
    if (!generation) generation = InterlockedIncrement(&box->generation);
    InterlockedExchange64(&box->command, SdlRumbleCommand(generation, 0));
    InterlockedExchange(&box->active, generation);
}

static inline LONG SdlRumbleGeneration(SdlRumbleMailbox *box) {
    return box ? InterlockedCompareExchange(&box->active, 0, 0) : 0;
}

static inline BOOL SdlRumblePublish(SdlRumbleMailbox *box, LONG generation, WORD left, WORD right) {
    if (!generation || SdlRumbleGeneration(box) != generation) return FALSE;
    const LONG64 next = SdlRumbleCommand(generation, MAKELONG(left, right));
    LONG64 previous = InterlockedCompareExchange64(&box->command, 0, 0);
    while ((DWORD)((UINT64)previous >> 32) == (DWORD)generation) {
        const LONG64 current = InterlockedCompareExchange64(&box->command, next, previous);
        if (current == previous) return TRUE;
        previous = current;
    }
    return FALSE;
}

static inline DWORD SdlRumbleRead(SdlRumbleMailbox *box) {
    return (DWORD)InterlockedCompareExchange64(&box->command, 0, 0);
}
