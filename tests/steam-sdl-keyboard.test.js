const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const vm = require("node:vm");

async function exercisePlayer(packed, pointer = false) {
    const source = fs.readFileSync(path.join(__dirname, "../ui/signin/steam-sdl-host.js"), "utf8");
    const elements = new Map();
    const listeners = {};
    const calls = [];
    const gamepadSamples = [];
    let focus;
    let pad = 0;
    let frame;
    let frameAvailable = true;
    let presented = 0;
    let expectedPixel = [0, 216, 255, 255];

    function element(id) {
        if (!elements.has(id)) {
            elements.set(id, {
                hidden: true, style: {}, width: 1, height: 1, clientWidth: 1280, clientHeight: 720,
                focus() { focus = this; },
                addEventListener() {},
                getAttribute(name) { return name === "data-apps" || pointer ? "646570" : null; },
                getContext() {
                    return { createImageData: () => ({ data: new Uint8Array(4) }), putImageData(image) {
                        presented++;
                        if (packed) assert.deepEqual(Array.from(image.data), expectedPixel);
                    } };
                }
            });
        }
        return elements.get(id);
    }

    const buttons = { a: 1, b: 2, start: 4, back: 8, up: 16, down: 32, left: 64, right: 128 };
    const window = {
        steamSdlGamepad: { buttons, read: () => ({gamepad: [pad, 0, 0, 0, 0, -32768, -32768], overlay: pad}) },
        addEventListener(name, listener) { listeners[name] = listener; }
    };
    const document = {
        getElementById: element,
        get activeElement() { return focus; },
        addEventListener() {}
    };
    const pane = {
        occludedRect: { height: 0 },
        tryShow() { calls.push(["open"]); },
        tryHide() { calls.push(["close"]); }
    };
    const Windows = {
        UI: { ViewManagement: { InputPane: { getForCurrentView: () => pane } } },
        Storage: {
            Streams: {
                ByteOrder: {},
                DataReader: { fromBuffer() {
                    const words = [1, 1, 1, 0, 0];
                    return { readUInt32: () => words.shift(), unconsumedBufferLength: 4, readBytes() {}, close() {} };
                } }
            }
        }
    };
    const runner = {
        launchSteamSdlGame: () => "steam_sdl_host=ready\r\n",
        steamSdlFrame: () => frameAvailable ? {} : null,
        steamSdlRunning: () => true,
        steamSdlKeyboardReady: () => true,
        sendSteamSdlInput() {},
        sendSteamSdlGamepad(mask) { gamepadSamples.push(mask); return gamepadSamples.length; },
        sendSteamSdlKey(scan, down) { calls.push(["key", scan, down]); return true; },
        sendSteamSdlText(value) { calls.push(["text", value]); return true; },
        resetSteamSdlKeyboard() { calls.push(["reset"]); }
    };
    if (packed) {
        runner.steamSdlRgbDeltaFrame = previous => {
            if (!frameAvailable) return "";
            if (previous === 0) return "1,1,1,0,0,F\n" + String.fromCharCode(0x1800, 0x1ffd);
            if (previous === 1) {
                expectedPixel = [1, 215, 252, 255];
                return "1,1,2,0,0,D\n" + String.fromCharCode(0x2579);
            }
            return "1,1," + (previous + 1) + ",0,0,D\n" + String.fromCharCode(0x4000);
        };
        runner.steamSdlFrame = () => { throw new Error("Unexpected byte-array fallback"); };
    }
    vm.runInNewContext(source, {
        document, window, Windows, navigator: {},
        requestAnimationFrame(callback) { frame = callback; }
    });
    window.steamSdlRuntimes[0].launch(runner, 646570, {});
    await new Promise(setImmediate);
    frame(100);
    assert.equal(element("sdl-keyboard").hidden, false);
    frame(100 + 1000 / 60);
    assert.equal(presented, 2);
    frameAvailable = false;
    frame(100 + 2 * 1000 / 60);
    assert.equal(presented, 2);
    frameAvailable = true;
    frame(150);
    assert.equal(presented, 3);
    assert.equal(gamepadSamples.length, pointer ? 0 : 1);

    function key(type, code, value, keyCode) {
        listeners[type]({ type, code, key: value, keyCode, preventDefault() {} });
    }

    key("keydown", "KeyA", "A", 65);
    key("keyup", "KeyA", "A", 65);
    key("keydown", "Backspace", "Backspace", 8);
    key("keyup", "Backspace", "Backspace", 8);
    assert.deepEqual(calls.slice(-5), [
        ["key", 4, true], ["text", "A"], ["key", 4, false], ["key", 42, true], ["key", 42, false]
    ]);

    pad = buttons.start | buttons.back;
    frame(200);
    frame(1000);
    assert.equal(focus, element("sdl-resume"));
    pad = buttons.down;
    frame(1100);
    assert.equal(focus, element("sdl-keyboard"));
    pad = buttons.a;
    frame(1200);
    assert.equal(focus, element("sdl-text-value"));
    key("keydown", "KeyX", "x", 88);
    assert.deepEqual(calls.at(-1), ["open"]);

    element("sdl-text-value").value = "Spire é";
    pad = buttons.right;
    frame(1300);
    assert.equal(focus, element("sdl-text-send"));
    pad = buttons.a;
    frame(1400);
    assert.deepEqual(calls.slice(-2), [["text", "Spire é"], ["close"]]);
    assert.equal(focus, element("sdl-frame"));

    listeners.blur();
    assert.deepEqual(calls.at(-1), ["reset"]);
}

test("the SDL player presents each refresh and routes keyboard input", () => exercisePlayer(false));
test("the SDL player decodes bulk RGB while retaining keyboard input", () => exercisePlayer(true));

test("pointer delivery follows package configuration for any hosted AppID", () => exercisePlayer(false, true));

test("the primary video polls the controller independently and a quick mouse click reaches SDL in order", async () => {
    const elements = new Map(), listeners = {}, edges = [], overlays = [], samples = [];
    let now = 100, pump, ack = 0, buttons = 0, serial = 0, padSerial = 0, rejectPad = false;
    function element(id) {
        if (!elements.has(id)) elements.set(id, {
            hidden: true, handlers: {}, getAttribute: name => name === "data-apps" ? "646570" : null, getContext: () => ({}), focus() {},
            getBoundingClientRect: () => ({left: 200, top: 100, width: 1200, height: 675}),
            addEventListener(name, callback) { this.handlers[name] = callback; }
        });
        return elements.get(id);
    }
    const document = {getElementById: element, addEventListener() {}};
    const gamepad = require("../ui/signin/steam-sdl-gamepad.js");
    const physical = {getCurrentReading() {
        const bits = gamepad.buttons;
        return {buttons: (buttons & bits.b ? 8 : 0) | (buttons & bits.right ? 512 : 0) |
            (buttons & bits.start ? 1 : 0) | (buttons & bits.back ? 2 : 0)};
    }};
    const window = {
        Windows: {Gaming: {Input: {Gamepad: {get gamepads() {
            return buttons & gamepad.connected ? [physical] : [];
        }}}}},
        addEventListener(name, fn) { listeners[name] = fn; },
        steamSdlGamepad: gamepad,
        steamSdlNativeView: {open(report, events) {
            events.presented("width=1920");
            return {primary: true, cover() {}};
        }},
        steamBigPictureHost: {async showForGame(id, view) { overlays.push([id, view]); return true; }}
    };
    const runner = {
        launchSteamSdlGame: () => "steam_sdl_host=ready\r\n", steamSdlRunning: () => true,
        sendSteamSdlGamepad(mask) {
            samples.push(mask);
            if (rejectPad) { rejectPad = false; return 0; }
            return ++padSerial;
        }, steamSdlInputAck: () => ack,
        sendSteamSdlInput(pointer) { edges.push(pointer); return ++serial; }
    };
    const Windows = {UI: {ViewManagement: {
        ApplicationView: {getForCurrentView: () => ({id: 7, isFullScreenMode: true, setDesiredBoundsMode() {}})},
        ApplicationViewBoundsMode: {useCoreWindow: 0}
    }}};
    vm.runInNewContext(fs.readFileSync(path.join(__dirname, "../ui/signin/steam-sdl-host.js"), "utf8"), {
        document, window, Windows, navigator: {}, performance: {now: () => now},
        requestAnimationFrame() { assert.fail("the media view must receive input without an HTML repaint"); },
        setTimeout(callback) { pump = callback; }
    });
    await window.steamSdlRuntimes[0].launch(runner, 646570, {bigPicture: true});
    const screen = element("sdl-game"), p = {clientX: 500, clientY: 437.5, pointerId: 1};
    screen.handlers.pointerdown({...p, buttons: 1, type: "pointerdown"});
    screen.handlers.pointerup({...p, buttons: 0, type: "pointerup"});
    assert.deepEqual(edges, [1024 | 2048 << 12 | 1 << 24]);
    now += 16; pump();
    assert.equal(edges.length, 1);
    ack = serial; now += 16; pump();
    assert.deepEqual(edges, [1024 | 2048 << 12 | 1 << 24, 1024 | 2048 << 12]);

    function sample(mask) { buttons = mask; now += 16; pump(); }
    const connected = gamepad.connected, back = connected | gamepad.buttons.b;
    sample(connected);
    const beforeBack = samples.length;
    sample(back); sample(connected); sample(back); sample(connected);
    assert.deepEqual(samples.slice(beforeBack), [back, connected, back, connected]);
    assert.deepEqual(overlays, [], "two B taps remain game input");
    rejectPad = true;
    const right = gamepad.connected | gamepad.buttons.right;
    sample(right); sample(right); sample(right);
    assert.deepEqual(samples.slice(-2), [right, right], "retry the rejected state, then coalesce duplicates");
    const both = gamepad.connected | gamepad.buttons.start | gamepad.buttons.back;
    sample(both); sample(both);
    sample(gamepad.connected | gamepad.buttons.back);
    assert.deepEqual(overlays, []);
    assert.ok(samples.every(mask => !(mask & (gamepad.buttons.start | gamepad.buttons.back))));
    sample(gamepad.connected); sample(gamepad.connected);
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(overlays, [[646570, 7]]);
    sample(both);
    listeners.blur();
    sample(0); listeners.focus(); sample(both); sample(gamepad.connected);
    assert.equal(overlays.length, 1, "focus loss cancels the gesture, including a held chord on reactivation");
    sample(both); sample(0); sample(both); sample(gamepad.connected);
    assert.equal(overlays.length, 1, "disconnect and reconnect must not synthesize a release");
    sample(both); sample(gamepad.connected);
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(overlays.length, 2, "a fresh complete gesture still works");
    sample(both);
    listeners.keydown({type: "keydown", keyCode: 121, preventDefault() {}});
    sample(gamepad.connected);
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(overlays.length, 3, "F10 opens without a second overlay on pad release");
    listeners["steam-game-foreground"]({detail: false});
    const count = edges.length;
    screen.handlers.pointerdown({...p, buttons: 1, type: "pointerdown"});
    assert.equal(edges.length, count);
});
