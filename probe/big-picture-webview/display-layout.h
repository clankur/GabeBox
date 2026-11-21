#pragma once
#include <algorithm>

namespace steam_display {
struct Bounds { float x, y, width, height; };
inline Bounds viewport(Bounds window, Bounds visible, float scale, bool safe, bool game) {
    if (game) return {0, 0, 1, 1};
    if (safe && visible.width > 0 && visible.height > 0) {
        const float left = std::max(window.x, visible.x), top = std::max(window.y, visible.y);
        const float right = std::min(window.x + window.width, visible.x + visible.width);
        const float bottom = std::min(window.y + window.height, visible.y + visible.height);
        if (right > left && bottom > top)
            return {(left - window.x)*scale, (top - window.y)*scale, (right-left)*scale, (bottom-top)*scale};
    }
    return {0, 0, window.width*scale, window.height*scale};
}
}
