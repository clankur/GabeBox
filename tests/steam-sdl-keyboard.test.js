const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const vm = require("node:vm");

async function exercisePlayer(packed) {
    const source = fs.readFileSync(path.join(__dirname, "../probe/signin/steam-sdl-host.js"), "utf8");
    const elements = new Map();
    const listeners = {};
    const calls = [];
    let focus;
    let pad = 0;
    let frame;
    let frameAvailable = true;
    let presented = 0;

    function element(id) {
        if (!elements.has(id)) {
            elements.set(id, {
                hidden: true, style: {}, width: 1, height: 1, clientWidth: 1280, clientHeight: 720,
                focus() { focus = this; },
                addEventListener() {},
                getAttribute() { return "646570"; },
                getContext() {
                    return { createImageData: () => ({ data: new Uint8Array(4) }), putImageData(image) {
                        presented++;
                        if (packed) assert.deepEqual(Array.from(image.data), [0, 216, 255, 255]);
                    } };
                }
            });
        }
        return elements.get(id);
    }

    const buttons = { a: 1, b: 2, start: 4, back: 8, up: 16, down: 32, left: 64, right: 128 };
    const window = {
        steamSdlGamepad: { buttons, read: () => [pad, 0, 0, 0, 0, -32768, -32768] },
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
        sendSteamSdlGamepad() {},
        sendSteamSdlKey(scan, down) { calls.push(["key", scan, down]); return true; },
        sendSteamSdlText(value) { calls.push(["text", value]); return true; },
        resetSteamSdlKeyboard() { calls.push(["reset"]); }
    };
    if (packed) {
        runner.steamSdlRgbFrame = () => frameAvailable ? "1,1,1,0,0\n" + String.fromCharCode(0x1800, 0x1ffd) : "";
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
