#pragma once
#include <cstddef>
#include <cstdint>
#include "../shared/sdl-gpu-probe-stats.h"

// Flags one source's frames (the game's renderbuffer, the exchange texture or the presenter's
// output) that flash white against their neighbours. tools/flash_probe.py applies the same rule
// with the same constants, so keep the two in step.
//
// A frame is a flash when it is brighter than the median of up to Baseline earlier frames, by
// JumpLuma in mean luma (and at least BrightLuma), or by whiteJump() near-white cells, and one of
// the next Lookahead frames is back within the return margin of that median. A change that lasts
// longer is a scene change, not a flash. A frame whose every cell is near white is flagged
// UniformWhite on its own.
namespace FhdFlash {
constexpr unsigned Baseline = 8, MinBaseline = 4, Lookahead = 3;
constexpr unsigned BrightLuma = 128 * 256, JumpLuma = 40 * 256, ReturnLuma = 16 * 256;
enum : uint16_t { Flash = 1, UniformWhite = 2, Mismatch = 4 };

inline unsigned whiteJump(unsigned cells) { return cells / 64 > 8 ? cells / 64 : 8; }
inline unsigned whiteReturn(unsigned cells) { return cells / 128 > 4 ? cells / 128 : 4; }
inline bool white(uint16_t flags) { return (flags & (Flash | UniformWhite)) != 0; }

// The upper median of n values (n <= Baseline).
template <class Get>
unsigned median(unsigned n, Get get) {
    unsigned values[Baseline];
    for (unsigned i = 0; i < n; ++i) {
        unsigned v = get(i), j = i;
        for (; j && values[j - 1] > v; --j) values[j] = values[j - 1];
        values[j] = v;
    }
    return n ? values[n / 2] : 0;
}

inline uint16_t classify(const SdlGpuProbeStats *before, unsigned beforeCount, const SdlGpuProbeStats &frame,
                         const SdlGpuProbeStats *after, unsigned afterCount) {
    uint16_t flags = frame.cells && frame.min >= SDL_GPU_PROBE_WHITE ? UniformWhite : 0;
    if (beforeCount < MinBaseline) return flags;
    const unsigned luma = median(beforeCount, [&](unsigned i) { return unsigned(before[i].luma); });
    const unsigned whites = median(beforeCount, [&](unsigned i) { return unsigned(before[i].white); });
    const bool bright = frame.luma >= BrightLuma && frame.luma >= luma + JumpLuma;
    if (!bright && frame.white < whites + whiteJump(frame.cells)) return flags;
    for (unsigned i = 0; i < afterCount; ++i)
        if (after[i].luma <= luma + ReturnLuma && after[i].white <= whites + whiteReturn(after[i].cells))
            return flags | Flash;
    return flags;
}

// Streams one source. Each pushed item comes back Lookahead items later with its flags.
template <class Item>
class Detector {
public:
    // Item has a member `stats` (SdlGpuProbeStats) and `flags` (uint16_t).
    bool push(const Item &item, Item &out) {
        pending_[pendingCount_++] = item;
        if (pendingCount_ <= Lookahead) return false;
        emit(out);
        return true;
    }
    // Releases the oldest pending item when the stream ends.
    bool drain(Item &out) {
        if (!pendingCount_) return false;
        emit(out);
        return true;
    }
    void reset() { historyCount_ = historyNext_ = pendingCount_ = 0; }
private:
    void emit(Item &out) {
        SdlGpuProbeStats before[Baseline], after[Lookahead];
        for (unsigned i = 0; i < historyCount_; ++i)
            before[i] = history_[(historyNext_ + Baseline - historyCount_ + i) % Baseline];
        const unsigned afterCount = pendingCount_ - 1;
        for (unsigned i = 0; i < afterCount; ++i) after[i] = pending_[i + 1].stats;
        out = pending_[0];
        out.flags = static_cast<uint16_t>(out.flags | classify(before, historyCount_, out.stats, after, afterCount));
        history_[historyNext_] = out.stats;
        historyNext_ = (historyNext_ + 1) % Baseline;
        if (historyCount_ < Baseline) ++historyCount_;
        for (unsigned i = 0; i < afterCount; ++i) pending_[i] = pending_[i + 1];
        --pendingCount_;
    }
    SdlGpuProbeStats history_[Baseline]{};
    Item pending_[Lookahead + 1]{};
    unsigned historyCount_ = 0, historyNext_ = 0, pendingCount_ = 0;
};
}
