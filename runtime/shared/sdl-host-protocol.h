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
    SDL_HOST_POINTER_MAX = 4095,
    SDL_HOST_PAD_BUTTONS = 15,
    SDL_HOST_PAD_AXES = 6,
    SDL_HOST_PAD_CONNECTED = 1 << 16,
    SDL_HOST_PAD_BUTTON_MASK = (1 << SDL_HOST_PAD_BUTTONS) - 1,
    SDL_HOST_KEY_LIMIT = 512,
    SDL_HOST_KEY_QUEUE_SIZE = 256,
    SDL_HOST_TEXT_BYTES = 32
};

typedef struct SdlHostFrame {
    uint32_t width;
    uint32_t height;
    uint32_t sequence;
    uint32_t input_ack;
    uint32_t input_events;
} SdlHostFrame;

/* SDL_GameController ordering. Virtual trigger axes rest at -32768. */
typedef struct SdlHostGamepad {
    volatile LONG serial;
    volatile LONG buttons;
    volatile LONG axes[SDL_HOST_PAD_AXES];
    volatile LONG ack;
} SdlHostGamepad;

typedef struct SdlHostGamepadState {
    LONG buttons;
    LONG axes[SDL_HOST_PAD_AXES];
} SdlHostGamepadState;

/* The host publishes complete entries with write. SDL owns read. */
typedef struct SdlHostKey {
    uint16_t scan;
    uint8_t down;
    char text[SDL_HOST_TEXT_BYTES];
} SdlHostKey;

typedef struct SdlHostKeyboard {
    volatile LONG available;
    volatile LONG read;
    volatile LONG write;
    volatile LONG reset;
    SdlHostKey entries[SDL_HOST_KEY_QUEUE_SIZE];
} SdlHostKeyboard;

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
    SdlHostGamepad gamepad;
    SdlHostKeyboard keyboard;
} SdlHostMailbox;

static inline LONG SdlHostLoad(volatile LONG *word)
{
    return InterlockedCompareExchange(word, 0, 0);
}

static inline SdlHostGamepadState SdlHostIdleGamepad(LONG buttons)
{
    SdlHostGamepadState state = {buttons & SDL_HOST_PAD_CONNECTED, {0, 0, 0, 0, -32768, -32768}};
    return state;
}

/* The active host owns publication. Each reader acknowledges after applying
   the complete state through its own API. */
static inline LONG SdlHostPublishGamepad(SdlHostGamepad *pad, const SdlHostGamepadState *state)
{
    InterlockedIncrement(&pad->serial);
    InterlockedExchange(&pad->buttons, state->buttons & (SDL_HOST_PAD_BUTTON_MASK | SDL_HOST_PAD_CONNECTED));
    for (unsigned i = 0; i < SDL_HOST_PAD_AXES; ++i) {
        const LONG axis = state->axes[i];
        InterlockedExchange(&pad->axes[i], axis < -32768 ? -32768 : axis > 32767 ? 32767 : axis);
    }
    LONG serial = InterlockedIncrement(&pad->serial);
    /* Zero denotes a mailbox with no published sample. */
    if (!serial) {
        InterlockedIncrement(&pad->serial);
        serial = InterlockedIncrement(&pad->serial);
    }
    return serial;
}

static inline LONG SdlHostReadGamepad(SdlHostGamepad *pad, LONG previous, SdlHostGamepadState *state)
{
    const LONG serial = SdlHostLoad(&pad->serial);
    if (!serial || (serial & 1) || serial == previous) return 0;
    SdlHostGamepadState sample;
    sample.buttons = SdlHostLoad(&pad->buttons);
    for (unsigned i = 0; i < SDL_HOST_PAD_AXES; ++i) sample.axes[i] = SdlHostLoad(&pad->axes[i]);
    if (serial != SdlHostLoad(&pad->serial)) return 0;
    *state = sample;
    return serial;
}
