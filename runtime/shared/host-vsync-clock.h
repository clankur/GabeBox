#pragma once
#include <stdint.h>

/* Estimates the host presenter's display cadence from the times it releases the game's frames, so a producer can
   pace multi-refresh presents (SyncInterval 2) to the host's real refresh and phase instead of a nominal 60 Hz.

   The host draws on its display's composition callback and signals each slot's done fence right after that draw,
   so done-fence completion times follow its refresh closely. Observe() feeds them to a phase-locked loop on the QPC
   clock that ignores the occasional early or late callback. Period and anchor use 16 fractional bits; all integer math. */
typedef struct XboxHostVsync {
    uint64_t anchor;   /* QPC of an estimated host tick (done-release time) */
    int64_t period16;  /* estimated host period, QPC ticks << 16 */
    int64_t nominal16; /* the period the estimate started from */
    uint64_t last;     /* QPC of the last accepted observation */
    uint64_t base;     /* QPC of the first observation since the last re-anchor */
    int64_t baseTicks; /* host ticks counted from base to last */
    unsigned samples;  /* accepted observations since the last re-anchor */
    unsigned outliers; /* consecutive observations more than a quarter period off the estimate */
} XboxHostVsync;

#define XBOX_HOST_VSYNC_LOCK_SAMPLES 16u
#define XBOX_HOST_VSYNC_PERIOD_TICKS 120u /* two seconds of host refreshes before the measured period is used */
/* Where in the host's refresh a paced frame is committed, as a fraction of a period after the estimated release.
   Releases follow the host's draw callback within about half a millisecond; the callback itself is within about
   +-2.5 ms of the refresh on 98% of refreshes (Fisk Tower; the rest reach +-8 ms). Early in the refresh leaves the
   most room for a game frame that finishes after its target (at Fisk Tower the game's own frame is 25-33 ms), while
   still clearing a callback up to about 5 ms late. Measured at Fisk Tower: 0.3 missed 12 slots a minute, 0.45 28
   and 0.7 14; on the street 0.3 missed none. */
#define XBOX_HOST_VSYNC_COMMIT16 19661u /* 0.3 */

/* Starts from 60000/1001 Hz, the console's usual output rate; the loop corrects the period from observations. */
static inline void XboxHostVsyncInit(XboxHostVsync *clock, uint64_t frequency)
{
    clock->anchor = 0;
    clock->nominal16 = clock->period16 = (int64_t)(((frequency << 16) / 60000u) * 1001u);
    clock->last = clock->base = 0;
    clock->baseTicks = 0;
    clock->samples = 0;
}

static inline int64_t XboxHostVsyncDiv(int64_t value, int64_t divisor) /* rounds to nearest, divisor > 0 */
{
    return value >= 0 ? (value + divisor / 2) / divisor : -((-value + divisor / 2) / divisor);
}

static inline int64_t XboxHostVsyncFloor16(int64_t value16) /* floor(value16 / 65536) */
{
    return value16 >= 0 ? value16 / 65536 : -((-value16 + 65535) / 65536);
}

static inline void XboxHostVsyncObserve(XboxHostVsync *clock, uint64_t now)
{
    if (!clock->anchor || now <= clock->anchor) {
        if (!clock->anchor) {
            clock->anchor = clock->last = clock->base = now;
            clock->baseTicks = 0;
            clock->samples = 1;
            clock->outliers = 0;
        }
        return;
    }
    const int64_t elapsed16 = (int64_t)(now - clock->anchor) * 65536;
    const int64_t ticks = XboxHostVsyncDiv(elapsed16, clock->period16);
    if (ticks < 1) return; /* another slot released in the same host draw */
    const int64_t error16 = elapsed16 - ticks * clock->period16;
    if (error16 > clock->period16 / 4 || error16 < -clock->period16 / 4 || ticks > 600) {
        /* The host's draw callback ran early or late (it is not vsync-exact under load), or play paused. Start
           again only when the cadence itself has moved. */
        if (++clock->outliers >= 4 || ticks > 600) {
            clock->anchor = clock->last = clock->base = now;
            clock->baseTicks = 0;
            clock->samples = 1;
            clock->outliers = 0;
        }
        return;
    }
    clock->outliers = 0;
    /* Follow a quarter of the phase error. The period is the mean over every tick counted since the base, so
       release jitter averages out as the count grows; it is used from two seconds of ticks on and stays within 1%
       of the nominal period. */
    clock->anchor += (uint64_t)XboxHostVsyncFloor16(ticks * clock->period16 + error16 / 4);
    clock->baseTicks += XboxHostVsyncDiv((int64_t)(now - clock->last) * 65536, clock->period16);
    if (clock->baseTicks >= XBOX_HOST_VSYNC_PERIOD_TICKS) {
        int64_t period16 = (int64_t)(now - clock->base) * 65536 / clock->baseTicks;
        const int64_t limit = clock->nominal16 / 100;
        if (period16 > clock->nominal16 + limit) period16 = clock->nominal16 + limit;
        if (period16 < clock->nominal16 - limit) period16 = clock->nominal16 - limit;
        clock->period16 = period16;
    }
    if (clock->baseTicks > 1000000) { /* about 4.6 hours: restart the average before the products can overflow */
        clock->base = now;
        clock->baseTicks = 0;
    }
    clock->last = now;
    if (clock->samples < 0xFFFFu) ++clock->samples;
}

/* True when the estimate has enough recent observations to pace by. */
static inline int XboxHostVsyncLocked(const XboxHostVsync *clock, uint64_t now, uint64_t frequency)
{
    return clock->samples >= XBOX_HOST_VSYNC_LOCK_SAMPLES && clock->baseTicks >= XBOX_HOST_VSYNC_PERIOD_TICKS &&
        now >= clock->last && now - clock->last < frequency / 2;
}

/* The first time at or after `earliest` that lies `fraction16`/65536 of a period after an estimated host tick. */
static inline uint64_t XboxHostVsyncNext(const XboxHostVsync *clock, uint64_t earliest, uint32_t fraction16)
{
    const int64_t offset16 = (int64_t)(((uint64_t)clock->period16 * fraction16) >> 16);
    const int64_t distance16 = (int64_t)(earliest - clock->anchor) * 65536 - offset16;
    int64_t ticks = distance16 >= 0 ? (distance16 + clock->period16 - 1) / clock->period16
                                    : -((-distance16) / clock->period16);
    uint64_t target = clock->anchor + (uint64_t)XboxHostVsyncFloor16(ticks * clock->period16 + offset16);
    if (target < earliest) target = clock->anchor + (uint64_t)XboxHostVsyncFloor16((ticks + 1) * clock->period16 + offset16);
    return target;
}
