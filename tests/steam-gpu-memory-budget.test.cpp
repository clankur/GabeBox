// Host tests for the video memory budget the DXGI adapter reports to Mesa's d3d12 driver.
#include "../runtime/win32/gpu-memory-budget.h"

#include <cassert>
#include <cstring>

using namespace gpu_memory_budget;

static Override Parse(const char* text) { return ParseOverride(text, static_cast<uint32_t>(std::strlen(text))); }

int main() {
    // Capacity is the adapter's carve-out plus its shared system memory, saturating.
    assert(Capacity(512 * mib, 832 * mib) == 1344 * mib);
    assert(Capacity(UINT64_MAX - 1, 2) == UINT64_MAX);
    assert(DefaultCap(1344 * mib) == 672 * mib);

    // A local budget sized from the process memory limit is capped; the non-local group gets what remains.
    Reported capped = Apply(672 * mib, 4147 * mib, 0);
    assert(capped.local == 672 * mib && capped.nonLocal == 0);
    capped = Apply(672 * mib, 400 * mib, 1000 * mib);
    assert(capped.local == 400 * mib && capped.nonLocal == 272 * mib);
    // The sum Mesa compares against never exceeds the cap.
    capped = Apply(672 * mib, 4147 * mib, 4147 * mib);
    assert(capped.local + capped.nonLocal == 672 * mib);
    // A native budget already under the cap passes through unchanged.
    capped = Apply(672 * mib, 300 * mib, 100 * mib);
    assert(capped.local == 300 * mib && capped.nonLocal == 100 * mib);
    capped = Apply(0, 300 * mib, 100 * mib);
    assert(capped.local == 0 && capped.nonLocal == 0);

    // Usage larger than the adapter counts more than GPU memory.
    assert(UsageFitsCapacity(900 * mib, 400 * mib, 1344 * mib));
    assert(UsageFitsCapacity(1344 * mib, 0, 1344 * mib));
    assert(!UsageFitsCapacity(1345 * mib, 0, 1344 * mib));
    assert(!UsageFitsCapacity(1000 * mib, 400 * mib, 1344 * mib));
    assert(!UsageFitsCapacity(10, UINT64_MAX, 1344 * mib));

    // Package-root override.
    assert(Parse("").mode == Mode::Default);
    assert(Parse(" \r\n").mode == Mode::Default);
    assert(Parse("native").mode == Mode::Native);
    assert(Parse("native\r\n").mode == Mode::Native);
    assert(Parse("Native").mode == Mode::Invalid);
    assert(Parse("nativex").mode == Mode::Invalid);
    Override value = Parse("768\n");
    assert(value.mode == Mode::Explicit && value.capMib == 768);
    value = Parse("64");
    assert(value.mode == Mode::Explicit && value.capMib == 64);
    assert(Parse("63").mode == Mode::Invalid);
    assert(Parse("1048577").mode == Mode::Invalid);
    assert(Parse("99999999999999999999999").mode == Mode::Invalid);
    assert(Parse("-5").mode == Mode::Invalid);
    assert(Parse("7 68").mode == Mode::Invalid);
    assert(Parse("0x300").mode == Mode::Invalid);
    return 0;
}
