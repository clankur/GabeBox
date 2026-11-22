/* Bridge XInput callers to the controller already owned by the Xbox host. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stddef.h>
#include "../sdl-host-protocol.h"
#include "loader-trace-output.h"

#define TRACE(record) AppendTrace(record "\r\n", sizeof(record "\r\n") - 1)

typedef DWORD (WINAPI *GetState)(DWORD, XINPUT_STATE *);
typedef DWORD (WINAPI *GetCapabilities)(DWORD, DWORD, XINPUT_CAPABILITIES *);
static INIT_ONCE configured = INIT_ONCE_STATIC_INIT;
static SRWLOCK stateLock = SRWLOCK_INIT;
static SdlHostMailbox *mailbox;
static GetState nativeGetState;
static GetCapabilities nativeGetCapabilities;
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
    const LONG serial = SdlHostLoad(&pad->serial);
    if (!serial || (serial & 1) || latest.dwPacketNumber == (DWORD)serial) return;
    const LONG buttons = SdlHostLoad(&pad->buttons);
    LONG axes[SDL_HOST_PAD_AXES];
    for (unsigned i = 0; i < ARRAYSIZE(axes); ++i) axes[i] = SdlHostLoad(&pad->axes[i]);
    if (serial != SdlHostLoad(&pad->serial)) return;

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
    const DWORD result = connected ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
    BOOL firstConnection = FALSE, firstActivity = FALSE;
    if (connected) {
        *state = latest;
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

DWORD WINAPI XboxXInputGetCapabilities(DWORD index, DWORD flags, XINPUT_CAPABILITIES *capabilities) {
    if (!capabilities || index >= XUSER_MAX_COUNT || (flags != 0 && flags != XINPUT_FLAG_GAMEPAD))
        return ERROR_BAD_ARGUMENTS;
    ConfigureController();
    if (nativeGetCapabilities) return nativeGetCapabilities(index, flags, capabilities);
    if (index || !mailbox || SdlHostLoad(&mailbox->quit) ||
            !(SdlHostLoad(&mailbox->gamepad.buttons) & SDL_HOST_PAD_CONNECTED)) return ERROR_DEVICE_NOT_CONNECTED;

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
