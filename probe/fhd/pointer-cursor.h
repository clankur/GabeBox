#pragma once
#include <array>
#include <cstdint>

namespace FhdPointer {
constexpr int size = 23;
constexpr int center = size / 2;

constexpr int distance(int n) { return n < 0 ? -n : n; }

// A dark keyline surrounds each bright crosshair arm. At a corner, the
// two inward-facing arms remain visible while the hotspot stays on its pixel.
constexpr auto image(bool pressed) {
    std::array<std::uint32_t, size * size> pixels{};
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int dx = distance(x - center), dy = distance(y - center);
            const int near = dx < dy ? dx : dy, far = dx > dy ? dx : dy;
            const bool outline = (near <= 2 && far >= 3) || far <= 2;
            const bool fill = (near <= 1 && far >= 4 && far < center) || far <= 1;
            const std::uint32_t ink = pressed ? 0xff00bfffU : 0xffffffffU;
            pixels[y * size + x] = fill ? ink : outline ? 0xff101010U : 0;
        }
    }
    return pixels;
}

// Use the same integer coordinate conversion as SDL_SendMouseMotion in
// sdl-host-runtime.h. Packed coordinates are measured from the top-left.
constexpr unsigned pixel(unsigned axis, unsigned extent) {
    return extent ? (axis & 4095U) * (extent - 1) / 4095U : 0;
}
}
