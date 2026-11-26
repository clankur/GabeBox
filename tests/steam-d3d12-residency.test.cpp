// Host tests for the Direct3D 12 residency switch and trace format (runtime/win32/d3d12-residency.c).
#include "../runtime/win32/d3d12-residency-policy.h"

#include <cassert>
#include <cstring>
#include <regex>
#include <set>
#include <string>

static uint32_t Parse(const char *text) { return XboxResidencyParseMode(text, static_cast<uint32_t>(std::strlen(text))); }

static std::string Window(const XboxResidencyWindow &window) {
    char line[XBOX_RESIDENCY_LINE_MAX + 4];
    char *end = XboxResidencyFormatWindow(line, &window);
    return std::string(line, end) + "20261126T015013Z";
}

static std::string Total(const XboxResidencyWindow &total, uint32_t mode, uint64_t seconds) {
    char line[XBOX_RESIDENCY_LINE_MAX + 4];
    char *end = XboxResidencyFormatTotal(line, &total, mode, seconds);
    return std::string(line, end) + "20261126T015013Z";
}

static XboxResidencyWindow Empty() {
    XboxResidencyWindow window;
    std::memset(&window, 0, sizeof(window));
    return window;
}

int main() {
    // Package-root file.
    assert(Parse("") == XBOX_RESIDENCY_DEFAULT);
    assert(Parse(" \r\n") == XBOX_RESIDENCY_DEFAULT);
    assert(Parse("pin") == XBOX_RESIDENCY_PIN);
    assert(Parse("pin\r\n") == XBOX_RESIDENCY_PIN);
    assert(Parse("native\n") == XBOX_RESIDENCY_NATIVE);
    assert(Parse("off") == XBOX_RESIDENCY_OFF);
    assert(Parse("Pin") == XBOX_RESIDENCY_INVALID);
    assert(Parse("pinned") == XBOX_RESIDENCY_INVALID);
    assert(Parse("pi") == XBOX_RESIDENCY_INVALID);
    assert(Parse(" pin") == XBOX_RESIDENCY_INVALID);
    assert(Parse("o") == XBOX_RESIDENCY_INVALID);

    // Default and invalid behave as native; only pin and off change anything.
    assert(XboxResidencyEffectiveMode(XBOX_RESIDENCY_DEFAULT) == XBOX_RESIDENCY_NATIVE);
    assert(XboxResidencyEffectiveMode(XBOX_RESIDENCY_INVALID) == XBOX_RESIDENCY_NATIVE);
    assert(XboxResidencyEffectiveMode(XBOX_RESIDENCY_NATIVE) == XBOX_RESIDENCY_NATIVE);
    assert(XboxResidencyEffectiveMode(XBOX_RESIDENCY_PIN) == XBOX_RESIDENCY_PIN);
    assert(XboxResidencyEffectiveMode(XBOX_RESIDENCY_OFF) == XBOX_RESIDENCY_OFF);
    assert(std::strcmp(XboxResidencyModeName(XBOX_RESIDENCY_DEFAULT), "native") == 0);
    assert(std::strcmp(XboxResidencyModeName(XBOX_RESIDENCY_PIN), "pin") == 0);
    assert(std::strcmp(XboxResidencySourceName(XBOX_RESIDENCY_DEFAULT, 0), "default") == 0);
    assert(std::strcmp(XboxResidencySourceName(XBOX_RESIDENCY_PIN, 1), "package-file") == 0);
    assert(std::strcmp(XboxResidencySourceName(XBOX_RESIDENCY_INVALID, 1), "default override=invalid") == 0);

    // A default launch only counts when the pipeline hooks already create a device; an explicit mode always
    // starts the hooks; off never does.
    assert(!XboxResidencyShouldStart(XBOX_RESIDENCY_DEFAULT, 0));
    assert(XboxResidencyShouldStart(XBOX_RESIDENCY_DEFAULT, 1));
    assert(!XboxResidencyShouldStart(XBOX_RESIDENCY_INVALID, 0));
    assert(XboxResidencyShouldStart(XBOX_RESIDENCY_INVALID, 1));
    assert(XboxResidencyShouldStart(XBOX_RESIDENCY_NATIVE, 0));
    assert(XboxResidencyShouldStart(XBOX_RESIDENCY_PIN, 0));
    assert(!XboxResidencyShouldStart(XBOX_RESIDENCY_OFF, 1));

    // Method-table slots, counted from d3d12.idl: ID3D12Device's methods start at 7 after IUnknown and
    // ID3D12Object; MakeResident and Evict are its 28th and 29th, EnqueueMakeResident the last of Device3.
    assert(XBOX_RESIDENCY_SLOT_MAKE_RESIDENT == 7 + 27 && XBOX_RESIDENCY_SLOT_EVICT == 7 + 28);
    assert(XBOX_RESIDENCY_SLOT_ENQUEUE_MAKE_RESIDENT == 7 + 37 + 3 + 1 + 3 - 1);
    // Device4 6, Device5 8, Device6 1, Device7 2, Device8 5, Device9 3, Device10 3, Device11-14 1 each.
    assert(XBOX_RESIDENCY_SLOT_REGISTER_TRIM == XBOX_RESIDENCY_SLOT_ENQUEUE_MAKE_RESIDENT + 1 + 6 + 8 + 1 + 2 + 5 + 3 + 3 + 4);

    // Evicted-address slots stay in range and spread neighbouring allocations.
    std::set<uint32_t> slots;
    for (uint64_t address = 0x7FF600000000ull; address < 0x7FF600000000ull + 4096 * 64; address += 64) {
        const uint32_t slot = XboxResidencyEvictedSlot(address);
        assert(slot < XBOX_RESIDENCY_EVICTED_SLOTS);
        slots.insert(slot);
    }
    assert(slots.size() > 3500);
    assert(XboxResidencyEvictedSlot(0x1234560) == XboxResidencyEvictedSlot(0x1234560));

    assert(XboxResidencyKib(0) == 0 && XboxResidencyKib(1) == 1 && XboxResidencyKib(1024) == 1 &&
           XboxResidencyKib(1025) == 2);

    // A quiet second writes no line.
    XboxResidencyWindow window = Empty();
    assert(!XboxResidencyWindowActive(&window));
    window.trims = 1;
    assert(XboxResidencyWindowActive(&window));
    window = Empty();
    window.failures = 1;
    assert(XboxResidencyWindowActive(&window));

    window = Empty();
    window.evictCalls = 2;
    window.enqueueCalls = 3;
    window.evictObjects = 140;
    window.evictBytes = 3 * 1024 * 1024 + 1;
    window.residentObjects = 150;
    window.residentBytes = 64 * 1024;
    window.restored = 128;
    assert(XboxResidencyWindowActive(&window));
    assert(Window(window) == "asset residency evict=2 makeresident=0 enqueue=3 evict_obj=140 evict_kib=3073 "
                             "res_obj=150 res_kib=64 restored=128 utc=20261126T015013Z");
    window.trims = 1;
    window.failures = 4;
    assert(Window(window).find(" restored=128 trim=1 failed=4 utc=") != std::string::npos);

    // The loader-trace record filter (tools/xbox_diagnostics.py) keeps these lines whole, even with large values.
    const std::regex record("asset [a-zA-Z0-9 ._?@$#=-]{1,174}");
    XboxResidencyWindow large = Empty();
    large.evictCalls = large.makeResidentCalls = large.enqueueCalls = 99999;
    large.evictObjects = large.residentObjects = large.restored = 9999999;
    large.evictBytes = large.residentBytes = 999ull * 1024 * 1024 * 1024;
    large.trims = 9999;
    large.failures = 9999;
    // A busy second (tens of thousands of objects, more GPU memory than the console has) keeps every field.
    XboxResidencyWindow busyWindow = Empty();
    busyWindow.evictCalls = busyWindow.makeResidentCalls = busyWindow.enqueueCalls = 9999;
    busyWindow.evictObjects = busyWindow.residentObjects = busyWindow.restored = 99999;
    busyWindow.evictBytes = busyWindow.residentBytes = 8ull * 1024 * 1024 * 1024;
    busyWindow.trims = 99;
    busyWindow.failures = 99;
    const std::string busy = Window(busyWindow);
    assert(busy.size() <= XBOX_RESIDENCY_LINE_MAX);
    assert(std::regex_match(busy, record));
    assert(busy.find(" restored=99999 trim=99 failed=99 utc=20261126T015013Z") != std::string::npos);
    // Past that, whole fields are dropped from the end; the stamp always stays and no number is cut.
    const std::string absurd = Window(large);
    assert(absurd.size() <= XBOX_RESIDENCY_LINE_MAX);
    assert(std::regex_match(absurd, record));
    assert(absurd.substr(absurd.size() - 20) == "utc=20261126T015013Z");
    assert(absurd.find("failed=") == std::string::npos);

    XboxResidencyWindow total = large;
    total.evictObjects = total.residentObjects = total.restored = 999999999;
    total.trimBytes = 9ull * 1024 * 1024 * 1024 * 1024;
    const std::string minute = Total(total, XBOX_RESIDENCY_NATIVE, 86400);
    assert(minute.size() <= XBOX_RESIDENCY_LINE_MAX);
    assert(std::regex_match(minute, record));
    assert(minute.rfind("asset residency-total mode=native t_s=86400 evict_obj=999999999 ", 0) == 0);
    assert(minute.substr(minute.size() - 20) == "utc=20261126T015013Z");
    assert(Total(Empty(), XBOX_RESIDENCY_PIN, 60) ==
           "asset residency-total mode=pin t_s=60 evict_obj=0 evict_mib=0 res_obj=0 res_mib=0 restored=0 trim=0 "
           "trim_mib=0 failed=0 utc=20261126T015013Z");
    return 0;
}
