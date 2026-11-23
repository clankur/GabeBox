#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

typedef BOOL (WINAPI *PulseTimerEvent)(HANDLE);

typedef struct Timer {
    struct Timer *next;
    PTP_TIMER work;
    LPTIMECALLBACK callback;
    PulseTimerEvent pulse;
    DWORD_PTR user;
    ULONGLONG due;
    UINT id;
    UINT delay;
    UINT flags;
    DWORD thread;
    BOOL cancelled;
    BOOL callbackDisposes;
} Timer;

static SRWLOCK timersLock = SRWLOCK_INIT;
static Timer *timers;
static UINT latestId;

static ULONGLONG TimerMilliseconds(void) {
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency))
        return GetTickCount64();
    return (ULONGLONG)(counter.QuadPart / frequency.QuadPart) * 1000 +
        (ULONGLONG)((counter.QuadPart % frequency.QuadPart) * 1000 / frequency.QuadPart);
}

static Timer **FindTimer(UINT id) {
    Timer **current = &timers;
    while (*current && (*current)->id != id) current = &(*current)->next;
    return current;
}

static void DisposeTimer(Timer *timer) {
    CloseThreadpoolTimer(timer->work);
    HeapFree(GetProcessHeap(), 0, timer);
}

static void ScheduleTimer(Timer *timer, ULONGLONG now) {
    const ULONGLONG remaining = timer->due > now ? timer->due - now : 0;
    const LARGE_INTEGER relative = {.QuadPart = remaining ? -(LONGLONG)(remaining * 10000) : -1};
    FILETIME due = {relative.LowPart, (DWORD)relative.HighPart};
    SetThreadpoolTimer(timer->work, &due, 0, 0);
}

static VOID CALLBACK TimerReady(PTP_CALLBACK_INSTANCE instance, PVOID argument, PTP_TIMER work) {
    (void)instance;
    (void)work;
    Timer *timer = argument;
    AcquireSRWLockExclusive(&timersLock);
    if (timer->cancelled) {
        ReleaseSRWLockExclusive(&timersLock);
        return;
    }
    timer->thread = GetCurrentThreadId();
    ReleaseSRWLockExclusive(&timersLock);

    if (timer->flags & TIME_CALLBACK_EVENT_SET)
        SetEvent((HANDLE)timer->callback);
    else if (timer->pulse)
        timer->pulse((HANDLE)timer->callback);
    else
        timer->callback(timer->id, 0, timer->user, 0, 0);

    AcquireSRWLockExclusive(&timersLock);
    timer->thread = 0;
    BOOL dispose = timer->callbackDisposes;
    if (!timer->cancelled) {
        if (timer->flags & TIME_PERIODIC) {
            const ULONGLONG now = TimerMilliseconds();
            timer->due += timer->delay;
            // Keep the original cadence and skip expirations missed by a slow callback.
            if (timer->due < now)
                timer->due += ((now - timer->due + timer->delay - 1) / timer->delay) * timer->delay;
            ScheduleTimer(timer, now);
        } else {
            *FindTimer(timer->id) = timer->next;
            dispose = TRUE;
        }
    }
    ReleaseSRWLockExclusive(&timersLock);
    if (dispose) DisposeTimer(timer);
}

MMRESULT WINAPI XboxTimeSetEvent(UINT delay, UINT resolution, LPTIMECALLBACK callback,
        DWORD_PTR user, UINT flags) {
    // Use the threadpool timer's most precise setting regardless of the requested coalescing.
    (void)resolution;
    const UINT eventFlags = TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE;
    if (!delay || !callback || (flags & ~(eventFlags | TIME_PERIODIC | TIME_KILL_SYNCHRONOUS)) ||
            (flags & eventFlags) == eventFlags) return 0;

    PulseTimerEvent pulse = NULL;
    if (flags & TIME_CALLBACK_EVENT_PULSE) {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (kernel) pulse = (PulseTimerEvent)(void *)GetProcAddress(kernel, "PulseEvent");
        if (!pulse) return 0;
    }
    Timer *timer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*timer));
    if (!timer) return 0;
    timer->callback = callback;
    timer->pulse = pulse;
    timer->user = user;
    timer->delay = delay;
    timer->flags = flags;
    timer->work = CreateThreadpoolTimer(TimerReady, timer, NULL);
    if (!timer->work) {
        HeapFree(GetProcessHeap(), 0, timer);
        return 0;
    }

    AcquireSRWLockExclusive(&timersLock);
    do {
        ++latestId;
    } while (!latestId || *FindTimer(latestId));
    const UINT id = timer->id = latestId;
    timer->next = timers;
    timers = timer;
    const ULONGLONG now = TimerMilliseconds();
    timer->due = now + delay;
    ScheduleTimer(timer, now);
    ReleaseSRWLockExclusive(&timersLock);
    return id;
}

MMRESULT WINAPI XboxTimeKillEvent(UINT id) {
    AcquireSRWLockExclusive(&timersLock);
    Timer **entry = FindTimer(id);
    Timer *timer = *entry;
    if (!timer) {
        ReleaseSRWLockExclusive(&timersLock);
        return TIMERR_NOCANDO;
    }
    *entry = timer->next;
    timer->cancelled = TRUE;
    SetThreadpoolTimer(timer->work, NULL, 0, 0);
    // A callback can cancel itself. For asynchronous cancellation, let any
    // active callback dispose the timer after it returns from the application.
    const BOOL defer = timer->thread &&
        (timer->thread == GetCurrentThreadId() || !(timer->flags & TIME_KILL_SYNCHRONOUS));
    timer->callbackDisposes = defer;
    ReleaseSRWLockExclusive(&timersLock);
    if (!defer) {
        WaitForThreadpoolTimerCallbacks(timer->work, TRUE);
        DisposeTimer(timer);
    }
    return TIMERR_NOERROR;
}
