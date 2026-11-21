#pragma once

namespace steam_capture {
// A signed empty catalog has no game tile. Valve's gamepad grid keeps its
// selected All Games tab and its GridWithControls even without any sections.
inline constexpr wchar_t emptyLibrary[] = LR"JS((() => {
    const main = document.querySelector('#popup_target #GamepadUI_Full_Root._3LYP1SIxyoky-yW9Ug_1fq #Main');
    const tab = main?.querySelector('[role=tab][aria-selected=true][aria-controls$="AllGames_Content"]');
    const panel = tab && document.getElementById(tab.getAttribute('aria-controls'));
    if (!panel || !main.contains(panel) || panel.getAttribute('role') !== 'tabpanel' ||
        !panel.querySelector('.rz1mrOnj9WN6UErES7qsq') || panel.querySelector('[role=gridcell] [role=link]')) return false;
    const visible = el => el.checkVisibility({opacityProperty:true, visibilityProperty:true}) &&
        el.getBoundingClientRect().width > 0 && el.getBoundingClientRect().height > 0;
    return visible(tab) && visible(panel);
})())JS";
inline constexpr wchar_t emptyForeground[] = LR"JS((() => {
    if (document.hidden || window.opener?.origin !== 'https://steamloopback.host') return false;
    const blockers = '[role=dialog],[aria-modal=true],dialog[open],._1yLBJnGBard9ajB79vr-6_,._16IUnAiPD_1s1KQ0g8vJpq,iframe[name="MainMenu_uid2"],iframe[name="QuickAccess_uid2"]';
    if ([...document.querySelectorAll(blockers)].some(el => el.checkVisibility({visibilityProperty:true}) &&
        el.getBoundingClientRect().width && el.getBoundingClientRect().height)) return false;
    const tab = document.querySelector('#Main [role=tab][aria-selected=true][aria-controls$="AllGames_Content"]');
    const r = tab.getBoundingClientRect();
    return tab.contains(document.elementFromPoint(r.x + r.width/2, r.y + r.height/2));
})())JS";
// For an operator-requested diagnostic image of the visible Steam shell.
// The separately verified Library image always uses the stricter predicate below.
inline constexpr wchar_t surface[] = LR"JS((() => {
    const target = document.getElementById('popup_target');
    return !document.hidden && window.opener?.origin === 'https://steamloopback.host' &&
        !!target?.firstElementChild && target.getBoundingClientRect().width > 0;
})())JS";
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
    if (Array.from(document.querySelectorAll('[role=dialog],[aria-modal=true],dialog[open],._1yLBJnGBard9ajB79vr-6_,._16IUnAiPD_1s1KQ0g8vJpq,iframe[name="MainMenu_uid2"],iframe[name="QuickAccess_uid2"]')).some(el =>
        el.checkVisibility({visibilityProperty:true}) && el.getBoundingClientRect().width > 0 && el.getBoundingClientRect().height > 0)) return false;
    return Array.from(grid.querySelectorAll('[role=gridcell] [role=link]')).some(el => {
        if (!inView(el)) return false;
        const r = el.getBoundingClientRect();
        const x = (Math.max(0, r.left) + Math.min(innerWidth, r.right)) / 2;
        const y = (Math.max(0, r.top) + Math.min(innerHeight, r.bottom)) / 2;
        return el.contains(document.elementFromPoint(x, y));
    });
})())JS";
}
