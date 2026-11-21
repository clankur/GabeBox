#include "../probe/big-picture-webview/document-address.h"
#include "../probe/big-picture-webview/display-layout.h"
#include "../probe/big-picture-webview/process-failure.h"
#include <cassert>

int main() {
    using namespace steam_document;
    assert(steam_webview::failureReason(5) == L"out-of-memory");
    assert(steam_webview::failureReason(-1) == L"unknown");
    assert(steam_webview::failureReason(100) == L"unknown");
    assert(steam_webview::processDescription(L"Audio Service") == L"audio");
    assert(steam_webview::processDescription(L"private account or path") == L"other");
    assert(steam_webview::processDescription(L"") == L"none");
    // 1280x720 logical Xbox view at 150% fills 1920x1080. Input and
    // visual placement agree at the centre, including safe-area insets.
    const steam_display::Bounds screen{10, 5, 1280, 720}, safe{50, 25, 1200, 680};
    for (float scale : {1.f, 1.5f, 2.f}) {
        const steam_display::Placement full{steam_display::viewport(screen, safe, false, false), scale};
        assert(full.view.x == 0 && full.view.y == 0);
        assert(full.pixelWidth() * full.visualScaleX() == 1280);
        assert(full.pixelHeight() * full.visualScaleY() == 720);
        assert(full.localPixelX(640) == full.pixelWidth() / 2);
        assert(full.localPixelY(360) == full.pixelHeight() / 2);
        const steam_display::Placement inset{steam_display::viewport(screen, safe, true, false), scale};
        assert(inset.view.x == 40 && inset.view.y == 20);
        assert(inset.view.x + inset.pixelWidth() * inset.visualScaleX() == 1240);
        assert(inset.view.y + inset.pixelHeight() * inset.visualScaleY() == 700);
        assert(inset.localPixelX(40) == 0 && inset.localPixelY(20) == 0);
        assert(inset.localPixelX(640) == 600 * scale && inset.localPixelY(360) == 340 * scale);
    }
    const steam_display::Bounds fhd{0, 0, 1920, 1080};
    assert(steam_display::fhdScale(fhd, 1.5f, 3840, 2160) == 1.f);
    assert(steam_display::fhdScale(screen, 1.5f, 1920, 1080) == 1.5f);
    assert(steam_display::fhdScale(screen, 2.f, 1280, 720) == 1.f);
    const steam_display::Bounds wide{0, 0, 2560, 1080};
    assert(steam_display::fhdScale(wide, 2.f, 3840, 2160) == .75f);
    assert(steam_display::overrunCorrection(1920, 1080, 2880, 1620) == 2.f / 3.f);
    assert(steam_display::overrunCorrection(1920, 1080, 1921, 1080) == 1.f);
    assert(steam_display::overrunCorrection(1920, 1080, 2880, 1080) == 1.f);
    const steam_display::Placement fractional{{4.5f, 8.5f, 1201.5f, 679.5f}, 1.3f};
    assert(std::abs(fractional.localPixelX(1206) - fractional.pixelWidth()) < .01f);
    assert(std::abs(fractional.localPixelY(688) - fractional.pixelHeight()) < .01f);
    const auto hidden = steam_display::viewport(screen, safe, true, true);
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
