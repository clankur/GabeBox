#pragma once
#include <chrono>
#include <cstdint>

// Media Foundation reports a displayed sample without a sample ID. The source
// submits only distinct SDL frames, so count display events independently from
// submissions and expose a per-mode displayed total only for unmixed streams.
struct MediaFrameCount {
    using Clock = std::chrono::steady_clock;
    std::uint64_t gpu = 0, cpu = 0, rendered = 0, sampleFrames = 0;
    std::int64_t sampleUs = 0;
    void submit(bool sharedTexture) {
        if (sharedTexture) ++gpu;
        else ++cpu;
    }
    void display(Clock::time_point now) {
        if (rendered >= gpu + cpu) return;
        ++rendered;
        if (rendered == 1) first = now;
        if (!startCount && now - first >= std::chrono::seconds(5)) {
            startCount = rendered;
            start = now;
        }
        if (startCount && !sampleUs && now - start >= std::chrono::seconds(30)) {
            sampleFrames = rendered - startCount;
            sampleUs = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
        }
    }
    bool singleMode() const { return !cpu || !gpu; }

private:
    std::uint64_t startCount = 0;
    Clock::time_point first{}, start{};
};
