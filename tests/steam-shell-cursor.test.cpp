#include "../probe/big-picture-webview/shell-cursor.h"
#include <cassert>

struct Window {
    mutable const char* cursor;
    const char* PointerCursor() const { return cursor; }
    void PointerCursor(const char* value) const { cursor = value; }
};

int main() {
    using namespace steam_input;
    static_assert(pointerRoute(true, true, true) == PointerRoute::controllerOnly);
    static_assert(pointerRoute(true, true, false) == PointerRoute::webMouse);
    static_assert(pointerRoute(true, false, true) == PointerRoute::webMouse);
    static_assert(pointerRoute(false, true, true) == PointerRoute::otherView);
    const char arrow[] = "initial", replacement[] = "XAML rollover", title[] = "game";
    Window window{arrow};
    ShellCursor<const char*> cursor;
    cursor.update(window, true);
    assert(!window.cursor);
    window.cursor = replacement;
    cursor.update(window, true);
    assert(!window.cursor);
    cursor.update(window, false);
    assert(window.cursor == arrow);
    cursor.update(window, true);
    window.cursor = title;
    cursor.update(window, false);
    assert(window.cursor == title);
}
