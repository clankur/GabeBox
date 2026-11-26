// A port of the mouse2xac output encoder (../mouse2xac.ino) for the simulator.
//
// report() is tuh_hid_report_received_cb's accumulation on core 1. step() is one
// pass of loop() on core 0 that sends a joystick report. Arithmetic goes through
// Math.fround so every intermediate result is the IEEE single-precision value
// the firmware computes; keep the order of operations identical to the sketch.
//
// Classic script so index.html works from file://; CommonJS module in Node.
(function (root) {
    "use strict";

    var f = Math.fround;

    var STICK_CENTER = 512;
    var STICK_REACH = 511;
    var MOUSE_BUTTONS = 0x1F;
    var WHEEL_UP_BUTTON = 0x20;
    var WHEEL_DOWN_BUTTON = 0x40;
    var WHEEL_PENDING_LIMIT = 16;

    function clampInt(value, low, high) {
        return value < low ? low : (value > high ? high : value);
    }

    // lroundf: nearest integer, halfway cases away from zero. x is a float32
    // value, so x + 0.5 is exact in double precision. 0 - n avoids -0.
    function lroundf(x) {
        return x < 0 ? 0 - Math.floor(-x + 0.5) : Math.floor(x + 0.5);
    }

    // encoding_for() in the sketch.
    function encodingFor(settings) {
        var countsPerUnit = f(f(f(settings.fullScaleCountsPerSecond) * f(f(settings.outputPeriodUs) / f(1e6))) /
            STICK_REACH);
        return {
            outputPeriodUs: settings.outputPeriodUs,
            countsPerUnit: countsPerUnit,
            backlogCounts: f(f(STICK_REACH * countsPerUnit) * f(settings.backlogPeriods)),
            invertX: !!settings.invertX,
            invertY: !!settings.invertY,
            wheelEnabled: !!settings.wheelEnabled
        };
    }

    // deflect() in the sketch, without the side effects.
    function deflect(pending, encoding) {
        var high = encoding.backlogCounts;
        var low = -high;
        var limited = pending < low ? low : (pending > high ? high : pending);
        var dropped = f(Math.abs(f(pending - limited)));
        var units = clampInt(lroundf(f(limited / encoding.countsPerUnit)), -STICK_REACH, STICK_REACH);
        return {
            units: units,
            pending: f(limited - f(units * encoding.countsPerUnit)),
            dropped: dropped
        };
    }

    function Encoder(settings) {
        this.setSettings(settings);
        this.reset();
    }

    Encoder.prototype.setSettings = function (settings) {
        this.settings = Object.assign({}, settings);
        this.encoding = encodingFor(this.settings);
    };

    Encoder.prototype.reset = function () {
        // struct Motion, shared with core 1.
        this.motion = { dx: 0, dy: 0, wheel: 0, held: 0, latched: 0 };
        // loop()'s statics.
        this.pendingX = 0;
        this.pendingY = 0;
        this.wheelPending = 0;
        this.wheelPressed = false;
        this.clippedFraction = 0;
        // Counters the config channel exposes.
        this.mouseReports = 0;
        this.backlogClippedCounts = 0;
        // Exact total of dropped counts, for the simulator's statistics.
        this.droppedTotal = 0;
    };

    // One boot-protocol mouse report: buttons, then int8 X, Y and wheel.
    Encoder.prototype.report = function (dx, dy, wheel, buttons) {
        wheel = wheel || 0;
        buttons = buttons || 0;
        [dx, dy, wheel].forEach(function (value) {
            if (!Number.isInteger(value) || value < -128 || value > 127) {
                throw new RangeError("boot report fields are int8, got " + value);
            }
        });
        var m = this.motion;
        m.dx += dx;
        m.dy += dy;
        m.wheel = clampInt(m.wheel + wheel, -WHEEL_PENDING_LIMIT, WHEEL_PENDING_LIMIT);
        m.held = buttons & 0xFF;
        m.latched |= buttons & 0xFF;
        this.mouseReports++;
    };

    // Drops held buttons, as when the last mouse unmounts.
    Encoder.prototype.unmount = function () {
        this.motion.held = 0;
    };

    // One output period. Returns the joystick report and the encoder state.
    Encoder.prototype.step = function () {
        var e = this.encoding;
        var m = this.motion;
        var dx = m.dx;
        var dy = m.dy;
        var wheel = m.wheel;
        var buttons = m.latched;
        m.dx = m.dy = m.wheel = 0;
        m.latched = m.held;

        this.pendingX = f(this.pendingX + f(e.invertX ? -dx : dx));
        this.pendingY = f(this.pendingY + f(e.invertY ? -dy : dy));
        if (!e.wheelEnabled) {
            wheel = 0;
            this.wheelPending = 0;
        }
        this.wheelPending = clampInt(this.wheelPending + wheel, -WHEEL_PENDING_LIMIT, WHEEL_PENDING_LIMIT);

        var wheelButton = 0;
        if (this.wheelPressed) {
            this.wheelPressed = false;
        } else if (this.wheelPending !== 0) {
            wheelButton = this.wheelPending > 0 ? WHEEL_UP_BUTTON : WHEEL_DOWN_BUTTON;
            this.wheelPending += this.wheelPending > 0 ? -1 : 1;
            this.wheelPressed = true;
        }

        var dropped = 0;
        var x = deflect(this.pendingX, e);
        this.pendingX = x.pending;
        dropped = f(dropped + x.dropped);
        var y = deflect(this.pendingY, e);
        this.pendingY = y.pending;
        dropped = f(dropped + y.dropped);

        this.droppedTotal += x.dropped + y.dropped;
        this.clippedFraction = f(this.clippedFraction + dropped);
        if (this.clippedFraction >= 1) {
            var whole = Math.trunc(this.clippedFraction);
            this.clippedFraction = f(this.clippedFraction - f(whole));
            this.backlogClippedCounts = (this.backlogClippedCounts + whole) >>> 0;
        }

        return {
            x: STICK_CENTER + x.units,
            y: STICK_CENTER + y.units,
            unitsX: x.units,
            unitsY: y.units,
            buttonsA: (buttons & MOUSE_BUTTONS) | wheelButton,
            wheelButton: wheelButton,
            pendingX: this.pendingX,
            pendingY: this.pendingY,
            dropped: x.dropped + y.dropped
        };
    };

    // Counts still owed to the stick: undrained reports plus the backlog.
    Encoder.prototype.inFlight = function () {
        var e = this.encoding;
        return {
            x: this.pendingX + (e.invertX ? -this.motion.dx : this.motion.dx),
            y: this.pendingY + (e.invertY ? -this.motion.dy : this.motion.dy)
        };
    };

    // The encoding contract in human units.
    function contract(settings, extra) {
        extra = extra || {};
        var e = encodingFor(settings);
        var periodMs = settings.outputPeriodUs / 1000;
        var fullPerPeriod = STICK_REACH * e.countsPerUnit;
        var out = {
            periodMs: periodMs,
            countsPerUnit: e.countsPerUnit,
            unitsPerCount: 1 / e.countsPerUnit,
            fullScaleCountsPerSecond: settings.fullScaleCountsPerSecond,
            smallestSpeedCountsPerSecond: settings.fullScaleCountsPerSecond / STICK_REACH,
            fullDeflectionCountsPerPeriod: fullPerPeriod,
            backlogCounts: e.backlogCounts,
            backlogMs: settings.backlogPeriods * periodMs,
            // After the hand stops, a full backlog takes this long to drain.
            drainMs: (settings.backlogPeriods - 1) * periodMs
        };
        if (extra.cpi > 0) {
            out.fullScaleInchesPerSecond = settings.fullScaleCountsPerSecond / extra.cpi;
            out.smallestSpeedInchesPerSecond = out.smallestSpeedCountsPerSecond / extra.cpi;
        }
        if (extra.reportRateHz > 0) {
            // A boot report carries at most 127 counts per axis.
            out.bootLimitCountsPerSecond = 127 * extra.reportRateHz;
            if (extra.cpi > 0) out.bootLimitInchesPerSecond = out.bootLimitCountsPerSecond / extra.cpi;
        }
        return out;
    }

    var api = {
        STICK_CENTER: STICK_CENTER,
        STICK_REACH: STICK_REACH,
        MOUSE_BUTTONS: MOUSE_BUTTONS,
        WHEEL_UP_BUTTON: WHEEL_UP_BUTTON,
        WHEEL_DOWN_BUTTON: WHEEL_DOWN_BUTTON,
        WHEEL_PENDING_LIMIT: WHEEL_PENDING_LIMIT,
        Encoder: Encoder,
        encodingFor: encodingFor,
        deflect: deflect,
        lroundf: lroundf,
        contract: contract
    };
    if (typeof module === "object" && module.exports) module.exports = api;
    else root.mouse2xacEncoder = api;
}(this));
