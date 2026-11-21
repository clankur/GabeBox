#pragma once
#include <chrono>
#include <cstdint>
#include <optional>

// Negotiate and timestamp the same 60 Hz stream. Dropped slots advance the
// timeline without submitting a burst of old game frames after a stall.
class MediaFrameClock {
public:
    using Tick = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;
    static constexpr int rate = 60;
    struct Sample { Tick timestamp, duration; };

    Tick wait(Tick now) const {
        if (!origin) return Tick::zero();
        const auto remaining = *origin + std::chrono::ceil<Tick>(next) - now;
        return remaining > Tick::zero() ? remaining : Tick::zero();
    }

    Sample advance(Tick now) {
        if (!origin) origin = now;
        const auto slot = std::chrono::duration_cast<Slot>(now - *origin);
        const auto timestamp = std::chrono::ceil<Tick>(slot);
        next = slot + Slot{1};
        return {timestamp, std::chrono::ceil<Tick>(next) - timestamp};
    }

private:
    using Slot = std::chrono::duration<std::int64_t, std::ratio<1, rate>>;
    std::optional<Tick> origin;
    Slot next{};
};
