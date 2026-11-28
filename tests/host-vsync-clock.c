#include "../runtime/shared/host-vsync-clock.h"
#include <assert.h>

/* A host near 60 Hz draws on each refresh and releases the frames it read 1.5 ms later, with +-0.6 ms of jitter;
   one release in 40 is 6 ms late. Under load (busyMs) the host's draw callback itself runs late on 30% of
   refreshes, by up to busyMs, and it shows whatever was committed before the callback ran. A producer presenting
   every second refresh at the target must land in every second host interval, with no slot repeated or skipped. */
static uint32_t seed = 12345;
static int64_t Jitter(int64_t range) { seed = seed * 1103515245u + 12345u; return (int64_t)(seed >> 8) % (2 * range + 1) - range; }
static uint64_t Uniform(uint64_t range) { seed = seed * 1103515245u + 12345u; return range ? (uint64_t)(seed >> 8) % range : 0; }

static uint64_t tick[40000], draw[40000];

static void Run(double hostHz, unsigned busyMs)
{
    const uint64_t hz = 1745500000ull;
    const uint64_t origin = 1000000000ull;
    for (unsigned i = 0; i < 40000; ++i) {
        tick[i] = origin + (uint64_t)(i * (hz / hostHz));
        draw[i] = tick[i] + (Uniform(10) < 3 ? Uniform(hz / 1000 * busyMs) : 0);
    }
    const int64_t release = (int64_t)(hz * 15 / 10000), wobble = (int64_t)(hz * 6 / 10000);

    XboxHostVsync clock;
    XboxHostVsyncInit(&clock, hz);
    assert(clock.period16 > 0);
    assert(!XboxHostVsyncLocked(&clock, origin, hz));

    /* Free-running 30 Hz producer: the host releases every second refresh. */
    for (unsigned i = 0; i < 80; i += 2) {
        int64_t late = (i % 40 == 20) ? (int64_t)(hz * 6 / 1000) : 0;
        XboxHostVsyncObserve(&clock, draw[i] + release + Jitter(wobble) + late);
    }
    assert(XboxHostVsyncLocked(&clock, tick[79], hz));

    /* Paced: each present is committed at the target; the host shows it on the first draw after the commit. */
    uint64_t deadline = 0;
    unsigned previousShown = 0, frames = 0, misses = 0;
    for (unsigned i = 80; i < 39000;) {
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
    Run(59.9432, 10);  /* the host's draw callback up to 10 ms late on 30% of refreshes */
    return 0;
}
