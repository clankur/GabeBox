#pragma once
/* Bounded, prioritized pool for D3D12 pipeline compiles in a hosted title (policy in
   runtime/shared/compile-slots.h).

   XboxCompileSlotsConfigure sets the pool once, before the first compile. Every compile, the
   title's and a replay thread's, runs between XboxCompileSlotsEnter and XboxCompileSlotsLeave
   with its class. Waiters re-check the commit-dependent limit every 50 ms as well as after each
   release. Available commit is GlobalMemoryStatusEx's ullAvailPageFile, the same figure the
   loader's commit records report.

   Each 512th compile, and on request, the pool emits one summary record through the trace
   sink:
     asset pso-slots n=<compiles> base=<B> boost=<X> min-mib=<M> boosted=<started above base>
       urg=<urgent compiles> urg-over=<urgent compiles above the limit> urg-wait-ms=<sum>
       urg-max-ms=<longest urgent wait> wait-ms=<normal wait sum> bg=<background compiles>
       bg-wait-ms=<sum> peak=<most at once>
   The counters are a few interlocked adds per compile; there are no readbacks. */
#include <windows.h>
#include "../shared/compile-slots.h"

typedef void (*XboxCompileSlotsTraceFn)(const char *line, DWORD size);

typedef struct XboxCompileSlotTicket {
    int kind;
    BOOL started;
    LARGE_INTEGER requested;
} XboxCompileSlotTicket;

static struct {
    SRWLOCK lock;
    CONDITION_VARIABLE changed;
    XboxCompileSlots slots;
    XboxCompileSlotsTraceFn trace;
    LONG64 frequency;
    volatile LONG count, boosted, urgent, urgentOver, background, peak;
    volatile LONG64 waitMicros[3];
    volatile LONG64 urgentWaitMax;
} xcs = {SRWLOCK_INIT, CONDITION_VARIABLE_INIT, {1, 0, 0, 0, 0, {0, 0, 0}}};

static void XboxCompileSlotsConfigure(unsigned base, unsigned boost, unsigned boostMinMiB, unsigned urgentMinMiB,
        XboxCompileSlotsTraceFn trace) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    AcquireSRWLockExclusive(&xcs.lock);
    xcs.slots.base = base ? base : 1;
    xcs.slots.boost = boost;
    xcs.slots.boostMinMiB = boostMinMiB;
    xcs.slots.urgentMinMiB = urgentMinMiB;
    xcs.trace = trace;
    xcs.frequency = frequency.QuadPart;
    ReleaseSRWLockExclusive(&xcs.lock);
}

static unsigned XcsAvailableMiB(void) {
    MEMORYSTATUSEX memory = {.dwLength = sizeof(memory)};
    if (!GlobalMemoryStatusEx(&memory)) return 0;
    const unsigned long long mib = memory.ullAvailPageFile >> 20;
    return mib > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned)mib;
}

static char *XcsText(char *out, const char *text) {
    while (*text) *out++ = *text++;
    return out;
}

static char *XcsDec(char *out, unsigned long long value) {
    char digits[20];
    int count = 0;
    do digits[count++] = (char)('0' + value % 10); while ((value /= 10) && count < 20);
    while (count) *out++ = digits[--count];
    return out;
}

static void XboxCompileSlotsSummary(void) {
    if (!xcs.trace) return;
    char line[400], *out = XcsText(line, "asset pso-slots n=");
    out = XcsDec(out, (unsigned long long)xcs.count);
    out = XcsDec(XcsText(out, " base="), xcs.slots.base);
    out = XcsDec(XcsText(out, " boost="), xcs.slots.boost);
    out = XcsDec(XcsText(out, " min-mib="), xcs.slots.boostMinMiB);
    out = XcsDec(XcsText(out, " boosted="), (unsigned long long)xcs.boosted);
    out = XcsDec(XcsText(out, " urg="), (unsigned long long)xcs.urgent);
    out = XcsDec(XcsText(out, " urg-over="), (unsigned long long)xcs.urgentOver);
    out = XcsDec(XcsText(out, " urg-wait-ms="), (unsigned long long)xcs.waitMicros[XboxCompileUrgent] / 1000);
    out = XcsDec(XcsText(out, " urg-max-ms="), (unsigned long long)xcs.urgentWaitMax / 1000);
    out = XcsDec(XcsText(out, " wait-ms="), (unsigned long long)xcs.waitMicros[XboxCompileNormal] / 1000);
    out = XcsDec(XcsText(out, " bg="), (unsigned long long)xcs.background);
    out = XcsDec(XcsText(out, " bg-wait-ms="), (unsigned long long)xcs.waitMicros[XboxCompileBackground] / 1000);
    out = XcsDec(XcsText(out, " peak="), (unsigned long long)xcs.peak);
    *out++ = '\r';
    *out++ = '\n';
    xcs.trace(line, (DWORD)(out - line));
}

static void XboxCompileSlotsEnter(int kind, XboxCompileSlotTicket *ticket) {
    if (kind < XboxCompileBackground || kind > XboxCompileUrgent) kind = XboxCompileNormal;
    ticket->kind = kind;
    ticket->started = FALSE;
    QueryPerformanceCounter(&ticket->requested);
    unsigned available = XcsAvailableMiB();
    AcquireSRWLockExclusive(&xcs.lock);
    ++xcs.slots.waiting[kind];
    while (!XboxCompileSlotMayStart(&xcs.slots, kind, available)) {
        SleepConditionVariableSRW(&xcs.changed, &xcs.lock, 50, 0);
        ReleaseSRWLockExclusive(&xcs.lock);
        available = XcsAvailableMiB();
        AcquireSRWLockExclusive(&xcs.lock);
    }
    --xcs.slots.waiting[kind];
    const unsigned before = xcs.slots.active++;
    const unsigned base = xcs.slots.base;
    const unsigned limit = XboxCompileSlotLimit(&xcs.slots, available);
    const LONG active = (LONG)xcs.slots.active;
    ReleaseSRWLockExclusive(&xcs.lock);
    ticket->started = TRUE;
    /* A class that yields may now be able to start. */
    WakeAllConditionVariable(&xcs.changed);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONG64 waited = xcs.frequency ? (now.QuadPart - ticket->requested.QuadPart) * 1000000 / xcs.frequency : 0;
    InterlockedAdd64(&xcs.waitMicros[kind], waited);
    if (before >= base) InterlockedIncrement(&xcs.boosted);
    if (kind == XboxCompileUrgent) {
        InterlockedIncrement(&xcs.urgent);
        if (before >= limit) InterlockedIncrement(&xcs.urgentOver);
        for (LONG64 seen = xcs.urgentWaitMax; waited > seen;) {
            const LONG64 previous = InterlockedCompareExchange64(&xcs.urgentWaitMax, waited, seen);
            if (previous == seen) break;
            seen = previous;
        }
    } else if (kind == XboxCompileBackground) {
        InterlockedIncrement(&xcs.background);
    }
    for (LONG seen = xcs.peak; active > seen;) {
        const LONG previous = InterlockedCompareExchange(&xcs.peak, active, seen);
        if (previous == seen) break;
        seen = previous;
    }
    if (!(InterlockedIncrement(&xcs.count) & 511)) XboxCompileSlotsSummary();
}

static void XboxCompileSlotsLeave(XboxCompileSlotTicket *ticket) {
    if (!ticket->started) return;
    ticket->started = FALSE;
    AcquireSRWLockExclusive(&xcs.lock);
    if (xcs.slots.active) --xcs.slots.active;
    ReleaseSRWLockExclusive(&xcs.lock);
    WakeAllConditionVariable(&xcs.changed);
}
