#pragma once
#include <algorithm>
#include <cmath>

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

inline float fhdScale(Bounds window, float nativeScale, float screenWidth, float screenHeight) {
    if (window.width <= 0 || window.height <= 0 || nativeScale <= 0) return 1.f;
    const float width = screenWidth > 0 ? std::min(1920.f, screenWidth) : 1920.f;
    const float height = screenHeight > 0 ? std::min(1080.f, screenHeight) : 1080.f;
    return std::min({nativeScale, width / window.width, height / window.height});
}

// A full-viewport frame's CSS extent times devicePixelRatio describes its
// raster extent. Only accept a uniform, corroborated overrun on both axes.
inline float overrunCorrection(float requestedWidth, float requestedHeight, float observedWidth, float observedHeight) {
    if (observedWidth <= 0 || observedHeight <= 0) return 1.f;
    const float x = requestedWidth / observedWidth, y = requestedHeight / observedHeight;
    if (x < .49f || y < .49f || x >= .98f || y >= .98f || std::abs(x - y) > .015f) return 1.f;
    return std::min(x, y);
}

// Composition places the surface in logical view units. The browser draws
// into a raw-pixel surface, whose local transform cancels the view DPI.
struct Placement {
    Bounds view;
    float pixelsPerDip;
    float pixelWidth() const { return std::max(1.f, std::round(view.width * pixelsPerDip)); }
    float pixelHeight() const { return std::max(1.f, std::round(view.height * pixelsPerDip)); }
    float visualScaleX() const { return view.width > 0 ? view.width / pixelWidth() : 1.f; }
    float visualScaleY() const { return view.height > 0 ? view.height / pixelHeight() : 1.f; }
    float localPixelX(float x) const { return (x - view.x) / visualScaleX(); }
    float localPixelY(float y) const { return (y - view.y) / visualScaleY(); }
};
}
