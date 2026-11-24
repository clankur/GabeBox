/* Exercise the normal hosted XInput adapter against a deterministic app-local mailbox. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stddef.h>
#include "../../runtime/shared/child-process.h"
#include "../../runtime/shared/sdl-host-protocol.h"
#include "../../runtime/win32/loader-trace-output.h"

#define CHECKPOINT(name) AppendTrace("input hosted-xinput-control=" name "\r\n", \
    sizeof("input hosted-xinput-control=" name "\r\n") - 1)

static WCHAR *AppendName(WCHAR *end, WCHAR const *name) {
    while (*name) *end++ = *name++;
    *end = 0;
    return end;
}

static BOOL SetFixtureTrace(WCHAR *path, DWORD prefix) {
    AppendName(path + prefix, L"hosted-xinput-control.txt");
    HANDLE file = CreateFile2FromAppW(path, GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle(file);
    return SetEnvironmentVariableW(L"XBOX_WIN32_LOADER_TRACE", path);
}

static void BeginInput(SdlHostGamepad *pad, LONG buttons, LONG const axes[SDL_HOST_PAD_AXES]) {
    InterlockedIncrement(&pad->serial);
    InterlockedExchange(&pad->buttons, buttons);
    for (UINT i = 0; i < SDL_HOST_PAD_AXES; ++i) InterlockedExchange(&pad->axes[i], axes[i]);
}

static BOOL IsNeutral(XINPUT_GAMEPAD const *pad) {
    return !pad->wButtons && !pad->bLeftTrigger && !pad->bRightTrigger && !pad->sThumbLX &&
        !pad->sThumbLY && !pad->sThumbRX && !pad->sThumbRY;
}

static unsigned CheckInput(SdlHostGamepad *pad) {
    const LONG neutral[SDL_HOST_PAD_AXES] = {0, 0, 0, 0, -32768, -32768};
    const LONG moved[SDL_HOST_PAD_AXES] = {12345, -32768, -23456, 32767, -32768, 32767};
    XINPUT_STATE state;
    XINPUT_CAPABILITIES caps;
    if (XInputGetState(0, &state) != ERROR_DEVICE_NOT_CONNECTED ||
            XInputGetCapabilities(0, XINPUT_FLAG_GAMEPAD, &caps) != ERROR_DEVICE_NOT_CONNECTED ||
            XInputGetState(XUSER_MAX_COUNT, &state) != ERROR_BAD_ARGUMENTS) return 0x6f02;
    CHECKPOINT("initial-disconnected");

    BeginInput(pad, SDL_HOST_PAD_CONNECTED, neutral);
    const LONG initial = InterlockedIncrement(&pad->serial);
    if (XInputGetCapabilities(0, XINPUT_FLAG_GAMEPAD, &caps) != ERROR_SUCCESS ||
            caps.Type != XINPUT_DEVTYPE_GAMEPAD || caps.SubType != XINPUT_DEVSUBTYPE_GAMEPAD ||
            caps.Gamepad.wButtons != 0xf3ff || caps.Gamepad.bLeftTrigger != 255 || caps.Gamepad.bRightTrigger != 255 ||
            caps.Gamepad.sThumbLX != 32767 || caps.Gamepad.sThumbLY != 32767 ||
            caps.Gamepad.sThumbRX != 32767 || caps.Gamepad.sThumbRY != 32767 ||
            caps.Vibration.wLeftMotorSpeed || caps.Vibration.wRightMotorSpeed ||
            SdlHostLoad(&pad->ack) || XInputGetState(1, &state) != ERROR_DEVICE_NOT_CONNECTED) return 0x6f03;
    CHECKPOINT("capabilities");
    if (XInputGetState(0, &state) != ERROR_SUCCESS || state.dwPacketNumber != (DWORD)initial ||
            SdlHostLoad(&pad->ack) != initial || !IsNeutral(&state.Gamepad)) return 0x6f04;
    CHECKPOINT("neutral-ack");

    // SDL A/Y/Back/Guide/Start/left stick/left shoulder/D-pad up/right.
    const LONG buttons = (1 << 0) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6) |
        (1 << 7) | (1 << 9) | (1 << 11) | (1 << 14);
    BeginInput(pad, SDL_HOST_PAD_CONNECTED | buttons, moved);
    if (XInputGetState(0, &state) != ERROR_SUCCESS || state.dwPacketNumber != (DWORD)initial ||
            SdlHostLoad(&pad->ack) != initial || !IsNeutral(&state.Gamepad)) return 0x6f05;
    CHECKPOINT("partial-publication-ignored");
    const LONG motion = InterlockedIncrement(&pad->serial);
    const WORD expected = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START |
        XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT;
    if (XInputGetState(0, &state) != ERROR_SUCCESS || state.dwPacketNumber != (DWORD)motion ||
            SdlHostLoad(&pad->ack) != motion || state.Gamepad.wButtons != expected ||
            state.Gamepad.sThumbLX != 12345 || state.Gamepad.sThumbLY != 32767 ||
            state.Gamepad.sThumbRX != -23456 || state.Gamepad.sThumbRY != -32768 ||
            state.Gamepad.bLeftTrigger != 0 || state.Gamepad.bRightTrigger != 255) return 0x6f06;
    CHECKPOINT("movement-ack");

    BeginInput(pad, 0, neutral);
    const LONG disconnected = InterlockedIncrement(&pad->serial);
    if (XInputGetCapabilities(0, 0, &caps) != ERROR_DEVICE_NOT_CONNECTED ||
            XInputGetState(0, &state) != ERROR_DEVICE_NOT_CONNECTED ||
            SdlHostLoad(&pad->ack) != disconnected) return 0x6f07;
    CHECKPOINT("disconnect-ack");
    return PROBE_EXPECTED_CHILD_EXIT;
}

static unsigned RunControl(void) {
    WCHAR path[512];
    const DWORD prefix = GetTempPathW(ARRAYSIZE(path), path);
    if (!prefix || prefix >= ARRAYSIZE(path) - 48 || !SetFixtureTrace(path, prefix)) return 0x6f01;
    WCHAR *end = AppendName(path + prefix, L"xbox-hosted-xinput-");
    const DWORD token = GetCurrentProcessId();
    for (int shift = 28; shift >= 0; shift -= 4) *end++ = L"0123456789abcdef"[(token >> shift) & 15];
    AppendName(end, L".bin");
    CREATEFILE2_EXTENDED_PARAMETERS options = {0};
    options.dwSize = sizeof(options);
    options.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
    options.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE;
    HANDLE file = CreateFile2FromAppW(path, GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, CREATE_NEW, &options);
    if (file == INVALID_HANDLE_VALUE) return 0x6f01;
    const DWORD bytes = offsetof(SdlHostMailbox, keyboard);
    HANDLE mapping = CreateFileMappingFromApp(file, NULL, PAGE_READWRITE, bytes, NULL);
    SdlHostMailbox *mailbox = mapping ? MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, bytes) : NULL;
    unsigned result = 0x6f01;
    if (mailbox && SetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", path)) {
        InterlockedExchange(&mailbox->magic, SDL_HOST_MAGIC);
        CHECKPOINT("mailbox-created");
        result = CheckInput(&mailbox->gamepad);
        SetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", NULL);
    }
    if (mailbox) UnmapViewOfFile(mailbox);
    if (mapping) CloseHandle(mapping);
    CloseHandle(file);
    return result;
}

void WINAPI ChildEntry(void) {
    ExitProcess(RunControl());
}
