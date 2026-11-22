const assert = require("node:assert/strict");
const fs = require("node:fs");
const test = require("node:test");
const vm = require("node:vm");

const source = fs.readFileSync(require.resolve("../probe/signin/steam-sdl-native-view.js"), "utf8");

test("the XAML game panel counts visible swaps and serializes replacement across a pending close", async () => {
    const calls = [], samples = [], first = [];
    let now = 0, swaps = 0, tick, visible = true, finishClose, keyboardSerial = 0, menus = 0;
    const drain = () => new Promise(resolve => setImmediate(resolve));
    const host = {
        async openPanel(path, pad) { calls.push(["open", path, pad]); visible = true; },
        async setPanelVisible(value) { visible = value; calls.push(["visible", value]); },
        closePanel() { calls.push(["close"]); return new Promise(resolve => { finishClose = resolve; }); },
        panelStatus() { return "width=1920\r\nheight=1080\r\nprimary_panel=1\r\nvisible=" +
            Number(visible) + "\r\npresented=" + swaps + "\r\npanel_keyboard=1\r\nmenu_serial=" + keyboardSerial + "\r\n"; },
        openMedia() { assert.fail("The XAML panel should own playback"); }
    };
    const events = {running: () => true, foreground: () => true, fallback: () => assert.fail("Panel failed"),
        menu: id => { assert.equal(id, 1); menus++; },
        presented: stats => first.push(stats), measured: (frames, us) => samples.push([frames, us])};
    const window = {Fhd: {Native: {RenderHost: host}}, addEventListener() {}, removeEventListener() {},
        steamBigPictureHost: {switchView: async (to, from) => { assert.equal(to, from); }}};
    const document = {hidden: false, documentElement: {getAttribute: () => "true"}, addEventListener() {}, removeEventListener() {}};
    const Windows = {UI: {ViewManagement: {ApplicationView: {getForCurrentView: () => ({id: 1})}}},
        Storage: {ApplicationData: {current: {localFolder: {path: "local"}}}}};
    vm.runInNewContext(source, {window, document, Windows, performance: {now: () => now},
        setInterval: callback => { tick = callback; return 1; }, clearInterval() {}});
    const open = id => window.steamSdlNativeView.open("steam_sdl_mailbox=sdl-host-" + id + "-2.bin\n", events, false, {hidden:false});
    const game = open(1);
    await drain();
    tick(); assert.equal(first.length, 0);
    swaps = 1; tick(); assert.match(first[0], /primary_panel=1/);
    assert.equal(game.ownsMenuKey(), true);
    keyboardSerial++; tick(); tick(); assert.equal(menus, 1);
    now += 10000; swaps += 600; tick();
    game.cover(); await drain();
    keyboardSerial++; tick(); assert.equal(menus, 1);
    now += 50000; swaps += 1200; tick();
    assert.deepEqual(samples, []);
    await game.resume(1); tick();
    assert.equal(menus, 1);
    now += 30000; swaps += 1780; tick();
    assert.deepEqual(samples, [[1780, 30000000]]);
    const stopped = game.close(); assert.equal(game.close(), stopped);
    const next = open(3);
    await drain();
    assert.deepEqual(calls, [["open", "local\\sdl-host-1-2.bin", false], ["visible", false], ["visible", true], ["close"]]);
    finishClose(); await stopped; await drain();
    assert.deepEqual(calls.at(-1), ["open", "local\\sdl-host-3-2.bin", false]);
    assert.equal(await game.resume(1), false);
    const closed = next.close(); await drain(); finishClose(); await closed;
});

async function checkPrimaryMedia(sameWindow) {
    let tick, now = 0, total = 0, running = true, finished = 0, firstFrames = 0;
    const measurements = [], switches = [], released = [], captures = [], listeners = new Map();
    let captureRequest = true, drawn = null, playCalls = 0, focused = true;
    const video = {
        paused:false, readyState:2, videoWidth:1920, videoHeight:1080, currentTime:1.5,
        async play() { playCalls++; this.paused = false; }, pause() { this.paused = true; }, removeAttribute() {}, setAttribute() {},
        addEventListener() {}, removeEventListener() {}, getBoundingClientRect: () => ({width:1920})
    };
    const screen = {hidden:false, contains:node => node === video, classList:{add() {}, remove() {}},
        appendChild(node) { node.parentNode = screen; }, removeChild(node) { node.parentNode = null; }};
    const canvas = {getContext:() => ({drawImage:node => { drawn = node; }}),
        msToBlob:() => ({size:4, type:"image/png"})};
    const document = {documentElement:{getAttribute: name => name === "data-steam-primary-xaml" && sameWindow ? "true" : null},
        hidden:false, elementFromPoint:() => video, createElement:name => name === "canvas" ? canvas : video,
        addEventListener: (event, callback) => listeners.set(event, callback),
        removeEventListener: event => listeners.delete(event)};
    const host = {
        openMedia(path, nativePad) { assert.equal(path, "C:\\LocalState\\sdl-host-1-2.bin"); assert.equal(nativePad, false); return {}; },
        closeMedia() { finished++; },
        mediaStatus() { return "width=1920\r\nheight=1080\r\npresented=" + total + "\r\nsubmitted=9999\r\n"; }
    };
    const window = {Fhd:{Native:{RenderHost:host}},
        addEventListener:document.addEventListener, removeEventListener:document.removeEventListener,
        steamBigPictureHost:{switchView:async (to, from) => { switches.push([to,from]); }}};
    const localFolder = {path:"C:\\LocalState",
        async tryGetItemAsync(name) {
            assert.equal(name, "big-picture-capture-game.request");
            return captureRequest && {async deleteAsync() { captureRequest = false; }};
        },
        async createFileAsync() { return {async renameAsync(name) { assert.equal(name,"big-picture-game-video.png"); }}; }};
    const Windows = {UI:{ViewManagement:{ApplicationView:{getForCurrentView:() => ({id:3})}}},
        Security:{Cryptography:{CryptographicBuffer:{createFromByteArray:array => array}}},
        Storage:{ApplicationData:{current:{localFolder}}, CreationCollisionOption:{generateUniqueName:1},
            NameCollisionOption:{replaceExisting:1}, FileIO:{async writeBufferAsync(_, data) { assert.equal(data.length,4); }}}};
    class FileReader {
        readAsArrayBuffer() { this.result = new Uint8Array([137,80,78,71]).buffer; this.onload(); }
    }
    vm.runInNewContext(source, {window,document,Windows,FileReader,Date:{now: () => now},performance:{now: () => now},
        setInterval:callback => { tick = callback; return 1; }, clearInterval() {},
        URL:{createObjectURL:() => "blob:media", revokeObjectURL:url => released.push(url)}});
    const control = window.steamSdlNativeView.open("steam_sdl_mailbox=sdl-host-1-2.bin\r\n", {
        running:() => running, foreground:() => focused, presented:() => firstFrames++, mediaProgress() {}, mediaCapture:stats => captures.push(stats),
        measured:(n,us,stats) => {
            measurements.push([n,us]);
            assert.match(stats,new RegExp("sample_window=" + measurements.length + "\\r"));
        }, exited() {}, fallback:() => assert.fail("media failed")
    }, false, screen);
    assert(control.primary);
    tick();
    assert.equal(firstFrames, 0);
    total = 1; tick();
    assert.equal(firstFrames, 1);
    now = 20000; total = 1201; tick();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(drawn,video);
    assert.match(captures[0], /capture_saved=1[\s\S]*video_ms=1500/);
    control.cover();
    assert.equal(video.paused,!sameWindow);
    captureRequest = true;
    now = 80000; total = 5000; tick();
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(measurements, []);
    assert(captureRequest);
    assert.equal(captures.length, 1);
    await control.resume(11);
    assert.equal(video.paused,false);
    assert.equal(playCalls, sameWindow ? 1 : 2);
    assert.deepEqual(switches, [[3,11]]);
    tick();
    now += 29000; total += 1700; tick();
    // The system can lose and regain focus, or hide and show the document,
    // between two 100 ms media polls. Those are separate sample windows.
    focused = false; listeners.get("blur")();
    now += 5000; total += 300;
    focused = true; listeners.get("focus")(); tick();
    now += 29000; total += 1700; tick();
    document.hidden = true; listeners.get("visibilitychange")();
    now += 5000; total += 300;
    document.hidden = false; listeners.get("visibilitychange")(); tick();
    assert.deepEqual(measurements, []);
    now += 30000; total += 1798; tick();
    assert.deepEqual(measurements, [[1798,30000000]]);
    now += 30000; tick();
    assert.deepEqual(measurements, [[1798,30000000],[0,30000000]]);
    running = false; tick(); control.close();
    assert.equal(finished, 1); assert.deepEqual(released, ["blob:media"]);
    assert.equal(listeners.size, 0);
}

test("WWA pauses the video for another view and counts only foreground frames", () => checkPrimaryMedia(false));
test("XAML keeps the video running under Valve and counts only foreground frames", () => checkPrimaryMedia(true));

function fixture(accept = true, gate = null, withSteam = false) {
    const calls = [];
    let tick, stats = "presented=0\n";
    const host = { open: path => calls.push(path), start: () => calls.push("start"),
        setNativeGamepad: enabled => calls.push(enabled), close: () => calls.push("close"),
        viewId: () => -31, status: () => stats };
    const window = { Fhd: { Native: { RenderHost: host } } };
    if (withSteam) window.steamBigPictureHost = {
        samplePresenter: () => {},
        switchView: async () => { calls.push("steam-switch"); }
    };
    const Windows = {
        UI: { ViewManagement: { ApplicationView: { getForCurrentView: () => ({ id: -73 }) }, ApplicationViewSwitcher: {
            tryShowAsStandaloneAsync: async () => { if (withSteam) calls.push("standalone"); return true; },
            switchAsync: (id, from) => {
                if (id === -73) { assert.equal(from, -31); calls.push("library"); return Promise.resolve(); }
                assert.equal(from, -73); calls.push(id);
                return gate || (accept ? Promise.resolve() : Promise.reject(Error()));
            }
        } } },
        Storage: { ApplicationData: { current: { localFolder: { path: "C:\\LocalState" } } } }
    };
    vm.runInNewContext(source, { window, Windows, Date, setInterval(f) { tick = f; return 1; }, clearInterval() {} });
    function open(value, nativePad) {
        return window.steamSdlNativeView.open(value, { running: () => true, fallback: () => calls.push("fallback"),
            presented: () => calls.push("presented"), menu: () => calls.push("menu"),
            measured: (n, us) => calls.push([n, us]) }, nativePad);
    }
    return { open, calls, poll(value) { if (value) stats = value; tick(); } };
}

test("a fresh game launched from Steam gets a standalone Xbox view before the switch", async () => {
    const run = fixture(true, null, true);
    const control = run.open("steam_sdl_mailbox=sdl-host-12-99.bin\r\n");
    run.poll();
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(run.calls.slice(-2), ["standalone", -31]);
    await control.close();
});

test("native presentation uses the package-local frame mailbox and returns to the player overlay", async () => {
    const run = fixture();
    assert.equal(run.open("steam_sdl_mailbox=../file"), null);
    const control = run.open("steam_sdl_mailbox=sdl-host-12-99.bin\r\n", false);
    run.poll();
    await new Promise(resolve => setImmediate(resolve));
    run.poll("presented=2\r\noverlay_requested=0\r\n");
    run.poll("presented=3\r\noverlay_requested=1\r\n");
    run.poll();
    control.resume();
    await new Promise(resolve => setImmediate(resolve));
    run.poll("presented=1800\r\nsample_frames=1799\r\nsample_us=30001000\r\noverlay_requested=1\r\n");
    run.poll();
    control.close();
    assert.deepEqual(run.calls, ["C:\\LocalState\\sdl-host-12-99.bin", false, "start", -31, "presented",
        "menu", "start", -31, [1799, 30001000], "menu", "close"]);
});

test("a failed view switch releases the native resources and lets the existing player resume", async () => {
    const run = fixture(false);
    run.open("steam_sdl_mailbox=sdl-host-2-3.bin\n");
    run.poll();
    await new Promise(resolve => setImmediate(resolve));
    run.poll("presented=1\n");
    assert.deepEqual(run.calls.slice(-3), ["close", "fallback", "library"]);
});

test("closing during a pending switch restores the primary library after the late switch", async () => {
    let finish;
    const run = fixture(true, new Promise(resolve => { finish = resolve; }));
    const control = run.open("steam_sdl_mailbox=sdl-host-2-3.bin\n");
    run.poll();
    await new Promise(resolve => setImmediate(resolve));
    const restored = control.close();
    assert.equal(run.calls.at(-1), "close");
    assert.equal(control.close(), restored);
    finish();
    await restored;
    assert.equal(run.calls.at(-1), "library");
});

test("a superseded close cannot steal the next game's view", async () => {
    let finish;
    const run = fixture(true, new Promise(resolve => { finish = resolve; }));
    const old = run.open("steam_sdl_mailbox=sdl-host-2-3.bin\n");
    run.poll();
    await new Promise(resolve => setImmediate(resolve));
    old.close();
    run.open("steam_sdl_mailbox=sdl-host-2-4.bin\n");
    finish();
    await new Promise(resolve => setImmediate(resolve));
    assert.ok(!run.calls.includes("library"));
});
