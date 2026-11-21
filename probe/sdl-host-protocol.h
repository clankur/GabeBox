#pragma once
#include <windows.h>
#include <stdint.h>

/* Same-package, little-endian SDL2 frame and input mailbox. All shared
   control words are naturally aligned and accessed with Interlocked APIs. */
enum {
    SDL_HOST_MAGIC = 0x53444c31,
    SDL_HOST_MAX_WIDTH = 1920,
    SDL_HOST_MAX_HEIGHT = 1080,
    SDL_HOST_EMPTY = 0,
    SDL_HOST_WRITING,
    SDL_HOST_READY,
    SDL_HOST_READING,
    SDL_HOST_POINTER_MAX = 4095
};

typedef struct SdlHostFrame {
    uint32_t width;
    uint32_t height;
    uint32_t sequence;
    uint32_t input_ack;
    uint32_t input_events;
} SdlHostFrame;

typedef struct SdlHostMailbox {
    volatile LONG magic;
    volatile LONG frame_state;
    volatile LONG frame_sequence;
    volatile LONG input_pointer;
    volatile LONG input_keys;
    volatile LONG input_serial;
    volatile LONG input_ack;
    volatile LONG input_events;
    volatile LONG quit;
    SdlHostFrame frame;
    uint8_t pixels[SDL_HOST_MAX_WIDTH * SDL_HOST_MAX_HEIGHT * 4];
} SdlHostMailbox;

static inline LONG SdlHostLoad(volatile LONG *word)
{
    return InterlockedCompareExchange(word, 0, 0);
}
