#include "../runtime/shared/host-vsync-clock.h"
#include <assert.h>

/* A host near 60 Hz releases frames 1.5 ms after its tick with up to +-0.6 ms of jitter, and one draw in 40 is
   6 ms late. A producer presenting every second tick at 0.3 of a period after the estimated release must land in
   every second host interval, with no slot repeated or skipped. */
static uint32_t seed = 12345;
static int64_t Jitter(int64_t range) { seed = seed * 1103515245u + 12345u; return (int64_t)(seed >> 8) % (2 * range + 1) - range; }

static uint64_t tick[40000];

static void Run(double hostHz)
{
    const uint64_t hz = 1745500000ull;
    const uint64_t origin = 1000000000ull;
    for (unsigned i = 0; i < 40000; ++i) tick[i] = origin + (uint64_t)(i * (hz / hostHz));
    const int64_t release = (int64_t)(hz * 15 / 10000), wobble = (int64_t)(hz * 6 / 10000);

    XboxHostVsync clock;
    XboxHostVsyncInit(&clock, hz);
    assert(clock.period16 > 0);
    assert(!XboxHostVsyncLocked(&clock, origin, hz));

    /* Free-running 30 Hz producer: the host releases every second tick. */
    for (unsigned i = 0; i < 80; i += 2) {
        int64_t late = (i % 40 == 20) ? (int64_t)(hz * 6 / 1000) : 0;
        XboxHostVsyncObserve(&clock, tick[i] + release + Jitter(wobble) + late);
    }
    assert(XboxHostVsyncLocked(&clock, tick[79], hz));

    /* Paced: each present is committed at the target; the host shows it at the first tick after the commit. */
    uint64_t deadline = 0;
    unsigned previousShown = 0, frames = 0, misses = 0;
    for (unsigned i = 80; i < 39000;) {
        const uint64_t period = (uint64_t)(clock.period16 >> 16);
        const uint64_t earliest = deadline ? deadline + 2 * period - period / 2 : tick[i];
        deadline = XboxHostVsyncNext(&clock, earliest, 19661 /* 0.3 */);
        unsigned shown = i;
        while (tick[shown] < deadline) ++shown;   /* first host tick after the commit */
        if (previousShown && shown - previousShown != 2) ++misses;
        previousShown = shown;
        ++frames;
        int64_t late = (frames % 40 == 20) ? (int64_t)(hz * 6 / 1000) : 0;
        XboxHostVsyncObserve(&clock, tick[shown] + release + Jitter(wobble) + late);
        i = shown;
    }
    assert(frames > 19000);
    assert(misses == 0);
    /* The estimate converges on the host's period. */
    const double estimated = (double)hz * 65536.0 / (double)clock.period16;
    assert(estimated > hostHz - 0.005 && estimated < hostHz + 0.005);

    /* A pause of more than 600 host ticks re-anchors instead of locking onto stale phase. */
    XboxHostVsyncObserve(&clock, tick[39999]);
    assert(clock.samples == 1);
    assert(!XboxHostVsyncLocked(&clock, tick[39999], hz));
}

int main(void)
{
    Run(59.9432);   /* the measured Series X output */
    Run(60.0);      /* a 60.000 Hz display */
    Run(59.94006);  /* exactly 60000/1001 */
    return 0;
}
