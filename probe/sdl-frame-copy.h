#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Xbox frame buffers are little-endian RGBA. Alpha in the game back buffer
// is meaningful to the game, but the fullscreen player presents it opaque.
inline void CopyOpaqueSdlFrame(std::uint8_t *output, const std::uint8_t *pixels, std::size_t pixelCount) {
    for (std::size_t i = 0; i < pixelCount; ++i) {
        std::uint32_t rgba;
        std::memcpy(&rgba, pixels + i * 4, sizeof(rgba));
        rgba |= 0xff000000u;
        std::memcpy(output + i * 4, &rgba, sizeof(rgba));
    }
}

// The Xbox HTML runtime copies a WinRT byte array one element at a time.
// A string crosses that boundary in bulk. Each pair of non-surrogate code
// units carries the 24 RGB bits, including black and colors containing NUL.
inline void EncodeSdlRgb(std::uint16_t *output, const std::uint8_t *pixels, std::size_t pixelCount) {
    for (std::size_t i = 0; i < pixelCount; ++i) {
        std::uint32_t rgba;
        std::memcpy(&rgba, pixels + 4 * i, sizeof(rgba));
        output[2 * i] = 0x1000 | (rgba & 0xfff);
        output[2 * i + 1] = 0x1000 | ((rgba >> 12) & 0xfff);
    }
}

// Predict against the last image acknowledged by the player. Unchanged runs
// use a single code unit, and small per-channel changes use one unit per pixel.
inline std::size_t EncodeSdlRgbDelta(std::uint16_t *out, const std::uint8_t *current,
        const std::uint8_t *previous, std::size_t pixels) {
    std::size_t used = 0, unchanged = 0;
    const auto flush = [&]() {
        if (unchanged) out[used++] = static_cast<std::uint16_t>(0x4000 | (unchanged - 1));
        unchanged = 0;
    };
    for (std::size_t i = 0; i < pixels; ++i, current += 4, previous += 4) {
        int r = current[0] - previous[0], g = current[1] - previous[1], b = current[2] - previous[2];
        if (!(r | g | b)) {
            if (++unchanged == 4096) flush();
        } else {
            flush();
            if (((r + 8) | (g + 8) | (b + 8)) >= 0 && ((r + 8) | (g + 8) | (b + 8)) <= 15) {
                out[used++] = static_cast<std::uint16_t>(0x2000 | ((b + 8) << 8) | ((g + 8) << 4) | (r + 8));
            } else {
                EncodeSdlRgb(out + used, current, 1);
                used += 2;
            }
        }
    }
    flush();
    return used;
}
