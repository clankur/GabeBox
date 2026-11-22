#pragma once

namespace steam_input {
enum class PointerRoute { otherView, controllerOnly, webMouse };

constexpr PointerRoute pointerRoute(bool shellVisible, bool xbox, bool signedAccount) {
    if (!shellVisible) return PointerRoute::otherView;
    return xbox && signedAccount ? PointerRoute::controllerOnly : PointerRoute::webMouse;
}

// CoreWindow has one cursor for the entire view, including a running game.
template <typename Cursor>
class ShellCursor {
    Cursor previous{nullptr};
    bool owned = false;
public:
    template <typename Window>
    void update(Window const& window, bool shell) {
        if (shell) {
            auto current = window.PointerCursor();
            if (!owned) {
                previous = current;
                owned = true;
            }
            if (current) window.PointerCursor(nullptr);
        } else if (owned) {
            if (!window.PointerCursor()) window.PointerCursor(previous);
            previous = nullptr;
            owned = false;
        }
    }
};
}
