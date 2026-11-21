const assert = require("node:assert/strict");
const fs = require("node:fs");
const test = require("node:test");
const vm = require("node:vm");

const source = fs.readFileSync(require.resolve("../probe/signin/steam-sdl-native-view.js"), "utf8");
function fixture(accept = true) {
    const calls = [];
    let tick, stats = "presented=0\n";
    const host = { open: path => calls.push(path), start: () => calls.push("start"),
        setNativeGamepad: enabled => calls.push(enabled), close: () => calls.push("close"),
        viewId: () => -31, status: () => stats };
    const window = { Fhd: { Native: { RenderHost: host } } };
    const Windows = {
        UI: { ViewManagement: { ApplicationView: { getForCurrentView: () => ({ id: -73 }) }, ApplicationViewSwitcher: {
            tryShowAsStandaloneAsync: () => Promise.resolve(true),
            switchAsync: (id, mainId) => { assert.equal(mainId, -73); calls.push(id); return accept ? Promise.resolve() : Promise.reject(Error()); }
        } } },
        Storage: { ApplicationData: { current: { localFolder: { path: "C:\\LocalState" } } } }
    };
    vm.runInNewContext(source, { window, Windows, Date, setInterval(f) { tick = f; return 1; }, clearInterval() {} });
    function open(value, nativePad) {
        return window.steamSdlNativeView.open(value, { running: () => true, fallback: () => calls.push("fallback"),
            presented: () => calls.push("presented"), menu: () => calls.push("menu") }, nativePad);
    }
    return { open, calls, poll(value) { if (value) stats = value; tick(); } };
}

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
    run.poll("presented=5\r\noverlay_requested=1\r\n");
    control.close();
    assert.deepEqual(run.calls, ["C:\\LocalState\\sdl-host-12-99.bin", false, "start", -31, "presented",
        "menu", "start", -31, "menu", "close"]);
});

test("a failed view switch releases the native resources and lets the existing player resume", async () => {
    const run = fixture(false);
    run.open("steam_sdl_mailbox=sdl-host-2-3.bin\n");
    run.poll();
    await new Promise(resolve => setImmediate(resolve));
    run.poll("presented=1\n");
    assert.deepEqual(run.calls.slice(-2), ["close", "fallback"]);
});
