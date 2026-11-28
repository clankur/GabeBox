#include "../runtime/shared/compile-slots.h"
#include <assert.h>

int main(void)
{
    /* Spider-Man play loader: 4 slots, 2 more at >= 768 MiB, urgent overflow at >= 256 MiB. */
    XboxCompileSlots s = {4, 2, 768, 256, 0, {0, 0, 0}};

    /* The boost applies only while commit is high. */
    assert(XboxCompileSlotLimit(&s, 1200) == 6);
    assert(XboxCompileSlotLimit(&s, 768) == 6);
    assert(XboxCompileSlotLimit(&s, 767) == 4);
    assert(XboxCompileSlotLimit(&s, 0) == 4);

    /* Boot: six worker compiles may run at once; a seventh waits. */
    s.active = 5;
    assert(XboxCompileSlotMayStart(&s, XboxCompileNormal, 1000));
    s.active = 6;
    assert(!XboxCompileSlotMayStart(&s, XboxCompileNormal, 1000));
    /* Gameplay headroom drops the pool back to four. */
    s.active = 4;
    assert(!XboxCompileSlotMayStart(&s, XboxCompileNormal, 400));

    /* An urgent request never queues behind a full pool while commit allows one more. */
    s.active = 6;
    assert(XboxCompileSlotMayStart(&s, XboxCompileUrgent, 1000));
    s.active = 7;
    assert(!XboxCompileSlotMayStart(&s, XboxCompileUrgent, 1000));
    s.active = 4;
    assert(XboxCompileSlotMayStart(&s, XboxCompileUrgent, 300));
    /* With little commit left it gets no extra slot, only priority for the next free one. */
    assert(!XboxCompileSlotMayStart(&s, XboxCompileUrgent, 200));
    s.active = 3;
    s.waiting[XboxCompileUrgent] = 1;
    s.waiting[XboxCompileNormal] = 3;
    assert(XboxCompileSlotMayStart(&s, XboxCompileUrgent, 200));
    assert(!XboxCompileSlotMayStart(&s, XboxCompileNormal, 200));
    assert(!XboxCompileSlotMayStart(&s, XboxCompileBackground, 200));

    /* Background work runs only when no title compile waits. */
    s.waiting[XboxCompileUrgent] = 0;
    s.waiting[XboxCompileBackground] = 1;
    assert(XboxCompileSlotMayStart(&s, XboxCompileNormal, 200));
    assert(!XboxCompileSlotMayStart(&s, XboxCompileBackground, 200));
    s.waiting[XboxCompileNormal] = 0;
    assert(XboxCompileSlotMayStart(&s, XboxCompileBackground, 200));
    s.active = 4;
    assert(!XboxCompileSlotMayStart(&s, XboxCompileBackground, 200));

    /* Without boost or urgency (PSO_BOOST_SLOTS=0, PSO_PRIORITY=0) the pool is a fixed semaphore. */
    XboxCompileSlots fixed = {4, 0, 768, 0xFFFFFFFFu, 3, {0, 0, 0}};
    assert(XboxCompileSlotLimit(&fixed, 4000) == 4);
    assert(XboxCompileSlotMayStart(&fixed, XboxCompileNormal, 4000));
    fixed.active = 4;
    assert(!XboxCompileSlotMayStart(&fixed, XboxCompileNormal, 4000));
    assert(!XboxCompileSlotMayStart(&fixed, XboxCompileUrgent, 4000));

    /* A zero base still admits one compile. */
    XboxCompileSlots zero = {0, 0, 0, 0xFFFFFFFFu, 0, {0, 0, 0}};
    assert(XboxCompileSlotLimit(&zero, 0) == 1);
    assert(XboxCompileSlotMayStart(&zero, XboxCompileNormal, 0));
    return 0;
}
