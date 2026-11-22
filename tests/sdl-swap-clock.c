#include "../probe/sdl-swap-clock.h"
#include <assert.h>

int main(void)
{
    const uint64_t hz = 10000003; /* not divisible by 60 */
    XboxSdlSwapClock clock = {0, hz, 0, 0};
    uint64_t now = 1000, origin = now;
    assert(XboxSdlNextSwap(&clock, now, 1) == now);
    for (unsigned draw = 1; draw <= 3600; ++draw) {
        now += hz / 200; /* 5 ms game render */
        uint64_t deadline = XboxSdlNextSwap(&clock, now, 1);
        assert(deadline == origin + draw * hz / 60);
        uint64_t sleep_ms = ((deadline - now) * 1000 + hz - 1) / hz;
        now += (sleep_ms + 1) * hz / 1000; /* an extra millisecond every sleep */
        assert(now - deadline < 2 * hz / 1000);
    }
    assert(clock.deadline == origin + hz * 60);

    /* Delayed presents do not cause a catch-up burst. Changing refresh
       intervals or disabling vsync must not inherit the old schedule. */
    now += hz * 2;
    assert(XboxSdlNextSwap(&clock, now, 1) == now);
    assert(XboxSdlNextSwap(&clock, now + 1, 1) == now + hz / 60);
    assert(XboxSdlNextSwap(&clock, now, 0) == now);
    assert(XboxSdlNextSwap(&clock, now, 2) == now);
    assert(XboxSdlNextSwap(&clock, now + 2, 2) == now + hz / 30);
    return 0;
}
