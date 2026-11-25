/* Bridge XInput callers to the controller already owned by the Xbox host.

   While the host mailbox is open, user index 0 stays attached for the whole
   session, with neutral state until the host publishes a connected pad. A
   console session always has a controller, and callers such as SDL query
   XInputGetCapabilities once at startup and re-scan only on device-change
   notifications, which a hosted pad never raises. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stddef.h>
#include "../shared/sdl-host-protocol.h"
#include "loader-trace-output.h"

#define TRACE(record) AppendTrace(record "\r\n", sizeof(record "\r\n") - 1)

typedef DWORD (WINAPI *GetState)(DWORD, XINPUT_STATE *);
typedef DWORD (WINAPI *GetCapabilities)(DWORD, DWORD, XINPUT_CAPABILITIES *);
typedef DWORD (WINAPI *SetState)(DWORD, XINPUT_VIBRATION *);
static INIT_ONCE configured = INIT_ONCE_STATIC_INIT;
static SRWLOCK stateLock = SRWLOCK_INIT;
static SdlHostMailbox *mailbox;
static GetState nativeGetState;
static GetCapabilities nativeGetCapabilities;
static SetState nativeSetState;
static XINPUT_STATE latest;
static BOOL connected;
static volatile LONG slotObserved;
static BOOL connectionObserved, activityObserved;

static BOOL CALLBACK OpenController(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    WCHAR path[1024];
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", path, ARRAYSIZE(path));
    if (!length && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        HMODULE system = GetModuleHandleW(L"xinput1_4.dll");
        if (!system) system = LoadLibraryExW(L"xinput1_4.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (system) {
            nativeGetState = (GetState)(void *)GetProcAddress(system, "XInputGetState");
            nativeGetCapabilities = (GetCapabilities)(void *)GetProcAddress(system, "XInputGetCapabilities");
            nativeSetState = (SetState)(void *)GetProcAddress(system, "XInputSetState");
        }
        return TRUE;
    }
    if (!length || length >= ARRAYSIZE(path)) return TRUE;

    HANDLE file = CreateFile2FromAppW(path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, NULL);
    if (file == INVALID_HANDLE_VALUE) return TRUE;
    const SIZE_T lengthNeeded = offsetof(SdlHostMailbox, keyboard);
    LARGE_INTEGER size;
    const BOOL valid = GetFileSizeEx(file, &size) && size.QuadPart >= (LONGLONG)lengthNeeded;
    HANDLE mapping = valid ? CreateFileMappingFromApp(file, NULL, PAGE_READWRITE, 0, NULL) : NULL;
    CloseHandle(file);
    if (!mapping) return TRUE;
    mailbox = MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, lengthNeeded);
    CloseHandle(mapping);
    if (mailbox && SdlHostLoad(&mailbox->magic) != SDL_HOST_MAGIC) {
        UnmapViewOfFile(mailbox);
        mailbox = NULL;
    }
    return TRUE;
}

static BYTE Trigger(LONG value) {
    return (BYTE)(((value + 32768) * 255 + 32767) / 65535);
}

static SHORT ThumbY(LONG value) {
    if (value == 32767) return -32768;
    return (SHORT)(value == -32768 ? 32767 : -value);
}

static void UpdateController(SdlHostGamepad *pad) {
    SdlHostGamepadState sample;
    const LONG serial = SdlHostReadGamepad(pad, (LONG)latest.dwPacketNumber, &sample);
    if (!serial) return;
    const LONG buttons = sample.buttons;
    const LONG *axes = sample.axes;

    static const WORD masks[] = {
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_BACK, 0, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB,
        XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
        XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT
    };
    _Static_assert(ARRAYSIZE(masks) == SDL_HOST_PAD_BUTTONS, "host gamepad button order");
    WORD mapped = 0;
    for (unsigned i = 0; i < ARRAYSIZE(masks); ++i)
        if (buttons & (1 << i)) mapped |= masks[i];

    connected = (buttons & SDL_HOST_PAD_CONNECTED) != 0;
    latest.dwPacketNumber = (DWORD)serial;
    latest.Gamepad = (XINPUT_GAMEPAD){mapped, Trigger(axes[4]), Trigger(axes[5]),
        (SHORT)axes[0], ThumbY(axes[1]), (SHORT)axes[2], ThumbY(axes[3])};
    InterlockedExchange(&pad->ack, serial);
}

static void ConfigureController(void) {
    const DWORD previous = GetLastError();
    InitOnceExecuteOnce(&configured, OpenController, NULL, NULL);
    SetLastError(previous);
}

DWORD WINAPI XboxXInputGetState(DWORD index, XINPUT_STATE *state) {
    if (!state || index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    ConfigureController();
    if (nativeGetState) return nativeGetState(index, state);
    const LONG slot = index ? 2 : 1;
    if (!(slotObserved & slot) && !(InterlockedOr(&slotObserved, slot) & slot)) {
        if (index) TRACE("input xinput-polled=other-slot");
        else if (mailbox) TRACE("input xinput-polled=hosted-slot");
        else TRACE("input xinput-polled=mailbox-unavailable");
    }
    if (index || !mailbox || SdlHostLoad(&mailbox->quit)) return ERROR_DEVICE_NOT_CONNECTED;

    AcquireSRWLockExclusive(&stateLock);
    UpdateController(&mailbox->gamepad);
    const DWORD result = ERROR_SUCCESS;
    *state = latest;
    if (!connected) state->Gamepad = (XINPUT_GAMEPAD){0};
    BOOL firstConnection = FALSE, firstActivity = FALSE;
    if (connected) {
        firstConnection = !connectionObserved;
        connectionObserved = TRUE;
        const XINPUT_GAMEPAD pad = latest.Gamepad;
        if (!activityObserved && (pad.wButtons || pad.bLeftTrigger || pad.bRightTrigger ||
                pad.sThumbLX || pad.sThumbLY || pad.sThumbRX || pad.sThumbRY)) {
            firstActivity = activityObserved = TRUE;
        }
    }
    ReleaseSRWLockExclusive(&stateLock);
    if (firstConnection) TRACE("input xinput-sample=connected");
    if (firstActivity) TRACE("input xinput-sample=active");
    return result;
}

/* Record the first capability queries with their slot and result, and whether
   the launcher's SDL backend overrides reached this process. */
static void TraceCapabilities(DWORD index, DWORD result) {
    static volatile LONG count;
    const LONG call = InterlockedIncrement(&count);
    if (call > 16) return;
    const DWORD error = GetLastError();
    char line[96] = "input xinput-capabilities slot=0 result=0x00000000";
    line[31] = (char)('0' + (index & 7));
    for (int digit = 0; digit < 8; ++digit) line[42 + digit] = "0123456789ABCDEF"[(result >> (28 - 4 * digit)) & 15];
    unsigned length = 50;
    if (call == 1) {
        WCHAR value[4];
        const DWORD size = GetEnvironmentVariableW(L"SDL_JOYSTICK_WGI", value, ARRAYSIZE(value));
        const char *suffix = size == 1 && value[0] == L'0' ? " sdl-backends=hosted-only" : " sdl-backends=default";
        while (*suffix) line[length++] = *suffix++;
    }
    line[length++] = '\r';
    line[length++] = '\n';
    AppendTrace(line, length);
    SetLastError(error);
}

DWORD WINAPI XboxXInputGetCapabilities(DWORD index, DWORD flags, XINPUT_CAPABILITIES *capabilities) {
    if (!capabilities || index >= XUSER_MAX_COUNT || (flags != 0 && flags != XINPUT_FLAG_GAMEPAD))
        return ERROR_BAD_ARGUMENTS;
    ConfigureController();
    if (nativeGetCapabilities) return nativeGetCapabilities(index, flags, capabilities);
    const DWORD result = index || !mailbox || SdlHostLoad(&mailbox->quit) ? ERROR_DEVICE_NOT_CONNECTED : ERROR_SUCCESS;
    TraceCapabilities(index, result);
    if (result != ERROR_SUCCESS) return result;

    *capabilities = (XINPUT_CAPABILITIES){
        XINPUT_DEVTYPE_GAMEPAD, XINPUT_DEVSUBTYPE_GAMEPAD, 0,
        {XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y |
         XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB |
         XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_DPAD_UP |
         XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT,
         255, 255, 32767, 32767, 32767, 32767},
        {0, 0}
    };
    return ERROR_SUCCESS;
}

DWORD WINAPI XboxXInputSetState(DWORD index, XINPUT_VIBRATION *vibration) {
    if (!vibration || index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    ConfigureController();
    if (nativeSetState) return nativeSetState(index, vibration);
    if (index || !mailbox || SdlHostLoad(&mailbox->quit)) return ERROR_DEVICE_NOT_CONNECTED;
    // The hosted capabilities advertise zero motor speeds until the host supports vibration.
    return vibration->wLeftMotorSpeed || vibration->wRightMotorSpeed ? ERROR_NOT_SUPPORTED : ERROR_SUCCESS;
}

DWORD WINAPI XboxXInputGetDSoundAudioDeviceGuids(DWORD index, GUID *render, GUID *capture) {
    if (!render || !capture) return ERROR_BAD_ARGUMENTS;
    XINPUT_CAPABILITIES capabilities;
    const DWORD result = XboxXInputGetCapabilities(index, XINPUT_FLAG_GAMEPAD, &capabilities);
    if (result != ERROR_SUCCESS) return result;
    // Modern Windows also reports no legacy DirectSound devices through this API.
    *render = (GUID){0};
    *capture = (GUID){0};
    return ERROR_SUCCESS;
}
