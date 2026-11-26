#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cfgmgr32.h>
#include "hosted-gamepad.h"

#define TRACE(record) AppendTrace("input controller-notify=" record "\r\n", \
    sizeof("input controller-notify=" record "\r\n") - 1)

typedef struct Subscription {
    struct Subscription *next;
    HCMNOTIFICATION handle;
    PTP_TIMER timer;
    SdlHostMailbox *mailbox;
    PCM_NOTIFY_CALLBACK callback;
    PVOID context;
    LONG serial;
    DWORD callbackThread;
    BOOL baselineKnown;
    BOOL connected;
    BOOL closing;
} Subscription;

static SRWLOCK subscriptionsLock = SRWLOCK_INIT;
static Subscription *subscriptions;
static ULONG_PTR nextHandle;
static const GUID hidInterface =
    {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
static const WCHAR hostedInterface[] = L"\\\\?\\GabeBox#HostedGamepad#0#{4d1e55b2-f16f-11cf-88cb-001111000030}";

static BOOL IsHidInterface(const GUID *guid) {
    const BYTE *first = (const BYTE *)guid, *second = (const BYTE *)&hidInterface;
    for (SIZE_T i = 0; i < sizeof(*guid); ++i) {
        if (first[i] != second[i]) return FALSE;
    }
    return TRUE;
}

static void SchedulePoll(PTP_TIMER timer) {
    const LARGE_INTEGER relative = {.QuadPart = -1000000}; // 100 ms in 100 ns units.
    FILETIME due = {relative.LowPart, (DWORD)relative.HighPart};
    SetThreadpoolTimer(timer, &due, 0, 0);
}

static void NotifyController(Subscription *subscription, BOOL connected) {
    enum { linkOffset = offsetof(CM_NOTIFY_EVENT_DATA, u.DeviceInterface.SymbolicLink) };
    union {
        CM_NOTIFY_EVENT_DATA data;
        BYTE bytes[linkOffset + sizeof(hostedInterface)];
    } event;
    event.data.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    event.data.Reserved = 0;
    event.data.u.DeviceInterface.ClassGuid = hidInterface;
    WCHAR *link = (WCHAR *)(event.bytes + linkOffset);
    for (SIZE_T i = 0; i < ARRAYSIZE(hostedInterface); ++i) link[i] = hostedInterface[i];
    const CM_NOTIFY_ACTION action = connected ? CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL :
        CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL;
    if (connected) TRACE("arrival");
    else TRACE("removal");
    subscription->callback(subscription->handle, subscription->context, action, &event.data, sizeof(event.bytes));
}

static VOID CALLBACK PollController(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_TIMER timer) {
    (void)instance;
    Subscription *subscription = context;
    AcquireSRWLockExclusive(&subscriptionsLock);
    if (subscription->closing || SdlHostLoad(&subscription->mailbox->quit)) {
        ReleaseSRWLockExclusive(&subscriptionsLock);
        return;
    }
    SdlHostGamepadState state;
    const LONG serial = SdlHostReadGamepad(&subscription->mailbox->gamepad, subscription->serial, &state);
    BOOL changed = FALSE;
    if (serial) {
        const BOOL connected = (state.buttons & SDL_HOST_PAD_CONNECTED) != 0;
        changed = subscription->baselineKnown && connected != subscription->connected;
        subscription->baselineKnown = TRUE;
        subscription->connected = connected;
        subscription->serial = serial;
    }
    if (changed && !SdlHostLoad(&subscription->mailbox->quit)) {
        subscription->callbackThread = GetCurrentThreadId();
        ReleaseSRWLockExclusive(&subscriptionsLock);
        NotifyController(subscription, subscription->connected);
        AcquireSRWLockExclusive(&subscriptionsLock);
        subscription->callbackThread = 0;
    }
    if (!subscription->closing && !SdlHostLoad(&subscription->mailbox->quit)) SchedulePoll(timer);
    ReleaseSRWLockExclusive(&subscriptionsLock);
}

CONFIGRET WINAPI XboxCMRegisterNotification(PCM_NOTIFY_FILTER filter, PVOID context,
        PCM_NOTIFY_CALLBACK callback, PHCMNOTIFICATION handleOut) {
    if (!handleOut) return CR_INVALID_POINTER;
    *handleOut = NULL;
    if (!filter || !callback) return CR_INVALID_POINTER;
    if (filter->cbSize != sizeof(*filter) || filter->Reserved) return CR_INVALID_DATA;
    if (filter->Flags || filter->FilterType != CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE ||
            !IsHidInterface(&filter->u.DeviceInterface.ClassGuid)) return CR_CALL_NOT_IMPLEMENTED;

    WCHAR path[1024];
    DWORD length = GetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) return CR_FAILURE;
    SdlHostMailbox *mailbox = MapHostedGamepad(path);
    if (!mailbox) {
        TRACE("mailbox-unavailable");
        return CR_FAILURE;
    }
    Subscription *subscription = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*subscription));
    if (!subscription) {
        UnmapViewOfFile(mailbox);
        return CR_OUT_OF_MEMORY;
    }
    subscription->mailbox = mailbox;
    subscription->callback = callback;
    subscription->context = context;
    const BOOL noSample = !SdlHostLoad(&mailbox->gamepad.serial);
    SdlHostGamepadState state;
    subscription->serial = SdlHostReadGamepad(&mailbox->gamepad, 0, &state);
    subscription->baselineKnown = subscription->serial || noSample;
    subscription->connected = subscription->serial && (state.buttons & SDL_HOST_PAD_CONNECTED);
    subscription->timer = CreateThreadpoolTimer(PollController, subscription, NULL);
    if (!subscription->timer) {
        UnmapViewOfFile(mailbox);
        HeapFree(GetProcessHeap(), 0, subscription);
        return CR_OUT_OF_MEMORY;
    }

    AcquireSRWLockExclusive(&subscriptionsLock);
    if (nextHandle == ~(ULONG_PTR)0) {
        ReleaseSRWLockExclusive(&subscriptionsLock);
        CloseThreadpoolTimer(subscription->timer);
        UnmapViewOfFile(mailbox);
        HeapFree(GetProcessHeap(), 0, subscription);
        return CR_OUT_OF_MEMORY;
    }
    subscription->handle = (HCMNOTIFICATION)++nextHandle;
    subscription->next = subscriptions;
    subscriptions = subscription;
    *handleOut = subscription->handle;
    SchedulePoll(subscription->timer);
    ReleaseSRWLockExclusive(&subscriptionsLock);
    TRACE("registered");
    return CR_SUCCESS;
}

CONFIGRET WINAPI XboxCMUnregisterNotification(HCMNOTIFICATION handle) {
    AcquireSRWLockExclusive(&subscriptionsLock);
    Subscription **link = &subscriptions;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    Subscription *subscription = *link;
    if (!subscription || subscription->closing) {
        ReleaseSRWLockExclusive(&subscriptionsLock);
        return CR_INVALID_POINTER;
    }
    // The public contract requires asynchronous unregister from a callback.
    const DWORD thread = GetCurrentThreadId();
    for (Subscription *current = subscriptions; current; current = current->next) {
        if (current->callbackThread == thread) {
            ReleaseSRWLockExclusive(&subscriptionsLock);
            return CR_FAILURE;
        }
    }
    subscription->closing = TRUE;
    SetThreadpoolTimer(subscription->timer, NULL, 0, 0);
    ReleaseSRWLockExclusive(&subscriptionsLock);
    WaitForThreadpoolTimerCallbacks(subscription->timer, TRUE);

    AcquireSRWLockExclusive(&subscriptionsLock);
    link = &subscriptions;
    while (*link != subscription) link = &(*link)->next;
    *link = subscription->next;
    ReleaseSRWLockExclusive(&subscriptionsLock);
    CloseThreadpoolTimer(subscription->timer);
    UnmapViewOfFile(subscription->mailbox);
    HeapFree(GetProcessHeap(), 0, subscription);
    TRACE("unregistered");
    return CR_SUCCESS;
}
