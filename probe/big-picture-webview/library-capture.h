#pragma once

namespace steam_capture {
// Read-only checks for the released Steam gamepad All Games page. A modal can
// cover the mounted grid, so verify the foreground tile by browser hit testing.
inline constexpr wchar_t library[] = LR"JS((() => {
    if (document.hidden || !window.opener || window.opener.origin !== 'https://steamloopback.host') return false;
    const grid = document.querySelector('#popup_target #GamepadUI_Full_Root._3LYP1SIxyoky-yW9Ug_1fq #Main [role=tabpanel][id$="AllGames_Content"] [role=grid]');
    if (!grid) return false;
    const inView = el => {
        if (!el.checkVisibility({opacityProperty: true, visibilityProperty: true})) return false;
        const r = el.getBoundingClientRect();
        return r.width > 0 && r.height > 0 && r.right > 0 && r.bottom > 0 && r.left < innerWidth && r.top < innerHeight;
    };
    if (Array.from(document.querySelectorAll('[role=dialog],[aria-modal=true],dialog[open],._1yLBJnGBard9ajB79vr-6_,iframe[name="MainMenu_uid2"],iframe[name="QuickAccess_uid2"]')).some(inView)) return false;
    return Array.from(grid.querySelectorAll('[role=gridcell] [role=link]')).some(el => {
        if (!inView(el)) return false;
        const r = el.getBoundingClientRect();
        const x = (Math.max(0, r.left) + Math.min(innerWidth, r.right)) / 2;
        const y = (Math.max(0, r.top) + Math.min(innerHeight, r.bottom)) / 2;
        return el.contains(document.elementFromPoint(x, y));
    });
})())JS";
}
