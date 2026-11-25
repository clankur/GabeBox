const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const tuner = path.join(__dirname, "..", "hardware", "mouse2xac", "tuner");
const protocol = require(path.join(tuner, "protocol.js"));
const { Encoder, encodingFor, lroundf, contract } = require(path.join(tuner, "encoder.js"));
const { Simulator } = require(path.join(tuner, "sim.js"));
const { ConfigClient } = require(path.join(tuner, "device.js"));
const sketch = fs.readFileSync(path.join(tuner, "..", "mouse2xac.ino"), "utf8");

const defaults = protocol.defaultSettings();

function run(encoder, periods) {
    const out = [];
    for (let i = 0; i < periods; i++) out.push(encoder.step());
    return out;
}

// Expected sticks in the parity tests come from the sketch's own encoding_for(),
// deflect() and loop() body, extracted and compiled with clang on the host
// (-ffp-contract=off, as the RP2040's soft float has no fused multiply-add).
// The same harness matched this port bit for bit over 320,000 fuzzed periods.

test("slow motion adds up through the remainder", () => {
    // 25600 CPI at 10 in/s: one count is a quarter of a stick unit.
    const encoder = new Encoder({ ...defaults, fullScaleCountsPerSecond: 256000 });
    const xs = [];
    const ys = [];
    for (let i = 0; i < 12; i++) {
        encoder.report(1, -1);
        const out = encoder.step();
        xs.push(out.x);
        ys.push(out.y);
    }
    assert.deepEqual(xs, [512, 512, 513, 512, 512, 512, 513, 512, 512, 512, 513, 512]);
    assert.deepEqual(ys, xs.map((x) => 1024 - x));
    // 12 counts went out as 3 units; the rest is still pending, not lost.
    const e = encodingFor(encoder.settings);
    assert.ok(Math.abs(12 - 3 * e.countsPerUnit - encoder.pendingX) < 1e-4);
    assert.deepEqual(run(encoder, 4).map((o) => o.x), [512, 512, 512, 512]);
});

test("motion beyond full deflection carries into later periods", () => {
    const encoder = new Encoder(defaults);
    encoder.report(100, 0);
    encoder.report(100, -20);
    const out = run(encoder, 5);
    assert.deepEqual(out.map((o) => o.x), [1023, 1023, 1023, 576, 512]);
    assert.deepEqual(out.map((o) => o.y), [352, 512, 512, 512, 512]);
    assert.equal(encoder.backlogClippedCounts, 0);
});

test("the backlog cap drops motion above it", () => {
    const encoder = new Encoder(defaults);
    for (let i = 0; i < 8; i++) encoder.report(127, 0);
    const out = run(encoder, 8);
    // 1016 counts against a 6-period backlog of 384: 632 are dropped at once.
    assert.equal(encodingFor(defaults).backlogCounts, 384);
    assert.deepEqual(out.map((o) => o.x), [1023, 1023, 1023, 1023, 1023, 1023, 512, 512]);
    assert.equal(encoder.backlogClippedCounts, 632);
    assert.equal(encoder.droppedTotal, 632);
});

test("each wheel detent is one period pressed then one released", () => {
    const encoder = new Encoder(defaults);
    encoder.report(0, 0, 2);
    encoder.report(0, 0, 0);
    assert.deepEqual(run(encoder, 5).map((o) => o.buttonsA), [0x20, 0, 0x20, 0, 0]);
    encoder.report(0, 0, -1);
    assert.deepEqual(run(encoder, 2).map((o) => o.buttonsA), [0x40, 0]);
    // A detent that arrives while a pulse is pressed waits for the release.
    encoder.report(0, 0, 1);
    encoder.step();
    encoder.report(0, 0, 1);
    assert.deepEqual(run(encoder, 4).map((o) => o.buttonsA), [0, 0x20, 0, 0]);
    encoder.setSettings({ ...defaults, wheelEnabled: false });
    encoder.report(0, 0, 3);
    assert.deepEqual(run(encoder, 2).map((o) => o.buttonsA), [0, 0]);
});

test("wheel backlog is capped at 16 detents", () => {
    const encoder = new Encoder(defaults);
    for (let i = 0; i < 10; i++) encoder.report(0, 0, 5);
    const pulses = run(encoder, 80).filter((o) => o.wheelButton).length;
    assert.equal(pulses, 16);
});

test("a click shorter than a period is latched for one period", () => {
    const encoder = new Encoder(defaults);
    encoder.report(0, 0, 0, 1);
    encoder.report(0, 0, 0, 0);
    assert.deepEqual(run(encoder, 2).map((o) => o.buttonsA), [1, 0]);
    // A held button stays down, and shows for the period it is released in.
    encoder.report(0, 0, 0, 2);
    assert.deepEqual(run(encoder, 2).map((o) => o.buttonsA), [2, 2]);
    encoder.report(0, 0, 0, 0);
    assert.deepEqual(run(encoder, 2).map((o) => o.buttonsA), [2, 0]);
    // Only the five mouse buttons reach the joystick.
    encoder.report(0, 0, 0, 0xE1);
    assert.equal(encoder.step().buttonsA, 1);
});

test("inversion flips the axis before encoding", () => {
    const encoder = new Encoder({ ...defaults, invertX: true });
    encoder.report(10, 10);
    const out = encoder.step();
    assert.deepEqual([out.x, out.y], [432, 592]);
});

test("the port rounds like lroundf and rejects values a boot report cannot carry", () => {
    assert.deepEqual([0.5, 1.5, -0.5, -1.5, 0.49999997, -0.49999997, 2.4].map(lroundf), [1, 2, -1, -2, 0, 0, 2]);
    assert.ok(Object.is(lroundf(-0.2), 0));
    const encoder = new Encoder(defaults);
    assert.throws(() => encoder.report(128, 0), RangeError);
    assert.throws(() => encoder.report(0, 0.5), RangeError);
});

test("the contract table matches the README defaults", () => {
    const c = contract(defaults, { cpi: 1600, reportRateHz: 1000 });
    assert.equal(c.periodMs, 8);
    assert.ok(Math.abs(c.countsPerUnit - 0.1252) < 1e-4);
    assert.equal(c.backlogCounts, 384);
    assert.equal(c.backlogMs, 48);
    assert.equal(c.fullScaleInchesPerSecond, 5);
    assert.equal(c.bootLimitCountsPerSecond, 127000);
});

test("protocol commands and answers round-trip", () => {
    assert.deepEqual([...protocol.encodeCommand(protocol.COMMANDS.select, protocol.REGISTERS.status)], [0, 0x15, 0, 0]);
    assert.deepEqual([...protocol.writeCommand("fullScaleCountsPerSecond", 0x0186A0)], [0x82, 0xA0, 0x86, 0x01]);
    assert.deepEqual([...protocol.writeCommand("invertY", true)], [0x85, 1, 0, 0]);
    assert.throws(() => protocol.encodeCommand(0x100, 0), RangeError);
    assert.throws(() => protocol.encodeCommand(0, 0x1000000), RangeError);
    assert.throws(() => protocol.encodeCommand(0, -1), RangeError);

    assert.deepEqual(protocol.decodeResponse(new Uint8Array([0x02, 0x40, 0x1F, 0x00])), { register: 2, value: 8000 });
    // A leading zero report ID is dropped.
    const withId = new DataView(new Uint8Array([0, 0x12, 0xFF, 0xFF, 0xFF]).buffer);
    assert.deepEqual(protocol.decodeResponse(withId), { register: 0x12, value: 0xFFFFFF });
    assert.throws(() => protocol.decodeResponse(new Uint8Array([1, 2])), RangeError);

    assert.deepEqual(protocol.decodeError(0x05_81_03), { code: 3, command: 0x81, valueLow: 5, message: "value out of range" });
    assert.deepEqual(protocol.decodeStatus(0x05), { flashValid: true, savePending: false, unsaved: true, saveFailed: false });
    assert.equal(protocol.formatVersion(0x000100), "0.1.0");
});

test("settings validation uses the firmware ranges", () => {
    assert.deepEqual(protocol.validateSettings(defaults), []);
    assert.match(protocol.validateSetting("outputPeriodUs", 1999), /2000/);
    assert.match(protocol.validateSetting("fullScaleCountsPerSecond", 0), /1–2000000/);
    assert.match(protocol.validateSetting("backlogPeriods", 2.5), /whole/);
    assert.equal(protocol.validateSetting("invertX", true), null);
});

test("joystick reports decode the 10-bit flight stick layout", () => {
    const report = { x: 1023, y: 1, hat: 0xF, twist: 0x80, buttonsA: 0x61, slider: 0x80, buttonsB: 0x9 };
    const bytes = protocol.encodeJoystickReport(report);
    assert.equal(bytes.length, 7);
    assert.deepEqual(protocol.decodeJoystickReport(bytes), report);
    // Idle report as the firmware sends it.
    assert.deepEqual([...protocol.encodeJoystickReport({ x: 512, y: 512, hat: 0xF, twist: 0x80, buttonsA: 0, slider: 0x80, buttonsB: 0 })],
        [0x00, 0x02, 0xF8, 0x80, 0x00, 0x80, 0x00]);
});

test("protocol numbers match the sketch", () => {
    const constant = (name) => {
        const match = sketch.match(new RegExp("constexpr \\w+ " + name + " = (0x[0-9A-Fa-f]+|\\d+);"));
        assert.ok(match, name + " is defined in the sketch");
        return Number(match[1]);
    };
    for (const [key, value] of Object.entries(protocol.COMMANDS)) {
        const name = "CONFIG_CMD_" + key.replace(/[A-Z]/g, (c) => "_" + c).toUpperCase();
        assert.equal(constant(name), value, name);
    }
    for (const [key, value] of Object.entries(protocol.REGISTERS)) {
        const name = "CONFIG_REG_" + key.replace(/[A-Z]/g, (c) => "_" + c).toUpperCase();
        assert.equal(constant(name), value, name);
    }
    for (const [key, value] of Object.entries(protocol.STATUS)) {
        const name = "CONFIG_STATUS_" + key.replace(/[A-Z]/g, (c) => "_" + c).toUpperCase();
        assert.equal(constant(name), value, name);
    }
    for (const [key, value] of Object.entries(protocol.ERRORS)) {
        const name = "CONFIG_ERROR_" + key.replace(/[A-Z]/g, (c) => "_" + c).toUpperCase();
        assert.equal(constant(name), value, name);
    }
    assert.equal(constant("PROTOCOL_VERSION"), protocol.PROTOCOL_VERSION);
    assert.equal(constant("SETTING_SLOTS"), protocol.SETTINGS.length + 1);
    const specs = [...sketch.matchAll(/\{(\d+), (\d+), (\d+)\},\s*\/\/ 0x([0-9A-F]{2})/g)];
    assert.equal(specs.length, protocol.SETTINGS.length + 1);
    for (const spec of protocol.SETTINGS) {
        const row = specs.find((m) => Number("0x" + m[4]) === spec.id);
        assert.ok(row, "spec row for setting " + spec.id);
        assert.deepEqual([Number(row[1]), Number(row[2]), Number(row[3])], [spec.initial, spec.min, spec.max], spec.key);
    }
    assert.equal(constant("WHEEL_PENDING_LIMIT"), 16);
    assert.equal(constant("STICK_REACH"), 511);
});

// A stand-in for the board's config channel, written from the protocol table
// in README.md, to exercise the WebHID client.
function fakeBoard() {
    const values = { 1: 8000, 2: 8000, 3: 6, 4: 0, 5: 0, 6: 1 };
    const saved = { ...values };
    let selected = 0x11;
    let lastError = 0;
    let savePolls = 0;
    const read = (reg) => {
        if (values[reg] !== undefined) return values[reg];
        if (reg === 0x10) return 0x000100;
        if (reg === 0x11) return 1;
        if (reg === 0x12) return 4321;
        if (reg === 0x13) return 1;
        if (reg === 0x14) return 0;
        if (reg === 0x16) return 0;
        if (reg === 0x15) {
            const unsaved = Object.keys(values).some((k) => values[k] !== saved[k]);
            return (savePolls > 0 ? 2 : 0) | (unsaved ? 4 : 0) | 1;
        }
        return lastError;
    };
    const reject = (code, command, value) => { lastError = code | (command << 8) | ((value & 0xFF) << 16); selected = 0xFF; };
    return {
        sent: [],
        sendFeatureReport(id, bytes) {
            assert.equal(id, 0);
            this.sent.push([...bytes]);
            const [command, a, b, c] = bytes;
            const value = a | (b << 8) | (c << 16);
            if (command & 0x80) {
                const spec = protocol.settingById(command & 0x7F);
                if (!spec) reject(2, command, command & 0x7F);
                else if (value < spec.min || value > spec.max) reject(3, command, value);
                else { values[spec.id] = value; selected = spec.id; }
            } else if (command === 0) selected = value;
            else if (command === 1) { savePolls = 2; Object.assign(saved, values); selected = 0x15; }
            else if (command === 2) { Object.assign(values, { 1: 8000, 2: 8000, 3: 6, 4: 0, 5: 0, 6: 1 }); selected = 0x15; }
            else reject(1, command, value);
            return Promise.resolve();
        },
        receiveFeatureReport(id) {
            assert.equal(id, 0);
            const value = read(selected);
            if (selected === 0x15 && savePolls > 0) savePolls--;
            // Answer with the zero report ID in front, as some backends do.
            return Promise.resolve(new DataView(new Uint8Array([0, selected, value & 0xFF, (value >> 8) & 0xFF, value >> 16]).buffer));
        }
    };
}

test("the WebHID client reads, writes and saves settings", async () => {
    const board = fakeBoard();
    const client = new ConfigClient(board);
    assert.deepEqual(await client.hello(), { protocolVersion: 1, firmwareVersion: 0x100, firmware: "0.1.0" });
    assert.deepEqual(await client.readSettings(), defaults);

    const tuned = { ...defaults, outputPeriodUs: 4000, fullScaleCountsPerSecond: 64000, invertY: true };
    assert.deepEqual(await client.writeSettings(tuned), tuned);
    assert.equal((await client.status()).unsaved, true);
    const status = await client.save();
    assert.equal(status.unsaved, false);
    assert.deepEqual(await client.restoreDefaults(), defaults);

    const counters = await client.readCounters();
    assert.equal(counters.mouseReports, 4321);
    assert.equal(counters.statusFlags.flashValid, true);
});

test("the WebHID client surfaces rejected writes and serializes exchanges", async () => {
    const board = fakeBoard();
    const client = new ConfigClient(board);
    await assert.rejects(client.write("outputPeriodUs", 1000), /2000/);
    assert.equal(board.sent.length, 0, "invalid values never reach the device");
    // Bypass local validation to see the device's own rejection.
    await assert.rejects(client.expect(protocol.COMMANDS.write | 1, 1000, 1), /value out of range/);
    await assert.rejects(client.expect(0x7E, 0, 0x15), /unknown command/);

    const reads = await Promise.all([client.read(1), client.read(2), client.read(3)]);
    assert.deepEqual(reads, [8000, 8000, 6]);
    assert.deepEqual(board.sent.slice(-3).map((bytes) => bytes[1]), [1, 2, 3]);
});

test("the simulator decodes steady motion without loss", () => {
    const sim = new Simulator({ settings: defaults, reportRateHz: 1000, consoleIntervalMs: 8, consoleJitterMs: 0,
        random: () => 0.25 });
    // 4000 counts/s to the right for one second: 4 counts every millisecond.
    for (let t = 1; t <= 1000; t++) sim.move(t, 4, 0);
    sim.advance(1500);
    assert.equal(sim.ref.x, 4000);
    assert.ok(Math.abs(sim.decoded.x - 4000) < 1, "decoded " + sim.decoded.x);
    assert.equal(sim.stats.backlogClippedCounts, 0);
    assert.equal(sim.stats.skippedValues, 0);
    assert.equal(sim.stats.gestures, 1);
    assert.ok(sim.stats.lastGesture.missPercent < 0.1);
});

test("the simulator shows loss when the console samples slower than the output period", () => {
    const settings = { outputPeriodUs: 8000 };
    const pulses = (interval) => {
        const sim = new Simulator({ settings: { ...defaults, ...settings }, consoleIntervalMs: interval, random: () => 0.5 });
        // Short flicks: each one fits in a single output period.
        for (let k = 0; k < 40; k++) sim.move(1 + k * 37, 30, 0);
        sim.advance(2000);
        return sim;
    };
    const matched = pulses(8);
    assert.equal(matched.stats.skippedValues, 0);
    assert.ok(Math.abs(matched.decoded.x - matched.ref.x) < 1);
    const slow = pulses(16.5);
    assert.ok(slow.stats.skippedValues > 5, "skipped " + slow.stats.skippedValues);
    assert.ok(Math.abs(slow.decoded.x - slow.ref.x) > 0.02 * slow.ref.x, "decoded " + slow.decoded.x);
});

test("the simulator clips fast motion at the boot report limit and the backlog", () => {
    const sim = new Simulator({ settings: defaults, reportRateHz: 1000, consoleIntervalMs: 8 });
    // 200 counts per millisecond is above the 127 a boot report can carry.
    for (let t = 1; t <= 50; t++) sim.move(t, 200, 0);
    sim.advance(500);
    assert.ok(Math.abs(sim.stats.bootClippedCounts - 50 * 73) < 1e-9);
    assert.ok(sim.stats.backlogClippedCounts > 0);
    assert.ok(sim.stats.saturatedPeriods > 0);
});

test("the simulator estimates the pipeline delay", () => {
    // Console samples land 4 ms after each output period.
    const sim = new Simulator({ settings: defaults, consoleIntervalMs: 8, consoleJitterMs: 0, random: () => 0.5 });
    // A slow sine sweep keeps the stick off the rails.
    let last = 0;
    for (let t = 1; t <= 1200; t++) {
        const target = Math.round(600 * Math.sin(t / 150));
        sim.move(t, target - last, 0);
        last = target;
    }
    const lag = sim.estimateLag(1000, 100);
    assert.ok(lag !== null && lag >= 3 && lag <= 6, "lag " + lag);
});

test("settings round-trip through the page link query", () => {
    const settings = { ...defaults, outputPeriodUs: 10000, fullScaleCountsPerSecond: 25600, invertY: true, wheelEnabled: false };
    const query = protocol.settingsToQuery(settings);
    assert.equal(query, "outputPeriodUs=10000&fullScaleCountsPerSecond=25600&backlogPeriods=6&invertX=0&invertY=1&wheelEnabled=0");
    assert.deepEqual(protocol.settingsFromQuery("?" + query), { settings, errors: [] });
});

test("a link query without settings is ignored, and bad values keep their defaults", () => {
    assert.equal(protocol.settingsFromQuery(""), null);
    assert.equal(protocol.settingsFromQuery("?utm_source=x"), null);
    const read = protocol.settingsFromQuery("?outputPeriodUs=1000&backlogPeriods=12&invertX=2&fullScaleCountsPerSecond=");
    assert.equal(read.settings.outputPeriodUs, defaults.outputPeriodUs);
    assert.equal(read.settings.backlogPeriods, 12);
    assert.equal(read.settings.invertX, defaults.invertX);
    assert.equal(read.settings.fullScaleCountsPerSecond, defaults.fullScaleCountsPerSecond);
    assert.equal(read.errors.length, 3);
});
