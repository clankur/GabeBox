// The flash probe's row statistics and white-flash rule (runtime/presenter/flash-detector.h).
// tests/test_flash_probe.py checks tools/flash_probe.py against the same streams.
#include "../runtime/presenter/flash-detector.h"
#include <cassert>
#include <cstring>
#include <vector>

namespace {
struct Item { SdlGpuProbeStats stats{}; uint16_t flags = 0; int index = 0; };

SdlGpuProbeStats frame(unsigned luma, unsigned white, unsigned low, unsigned high) {
    SdlGpuProbeStats s{};
    s.luma = static_cast<uint16_t>(luma * 256); s.white = static_cast<uint16_t>(white); s.cells = 512;
    s.min = static_cast<uint8_t>(low); s.max = static_cast<uint8_t>(high);
    return s;
}

std::vector<uint16_t> classifyAll(const std::vector<SdlGpuProbeStats> &frames) {
    FhdFlash::Detector<Item> detector;
    std::vector<uint16_t> flags(frames.size(), 0xffff);
    Item out;
    for (size_t i = 0; i < frames.size(); ++i) {
        Item item; item.stats = frames[i]; item.index = static_cast<int>(i);
        if (detector.push(item, out)) flags[out.index] = out.flags;
    }
    while (detector.drain(out)) flags[out.index] = out.flags;
    return flags;
}
}

int main() {
    assert(SdlGpuProbeRowY(0, 1080) == 33 && SdlGpuProbeRowY(15, 1080) == 1046);
    assert(SdlGpuProbeLuma(255, 255, 255) == 255 && SdlGpuProbeLuma(0, 0, 0) == 0);

    // A uniformly white frame: every cell is white and the mean is full scale.
    const unsigned width = 64;
    std::vector<uint8_t> rows(SDL_GPU_PROBE_ROWS * width * 4, 255);
    SdlGpuProbeStats white{};
    SdlGpuProbeMeasure(rows.data(), width * 4, width, 0, &white);
    assert(white.luma == 255 * 256 && white.white == 512 && white.cells == 512 && white.min == 255 && white.max == 255);
    assert(FhdFlash::classify(nullptr, 0, white, nullptr, 0) == FhdFlash::UniformWhite);

    // Channel order: pure blue in RGBA reads as red in BGRA.
    for (size_t i = 0; i < rows.size(); i += 4) { rows[i] = 0; rows[i + 1] = 0; rows[i + 2] = 255; rows[i + 3] = 255; }
    SdlGpuProbeStats rgba{}, bgra{};
    SdlGpuProbeMeasure(rows.data(), width * 4, width, 0, &rgba);
    SdlGpuProbeMeasure(rows.data(), width * 4, width, 1, &bgra);
    assert(rgba.luma == 19 * 256 && bgra.luma == 54 * 256 && rgba.white == 0 && rgba.hash == bgra.hash);

    // The pitch skips padding, and one changed sampled byte changes the hash.
    const size_t pitch = width * 4 + 256;
    std::vector<uint8_t> padded(SDL_GPU_PROBE_ROWS * pitch, 7);
    for (unsigned r = 0; r < SDL_GPU_PROBE_ROWS; ++r) std::memcpy(&padded[r * pitch], &rows[r * width * 4], width * 4);
    SdlGpuProbeStats same{};
    SdlGpuProbeMeasure(padded.data(), pitch, width, 0, &same);
    assert(same.hash == rgba.hash && same.luma == rgba.luma);
    padded[5 * pitch + 9] ^= 1;
    SdlGpuProbeMeasure(padded.data(), pitch, width, 0, &same);
    assert(same.hash != rgba.hash);

    // Narrow frames keep only nonempty cells.
    SdlGpuProbeStats narrow{};
    SdlGpuProbeMeasure(rows.data(), 16, 4, 0, &narrow);
    assert(narrow.cells == 4 * SDL_GPU_PROBE_ROWS);

    const auto dark = frame(40, 0, 30, 50), bright = frame(250, 500, 200, 255);
    // A one-frame flash between steady frames.
    std::vector<SdlGpuProbeStats> flash(10, dark);
    flash.push_back(bright);
    flash.insert(flash.end(), 4, dark);
    auto flags = classifyAll(flash);
    for (size_t i = 0; i < flags.size(); ++i) assert(flags[i] == (i == 10 ? FhdFlash::Flash : 0));

    // A two-frame flash flags both frames.
    std::vector<SdlGpuProbeStats> pair(10, dark);
    pair.insert(pair.end(), 2, bright);
    pair.insert(pair.end(), 4, dark);
    flags = classifyAll(pair);
    assert(flags[10] == FhdFlash::Flash && flags[11] == FhdFlash::Flash && flags[12] == 0 && flags[9] == 0);

    // A lasting change to a bright scene is not a flash.
    std::vector<SdlGpuProbeStats> scene(10, dark);
    scene.insert(scene.end(), 10, bright);
    for (auto f : classifyAll(scene)) assert(f == 0);

    // White blocks over a steady frame: 12 more white cells, then gone.
    std::vector<SdlGpuProbeStats> blocks(10, frame(90, 2, 20, 240));
    blocks.push_back(frame(95, 14, 20, 255));
    blocks.insert(blocks.end(), 3, frame(90, 2, 20, 240));
    flags = classifyAll(blocks);
    assert(flags[10] == FhdFlash::Flash && flags[9] == 0 && flags[11] == 0);

    // Too little history: no flash verdict, but a uniform white frame is still flagged.
    std::vector<SdlGpuProbeStats> early{dark, dark, frame(255, 512, 255, 255), dark, dark, dark};
    flags = classifyAll(early);
    assert(flags[2] == FhdFlash::UniformWhite);
    return 0;
}
