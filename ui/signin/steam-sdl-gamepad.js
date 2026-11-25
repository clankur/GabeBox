(function (root) {
    "use strict";

    var connected = 1 << 16;
    var buttons = { a: 1, b: 2, back: 16, start: 64, up: 2048, down: 4096, left: 8192, right: 16384 };
    // Both lookup tables are indexed by SDL_GameControllerButton.
    var windowsBits = [4, 8, 16, 32, 2, 0, 1, 4096, 8192, 1024, 2048, 64, 128, 256, 512];
    var browserButtons = [0, 1, 2, 3, 8, 16, 9, 10, 11, 4, 5, 12, 13, 14, 15];
    var idle = [0, 0, 0, 0, 0, -32768, -32768];
    var chord = buttons.start | buttons.back;

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

    function read(windows, navigator, tracker) {
        var primary, ownerSample, chordCandidate;
        var owner = tracker.device;
        function visit(device, sample) {
            if (!primary) primary = sample;
            if (owner === device) ownerSample = sample;
            if (!chordCandidate && (sample[0] & chord) === chord) chordCandidate = {device: device, sample: sample};
        }
        function snapshot() {
            if (owner == null && chordCandidate) {
                tracker.device = chordCandidate.device;
                ownerSample = chordCandidate.sample;
            } else if (owner != null && !ownerSample) {
                tracker.device = null;
            }
            // The overlay can follow a second pad without replacing the
            // gamepad sample. A disconnected owner cannot release a chord.
            var overlay = ownerSample || (owner == null ? primary : null) || idle;
            var output = primary || idle;
            if (ownerSample && ownerSample === primary && (primary[0] & chord)) {
                output = primary.slice();
                output[0] &= ~chord;
            }
            return {gamepad: output, overlay: overlay[0]};
        }
        try {
            var pads = windows.Gaming.Input.Gamepad.gamepads;
            var count = typeof pads.size === "number" ? pads.size : pads.length;
            for (var i = 0; i < count; ++i) {
                try {
                    var device = pads.getAt ? pads.getAt(i) : pads[i];
                    visit(device, windowsSample(device.getCurrentReading()));
                } catch (_) {
                    // A device can disconnect between enumeration and reading.
                }
            }
        } catch (_) {
            // Web Gamepad also covers hosts without the Xbox Runtime projection.
        }
        if (primary) return snapshot();
        var web = navigator.getGamepads ? navigator.getGamepads() : [];
        for (var j = 0; j < web.length; ++j) {
            if (!web[j] || !web[j].connected || web[j].mapping !== "standard") continue;
            visit(j, browserSample(web[j]));
        }
        return snapshot();
    }

    var api = { read: read, idle: idle, connected: connected, buttons: buttons, windowsSample: windowsSample };
    if (typeof module === "object" && module.exports) module.exports = api;
    else root.steamSdlGamepad = api;
}(this));
