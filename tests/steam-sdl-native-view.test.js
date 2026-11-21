const assert = require("node:assert/strict");
const fs = require("node:fs");
const test = require("node:test");
const vm = require("node:vm");

const source = fs.readFileSync(require.resolve("../probe/signin/steam-sdl-native-view.js"), "utf8");
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
