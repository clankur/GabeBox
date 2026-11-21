#pragma once
#include <algorithm>

namespace steam_display {
struct Bounds { float x, y, width, height; };
inline Bounds viewport(Bounds window, Bounds visible, bool safe, bool game) {
    if (game) return {0, 0, 1, 1};
    if (safe && visible.width > 0 && visible.height > 0) {
        const float left = std::max(window.x, visible.x), top = std::max(window.y, visible.y);
        const float right = std::min(window.x + window.width, visible.x + visible.width);
        const float bottom = std::min(window.y + window.height, visible.y + visible.height);
        if (right > left && bottom > top)
            return {left - window.x, top - window.y, right - left, bottom - top};
    }
    return {0, 0, window.width, window.height};
}

// Composition places the surface in logical view units. The browser draws
// into a raw-pixel surface, whose local transform cancels the view DPI.
struct Placement {
    Bounds view;
    float pixelsPerDip;
    float pixelWidth() const { return view.width * pixelsPerDip; }
    float pixelHeight() const { return view.height * pixelsPerDip; }
    float visualScale() const { return 1.f / pixelsPerDip; }
    float localPixelX(float x) const { return (x - view.x) * pixelsPerDip; }
    float localPixelY(float y) const { return (y - view.y) * pixelsPerDip; }
};
}
