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
