#pragma once
#include "sdl-host-protocol.h"
#include "../events/SDL_keyboard_c.h"
#include "../events/SDL_mouse_c.h"
#include "../events/SDL_events_c.h"
#include "SDL_timer.h"

static SdlHostMailbox *xbox_host_mailbox;
static Uint32 xbox_host_window;

static void XboxSdlOpenHost(void)
{
    static SDL_bool tried;
    WCHAR path[1024];
    DWORD length;
    HANDLE file, mapping;
    if (tried) return;
    tried = SDL_TRUE;
    length = GetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", path, ARRAYSIZE(path));
    if (length == 0 || length >= ARRAYSIZE(path)) return;
    file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        XboxSdlTrace("sdl_host=open-error\n");
        return;
    }
    mapping = CreateFileMappingFromApp(file, NULL, PAGE_READWRITE, 0, NULL);
    if (mapping) {
        xbox_host_mailbox = MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, sizeof(SdlHostMailbox));
        CloseHandle(mapping);
    }
    CloseHandle(file);
    if (xbox_host_mailbox && SdlHostLoad(&xbox_host_mailbox->magic) != SDL_HOST_MAGIC) {
        UnmapViewOfFile(xbox_host_mailbox);
        xbox_host_mailbox = NULL;
    }
    XboxSdlTrace(xbox_host_mailbox ? "sdl_host=mapped\n" : "sdl_host=map-error\n");
}

static void XboxSdlPublishHostFrame(SDL_Renderer *renderer)
{
    static Uint32 last;
    SDL_Window *window;
    Uint32 now;
    int width, height;
    LONG old_state;
    SdlHostMailbox *mailbox;
    XboxSdlOpenHost();
    mailbox = xbox_host_mailbox;
    if (!mailbox || SdlHostLoad(&mailbox->quit)) return;
    now = SDL_GetTicks();
    if (now - last < 50 || SDL_GetRenderTarget(renderer)) return;
    window = SDL_RenderGetWindow(renderer);
    if (!window || SDL_GetRendererOutputSize(renderer, &width, &height) < 0 ||
        width < 1 || width > SDL_HOST_MAX_WIDTH || height < 1 || height > SDL_HOST_MAX_HEIGHT) return;
    old_state = SdlHostLoad(&mailbox->frame_state);
    if ((old_state != SDL_HOST_EMPTY && old_state != SDL_HOST_READY) ||
        InterlockedCompareExchange(&mailbox->frame_state, SDL_HOST_WRITING, old_state) != old_state) return;
    last = now;
    xbox_host_window = SDL_GetWindowID(window);
    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGBA32, mailbox->pixels, width * 4) < 0) {
        InterlockedExchange(&mailbox->frame_state, SDL_HOST_EMPTY);
        return;
    }
    mailbox->frame.width = (uint32_t)width;
    mailbox->frame.height = (uint32_t)height;
    mailbox->frame.sequence = (uint32_t)InterlockedIncrement(&mailbox->frame_sequence);
    mailbox->frame.input_ack = (uint32_t)SdlHostLoad(&mailbox->input_ack);
    mailbox->frame.input_events = (uint32_t)SdlHostLoad(&mailbox->input_events);
    InterlockedExchange(&mailbox->frame_state, SDL_HOST_READY);
}

void XboxSdlPollHostInput(void)
{
    static LONG last_serial;
    static LONG last_pointer = -1;
    static LONG last_keys;
    static SDL_bool requested_quit;
    static const SDL_Scancode keys[] = {
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_SPACE
    };
    SdlHostMailbox *mailbox = xbox_host_mailbox;
    SDL_Window *window;
    LONG serial, pointer, pressed, events = 0;
    if (!mailbox) return;
    if (!requested_quit && SdlHostLoad(&mailbox->quit)) {
        requested_quit = SDL_TRUE;
        SDL_SendQuit();
    }
    serial = SdlHostLoad(&mailbox->input_serial);
    if (serial == last_serial || (serial & 1)) return;
    window = SDL_GetWindowFromID(xbox_host_window);
    if (!window) return;
    pointer = SdlHostLoad(&mailbox->input_pointer);
    pressed = SdlHostLoad(&mailbox->input_keys);
    if (SdlHostLoad(&mailbox->input_serial) != serial) return;
    if (pointer != last_pointer) {
        int width, height;
        SDL_GetWindowSize(window, &width, &height);
        SDL_SetMouseFocus(window);
        events += SDL_SendMouseMotion(window, 0, 0, (pointer & 4095) * (width - 1) / 4095,
                                      ((pointer >> 12) & 4095) * (height - 1) / 4095);
        if ((pointer ^ (last_pointer < 0 ? 0 : last_pointer)) & (1 << 24)) {
            events += SDL_SendMouseButton(window, 0, (pointer & (1 << 24)) ? SDL_PRESSED : SDL_RELEASED, SDL_BUTTON_LEFT);
        }
        last_pointer = pointer;
    }
    SDL_SetKeyboardFocus(window);
    for (size_t i = 0; i < SDL_arraysize(keys); ++i) {
        if ((pressed ^ last_keys) & (1 << i)) {
            events += SDL_SendKeyboardKey((pressed & (1 << i)) ? SDL_PRESSED : SDL_RELEASED, keys[i]);
        }
    }
    last_keys = pressed;
    last_serial = serial;
    InterlockedExchangeAdd(&mailbox->input_events, events);
    InterlockedExchange(&mailbox->input_ack, serial);
}
