/* Exercise the normal hosted XInput adapter against a deterministic app-local mailbox. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stddef.h>
#include "../../runtime/shared/child-process.h"
#include "../../runtime/shared/sdl-host-protocol.h"
#include "../../runtime/win32/loader-trace-output.h"
#ifdef PROBE_XINPUT_RUMBLE
#include "../../runtime/shared/sdl-rumble-protocol.h"
#endif

#define CHECKPOINT(name) AppendTrace("input hosted-xinput-control=" name "\r\n", \
    sizeof("input hosted-xinput-control=" name "\r\n") - 1)

/* Slot 0 without a connected host pad: detached by default, or attached and neutral
   for the whole session when the profile makes the hosted pad the only controller. */
#ifdef PROBE_XINPUT_SESSION
#define DETACHED ERROR_SUCCESS
#else
#define DETACHED ERROR_DEVICE_NOT_CONNECTED
#endif

#ifdef PROBE_XINPUT_LOADER
static DWORD (WINAPI *GetPadState)(DWORD, XINPUT_STATE *);
static DWORD (WINAPI *GetPadCapabilities)(DWORD, DWORD, XINPUT_CAPABILITIES *);
static DWORD (WINAPI *SetPadState)(DWORD, XINPUT_VIBRATION *);

static BOOL RouteMatches(LPCWSTR name, BOOL hosted) {
    HMODULE module = LoadLibraryW(name);
    const BOOL selected = module && module == GetModuleHandleW(L"XboxWin32XInput910.dll");
    return (!module || FreeLibrary(module)) && selected == hosted;
}

static unsigned SelectControllerModule(HMODULE *selected) {
    if (!SetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_XINPUT", L"0") ||
            !RouteMatches(L"xinput1_4.dll", FALSE) || !RouteMatches(L"xinput1_3.dll", FALSE) ||
            !RouteMatches(L"xinput9_1_0.dll", TRUE)) return 0x6f0a;
    if (!SetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_XINPUT", L"1") ||
            !RouteMatches(L".\\xinput1_4.dll", FALSE) ||
            !RouteMatches(L"XInput1_3.dll", TRUE) || !RouteMatches(L"xinput9_1_0.dll", TRUE)) return 0x6f0b;
    SetLastError(ERROR_RETRY);
    *selected = LoadLibraryW(L"XInput1_4.dll");
    if (!*selected || GetLastError() != ERROR_RETRY ||
            *selected != GetModuleHandleW(L"XboxWin32XInput910.dll")) return 0x6f0c;
    if (GetProcAddress(*selected, MAKEINTRESOURCEA(100)) || GetLastError() != ERROR_PROC_NOT_FOUND ||
            GetProcAddress(*selected, MAKEINTRESOURCEA(108)) || GetLastError() != ERROR_PROC_NOT_FOUND ||
            GetProcAddress(*selected, "XInputGetBatteryInformation")) return 0x6f0d;
    GetPadState = (void *)GetProcAddress(*selected, "XInputGetState");
    GetPadCapabilities = (void *)GetProcAddress(*selected, "XInputGetCapabilities");
    SetPadState = (void *)GetProcAddress(*selected, "XInputSetState");
    if (!GetPadState || !GetPadCapabilities || !SetPadState) return 0x6f0e;
    CHECKPOINT("dynamic-resolution");
    return 0;
}
#else
#define GetPadState XInputGetState
#define GetPadCapabilities XInputGetCapabilities
#define SetPadState XInputSetState
#endif

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
    XINPUT_VIBRATION stop = {0};
    if (GetPadState(0, &state) != DETACHED || (DETACHED == ERROR_SUCCESS && !IsNeutral(&state.Gamepad)) ||
            GetPadCapabilities(0, XINPUT_FLAG_GAMEPAD, &caps) != DETACHED || SdlHostLoad(&pad->ack) ||
            GetPadState(XUSER_MAX_COUNT, &state) != ERROR_BAD_ARGUMENTS) return 0x6f02;
#ifdef PROBE_XINPUT_SESSION
    // The hosted slot is attached for the whole session and neutral until the host publishes.
    CHECKPOINT("initial-attached");
#else
    CHECKPOINT("initial-disconnected");
#endif

    BeginInput(pad, SDL_HOST_PAD_CONNECTED, neutral);
    if (GetPadCapabilities(0, XINPUT_FLAG_GAMEPAD, &caps) != DETACHED ||
            SetPadState(0, &stop) != DETACHED ||
            GetPadState(0, &state) != DETACHED || SdlHostLoad(&pad->ack)) return 0x6f08;
    CHECKPOINT("partial-connect-ignored");
    const LONG initial = InterlockedIncrement(&pad->serial);
    if (GetPadCapabilities(0, XINPUT_FLAG_GAMEPAD, &caps) != ERROR_SUCCESS ||
            caps.Type != XINPUT_DEVTYPE_GAMEPAD || caps.SubType != XINPUT_DEVSUBTYPE_GAMEPAD ||
            caps.Gamepad.wButtons != 0xf3ff || caps.Gamepad.bLeftTrigger != 255 || caps.Gamepad.bRightTrigger != 255 ||
            caps.Gamepad.sThumbLX != 32767 || caps.Gamepad.sThumbLY != 32767 ||
            caps.Gamepad.sThumbRX != 32767 || caps.Gamepad.sThumbRY != 32767 ||
            caps.Vibration.wLeftMotorSpeed || caps.Vibration.wRightMotorSpeed ||
            SdlHostLoad(&pad->ack) || GetPadState(1, &state) != ERROR_DEVICE_NOT_CONNECTED) return 0x6f03;
    CHECKPOINT("capabilities");
    if (GetPadState(0, &state) != ERROR_SUCCESS || state.dwPacketNumber != (DWORD)initial ||
            SdlHostLoad(&pad->ack) != initial || !IsNeutral(&state.Gamepad)) return 0x6f04;
    CHECKPOINT("neutral-ack");

    // SDL A/Y/Back/Guide/Start/left stick/left shoulder/D-pad up/right.
    const LONG buttons = (1 << 0) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6) |
        (1 << 7) | (1 << 9) | (1 << 11) | (1 << 14);
    BeginInput(pad, SDL_HOST_PAD_CONNECTED | buttons, moved);
    if (GetPadState(0, &state) != ERROR_SUCCESS || state.dwPacketNumber != (DWORD)initial ||
            SdlHostLoad(&pad->ack) != initial || !IsNeutral(&state.Gamepad)) return 0x6f05;
    CHECKPOINT("partial-publication-ignored");
    const LONG motion = InterlockedIncrement(&pad->serial);
    const WORD expected = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START |
        XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT;
    if (GetPadState(0, &state) != ERROR_SUCCESS || state.dwPacketNumber != (DWORD)motion ||
            SdlHostLoad(&pad->ack) != motion || state.Gamepad.wButtons != expected ||
            state.Gamepad.sThumbLX != 12345 || state.Gamepad.sThumbLY != 32767 ||
            state.Gamepad.sThumbRX != -23456 || state.Gamepad.sThumbRY != -32768 ||
            state.Gamepad.bLeftTrigger != 0 || state.Gamepad.bRightTrigger != 255) return 0x6f06;
    CHECKPOINT("movement-ack");

    BeginInput(pad, 0, neutral);
    if (GetPadCapabilities(0, XINPUT_FLAG_GAMEPAD, &caps) != ERROR_SUCCESS ||
            SetPadState(0, &stop) != ERROR_SUCCESS ||
            GetPadState(0, &state) != ERROR_SUCCESS || SdlHostLoad(&pad->ack) != motion) return 0x6f09;
    CHECKPOINT("partial-disconnect-ignored");
    const LONG disconnected = InterlockedIncrement(&pad->serial);
    if (GetPadCapabilities(0, 0, &caps) != DETACHED ||
            SetPadState(0, &stop) != DETACHED || SdlHostLoad(&pad->ack) != motion ||
            GetPadState(0, &state) != DETACHED || (DETACHED == ERROR_SUCCESS && !IsNeutral(&state.Gamepad)) ||
            SdlHostLoad(&pad->ack) != disconnected) return 0x6f07;
    CHECKPOINT("host-idle-neutral");
    return PROBE_EXPECTED_CHILD_EXIT;
}

#ifdef PROBE_XINPUT_RUMBLE
static unsigned CheckRumble(SdlHostGamepad *pad, SdlRumbleMailbox *rumble) {
    const SdlHostGamepadState idle = SdlHostIdleGamepad(SDL_HOST_PAD_CONNECTED);
    SdlHostPublishGamepad(pad, &idle);
    XINPUT_VIBRATION stop = {0}, pulse = {0x4321, 0xcdef};
    XINPUT_CAPABILITIES caps;
    if (GetPadCapabilities(0, 0, &caps) != ERROR_SUCCESS || caps.Vibration.wLeftMotorSpeed ||
            caps.Vibration.wRightMotorSpeed || SetPadState(0, &stop) != ERROR_SUCCESS ||
            SetPadState(0, &pulse) != ERROR_NOT_SUPPORTED) return 0x6f110000u;

    SdlRumbleArm(rumble);
    const LONG generation = SdlRumbleGeneration(rumble);
    if (GetPadCapabilities(0, 0, &caps) != ERROR_SUCCESS || caps.Vibration.wLeftMotorSpeed != 65535 ||
            caps.Vibration.wRightMotorSpeed != 65535 || SetPadState(0, &pulse) != ERROR_SUCCESS ||
            SdlRumbleRead(rumble) != (DWORD)MAKELONG(pulse.wLeftMotorSpeed, pulse.wRightMotorSpeed))
        return 0x6f120000u;
    if (SetPadState(0, &stop) != ERROR_SUCCESS || SdlRumbleRead(rumble)) return 0x6f130000u;

    SdlRumbleDisarm(rumble);
    if (SetPadState(0, &pulse) != ERROR_NOT_SUPPORTED || SetPadState(0, &stop) != ERROR_SUCCESS ||
            SdlRumbleRead(rumble)) return 0x6f140000u;
    SdlRumbleArm(rumble);
    if (SdlRumbleGeneration(rumble) == generation ||
            SdlRumblePublish(rumble, generation, pulse.wLeftMotorSpeed, pulse.wRightMotorSpeed) ||
            SdlRumbleRead(rumble)) return 0x6f150000u;
    const SdlHostGamepadState disconnected = SdlHostIdleGamepad(0);
    SdlHostPublishGamepad(pad, &disconnected);
    if (SetPadState(0, &pulse) != ERROR_DEVICE_NOT_CONNECTED || SdlRumbleRead(rumble)) return 0x6f160000u;
    CHECKPOINT("rumble-sidecar");
    return PROBE_EXPECTED_CHILD_EXIT;
}

static unsigned RunRumbleControl(SdlHostGamepad *pad, LPCWSTR basePath,
        CREATEFILE2_EXTENDED_PARAMETERS *options) {
    WCHAR path[512];
    WCHAR *end = AppendName(path, basePath);
    AppendName(end, SDL_RUMBLE_SUFFIX);
    HANDLE file = CreateFile2FromAppW(path, GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, CREATE_NEW, options);
    if (file == INVALID_HANDLE_VALUE) return 0x6f100000u | (GetLastError() & 0xffffu);
    HANDLE mapping = CreateFileMappingFromApp(file, NULL, PAGE_READWRITE, sizeof(SdlRumbleMailbox), NULL);
    SdlRumbleMailbox *box = mapping ? MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, sizeof(*box)) : NULL;
    unsigned result = 0x6f100000u | (GetLastError() & 0xffffu);
    if (box) {
        InterlockedExchange(&box->magic, SDL_RUMBLE_MAGIC);
        result = CheckInput(pad);
        if (result == PROBE_EXPECTED_CHILD_EXIT) result = CheckRumble(pad, box);
        SdlRumbleDisarm(box);
        UnmapViewOfFile(box);
    }
    if (mapping) CloseHandle(mapping);
    CloseHandle(file);
    return result;
}
#endif

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
#ifdef PROBE_XINPUT_LOADER
        HMODULE selected = NULL;
        result = SelectControllerModule(&selected);
        if (!result) result = CheckInput(&mailbox->gamepad);
        if (selected && !FreeLibrary(selected) && result == PROBE_EXPECTED_CHILD_EXIT) result = 0x6f0f;
        SetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_XINPUT", NULL);
#elif defined(PROBE_XINPUT_RUMBLE)
        result = RunRumbleControl(&mailbox->gamepad, path, &options);
#elif defined(PROBE_XINPUT_SESSION)
        result = SetEnvironmentVariableW(L"XBOX_WIN32_HOST_XINPUT", L"1") ? CheckInput(&mailbox->gamepad) : 0x6f01;
        SetEnvironmentVariableW(L"XBOX_WIN32_HOST_XINPUT", NULL);
#else
        result = CheckInput(&mailbox->gamepad);
#endif
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
