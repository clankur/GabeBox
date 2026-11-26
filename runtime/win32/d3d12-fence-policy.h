/* Direct3D 12 fence-wait trace: package-root switch, the check made after a
   blocking ID3D12Fence::SetEventOnCompletion, and the trace format. Shared by
   the loader-trace adapter (d3d12-fence-trace.c) and its host-side unit
   tests.

   Platform-neutral C (also valid C++) with no C runtime calls, so the
   CRT-free adapter and a hosted g++ test can both include it.

   Package-root file xbox-d3d12-fence-trace.txt:
     absent           no hooks, no trace line (the default)
     empty, trace,    trace: time each SetEventOnCompletion(value, NULL), then
     mode=trace       compare GetCompletedValue() with value
     wait, mode=wait  trace, and after a wait that returned before the fence
                      reached value, keep polling GetCompletedValue() until it
                      does (bounded at XBOX_FENCE_SETTLE_TIMEOUT_US)
     off, mode=off    no hooks
   Trailing whitespace is ignored. Anything else is invalid and selects
   trace, which changes no behavior. */
#pragma once
#include "d3d12-residency-policy.h"
#include <stdint.h>

#define XBOX_FENCE_ABSENT 0u
#define XBOX_FENCE_TRACE 1u
#define XBOX_FENCE_WAIT 2u
#define XBOX_FENCE_OFF 3u
#define XBOX_FENCE_INVALID 4u

/* Method-table slots counted from d3d12.idl in microsoft/DirectX-Headers.
   ID3D12Device: IUnknown (0-2), ID3D12Object (3-6), then GetNodeCount (7),
   CreateCommandQueue (8), ... MakeResident (34), Evict (35), CreateFence (36).
   ID3D12Fence: IUnknown, ID3D12Object, ID3D12DeviceChild::GetDevice (7), then
   GetCompletedValue (8), SetEventOnCompletion (9), Signal (10).
   ID3D12CommandQueue: ..., GetDevice (7), UpdateTileMappings (8),
   CopyTileMappings (9), ExecuteCommandLists (10), SetMarker, BeginEvent,
   EndEvent, Signal (14). */
#define XBOX_FENCE_SLOT_CREATE_COMMAND_QUEUE 8u
#define XBOX_FENCE_SLOT_CREATE_FENCE 36u
#define XBOX_FENCE_SLOT_GET_COMPLETED_VALUE 8u
#define XBOX_FENCE_SLOT_SET_EVENT_ON_COMPLETION 9u
#define XBOX_FENCE_SLOT_QUEUE_EXECUTE 10u
#define XBOX_FENCE_SLOT_QUEUE_SIGNAL 14u

/* The longest the wait mode keeps polling after an early return. */
#define XBOX_FENCE_SETTLE_TIMEOUT_US 2000000u

/* Same filter limit as the residency lines. */
#define XBOX_FENCE_LINE_MAX XBOX_RESIDENCY_LINE_MAX
/* YYYYMMDDTHHMMSS.mmmZ */
#define XBOX_FENCE_UTC_CHARS 20u

static inline int XboxFenceSame(const char *text, uint32_t length, const char *name) {
    uint32_t at = 0;
    while (at < length && name[at] && text[at] == name[at]) ++at;
    return at == length && !name[at];
}

static inline uint32_t XboxFenceParseMode(const char *text, uint32_t length) {
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r' ||
                      text[length - 1] == '\n'))
        --length;
    if (!length) return XBOX_FENCE_TRACE;
    if (length > 5 && XboxFenceSame(text, 5, "mode=")) {
        text += 5;
        length -= 5;
    }
    if (XboxFenceSame(text, length, "trace")) return XBOX_FENCE_TRACE;
    if (XboxFenceSame(text, length, "wait")) return XBOX_FENCE_WAIT;
    if (XboxFenceSame(text, length, "off")) return XBOX_FENCE_OFF;
    return XBOX_FENCE_INVALID;
}

/* The behavior a parsed file selects: absent and off install nothing;
   invalid behaves as trace. */
static inline uint32_t XboxFenceEffectiveMode(uint32_t parsed) {
    return parsed == XBOX_FENCE_ABSENT || parsed == XBOX_FENCE_OFF ? XBOX_FENCE_OFF :
           parsed == XBOX_FENCE_WAIT ? XBOX_FENCE_WAIT : XBOX_FENCE_TRACE;
}

static inline const char *XboxFenceModeName(uint32_t mode) {
    return mode == XBOX_FENCE_WAIT ? "wait" : mode == XBOX_FENCE_TRACE ? "trace" : "off";
}

static inline const char *XboxFenceSourceName(uint32_t parsed) {
    return parsed == XBOX_FENCE_INVALID ? "package-file override=invalid" : "package-file";
}

/* ------------------------------------------------- the check after a wait */

/* GetCompletedValue on the waited fence, a microsecond clock and a pause
   between polls (round counts from 0). A fake fence implements these in the
   host tests. */
typedef struct XboxFenceOps {
    uint64_t (*completed)(void *context);
    uint64_t (*nowUs)(void *context);
    void (*pause)(void *context, uint32_t round);
    void *context;
} XboxFenceOps;

typedef struct XboxFenceCheck {
    uint64_t completedAfterWait; /* the first GetCompletedValue after the driver returned */
    uint64_t completed;          /* the last value read */
    uint64_t settleUs;           /* wait mode: how long the polling ran */
    int early;                   /* completedAfterWait < value */
    int settled;                 /* wait mode: the fence reached value while polling */
    int timedOut;                /* wait mode: it had not after timeoutUs */
} XboxFenceCheck;

/* Called after SetEventOnCompletion(value, NULL) returned success. A device
   removal makes GetCompletedValue return UINT64_MAX, which is never early.
   Only wait mode polls, and only after an early return. */
static inline void XboxFenceCheckWait(const XboxFenceOps *ops, uint64_t value, uint32_t mode, uint64_t timeoutUs,
        XboxFenceCheck *check) {
    check->completedAfterWait = check->completed = ops->completed(ops->context);
    check->settleUs = 0;
    check->early = check->completed < value;
    check->settled = 0;
    check->timedOut = 0;
    if (!check->early || mode != XBOX_FENCE_WAIT) return;
    const uint64_t start = ops->nowUs(ops->context);
    for (uint32_t round = 0;; ++round) {
        ops->pause(ops->context, round);
        check->completed = ops->completed(ops->context);
        const uint64_t elapsed = ops->nowUs(ops->context) - start;
        if (check->completed >= value) {
            check->settled = 1;
            check->settleUs = elapsed;
            return;
        }
        if (elapsed >= timeoutUs) {
            check->timedOut = 1;
            check->settleUs = elapsed;
            return;
        }
    }
}

/* ------------------------------------------------------------- trace */

typedef struct XboxFenceWindow {
    uint64_t nullWaits;   /* SetEventOnCompletion(value, NULL) calls */
    uint64_t early;       /* of those, returned success with GetCompletedValue() < value */
    uint64_t maxWaitUs;   /* the longest NULL-event call */
    uint64_t exec;        /* ID3D12CommandQueue::ExecuteCommandLists calls */
    uint64_t eventWaits;  /* SetEventOnCompletion with an event (passed through) */
    uint64_t settled;     /* wait mode: early returns that then reached the value */
    uint64_t maxSettleUs; /* wait mode: the longest polling */
    uint64_t timeouts;    /* wait mode: early returns still short after the timeout */
    uint64_t failed;      /* SetEventOnCompletion calls that returned a failure */
} XboxFenceWindow;

static inline int XboxFenceWindowActive(const XboxFenceWindow *window) {
    return window->nullWaits || window->early || window->exec || window->eventWaits || window->timeouts ||
        window->failed;
}

/* "asset d3d12-fence null_waits=... utc=" for one second; the caller appends
   the UTC stamp (XBOX_FENCE_UTC_CHARS) and the line end. */
static inline char *XboxFenceFormatWindow(char *out, const XboxFenceWindow *window) {
    const char *end = out + XBOX_FENCE_LINE_MAX - XBOX_FENCE_UTC_CHARS;
    const char *fields = end - 5;
    out = XboxResidencyText(out, fields, "asset d3d12-fence");
    out = XboxResidencyField(out, fields, " null_waits=", window->nullWaits);
    out = XboxResidencyField(out, fields, " early=", window->early);
    out = XboxResidencyField(out, fields, " max_wait_us=", window->maxWaitUs);
    out = XboxResidencyField(out, fields, " exec=", window->exec);
    if (window->eventWaits) out = XboxResidencyField(out, fields, " event_waits=", window->eventWaits);
    if (window->settled) {
        out = XboxResidencyField(out, fields, " settled=", window->settled);
        out = XboxResidencyField(out, fields, " max_settle_us=", window->maxSettleUs);
    }
    if (window->timeouts) out = XboxResidencyField(out, fields, " timeouts=", window->timeouts);
    if (window->failed) out = XboxResidencyField(out, fields, " failed=", window->failed);
    return XboxResidencyText(out, end, " utc=");
}

/* "asset d3d12-fence-total mode=... t_s=..." every 60 s, so a quiet trace
   still shows the hooks alive. max_wait_us is the process maximum. */
static inline char *XboxFenceFormatTotal(char *out, const XboxFenceWindow *total, uint32_t mode, uint64_t seconds) {
    const char *end = out + XBOX_FENCE_LINE_MAX - XBOX_FENCE_UTC_CHARS;
    const char *fields = end - 5;
    out = XboxResidencyText(XboxResidencyText(out, fields, "asset d3d12-fence-total mode="), fields,
        XboxFenceModeName(mode));
    out = XboxResidencyField(out, fields, " t_s=", seconds);
    out = XboxResidencyField(out, fields, " null_waits=", total->nullWaits);
    out = XboxResidencyField(out, fields, " early=", total->early);
    out = XboxResidencyField(out, fields, " max_wait_us=", total->maxWaitUs);
    out = XboxResidencyField(out, fields, " exec=", total->exec);
    out = XboxResidencyField(out, fields, " event_waits=", total->eventWaits);
    out = XboxResidencyField(out, fields, " settled=", total->settled);
    out = XboxResidencyField(out, fields, " timeouts=", total->timeouts);
    out = XboxResidencyField(out, fields, " failed=", total->failed);
    return XboxResidencyText(out, end, " utc=");
}

/* "asset d3d12-fence first-early|timeout mode=... value=... completed=...
   wait_us=... [settle_us=... settle_completed=...] utc=" for the first early return and for each of
   the first wait-mode timeouts. */
static inline char *XboxFenceFormatEvent(char *out, const char *kind, uint64_t value, uint64_t completed,
        uint64_t waitUs, const XboxFenceCheck *check, uint32_t mode) {
    const char *end = out + XBOX_FENCE_LINE_MAX - XBOX_FENCE_UTC_CHARS;
    const char *fields = end - 5;
    out = XboxResidencyText(XboxResidencyText(out, fields, "asset d3d12-fence "), fields, kind);
    out = XboxResidencyText(XboxResidencyText(out, fields, " mode="), fields, XboxFenceModeName(mode));
    out = XboxResidencyField(out, fields, " value=", value);
    out = XboxResidencyField(out, fields, " completed=", completed);
    out = XboxResidencyField(out, fields, " wait_us=", waitUs);
    if (check->settled || check->timedOut) {
        out = XboxResidencyField(out, fields, " settle_us=", check->settleUs);
        out = XboxResidencyField(out, fields, " settle_completed=", check->completed);
    }
    return XboxResidencyText(out, end, " utc=");
}

/* YYYYMMDDTHHMMSS.mmmZ from broken-down UTC. */
static inline char *XboxFenceUtc(char *out, uint32_t year, uint32_t month, uint32_t day, uint32_t hour,
        uint32_t minute, uint32_t second, uint32_t millisecond) {
    const uint32_t parts[6] = {year / 100u, year % 100u, month, day, hour, minute};
    for (int i = 0; i < 6; ++i) {
        if (i == 4) *out++ = 'T';
        *out++ = (char)('0' + parts[i] / 10u % 10u);
        *out++ = (char)('0' + parts[i] % 10u);
    }
    *out++ = (char)('0' + second / 10u % 10u);
    *out++ = (char)('0' + second % 10u);
    *out++ = '.';
    *out++ = (char)('0' + millisecond / 100u % 10u);
    *out++ = (char)('0' + millisecond / 10u % 10u);
    *out++ = (char)('0' + millisecond % 10u);
    *out++ = 'Z';
    return out;
}
