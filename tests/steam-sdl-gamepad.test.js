const assert = require("node:assert/strict");
const test = require("node:test");
const pad = require("../probe/signin/steam-sdl-gamepad.js");

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
    const sample = pad.read({}, { getGamepads: () => [{ connected: true, mapping: "standard", buttons, axes: [0, 0, 0, 1] }] });
    assert.deepEqual(sample, [pad.connected | pad.buttons.b | pad.buttons.right, 0, 0, 0, 32767, 0, -32768]);
    assert.strictEqual(pad.read({}, {}), pad.idle);
});

test("a controller in the second Xbox slot can open Steam over a single-player game", () => {
    const reading = {buttons: 0, leftTrigger: 0, rightTrigger: 0};
    const devices = [{getCurrentReading: () => reading}, {getCurrentReading: () => ({...reading, buttons: 3})}];
    for (const gamepads of [devices, {size: 2, getAt: i => devices[i]}]) {
        assert.equal(pad.read({Gaming: {Input: {Gamepad: {gamepads}}}}, {})[0],
            pad.connected | pad.buttons.start | pad.buttons.back);
    }
});
