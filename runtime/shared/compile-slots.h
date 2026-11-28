#pragma once
/* Admission policy for a bounded pool of pipeline (PSO) compiles.

   The Xbox D3D12 runtime has no pipeline cache, so a title recompiles every pipeline on each
   launch. Each compile is single-threaded, so throughput is bounded by how many run at once.
   Running too many at once with little commit left faulted the Xbox shader compiler, so the
   pool has a fixed base and grows only while available commit is high, as it is during boot.

   Requests come in three classes:
   - urgent: a thread that blocks frames while it waits (the render or main thread). It goes
     ahead of every waiting request, and while commit allows it may run one compile beyond the
     limit, so it never waits behind a queue of bulk compiles.
   - normal: the title's own worker-thread compiles.
   - background: speculative work such as a pipeline-replay thread. It runs only when no title
     request is waiting, so it never delays a compile the title is blocked on.

   The policy is a pure function of the counters so it can be tested off-console. The caller
   holds its own lock and re-evaluates after every release and periodically, because the
   commit-dependent limit can change while a request waits. */

enum {
    XboxCompileBackground = 0,
    XboxCompileNormal = 1,
    XboxCompileUrgent = 2,
};

typedef struct XboxCompileSlots {
    unsigned base;          /* compiles allowed at any commit level, at least 1 */
    unsigned boost;         /* extra compiles allowed while available commit >= boostMinMiB */
    unsigned boostMinMiB;
    unsigned urgentMinMiB;  /* an urgent request may exceed the limit by one at or above this */
    unsigned active;        /* compiles in progress */
    unsigned waiting[3];    /* requests waiting, by class */
} XboxCompileSlots;

static inline unsigned XboxCompileSlotLimit(const XboxCompileSlots *slots, unsigned availableMiB)
{
    const unsigned base = slots->base ? slots->base : 1;
    return base + (slots->boost && availableMiB >= slots->boostMinMiB ? slots->boost : 0);
}

/* Whether a request of this class may start a compile now. A waiting request counts itself in
   waiting[kind]; that never blocks it, since each class yields only to higher classes. */
static inline int XboxCompileSlotMayStart(const XboxCompileSlots *slots, int kind, unsigned availableMiB)
{
    const unsigned limit = XboxCompileSlotLimit(slots, availableMiB);
    if (kind == XboxCompileUrgent)
        return slots->active < limit + (availableMiB >= slots->urgentMinMiB ? 1u : 0u);
    if (slots->waiting[XboxCompileUrgent]) return 0;
    if (kind == XboxCompileBackground && slots->waiting[XboxCompileNormal]) return 0;
    return slots->active < limit;
}
