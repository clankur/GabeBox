// Host tests for the Direct3D 12 fence-wait trace (runtime/win32/d3d12-fence-trace.c): the package-root switch,
// the check after a NULL-event SetEventOnCompletion against a fake fence, and the trace format.
#include "../runtime/win32/d3d12-fence-policy.h"

#include <cassert>
#include <cstring>
#include <regex>
#include <string>

static uint32_t Parse(const char *text) { return XboxFenceParseMode(text, static_cast<uint32_t>(std::strlen(text))); }

// A fence whose completed value reaches `target` after `pollsUntilDone` reads, on a clock that advances
// `usPerRead` per clock read.
struct FakeFence {
    uint64_t completed = 0;
    uint64_t target = 0;
    uint32_t reads = 0;
    uint32_t pollsUntilDone = 0;
    uint64_t clockUs = 1000;
    uint64_t usPerRead = 0;
    uint32_t pauses = 0;
    uint32_t lastRound = 0;
};

static uint64_t Completed(void *context) {
    FakeFence *fence = static_cast<FakeFence *>(context);
    if (fence->reads++ >= fence->pollsUntilDone) fence->completed = fence->target;
    return fence->completed;
}

static uint64_t NowUs(void *context) {
    FakeFence *fence = static_cast<FakeFence *>(context);
    fence->clockUs += fence->usPerRead;
    return fence->clockUs;
}

static void Pause(void *context, uint32_t round) {
    FakeFence *fence = static_cast<FakeFence *>(context);
    ++fence->pauses;
    fence->lastRound = round;
}

static XboxFenceCheck Check(FakeFence &fence, uint64_t value, uint32_t mode, uint64_t timeoutUs) {
    const XboxFenceOps ops = {Completed, NowUs, Pause, &fence};
    XboxFenceCheck check;
    std::memset(&check, 0xA5, sizeof(check));
    XboxFenceCheckWait(&ops, value, mode, timeoutUs, &check);
    return check;
}

static XboxFenceWindow Empty() {
    XboxFenceWindow window;
    std::memset(&window, 0, sizeof(window));
    return window;
}

static const char Stamp[] = "20261126T133859.960Z";

static std::string Window(const XboxFenceWindow &window) {
    char line[XBOX_FENCE_LINE_MAX + 4];
    char *end = XboxFenceFormatWindow(line, &window);
    return std::string(line, end) + Stamp;
}

static std::string Total(const XboxFenceWindow &total, uint32_t mode, uint64_t seconds) {
    char line[XBOX_FENCE_LINE_MAX + 4];
    char *end = XboxFenceFormatTotal(line, &total, mode, seconds);
    return std::string(line, end) + Stamp;
}

static std::string Event(const char *kind, uint64_t value, uint64_t completed, uint64_t waitUs,
                         const XboxFenceCheck &check, uint32_t mode) {
    char line[XBOX_FENCE_LINE_MAX + 4];
    char *end = XboxFenceFormatEvent(line, kind, value, completed, waitUs, &check, mode);
    return std::string(line, end) + Stamp;
}

int main() {
    // Package-root file: present and empty traces; only wait changes behavior; off and absent install nothing.
    assert(Parse("") == XBOX_FENCE_TRACE);
    assert(Parse("\r\n") == XBOX_FENCE_TRACE);
    assert(Parse("trace") == XBOX_FENCE_TRACE);
    assert(Parse("mode=trace\n") == XBOX_FENCE_TRACE);
    assert(Parse("wait") == XBOX_FENCE_WAIT);
    assert(Parse("mode=wait\r\n") == XBOX_FENCE_WAIT);
    assert(Parse("off") == XBOX_FENCE_OFF);
    assert(Parse("mode=off") == XBOX_FENCE_OFF);
    assert(Parse("mode=") == XBOX_FENCE_INVALID);
    assert(Parse("mode=waiting") == XBOX_FENCE_INVALID);
    assert(Parse("Wait") == XBOX_FENCE_INVALID);
    assert(Parse(" wait") == XBOX_FENCE_INVALID);
    assert(Parse("wai") == XBOX_FENCE_INVALID);
    assert(XboxFenceEffectiveMode(XBOX_FENCE_ABSENT) == XBOX_FENCE_OFF);
    assert(XboxFenceEffectiveMode(XBOX_FENCE_OFF) == XBOX_FENCE_OFF);
    assert(XboxFenceEffectiveMode(XBOX_FENCE_INVALID) == XBOX_FENCE_TRACE);
    assert(XboxFenceEffectiveMode(XBOX_FENCE_TRACE) == XBOX_FENCE_TRACE);
    assert(XboxFenceEffectiveMode(XBOX_FENCE_WAIT) == XBOX_FENCE_WAIT);
    assert(std::strcmp(XboxFenceModeName(XBOX_FENCE_WAIT), "wait") == 0);
    assert(std::strcmp(XboxFenceModeName(XBOX_FENCE_TRACE), "trace") == 0);
    assert(std::strcmp(XboxFenceModeName(XBOX_FENCE_OFF), "off") == 0);
    assert(std::strcmp(XboxFenceSourceName(XBOX_FENCE_INVALID), "package-file override=invalid") == 0);
    assert(std::strcmp(XboxFenceSourceName(XBOX_FENCE_WAIT), "package-file") == 0);

    // Method-table slots, counted from d3d12.idl. ID3D12Device: CreateCommandQueue is its 2nd method and
    // CreateFence its 30th (after MakeResident and Evict). ID3D12Fence and ID3D12CommandQueue methods start at
    // 8, after IUnknown (3), ID3D12Object (4) and ID3D12DeviceChild::GetDevice; ID3D12Pageable adds none.
    assert(XBOX_FENCE_SLOT_CREATE_COMMAND_QUEUE == 7 + 1);
    assert(XBOX_FENCE_SLOT_CREATE_FENCE == XBOX_RESIDENCY_SLOT_EVICT + 1);
    assert(XBOX_FENCE_SLOT_GET_COMPLETED_VALUE == 8 && XBOX_FENCE_SLOT_SET_EVENT_ON_COMPLETION == 9);
    assert(XBOX_FENCE_SLOT_QUEUE_EXECUTE == 8 + 2 && XBOX_FENCE_SLOT_QUEUE_SIGNAL == 8 + 6);

    // A wait that blocked until the fence passed: not early, one read, no polling.
    FakeFence done;
    done.target = 41;
    XboxFenceCheck check = Check(done, 41, XBOX_FENCE_WAIT, XBOX_FENCE_SETTLE_TIMEOUT_US);
    assert(!check.early && !check.settled && !check.timedOut && check.settleUs == 0);
    assert(check.completedAfterWait == 41 && check.completed == 41 && done.reads == 1 && done.pauses == 0);

    // A fence past the value (a later batch already completed) is not early either.
    FakeFence ahead;
    ahead.target = 90;
    check = Check(ahead, 41, XBOX_FENCE_TRACE, XBOX_FENCE_SETTLE_TIMEOUT_US);
    assert(!check.early && check.completedAfterWait == 90);

    // Device removal: GetCompletedValue returns UINT64_MAX, which is never early.
    FakeFence removed;
    removed.target = UINT64_MAX;
    check = Check(removed, 41, XBOX_FENCE_WAIT, XBOX_FENCE_SETTLE_TIMEOUT_US);
    assert(!check.early && removed.pauses == 0);

    // An early return in trace mode is recorded and nothing else happens: one read, no pause, no clock.
    FakeFence early;
    early.completed = 40;
    early.target = 41;
    early.pollsUntilDone = 5;
    early.usPerRead = 100;
    check = Check(early, 41, XBOX_FENCE_TRACE, XBOX_FENCE_SETTLE_TIMEOUT_US);
    assert(check.early && !check.settled && !check.timedOut);
    assert(check.completedAfterWait == 40 && check.completed == 40);
    assert(early.reads == 1 && early.pauses == 0 && early.clockUs == 1000);

    // The same early return in wait mode polls until the fence reaches the value.
    FakeFence settles;
    settles.completed = 40;
    settles.target = 41;
    settles.pollsUntilDone = 4; // the post-wait read and three polls see 40, the fourth poll 41
    settles.usPerRead = 250;
    check = Check(settles, 41, XBOX_FENCE_WAIT, XBOX_FENCE_SETTLE_TIMEOUT_US);
    assert(check.early && check.settled && !check.timedOut);
    assert(check.completedAfterWait == 40 && check.completed == 41);
    assert(settles.pauses == 4 && settles.lastRound == 3);
    assert(check.settleUs == 4 * 250);

    // A fence that never reaches the value stops at the timeout and reports how far it got.
    FakeFence stuck;
    stuck.completed = 7;
    stuck.target = 7;
    stuck.usPerRead = 1000;
    check = Check(stuck, 41, XBOX_FENCE_WAIT, 5000);
    assert(check.early && !check.settled && check.timedOut);
    assert(check.completed == 7 && check.settleUs == 5000 && stuck.pauses == 5);
    // The real bound is 2 s, polled in 1 ms sleeps after the first rounds.
    stuck = FakeFence();
    stuck.usPerRead = 1000;
    check = Check(stuck, 1, XBOX_FENCE_WAIT, XBOX_FENCE_SETTLE_TIMEOUT_US);
    assert(check.timedOut && check.settleUs == XBOX_FENCE_SETTLE_TIMEOUT_US && stuck.pauses == 2000);

    // A quiet second writes no line.
    XboxFenceWindow window = Empty();
    assert(!XboxFenceWindowActive(&window));
    window.exec = 1;
    assert(XboxFenceWindowActive(&window));
    window = Empty();
    window.eventWaits = 1;
    assert(XboxFenceWindowActive(&window));

    // The per-second line: the four fields always, the rest only when non-zero.
    window = Empty();
    window.nullWaits = 476;
    window.maxWaitUs = 16210;
    window.exec = 478;
    assert(Window(window) == "asset d3d12-fence null_waits=476 early=0 max_wait_us=16210 exec=478 "
                             "utc=20261126T133859.960Z");
    window.early = 3;
    window.eventWaits = 2;
    window.settled = 3;
    window.maxSettleUs = 820;
    window.timeouts = 1;
    window.failed = 1;
    assert(Window(window) == "asset d3d12-fence null_waits=476 early=3 max_wait_us=16210 exec=478 event_waits=2 "
                             "settled=3 max_settle_us=820 timeouts=1 failed=1 utc=20261126T133859.960Z");

    // The loader-trace record filter (tools/xbox_diagnostics.py) keeps these lines whole, even with large values.
    const std::regex record("asset [a-zA-Z0-9 ._?@$#=-]{1,174}");
    XboxFenceWindow large;
    std::memset(&large, 0xFF, sizeof(large));
    const std::string absurd = Window(large);
    assert(absurd.size() <= XBOX_FENCE_LINE_MAX);
    assert(std::regex_match(absurd, record));
    assert(absurd.substr(absurd.size() - 24) == std::string("utc=") + Stamp);
    // Whole fields are dropped from the end; no number is cut.
    assert(absurd.find("failed=") == std::string::npos);
    XboxFenceWindow busy = Empty();
    busy.nullWaits = busy.exec = busy.eventWaits = 99999;
    busy.early = busy.settled = busy.timeouts = busy.failed = 9999;
    busy.maxWaitUs = busy.maxSettleUs = XBOX_FENCE_SETTLE_TIMEOUT_US;
    const std::string full = Window(busy);
    assert(full.size() <= XBOX_FENCE_LINE_MAX && std::regex_match(full, record));
    assert(full.find(" timeouts=9999 failed=9999 utc=") != std::string::npos);

    const std::string minute = Total(window, XBOX_FENCE_WAIT, 60);
    assert(minute == "asset d3d12-fence-total mode=wait t_s=60 null_waits=476 early=3 max_wait_us=16210 exec=478 "
                     "event_waits=2 settled=3 timeouts=1 failed=1 utc=20261126T133859.960Z");
    const std::string huge = Total(large, XBOX_FENCE_TRACE, 86400);
    assert(huge.size() <= XBOX_FENCE_LINE_MAX && std::regex_match(huge, record));

    // The first early return and a timeout carry the requested and completed values.
    FakeFence first;
    first.completed = 1233;
    first.target = 1234;
    first.pollsUntilDone = 1;
    check = Check(first, 1234, XBOX_FENCE_TRACE, XBOX_FENCE_SETTLE_TIMEOUT_US);
    assert(check.early);
    assert(Event("first-early", 1234, check.completedAfterWait, 12, check, XBOX_FENCE_TRACE) ==
           "asset d3d12-fence first-early mode=trace value=1234 completed=1233 wait_us=12 utc=20261126T133859.960Z");
    stuck = FakeFence();
    stuck.usPerRead = 1000;
    check = Check(stuck, 1234, XBOX_FENCE_WAIT, 3000);
    assert(Event("timeout", 1234, check.completedAfterWait, 7, check, XBOX_FENCE_WAIT) ==
           "asset d3d12-fence timeout mode=wait value=1234 completed=0 wait_us=7 settle_us=3000 settle_completed=0 "
           "utc=20261126T133859.960Z");
    XboxFenceCheck extreme = check;
    extreme.settleUs = extreme.completed = UINT64_MAX;
    const std::string widest = Event("first-early", UINT64_MAX, UINT64_MAX - 1, UINT64_MAX, extreme, XBOX_FENCE_WAIT);
    assert(widest.size() <= XBOX_FENCE_LINE_MAX && std::regex_match(widest, record));
    assert(widest.rfind("asset d3d12-fence first-early mode=wait value=", 0) == 0);

    // Millisecond UTC stamp.
    char stamp[XBOX_FENCE_UTC_CHARS + 1];
    char *end = XboxFenceUtc(stamp, 2026, 11, 26, 13, 38, 59, 960);
    assert(end - stamp == static_cast<long>(XBOX_FENCE_UTC_CHARS));
    assert(std::string(stamp, end) == Stamp);
    end = XboxFenceUtc(stamp, 2027, 1, 2, 3, 4, 5, 6);
    assert(std::string(stamp, end) == "20270102T030405.006Z");
    return 0;
}
