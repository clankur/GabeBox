#pragma once
// Video memory budget policy for processes that render through Mesa's d3d12 driver.
//
// Mesa's residency manager trims and evicts against IDXGIAdapter3::QueryVideoMemoryInfo: it keeps every
// allocation resident while usage stays under 70% of local + non-local Budget, and only evicts to make room
// once usage reaches Budget. Xbox reports a local Budget sized from the process memory limit (Factorio logged
// 4147 MB on a Series X), while the adapter's GPU-visible memory is its DedicatedVideoMemory carve-out plus
// SharedSystemMemory, and every process in the partition shares that. Mesa therefore never trims, and the
// adapter runs out of memory during a long session. The policy below caps the reported budget at a share of
// the adapter's GPU-visible capacity so Mesa trims and evicts before the partition is exhausted.
//
// Header-only and free of Windows types so the host tests can exercise it.
#include <stdint.h>

namespace gpu_memory_budget {

constexpr uint64_t mib = 1ull << 20;
// An explicit cap below this cannot hold a 1080p frame's render targets; above it no adapter is that large.
constexpr uint64_t minimumCapMib = 64, maximumCapMib = 1ull << 20;

// The adapter's GPU-visible capacity, saturating instead of wrapping.
constexpr uint64_t Capacity(uint64_t dedicated, uint64_t shared) {
    return dedicated > UINT64_MAX - shared ? UINT64_MAX : dedicated + shared;
}

// Half of the capacity: the rest stays available to the other processes that render in the same partition.
constexpr uint64_t DefaultCap(uint64_t capacity) { return capacity / 2; }

struct Reported {
    uint64_t local, nonLocal;
};

// Caps the two segment groups so their sum, which is what Mesa compares usage against, never exceeds `cap`.
// The local group keeps its share first, as Mesa allocates in it on unified-memory adapters. A native budget
// below the cap passes through unchanged.
constexpr Reported Apply(uint64_t cap, uint64_t nativeLocal, uint64_t nativeNonLocal) {
    const uint64_t local = nativeLocal < cap ? nativeLocal : cap;
    const uint64_t rest = cap - local;
    return {local, nativeNonLocal < rest ? nativeNonLocal : rest};
}

// GPU usage cannot exceed the adapter's capacity. If the native usage does, it counts more than GPU memory,
// and a cap compared against it would make Mesa evict every allocation it holds.
constexpr bool UsageFitsCapacity(uint64_t localUsage, uint64_t nonLocalUsage, uint64_t capacity) {
    return localUsage <= capacity && nonLocalUsage <= capacity - localUsage;
}

enum class Mode { Default, Explicit, Native, Invalid };

struct Override {
    Mode mode;
    uint64_t capMib;
};

// Parses the optional package-root override: "native" keeps the console's budget, a decimal number of MiB
// sets the cap, and an empty file selects the default. Trailing whitespace is ignored.
inline Override ParseOverride(const char* text, uint32_t length) {
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r' ||
                      text[length - 1] == '\n'))
        --length;
    if (!length) return {Mode::Default, 0};
    static constexpr char native[] = "native";
    if (length == sizeof(native) - 1) {
        uint32_t index = 0;
        while (index < length && text[index] == native[index]) ++index;
        if (index == length) return {Mode::Native, 0};
    }
    uint64_t value = 0;
    for (uint32_t index = 0; index < length; ++index) {
        if (text[index] < '0' || text[index] > '9' || value > maximumCapMib) return {Mode::Invalid, 0};
        value = value * 10 + static_cast<uint64_t>(text[index] - '0');
    }
    if (value < minimumCapMib || value > maximumCapMib) return {Mode::Invalid, 0};
    return {Mode::Explicit, value};
}

}  // namespace gpu_memory_budget
