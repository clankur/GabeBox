#include "../runtime/shared/host-vsync-clock.h"
#include <assert.h>

/* A host near 60 Hz draws on each refresh, +-0.6 ms, and releases the frames it read 0.5 ms later; one release in
   40 is 6 ms late. Under load (jitterMs) the draw callback itself comes up to jitterMs early or late on 3% of
   refreshes (at Fisk Tower 98% of callbacks are within +-2.5 ms), and shows whatever was committed before it ran. A producer presenting every
   second refresh at the target must land in every second host interval, with no slot repeated or skipped. */
static uint32_t seed = 12345;
static int64_t Jitter(int64_t range) { seed = seed * 1103515245u + 12345u; return (int64_t)(seed >> 8) % (2 * range + 1) - range; }
static uint64_t Uniform(uint64_t range) { seed = seed * 1103515245u + 12345u; return range ? (uint64_t)(seed >> 8) % range : 0; }

static uint64_t tick[40000], draw[40000];

static void Run(double hostHz, unsigned jitterMs)
{
    const uint64_t hz = 1745500000ull;
    const uint64_t origin = 1000000000ull;
    for (unsigned i = 0; i < 40000; ++i) {
        tick[i] = origin + (uint64_t)(i * (hz / hostHz));
        draw[i] = tick[i] + (uint64_t)Jitter(hz * 6 / 10000) +
            (uint64_t)(Uniform(100) < 3 ? Jitter((int64_t)(hz / 1000) * jitterMs) : 0);
    }
    const int64_t release = (int64_t)(hz * 5 / 10000), wobble = (int64_t)(hz * 3 / 10000);

    XboxHostVsync clock;
    XboxHostVsyncInit(&clock, hz);
    assert(clock.period16 > 0);
    assert(!XboxHostVsyncLocked(&clock, origin, hz));

    /* Free-running 30 Hz producer: the host releases every second refresh; the very first release is 8 ms late. */
    for (unsigned i = 0; i < 300; i += 2) {
        int64_t late = (i % 40 == 20) ? (int64_t)(hz * 6 / 1000) : 0;
        XboxHostVsyncObserve(&clock, draw[i] + release + Jitter(wobble) + late + (i == 0 ? (int64_t)(hz / 1000 * 8) : 0));
        if (i == 100) assert(!XboxHostVsyncLocked(&clock, tick[101], hz)); /* not before two seconds of refreshes */
    }
    assert(XboxHostVsyncLocked(&clock, tick[299], hz));

    /* Paced: each present is committed at the target; the host shows it on the first draw after the commit. */
    uint64_t deadline = 0;
    unsigned previousShown = 0, frames = 0, misses = 0;
    for (unsigned i = 300; i < 39000;) {
        const uint64_t period = (uint64_t)(clock.period16 >> 16);
        const uint64_t earliest = deadline ? deadline + 2 * period - period / 2 : tick[i];
        deadline = XboxHostVsyncNext(&clock, earliest, XBOX_HOST_VSYNC_COMMIT16);
        unsigned shown = i;
        while (draw[shown] < deadline) ++shown;
        if (previousShown && shown - previousShown != 2) ++misses;
        previousShown = shown;
        ++frames;
        int64_t late = (frames % 40 == 20) ? (int64_t)(hz * 6 / 1000) : 0;
        XboxHostVsyncObserve(&clock, draw[shown] + release + Jitter(wobble) + late);
        i = shown;
    }
    assert(frames > 19000);
    assert(misses == 0);
    /* The estimate converges on the host's period. */
    const double estimated = (double)hz * 65536.0 / (double)clock.period16;
    assert(estimated > hostHz - 0.005 && estimated < hostHz + 0.005);

    /* A pause of more than 600 host refreshes starts again instead of pacing by a stale phase. */
    XboxHostVsyncObserve(&clock, tick[39999]);
    assert(clock.samples == 1);
    assert(!XboxHostVsyncLocked(&clock, tick[39999], hz));
}

int main(void)
{
    Run(59.9432, 0);   /* the measured Series X output */
    Run(60.0, 0);      /* a 60.000 Hz display */
    Run(59.94006, 0);  /* exactly 60000/1001 */
    Run(59.9432, 5);   /* the host's draw callback up to 5 ms early or late on 3% of refreshes */
    Run(60.0, 5);
    return 0;
}
