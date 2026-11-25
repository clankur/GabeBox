// Time-stepped model of the whole route, for the tuner's simulator panel:
//
//   mouse counts -> boot reports at the mouse's report rate -> firmware encoder
//   once per output period -> console samples the stick at its own interval ->
//   package integrates deflection / 511 * full scale * elapsed.
//
// Everything is driven by timestamps in milliseconds, so browser timer jitter
// does not change the result. The DOM side lives in app.js.
//
// Classic script so index.html works from file://; CommonJS module in Node.
(function (root) {
    "use strict";

    var encoderApi = typeof module === "object" && module.exports ? require("./encoder.js") : root.mouse2xacEncoder;
    var Encoder = encoderApi.Encoder;
    var STICK_REACH = encoderApi.STICK_REACH;

    var HISTORY_MS = 4000;

    function clampInt8(value) {
        return value < -127 ? -127 : (value > 127 ? 127 : value);
    }

    // options: settings (encoder settings), reportRateHz, clampBootReport,
    // inputScale, consoleIntervalMs, consoleJitterMs, random (() => [0, 1)).
    function Simulator(options) {
        this.options = Object.assign({
            reportRateHz: 1000,
            clampBootReport: true,
            inputScale: 1,
            consoleIntervalMs: 8,
            consoleJitterMs: 0,
            random: Math.random
        }, options);
        this.encoder = new Encoder(this.options.settings);
        this.reset();
    }

    Simulator.prototype.reset = function () {
        this.encoder.reset();
        this.started = false;
        this.now = 0;
        this.nextReport = 0;
        this.nextPeriod = 0;
        this.nextSample = 0;
        this.lastSample = 0;
        this.bucket = { dx: 0, dy: 0, wheel: 0, dirty: false };
        this.carry = { x: 0, y: 0 };
        this.held = 0;
        this.stick = { unitsX: 0, unitsY: 0, x: 512, y: 512, buttonsA: 0 };
        // Reference: the motion the encoder is asked to carry, with the same
        // inversion applied. Decoded: what the package reconstructs.
        this.ref = { x: 0, y: 0 };
        this.decoded = { x: 0, y: 0 };
        this.refHistory = [];
        this.decodedHistory = [];
        this.lastInput = -Infinity;
        this.resetStats();
    };

    Simulator.prototype.resetStats = function () {
        this.stats = {
            pathCounts: 0,           // reference path length
            bootClippedCounts: 0,    // lost to the boot report's +-127 per report
            backlogClippedCounts: 0, // dropped by the backlog cap
            periods: 0,
            movingPeriods: 0,
            saturatedPeriods: 0,
            samples: 0,
            skippedValues: 0,        // stick values the console never sampled
            gestures: 0,
            lastGesture: null
        };
        this.droppedBase = this.encoder.droppedTotal;
        this.gesture = { path: 0, startRef: { x: this.ref.x, y: this.ref.y }, startDecoded: { x: this.decoded.x, y: this.decoded.y } };
        this.valueSampled = true;
    };

    Simulator.prototype.setOptions = function (options) {
        Object.assign(this.options, options);
        if (options.settings) this.encoder.setSettings(options.settings);
    };

    Simulator.prototype.periodMs = function () {
        return this.encoder.settings.outputPeriodUs / 1000;
    };

    Simulator.prototype.start = function (t) {
        this.started = true;
        this.now = t;
        this.nextReport = t + 1000 / this.options.reportRateHz;
        this.nextPeriod = t + this.periodMs();
        // The console's sampling is not in phase with the board's periods.
        this.lastSample = t;
        this.nextSample = t + this.options.random() * this.options.consoleIntervalMs + 0.001;
    };

    Simulator.prototype.sampleGap = function () {
        var jitter = this.options.consoleJitterMs;
        var gap = this.options.consoleIntervalMs + (jitter > 0 ? (this.options.random() * 2 - 1) * jitter : 0);
        return Math.max(0.1, gap);
    };

    // Process every report, period and console sample due at or before t.
    Simulator.prototype.advance = function (t) {
        if (!this.started) this.start(t);
        if (t < this.now) return;
        for (;;) {
            var next = Math.min(this.nextReport, this.nextPeriod, this.nextSample);
            if (next > t) break;
            this.now = next;
            if (next === this.nextReport) {
                this.flushReport();
                this.nextReport += 1000 / this.options.reportRateHz;
                if (this.nextReport <= next) this.nextReport = next + 1000 / this.options.reportRateHz;
            } else if (next === this.nextPeriod) {
                this.outputPeriod(next);
                this.nextPeriod = next + this.periodMs();
            } else {
                this.consoleSample(next);
                this.nextSample = next + this.sampleGap();
            }
        }
        this.now = t;
        this.trimHistory(t);
    };

    // Mouse motion in the computer mouse's counts, at time t.
    Simulator.prototype.move = function (t, dx, dy) {
        this.advance(t);
        var scale = this.options.inputScale;
        var sx = dx * scale + this.carry.x;
        var sy = dy * scale + this.carry.y;
        var ix = Math.trunc(sx);
        var iy = Math.trunc(sy);
        this.carry.x = sx - ix;
        this.carry.y = sy - iy;
        if (ix === 0 && iy === 0) return;
        this.bucket.dx += ix;
        this.bucket.dy += iy;
        this.bucket.dirty = true;
        var settings = this.encoder.settings;
        this.ref.x += settings.invertX ? -ix : ix;
        this.ref.y += settings.invertY ? -iy : iy;
        var step = Math.hypot(ix, iy);
        this.stats.pathCounts += step;
        this.gesture.path += step;
        this.lastInput = this.now;
        this.refHistory.push({ t: this.now, x: this.ref.x, y: this.ref.y });
    };

    // Wheel detents at time t, positive up.
    Simulator.prototype.wheel = function (t, detents) {
        this.advance(t);
        this.bucket.wheel += detents;
        this.bucket.dirty = true;
    };

    // A button change goes out in a report of its own, as a mouse sends it.
    Simulator.prototype.buttons = function (t, mask) {
        this.advance(t);
        this.held = mask & 0x1F;
        this.bucket.dirty = true;
        this.flushReport();
    };

    Simulator.prototype.flushReport = function () {
        var b = this.bucket;
        if (!b.dirty) return;
        var dx = b.dx;
        var dy = b.dy;
        var wheel = b.wheel;
        b.dx = b.dy = b.wheel = 0;
        b.dirty = false;
        if (this.options.clampBootReport) {
            var cx = clampInt8(dx);
            var cy = clampInt8(dy);
            this.stats.bootClippedCounts += Math.hypot(dx - cx, dy - cy);
            this.encoder.report(cx, cy, clampInt8(wheel), this.held);
            return;
        }
        // Without the clamp, model a mouse that splits fast motion over
        // several reports instead of losing it.
        do {
            var sx = clampInt8(dx);
            var sy = clampInt8(dy);
            var sw = clampInt8(wheel);
            this.encoder.report(sx, sy, sw, this.held);
            dx -= sx;
            dy -= sy;
            wheel -= sw;
        } while (dx !== 0 || dy !== 0 || wheel !== 0);
    };

    Simulator.prototype.outputPeriod = function (t) {
        var out = this.encoder.step();
        if (!this.valueSampled && (this.stick.unitsX !== 0 || this.stick.unitsY !== 0)) {
            this.stats.skippedValues++;
        }
        this.stick = out;
        this.stick.t = t;
        this.valueSampled = false;
        var s = this.stats;
        s.periods++;
        s.backlogClippedCounts = this.encoder.droppedTotal - this.droppedBase;
        if (out.unitsX !== 0 || out.unitsY !== 0) {
            s.movingPeriods++;
            if (Math.abs(out.unitsX) >= STICK_REACH || Math.abs(out.unitsY) >= STICK_REACH) s.saturatedPeriods++;
        }
    };

    // The package's decode: displacement = deflection / 511 * full scale * elapsed.
    Simulator.prototype.consoleSample = function (t) {
        var elapsed = (t - this.lastSample) / 1000;
        this.lastSample = t;
        var fullScale = this.encoder.settings.fullScaleCountsPerSecond;
        this.decoded.x += this.stick.unitsX / STICK_REACH * fullScale * elapsed;
        this.decoded.y += this.stick.unitsY / STICK_REACH * fullScale * elapsed;
        this.valueSampled = true;
        this.stats.samples++;
        this.decodedHistory.push({ t: t, x: this.decoded.x, y: this.decoded.y });
        this.checkSettled(t);
    };

    // Counts the encoder still owes the stick.
    Simulator.prototype.inFlight = function () {
        var f = this.encoder.inFlight();
        return Math.hypot(f.x, f.y);
    };

    Simulator.prototype.backlogFill = function () {
        var cap = this.encoder.encoding.backlogCounts;
        var f = this.encoder.inFlight();
        return cap > 0 ? Math.min(1, Math.max(Math.abs(f.x), Math.abs(f.y)) / cap) : 0;
    };

    // Once the hand has stopped and the stick is back at rest, score the
    // gesture: how far the decoded cursor landed from where it should have.
    Simulator.prototype.checkSettled = function (t) {
        var g = this.gesture;
        if (g.path === 0 || t - this.lastInput < 250) return;
        if (this.stick.unitsX !== 0 || this.stick.unitsY !== 0) return;
        var refMove = { x: this.ref.x - g.startRef.x, y: this.ref.y - g.startRef.y };
        var decMove = { x: this.decoded.x - g.startDecoded.x, y: this.decoded.y - g.startDecoded.y };
        var miss = Math.hypot(decMove.x - refMove.x, decMove.y - refMove.y);
        this.stats.gestures++;
        this.stats.lastGesture = {
            path: g.path,
            distance: Math.hypot(refMove.x, refMove.y),
            miss: miss,
            missPercent: g.path > 0 ? 100 * miss / g.path : 0
        };
        this.gesture = { path: 0, startRef: { x: this.ref.x, y: this.ref.y }, startDecoded: { x: this.decoded.x, y: this.decoded.y } };
    };

    // Move the decoded cursor onto the reference, as a user would re-center.
    Simulator.prototype.align = function () {
        this.decoded.x = this.ref.x;
        this.decoded.y = this.ref.y;
        this.decodedHistory.length = 0;
        this.refHistory.length = 0;
        this.gesture = { path: 0, startRef: { x: this.ref.x, y: this.ref.y }, startDecoded: { x: this.decoded.x, y: this.decoded.y } };
    };

    Simulator.prototype.trimHistory = function (t) {
        var cutoff = t - HISTORY_MS;
        trim(this.refHistory, cutoff);
        trim(this.decodedHistory, cutoff);
    };

    function trim(history, cutoff) {
        var drop = 0;
        while (drop < history.length - 1 && history[drop + 1].t < cutoff) drop++;
        if (drop > 0) history.splice(0, drop);
    }

    // Reference position at time t (it moves in steps at input events).
    function refAt(history, t, fallback) {
        var lo = 0;
        var hi = history.length - 1;
        if (hi < 0 || t < history[0].t) return fallback;
        while (lo < hi) {
            var mid = (lo + hi + 1) >> 1;
            if (history[mid].t <= t) lo = mid;
            else hi = mid - 1;
        }
        return history[lo];
    }

    // Best-fit delay between the reference and decoded paths over the last
    // windowMs: the shift that makes their motion agree best. null when the
    // window holds too little motion to tell.
    Simulator.prototype.estimateLag = function (windowMs, maxLagMs) {
        windowMs = windowMs || 1000;
        maxLagMs = maxLagMs || 250;
        var end = this.now;
        var samples = this.decodedHistory.filter(function (p) { return p.t >= end - windowMs; });
        if (samples.length < 8) return null;
        var first = samples[0];
        var span = 0;
        for (var i = 1; i < samples.length; i++) {
            span += Math.hypot(samples[i].x - samples[i - 1].x, samples[i].y - samples[i - 1].y);
        }
        if (span < 20) return null;
        var best = null;
        var bestErr = Infinity;
        var origin = this.refHistory.length ? this.refHistory[0] : this.ref;
        for (var lag = 0; lag <= maxLagMs; lag += 1) {
            var base = refAt(this.refHistory, first.t - lag, origin);
            var err = 0;
            for (var k = 0; k < samples.length; k++) {
                var p = refAt(this.refHistory, samples[k].t - lag, origin);
                var ex = (samples[k].x - first.x) - (p.x - base.x);
                var ey = (samples[k].y - first.y) - (p.y - base.y);
                err += ex * ex + ey * ey;
            }
            if (err < bestErr) {
                bestErr = err;
                best = lag;
            }
        }
        return best;
    };

    var api = { Simulator: Simulator, HISTORY_MS: HISTORY_MS };
    if (typeof module === "object" && module.exports) module.exports = api;
    else root.mouse2xacSim = api;
}(this));
