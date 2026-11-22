#include "../probe/fhd/media-frame-count.h"
#include "../probe/fhd/media-frame-clock.h"
#include <cassert>

int main() {
    using namespace std::chrono_literals;
    MediaFrameCount frames;
    MediaFrameCount::Clock::time_point begin{};
    frames.display(begin);
    assert(frames.rendered == 0);
    for (int i = 0; i < 10; ++i) frames.submit(true);
    frames.display(begin);
    frames.display(begin + 5s);
    frames.display(begin + 34999ms);
    assert(frames.sampleUs == 0);
    frames.display(begin + 35005ms);
    assert(frames.sampleFrames == 2 && frames.sampleUs == 30005000);
    assert(frames.singleMode() && frames.gpu == 10 && frames.rendered == 4);
    frames.submit(false);
    assert(!frames.singleMode());
    for (int i = 0; i < 20; ++i) frames.display(begin + 36s);
    assert(frames.rendered == 11 && frames.sampleFrames == 2);

    MediaFrameClock clock;
    using Tick = MediaFrameClock::Tick;
    const Tick origin = 731ms;
    assert(clock.position(origin) == 0s);
    assert(clock.wait(origin) == 0s);
    auto initial = clock.advance(origin);
    assert(initial.timestamp == 0s && initial.duration == Tick{166667});
    // A fast producer cannot fit two samples in one display interval.
    assert(clock.wait(origin + 10ms) == initial.duration - 10ms);
    assert(clock.position(origin + 10ms) == initial.duration);
    auto end = initial.duration;
    for (int i = 1; i < 60 * 3600; ++i) {
        assert(clock.wait(origin + end - Tick{1}) == Tick{1});
        assert(clock.wait(origin + end) == 0s);
        const auto sample = clock.advance(origin + end);
        assert(sample.timestamp == end);
        assert(sample.duration == Tick{166666} || sample.duration == Tick{166667});
        end += sample.duration;
    }
    assert(end == 1h);
    const auto late = origin + 1h + 10s + 2ms;
    assert(clock.wait(late) == 0s);
    assert(clock.position(late) == 1h + 10s);
    const auto resumed = clock.advance(late);
    assert(resumed.timestamp == 1h + 10s);
    assert(clock.wait(late) == resumed.duration - 2ms);
}
