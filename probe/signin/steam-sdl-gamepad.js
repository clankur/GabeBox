(function (root) {
    "use strict";

    var connected = 1 << 16;
    var buttons = { a: 1, b: 2, back: 16, start: 64, up: 2048, down: 4096, left: 8192, right: 16384 };
    // Both lookup tables are indexed by SDL_GameControllerButton.
    var windowsBits = [4, 8, 16, 32, 2, 0, 1, 4096, 8192, 1024, 2048, 64, 128, 256, 512];
    var browserButtons = [0, 1, 2, 3, 8, 16, 9, 10, 11, 4, 5, 12, 13, 14, 15];
    var idle = [0, 0, 0, 0, 0, -32768, -32768];

    function axis(value) {
        value = Math.max(-1, Math.min(1, Number(value) || 0));
        return Math.round(value * (value < 0 ? 32768 : 32767));
    }

    function trigger(value) {
        return axis(2 * Math.max(0, Math.min(1, Number(value) || 0)) - 1);
    }

    function windowsSample(reading) {
        var mask = connected;
        windowsBits.forEach(function (flag, i) {
            if (reading.buttons & flag) mask |= 1 << i;
        });
        return [mask, axis(reading.leftThumbstickX), axis(-reading.leftThumbstickY),
            axis(reading.rightThumbstickX), axis(-reading.rightThumbstickY),
            trigger(reading.leftTrigger), trigger(reading.rightTrigger)];
    }

    function browserSample(pad) {
        var mask = connected;
        browserButtons.forEach(function (index, i) {
            if (pad.buttons[index] && pad.buttons[index].pressed) mask |= 1 << i;
        });
        return [mask, axis(pad.axes[0]), axis(pad.axes[1]), axis(pad.axes[2]), axis(pad.axes[3]),
            trigger(pad.buttons[6] && pad.buttons[6].value), trigger(pad.buttons[7] && pad.buttons[7].value)];
    }

    function read(windows, navigator) {
        try {
            var pads = windows.Gaming.Input.Gamepad.gamepads;
            if (pads.size) return windowsSample(pads.getAt(0).getCurrentReading());
        } catch (_) {
            // Web Gamepad also covers hosts without the Xbox Runtime projection.
        }
        var web = navigator.getGamepads ? navigator.getGamepads() : [];
        for (var i = 0; i < web.length; ++i) {
            if (web[i] && web[i].connected && web[i].mapping === "standard") return browserSample(web[i]);
        }
        return idle;
    }

    var api = { read: read, idle: idle, connected: connected, buttons: buttons, windowsSample: windowsSample };
    if (typeof module === "object" && module.exports) module.exports = api;
    else root.steamSdlGamepad = api;
}(this));
