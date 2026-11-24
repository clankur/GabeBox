#pragma once
#include <stdint.h>

/* The native swap chain presents at 60 Hz. Carry fractional QPC ticks and OS
   sleep overshoot between swaps so whole-millisecond sleeps cannot accumulate. */
typedef struct XboxSdlSwapClock {
    uint64_t deadline;
    uint64_t frequency;
    unsigned remainder;
    int interval;
} XboxSdlSwapClock;

static inline uint64_t XboxSdlNextSwap(XboxSdlSwapClock *clock, uint64_t now, int interval)
{
    if (interval <= 0 || !clock->frequency || interval != clock->interval) {
        clock->interval = interval;
        clock->deadline = now;
        clock->remainder = 0;
        return now;
    }

    uint64_t numerator = clock->frequency * (unsigned)interval + clock->remainder;
    uint64_t period = numerator / 60;
    clock->deadline += period;
    clock->remainder = (unsigned)(numerator % 60);
    /* A missed whole refresh or a suspended window starts a new schedule.
       A small late wake-up consumes the next frame's time budget. */
    if (now > clock->deadline && now - clock->deadline >= period) {
        clock->deadline = now;
        clock->remainder = 0;
    }
    return clock->deadline;
}
