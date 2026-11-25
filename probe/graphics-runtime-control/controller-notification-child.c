#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cfgmgr32.h>
#include "../../runtime/shared/child-process.h"

typedef CONFIGRET (WINAPI *RegisterNotification)(PCM_NOTIFY_FILTER, PVOID, PCM_NOTIFY_CALLBACK, PHCMNOTIFICATION);
typedef CONFIGRET (WINAPI *UnregisterNotification)(HCMNOTIFICATION);
_Static_assert(sizeof(CM_NOTIFY_FILTER) == 0x1a0, "x64 Configuration Manager notification filter");

static const GUID hid = {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};

static void MakeFilter(CM_NOTIFY_FILTER *filter) {
    // Match the desktop title's exact filter, including the reserved bytes.
    volatile BYTE *bytes = (volatile BYTE *)filter;
    for (SIZE_T i = 0; i < sizeof(*filter); ++i) bytes[i] = 0;
    filter->cbSize = sizeof(*filter);
    filter->FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter->u.DeviceInterface.ClassGuid = hid;
}

#ifndef PROBE_HOSTED_CONTROLLER_NOTIFY
static DWORD CALLBACK OnHidChange(HCMNOTIFICATION notification, PVOID context, CM_NOTIFY_ACTION action,
        PCM_NOTIFY_EVENT_DATA data, DWORD size) {
    (void)notification;
    (void)context;
    (void)action;
    (void)data;
    (void)size;
    return ERROR_SUCCESS;
}

static unsigned TryControllerNotification(void) {
    HMODULE module = LoadLibraryA("cfgmgr32.dll");
    if (!module) return 0x73800000u | (GetLastError() & 0xffffu);
    RegisterNotification registerNotification =
        (RegisterNotification)(void *)GetProcAddress(module, "CM_Register_Notification");
    if (!registerNotification) return 0x73810000u | (GetLastError() & 0xffffu);
    UnregisterNotification unregisterNotification =
        (UnregisterNotification)(void *)GetProcAddress(module, "CM_Unregister_Notification");
    if (!unregisterNotification) return 0x73820000u | (GetLastError() & 0xffffu);

    CM_NOTIFY_FILTER filter;
    MakeFilter(&filter);
    HCMNOTIFICATION notification = NULL;
    CONFIGRET result = registerNotification(&filter, NULL, OnHidChange, &notification);
    if (result != CR_SUCCESS) return 0x73830000u | (result & 0xffffu);
    if (!notification) return 0x73840000u;
    result = unregisterNotification(notification);
    if (result != CR_SUCCESS) return 0x73850000u | (result & 0xffffu);
    if (!FreeLibrary(module)) return 0x73860000u | (GetLastError() & 0xffffu);
    return PROBE_EXPECTED_CHILD_EXIT;
}
#else
#include <xinput.h>
#include "../../runtime/win32/hosted-gamepad.h"

typedef struct CallbackRecord {
    HCMNOTIFICATION handle;
    HANDLE ready;
    WCHAR link[128];
    volatile LONG count;
    volatile LONG lastAction;
    volatile LONG badData;
} CallbackRecord;

static DWORD CALLBACK OnHostedChange(HCMNOTIFICATION notification, PVOID argument, CM_NOTIFY_ACTION action,
        PCM_NOTIFY_EVENT_DATA data, DWORD size) {
    CallbackRecord *record = argument;
    const DWORD header = offsetof(CM_NOTIFY_EVENT_DATA, u.DeviceInterface.SymbolicLink);
    BOOL valid = notification == record->handle && data && size > header &&
        (size - header) % sizeof(WCHAR) == 0 && size - header <= sizeof(record->link) &&
        data->FilterType == CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE && !data->Reserved;
    if (valid) {
        const BYTE *actual = (const BYTE *)&data->u.DeviceInterface.ClassGuid;
        const BYTE *expected = (const BYTE *)&hid;
        for (SIZE_T i = 0; i < sizeof(hid); ++i) valid &= actual[i] == expected[i];
        const size_t count = (size - header) / sizeof(WCHAR);
        const WCHAR *link = (const WCHAR *)((const BYTE *)data + header);
        if (count < 2 || !link[0] || link[count - 1]) {
            valid = FALSE;
        } else if (!SdlHostLoad(&record->count)) {
            for (SIZE_T i = 0; i < count; ++i) record->link[i] = link[i];
        } else if (CompareStringOrdinal(record->link, -1, link, -1, FALSE) != CSTR_EQUAL) {
            valid = FALSE;
        }
    }
    if (!valid) InterlockedExchange(&record->badData, 1);
    InterlockedExchange(&record->lastAction, action);
    InterlockedIncrement(&record->count);
    SetEvent(record->ready);
    return ERROR_SUCCESS;
}

static BOOL SawChange(CallbackRecord *record, LONG count, CM_NOTIFY_ACTION action) {
    return WaitForSingleObject(record->ready, 1800) == WAIT_OBJECT_0 &&
        !SdlHostLoad(&record->badData) && SdlHostLoad(&record->count) == count &&
        SdlHostLoad(&record->lastAction) == (LONG)action;
}

static BOOL StillQuiet(CallbackRecord *record, LONG count) {
    return WaitForSingleObject(record->ready, 300) == WAIT_TIMEOUT && SdlHostLoad(&record->count) == count;
}

static unsigned CheckEdges(SdlHostGamepad *pad, RegisterNotification registerNotification,
        UnregisterNotification unregisterNotification, DWORD (WINAPI *caps)(DWORD, DWORD, XINPUT_CAPABILITIES *),
        CallbackRecord *record, CallbackRecord *existing) {
    CM_NOTIFY_FILTER filter;
    MakeFilter(&filter);
    if (registerNotification(&filter, record, OnHostedChange, &record->handle) != CR_SUCCESS ||
            !record->handle || !StillQuiet(record, 0)) return 0x73930000u;
    SdlHostGamepadState connected = SdlHostIdleGamepad(SDL_HOST_PAD_CONNECTED);
    // Expose an in-progress publication for several notification polls.
    InterlockedIncrement(&pad->serial);
    InterlockedExchange(&pad->buttons, connected.buttons);
    for (SIZE_T i = 0; i < ARRAYSIZE(connected.axes); ++i)
        InterlockedExchange(&pad->axes[i], connected.axes[i]);
    if (!StillQuiet(record, 0)) return 0x73940000u;
    InterlockedIncrement(&pad->serial);
    XINPUT_CAPABILITIES capabilities;
    if (!SawChange(record, 1, CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) ||
            caps(0, XINPUT_FLAG_GAMEPAD, &capabilities) != ERROR_SUCCESS) return 0x73950000u;

    if (registerNotification(&filter, existing, OnHostedChange, &existing->handle) != CR_SUCCESS ||
            !existing->handle || !StillQuiet(existing, 0)) return 0x73960000u;
    connected.buttons |= 1;
    connected.axes[0] = 100;
    SdlHostPublishGamepad(pad, &connected);
    if (!StillQuiet(record, 1) || !StillQuiet(existing, 0)) return 0x73970000u;

    const SdlHostGamepadState disconnected = SdlHostIdleGamepad(0);
    SdlHostPublishGamepad(pad, &disconnected);
    if (!SawChange(record, 2, CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) ||
            !SawChange(existing, 1, CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) ||
            caps(0, XINPUT_FLAG_GAMEPAD, &capabilities) != ERROR_DEVICE_NOT_CONNECTED) return 0x73980000u;
    SdlHostPublishGamepad(pad, &connected);
    if (!SawChange(record, 3, CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) ||
            !SawChange(existing, 2, CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) ||
            caps(0, XINPUT_FLAG_GAMEPAD, &capabilities) != ERROR_SUCCESS || SdlHostLoad(&pad->ack)) return 0x73990000u;

    HCMNOTIFICATION released = record->handle;
    if (unregisterNotification(released) != CR_SUCCESS) return 0x739a0000u;
    record->handle = NULL;
    if (unregisterNotification(released) == CR_SUCCESS) return 0x739b0000u;
    SdlHostPublishGamepad(pad, &disconnected);
    if (!SawChange(existing, 3, CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) ||
            !StillQuiet(record, 3)) return 0x739c0000u;
    return PROBE_EXPECTED_CHILD_EXIT;
}

static unsigned CheckHostedControllerNotification(SdlHostMailbox *mailbox) {
    HMODULE cfgmgr = LoadLibraryA("CfgMgr32.dll");
    HMODULE xinput = LoadLibraryW(L"XInput1_4.dll");
    unsigned result = 0x73910000u;
    static CallbackRecord record, existing;
    if (!cfgmgr || !xinput || cfgmgr != GetModuleHandleW(L"XboxWin32ControllerNotify.dll") ||
            xinput != GetModuleHandleW(L"XboxWin32XInput910.dll")) goto cleanup;
    RegisterNotification reg = (RegisterNotification)(void *)GetProcAddress(cfgmgr, "CM_Register_Notification");
    UnregisterNotification unreg = (UnregisterNotification)(void *)GetProcAddress(cfgmgr, "CM_Unregister_Notification");
    DWORD (WINAPI *caps)(DWORD, DWORD, XINPUT_CAPABILITIES *) =
        (void *)GetProcAddress(xinput, "XInputGetCapabilities");
    result = 0x73920000u;
    if (!reg || !unreg || !caps) goto cleanup;
    record.ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    existing.ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (record.ready && existing.ready) {
        result = CheckEdges(&mailbox->gamepad, reg, unreg, caps, &record, &existing);
        if (record.handle && unreg(record.handle) != CR_SUCCESS) ExitProcess(0x739d0000u);
        if (existing.handle && unreg(existing.handle) != CR_SUCCESS) ExitProcess(0x739d0001u);
    }
    if (record.ready) CloseHandle(record.ready);
    if (existing.ready) CloseHandle(existing.ready);
cleanup:
    if (cfgmgr) FreeLibrary(cfgmgr);
    if (xinput) FreeLibrary(xinput);
    return result;
}

static unsigned TryControllerNotification(void) {
    WCHAR path[512];
    DWORD length = GetTempPathW(ARRAYSIZE(path), path);
    const WCHAR prefix[] = L"xbox-controller-notify-";
    if (!length || length + ARRAYSIZE(prefix) + 8 > ARRAYSIZE(path)) return 0x73900000u;
    for (SIZE_T i = 0; i + 1 < ARRAYSIZE(prefix); ++i) path[length++] = prefix[i];
    const DWORD pid = GetCurrentProcessId();
    for (int shift = 28; shift >= 0; shift -= 4) path[length++] = L"0123456789abcdef"[(pid >> shift) & 15];
    path[length] = 0;
    CREATEFILE2_EXTENDED_PARAMETERS options = {0};
    options.dwSize = sizeof(options);
    options.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
    options.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE;
    HANDLE file = CreateFile2FromAppW(path, GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, CREATE_NEW, &options);
    if (file == INVALID_HANDLE_VALUE) return 0x73900001u;
    const DWORD size = offsetof(SdlHostMailbox, keyboard);
    HANDLE mapping = CreateFileMappingFromApp(file, NULL, PAGE_READWRITE, size, NULL);
    SdlHostMailbox *mailbox = mapping ? MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, size) : NULL;
    unsigned result = 0x73900002u;
    if (mailbox && SetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", path) &&
            SetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_XINPUT", L"1") &&
            SetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_CONTROLLER_NOTIFY", L"1")) {
        InterlockedExchange(&mailbox->magic, SDL_HOST_MAGIC);
        result = CheckHostedControllerNotification(mailbox);
    }
    if (mailbox) UnmapViewOfFile(mailbox);
    if (mapping) CloseHandle(mapping);
    CloseHandle(file);
    return result;
}
#endif

void WINAPI ChildEntry(void) {
    ExitProcess(TryControllerNotification());
}
