const assert = require("node:assert/strict");
const test = require("node:test");
const pad = require("../ui/signin/steam-sdl-gamepad.js");

test("Xbox input retains SDL button order and scales both sticks and triggers", () => {
    const sample = pad.windowsSample({
        buttons: 1 | 2 | 4 | 128 | 2048 | 4096,
        leftThumbstickX: -1,
        leftThumbstickY: 1,
        rightThumbstickX: 0.5,
        rightThumbstickY: -1,
        leftTrigger: 0,
        rightTrigger: 1
    });
    assert.deepEqual(sample, [pad.connected | 1 | 16 | 64 | 128 | 1024 | 4096, -32768, -32768,
        16384, 32767, -32768, 32767]);
});

test("the standard web gamepad fallback preserves D-pad and trigger inputs", () => {
    const buttons = Array.from({ length: 17 }, () => ({ pressed: false, value: 0 }));
    buttons[1].pressed = true;
    buttons[15].pressed = true;
    buttons[6].value = 0.5;
    const sample = pad.read({}, { getGamepads: () => [{ connected: true, mapping: "standard", buttons, axes: [0, 0, 0, 1] }] },
        {device: null});
    assert.deepEqual(sample.gamepad, [pad.connected | pad.buttons.b | pad.buttons.right, 0, 0, 0, 32767, 0, -32768]);
    assert.strictEqual(pad.read({}, {}, {device: null}).gamepad, pad.idle);
});

test("a second controller can open Steam without changing the game's controller sample", () => {
    const reading = {buttons: 4, leftTrigger: 0, rightTrigger: 0, leftThumbstickX: 0.5};
    const devices = [{getCurrentReading: () => reading},
        {getCurrentReading: () => ({...reading, buttons: 3 | 8, leftThumbstickX: -1})}];
    for (const gamepads of [devices, {size: 2, getAt: i => devices[i]}]) {
        const {gamepad, overlay} = pad.read({Gaming: {Input: {Gamepad: {gamepads}}}}, {}, {device: null});
        assert.equal(overlay, pad.connected | pad.buttons.start | pad.buttons.back | pad.buttons.b);
        assert.equal(gamepad[0], pad.connected | pad.buttons.a);
        assert.equal(gamepad[1], 16384);
    }
});

test("the release must come from the controller that held View and Menu", () => {
    const first = {getCurrentReading: () => ({buttons: 0})};
    let held = 3;
    const second = {getCurrentReading: () => ({buttons: held})};
    const devices = [first, second];
    const windows = {Gaming: {Input: {Gamepad: {gamepads: devices}}}};
    const tracker = {device: null};
    const read = () => pad.read(windows, {}, tracker);
    assert.equal(read().overlay, pad.connected | pad.buttons.start | pad.buttons.back);
    held = 2;
    assert.equal(read().overlay, pad.connected | pad.buttons.back);
    devices.splice(1);
    assert.equal(read().overlay, 0);
    assert.equal(tracker.device, null);
    assert.equal(read().gamepad[0], pad.connected);
});

test("a controller that disconnects during the read leaves the primary sample available", () => {
    const primary = {getCurrentReading: () => ({buttons: 4, leftThumbstickX: 0.5})};
    const secondary = {getCurrentReading() { throw new Error("device disconnected"); }};
    const gamepads = [primary, secondary];
    const tracker = {device: secondary};
    const frame = pad.read({Gaming: {Input: {Gamepad: {gamepads}}}}, {}, tracker);
    assert.equal(frame.gamepad[0], pad.connected | pad.buttons.a);
    assert.equal(frame.gamepad[1], 16384);
    assert.equal(frame.overlay, 0);
    assert.equal(tracker.device, null);
});

test("a browser controller in slot zero stays selected until both buttons are released", () => {
    let pressed = [8, 9];
    const web = {getGamepads() {
        return [pressed, [8, 9]].map(indices => ({
            mapping: "standard", connected: true, axes: [0, 0, 0, 0],
            buttons: Array.from({length: 17}, (_, i) => ({pressed: indices.includes(i), value: 0}))
        }));
    }};
    const tracker = {device: null};
    assert.equal(pad.read({}, web, tracker).gamepad[0], pad.connected);
    assert.equal(tracker.device, 0);
    pressed = [9];
    const partial = pad.read({}, web, tracker);
    assert.equal(partial.overlay, pad.connected | pad.buttons.start);
    assert.equal(partial.gamepad[0], pad.connected);
    pressed = [];
    assert.equal(pad.read({}, web, tracker).overlay, pad.connected);
});
