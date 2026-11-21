#include "../probe/big-picture-webview/document-address.h"
#include "../probe/big-picture-webview/display-layout.h"
#include <cassert>

int main() {
    using namespace steam_document;
    const steam_display::Bounds screen{10, 5, 1920, 1080}, safe{40, 15, 1800, 1020};
    const auto inset = steam_display::viewport(screen, safe, 1.5f, true, false);
    assert(inset.x == 45 && inset.y == 15 && inset.width == 2700 && inset.height == 1530);
    const auto full = steam_display::viewport(screen, safe, 1.5f, false, false);
    assert(full.x == 0 && full.y == 0 && full.width == 2880 && full.height == 1620);
    const auto hidden = steam_display::viewport(screen, safe, 1.5f, true, true);
    assert(hidden.width == 1 && hidden.height == 1);
    constexpr auto route = L"https://steamloopback.host/routes/oobe/1";
    Owner doc;
    assert(!doc.start(route, 1));
    assert(!doc.start(signedEntry, 2));
    assert(doc.start(entry, 3));
    doc.content(3, false);
    assert(doc.accepts(entry) && !doc.accepts(route));

    doc = {};
    doc.signedMode = true;
    assert(doc.start(signedEntry, 7));
    assert(!doc.accepts(route));
    doc.content(6, false);
    assert(!doc.accepts(signedEntry));
    doc.content(7, false);
    assert(doc.accepts(route) && doc.accepts(signedEntry));
    assert(!doc.start(route, 8) && doc.accepts(route));
    assert(!doc.accepts(L"https://steamloopback.host/routes/../index.html"));
    assert(!doc.accepts(L"https://steamloopback.host/routes/%2F"));
    assert(!doc.accepts(L"https://other.host/routes/oobe/1"));
    assert(!doc.accepts(entry));
    assert(doc.start(signedEntry, 9));
    doc.content(9, true);
    assert(!doc.accepts(route));
}
