const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const source = fs.readFileSync(require.resolve('../../ui/big-picture-bridge/xbox-window.js'), 'utf8');

function fixture(lookupPopup) {
    const messages = [], posted = [], timers = [];
    const origin = 'https://steamloopback.host';
    let receive, portal = false;
    const seed = {viewId: 41, pid: 3804, ownerId: 1, popupId: 2, visible: true,
        focused: true, popupVisible: false, popupOpen: false, x: 0, y: 0, width: 1280, height: 720,
        usableX: 32, usableY: 18, usableWidth: 1216, usableHeight: 684, scale: 1.5,
        screenWidth: 1920, screenHeight: 1080};
    const window = {open: () => popup, postMessage() {}, chrome: {webview: {
        addEventListener(type, callback) { receive = callback; }, postMessage: name => messages.push(name)
    }}};
    const popup = {opener: window, location: {href: 'about:blank'}, closed: false, document: {
        getElementById: id => portal && id === 'popup_target', hidden: false
    }, postMessage: (...args) => posted.push(args)};
    const client = {};
    const context = {window, client, lookupPopup, location: {origin}, queueMicrotask, setTimeout: f => timers.push(f), xboxNativeWindow: seed};
    vm.runInNewContext(source + '\ninstallXboxWindow(client, undefined, lookupPopup); this.active = getXboxGamepadWindow;', context);
    return {window, popup, client, seed, messages, posted, active: context.active,
        portal() { portal = true; timers.splice(0).forEach(f => f()); },
        snapshot(extra) { receive({data: {event: 'xbox-window', state: {...seed, ...extra}}}); }};
}

test('delivers native creation after Valve registers a previously empty popup', () => {
    let registered;
    const f = fixture(() => registered);
    f.window.open('about:blank?createflags=2', 'SP BPM_uid3804');
    f.snapshot({popupOpen: true, popupVisible: true});
    f.portal();
    assert.equal(f.posted.length, 0);
    registered = {m_bCreated: false};
    f.portal();
    assert.equal(f.posted.length, 1);
    registered.m_bCreated = true;
    f.portal();
    assert.equal(f.posted.length, 1);
});

test('the Valve window manager requests the gamepad UI for the owning CoreWindow and browser', async () => {
    const f = fixture();
    let calls = 0;
    const reg = f.client.UI.RegisterDesiredSteamUIWindowsChanged(() => { calls++; });
    await Promise.resolve();
    assert.equal(calls, 1);
    assert.deepEqual(JSON.parse(JSON.stringify(await f.client.UI.GetDesiredSteamUIWindows())),
        [{unID: 41, unPID: 3804, nBrowserID: 1, appid: 0, windowType: 0}]);
    f.client.UI.EnsureMainWindowCreated();
    assert.equal(calls, 2);
    reg.unregister();
    f.client.UI.EnsureMainWindowCreated();
    assert.equal(calls, 2);
    assert.equal(f.client.Browser.GetBrowserID(), 1);
    const monitor = await f.client.Window.GetDefaultMonitorDimensions();
    assert.equal(monitor.nFullWidth, 1920);
    assert.equal(monitor.nUsableHeight, 1026);
});

test('only the real matching popup receives its window bindings and one creation event after both milestones', () => {
    const f = fixture();
    const url = 'about:blank?createflags=2&pid=3804&browser=1';
    f.window.open(url, 'SP BPM_uid3805');
    f.window.open('https://example.com/', 'SP BPM_uid3804');
    assert.equal(f.popup.SteamClient, undefined);
    f.window.open(url, 'SP BPM_uid3804');
    assert.equal(f.popup.SteamClient.Browser.GetBrowserID(), 2);
    assert.equal(f.popup.SteamClient.Apps, undefined);
    f.portal();
    assert.deepEqual(f.posted, []);
    f.snapshot({pid: 42, popupOpen: true});
    assert.deepEqual(f.posted, []);
    f.snapshot({popupOpen: true, popupVisible: true});
    f.snapshot({popupOpen: true, popupVisible: true});
    assert.deepEqual(f.posted, [['popup-created', 'https://steamloopback.host']]);
    f.popup.SteamClient.Window.HideWindow();
    f.popup.SteamClient.Window.BringToFront();
    f.popup.SteamClient.Browser.NotifyUserActivation();
    assert.deepEqual(f.messages, ['xbox-window-hide', 'xbox-window-show', 'xbox-window-focus']);
    assert.equal(f.active(), f.popup);
    f.popup.closed = true;
    assert.equal(f.active(), f.window);
});

test('real monitor snapshots drive geometry and display change subscription', async () => {
    const f = fixture();
    let calls = 0;
    const sub = f.client.UI.RegisterMoveGamepadUIMainWindowToPrimaryDisplay(() => { calls++; });
    f.snapshot({width: 1000, height: 600, visible: false, screenWidth: 3840});
    assert.equal(calls, 1);
    assert.equal(await f.client.Window.IsWindowMinimized(), true);
    assert.equal((await f.client.Window.GetWindowDimensions()).width, 1000);
    sub.unregister();
    f.snapshot({});
    assert.equal(calls, 1);
});

test('focus observers report the foreground application view and clear it when the game takes over', async () => {
    const f = fixture();
    const focused = [], modes = [], overlays = [];
    const sys = f.client.System.UI;
    const reg = sys.RegisterForFocusChangeEvents(e => focused.push(e.focusedApp.windowid));
    f.client.UI.RegisterForUIModeChanged(mode => modes.push(mode));
    sys.RegisterForOverlayGameWindowFocusChanged((app, pid) => overlays.push([app,pid]));
    await Promise.resolve();
    f.snapshot({focused: false});
    f.snapshot({focused: true});
    reg.unregister();
    f.snapshot({focused: false});
    assert.deepEqual(focused, [41, 0, 41]);
    assert.deepEqual(modes, [4]);
    assert.deepEqual(overlays, [[0,0],[0,0]]);
});

test('opaque and hidden composition affect only the actual gamepad controller', () => {
    const f = fixture();
    f.window.open('about:blank?createflags=2', 'SP BPM_uid3804');
    const child = f.popup.SteamClient.Window;
    child.SetComposition(3, [769], 0);
    child.SetComposition(0, [769], 0);
    assert.throws(() => child.SetComposition(2, [769, 646570], 0), /cannot composite/);
    assert.throws(() => child.SetComposition(3, [769], 9999), /cannot composite/);
    assert.deepEqual(f.messages, ['xbox-window-show', 'xbox-window-hide']);
});

test('a running game gets one stable, empty window list that is dropped when it stops', () => {
    // Valve's WindowStore returns a new empty array for an app without an entry, which kept the
    // running-game controls re-rendering (86,089 window-title requests in 3 s offline).
    const context = {};
    vm.runInNewContext(source + '\nthis.track = trackXboxGameWindows;', context);
    const entries = new Map(), calls = [];
    const store = {
        GetAppWindowIDs: appId => entries.get(appId)?.windowids ?? [],
        SetRunningAppWindowIDs(appId, ids) {
            calls.push(['set', appId, ids.length]);
            if (!entries.has(appId)) entries.set(appId, {appid: appId, focusedWindowID: 0, windowids: ids});
        },
        RemoveRunningAppWindowIDs(appId) { calls.push(['remove', appId]); entries.delete(appId); }
    };
    assert.notEqual(store.GetAppWindowIDs(2868840), store.GetAppWindowIDs(2868840));
    context.track(store, 2868840, 0);
    assert.equal(store.GetAppWindowIDs(2868840), store.GetAppWindowIDs(2868840));
    assert.equal(store.GetAppWindowIDs(2868840).length, 0);
    context.track(store, 0, 2868840);
    assert.equal(entries.size, 0);
    assert.deepEqual(calls, [['set', 2868840, 0], ['remove', 2868840]]);
    // A release without the store, or with a different one, is left alone.
    context.track(undefined, 42, 0);
    context.track({}, 42, 0);
});

test('the controller page loading a layout to edit receives a rejection it handles', async () => {
    const input = fs.readFileSync(require.resolve('../../ui/big-picture-bridge/xbox-input.js'), 'utf8');
    const client = {System: {}};
    const install = vm.runInNewContext(input + '\ninstallXboxInput', {window: {addEventListener() {}},
        getXboxGamepadWindow: () => undefined});
    install(client, {read: () => []});
    // Valve calls .then().catch() on the result inside a render. A missing method threw there
    // (minified React error #520 on Slay the Spire 2).
    const pending = client.Input.StartEditingControllerConfigurationForAppIDAndControllerIndex(2868840, 0);
    assert.equal(typeof pending.then, 'function');
    await assert.rejects(pending, /Steam Input configuration is unavailable/);
});
