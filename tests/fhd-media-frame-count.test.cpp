#include "../probe/fhd/media-frame-count.h"
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
}
