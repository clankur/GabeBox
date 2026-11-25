#pragma once
#include "sdl-host-protocol.h"
#include "../events/SDL_keyboard_c.h"
#include "../events/SDL_mouse_c.h"
#include "../events/SDL_events_c.h"
#include "../events/SDL_windowevents_c.h"
#include "SDL_timer.h"
#include "SDL.h"
#include "SDL_gamecontroller.h"
#include <stddef.h>

static SdlHostMailbox *xbox_host_mailbox;
static Uint32 xbox_host_window;
static SDL_bool xbox_host_has_gamepad;
static SDL_bool xbox_host_has_keyboard;
static SDL_Joystick *xbox_host_gamepad;
static SDL_JoystickID xbox_host_gamepad_id = -1;

static void XboxSdlOpenHost(void)
{
    static SDL_bool tried;
    WCHAR path[1024];
    DWORD length;
    HANDLE file, mapping;
    LARGE_INTEGER size;
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
        if (GetFileSizeEx(file, &size) && size.QuadPart >= sizeof(SdlHostMailbox)) {
            xbox_host_mailbox = MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, sizeof(SdlHostMailbox));
            xbox_host_has_keyboard = xbox_host_mailbox != NULL;
        }
        if (!xbox_host_mailbox) {
            xbox_host_mailbox = MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, offsetof(SdlHostMailbox, keyboard));
        }
        xbox_host_has_gamepad = xbox_host_mailbox != NULL;
        if (!xbox_host_mailbox) {
            xbox_host_mailbox = MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, offsetof(SdlHostMailbox, gamepad));
        }
        CloseHandle(mapping);
    }
    CloseHandle(file);
    if (xbox_host_mailbox && SdlHostLoad(&xbox_host_mailbox->magic) != SDL_HOST_MAGIC) {
        UnmapViewOfFile(xbox_host_mailbox);
        xbox_host_mailbox = NULL;
    }
    XboxSdlTrace(xbox_host_mailbox ? "sdl_host=mapped\n" : "sdl_host=map-error\n");
}

static void XboxSdlPollHostKeyboard(SdlHostKeyboard *keyboard, SDL_Window *window)
{
    static Uint32 last_reset;
    Uint32 reset = (Uint32)SdlHostLoad(&keyboard->reset);
    Uint32 read = (Uint32)SdlHostLoad(&keyboard->read);
    Uint32 write = (Uint32)SdlHostLoad(&keyboard->write);
    if (!SdlHostLoad(&keyboard->available)) InterlockedExchange(&keyboard->available, 1);
    if (last_reset != reset) {
        SDL_ResetKeyboard();
        last_reset = reset;
        if ((Sint32)(reset - read) > 0) read = reset;
    }
    if (read != write) SDL_SetKeyboardFocus(window);
    while (read != write) {
        const SdlHostKey *entry = &keyboard->entries[read++ % SDL_HOST_KEY_QUEUE_SIZE];
        if (entry->scan && entry->scan < SDL_NUM_SCANCODES) {
            SDL_SendKeyboardKey(entry->down ? SDL_PRESSED : SDL_RELEASED, (SDL_Scancode)entry->scan);
        } else if (!entry->scan && entry->text[0]) {
            SDL_SendKeyboardText(entry->text);
        }
    }
    InterlockedExchange(&keyboard->read, (LONG)read);
}

static void XboxSdlDetachGamepad(void)
{
    if (xbox_host_gamepad) SDL_JoystickClose(xbox_host_gamepad);
    xbox_host_gamepad = NULL;
    for (int device = 0; xbox_host_gamepad_id >= 0 && device < SDL_NumJoysticks(); ++device) {
        if (SDL_JoystickGetDeviceInstanceID(device) == xbox_host_gamepad_id) {
            SDL_JoystickDetachVirtual(device);
            break;
        }
    }
    xbox_host_gamepad_id = -1;
}

static void XboxSdlPollHostGamepad(SdlHostGamepad *pad)
{
    SdlHostGamepadState sample;
    const LONG serial = SdlHostReadGamepad(pad, SdlHostLoad(&pad->ack), &sample);
    if (!serial || !SDL_WasInit(SDL_INIT_JOYSTICK)) return;
    const LONG buttons = sample.buttons;
    const LONG *axes = sample.axes;
    if (!(buttons & SDL_HOST_PAD_CONNECTED)) {
        XboxSdlDetachGamepad();
    } else {
        if (!xbox_host_gamepad) {
            SDL_VirtualJoystickDesc desc;
            SDL_zero(desc);
            desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
            desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
            desc.naxes = SDL_HOST_PAD_AXES;
            desc.nbuttons = SDL_HOST_PAD_BUTTONS;
            desc.axis_mask = (1 << SDL_HOST_PAD_AXES) - 1;
            desc.button_mask = (1 << SDL_HOST_PAD_BUTTONS) - 1;
            desc.name = "Xbox Controller";
            int device = SDL_JoystickAttachVirtualEx(&desc);
            if (device < 0) return;
            xbox_host_gamepad_id = SDL_JoystickGetDeviceInstanceID(device);
            xbox_host_gamepad = SDL_JoystickOpen(device);
            if (!xbox_host_gamepad) {
                XboxSdlDetachGamepad();
                return;
            }
            XboxSdlTrace("sdl_host_gamepad=attached\n");
        }
        for (int i = 0; i < SDL_HOST_PAD_AXES; ++i) SDL_JoystickSetVirtualAxis(xbox_host_gamepad, i, (Sint16)axes[i]);
        for (int i = 0; i < SDL_HOST_PAD_BUTTONS; ++i) SDL_JoystickSetVirtualButton(xbox_host_gamepad, i, (buttons >> i) & 1);
        SDL_JoystickUpdate();
        for (int i = 0; i < SDL_HOST_PAD_AXES; ++i) if (SDL_JoystickGetAxis(xbox_host_gamepad, i) != axes[i]) return;
        for (int i = 0; i < SDL_HOST_PAD_BUTTONS; ++i) if (SDL_JoystickGetButton(xbox_host_gamepad, i) != ((buttons >> i) & 1)) return;
    }
    InterlockedExchange(&pad->ack, serial);
}

SDL_bool XboxSdlBindHostWindow(SDL_Window *window)
{
    XboxSdlOpenHost();
    if (!window || !xbox_host_mailbox || SdlHostLoad(&xbox_host_mailbox->quit)) return SDL_FALSE;
    xbox_host_window = SDL_GetWindowID(window);
    return SDL_TRUE;
}

Uint8 *XboxSdlBeginHostFrame(SDL_Window *window, int width, int height)
{
    SdlHostMailbox *mailbox;
    XboxSdlOpenHost();
    mailbox = xbox_host_mailbox;
    if (!mailbox || !window || SdlHostLoad(&mailbox->quit) ||
        width < 1 || width > SDL_HOST_MAX_WIDTH || height < 1 || height > SDL_HOST_MAX_HEIGHT) return NULL;
    /* Leave the published frame ready until the player consumes it. */
    if (InterlockedCompareExchange(&mailbox->frame_state, SDL_HOST_WRITING, SDL_HOST_EMPTY) != SDL_HOST_EMPTY)
        return NULL;
    xbox_host_window = SDL_GetWindowID(window);
    mailbox->frame.width = (uint32_t)width;
    mailbox->frame.height = (uint32_t)height;
    return mailbox->pixels;
}

void XboxSdlEndHostFrame(SDL_bool success)
{
    SdlHostMailbox *mailbox = xbox_host_mailbox;
    if (!success) {
        InterlockedExchange(&mailbox->frame_state, SDL_HOST_EMPTY);
        return;
    }
    mailbox->frame.sequence = (uint32_t)InterlockedIncrement(&mailbox->frame_sequence);
    mailbox->frame.input_ack = (uint32_t)SdlHostLoad(&mailbox->input_ack);
    mailbox->frame.input_events = (uint32_t)SdlHostLoad(&mailbox->input_events);
    InterlockedExchange(&mailbox->frame_state, SDL_HOST_READY);
}

static void XboxSdlPublishHostFrame(SDL_Renderer *renderer)
{
    int width, height;
    SDL_Window *window = SDL_RenderGetWindow(renderer);
    if (!window || SDL_GetRenderTarget(renderer) || SDL_GetRendererOutputSize(renderer, &width, &height) < 0) {
        return;
    }
    Uint8 *pixels = XboxSdlBeginHostFrame(window, width, height);
    if (pixels) {
        XboxSdlEndHostFrame(SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGBA32, pixels, width * 4) == 0);
    }
}

void XboxSdlPollHostInput(void)
{
    static LONG last_serial;
    static LONG last_pointer = -1;
    static LONG last_keys;
    static const SDL_Scancode keys[] = {
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_SPACE
    };
    SdlHostMailbox *mailbox = xbox_host_mailbox;
    SDL_Window *window;
    LONG serial, pointer, pressed, events = 0;
    if (!mailbox) return;
    if (InterlockedCompareExchange(&mailbox->quit, 0, 1) == 1) {
        // Give the game the same window-close event it receives on desktop.
        // SDL also posts SDL_QUIT for the last window when its normal hint permits.
        window = SDL_GetWindowFromID(xbox_host_window);
        if (window) SDL_SendWindowEvent(window, SDL_WINDOWEVENT_CLOSE, 0, 0);
        else SDL_SendQuit();
        XboxSdlTrace("sdl_host_close=delivered\n");
    }
    if (xbox_host_has_gamepad) XboxSdlPollHostGamepad(&mailbox->gamepad);
    window = SDL_GetWindowFromID(xbox_host_window);
    if (!window) return;
    if (xbox_host_has_keyboard) XboxSdlPollHostKeyboard(&mailbox->keyboard, window);
    serial = SdlHostLoad(&mailbox->input_serial);
    if (serial == last_serial || (serial & 1)) return;
    pointer = SdlHostLoad(&mailbox->input_pointer) & (0xffffff | SDL_HOST_POINTER_LEFT);
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
