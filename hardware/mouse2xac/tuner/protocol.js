// mouse2xac config protocol and joystick report helpers.
//
// Mirrors the "Settings" and "Config protocol" sections of ../mouse2xac.ino.
// tests/mouse2xac-tuner.test.js checks the numbers below against the sketch.
//
// Loaded as a classic script so index.html also works from a file:// URL, where
// Chrome refuses to load ES modules. In Node it is a CommonJS module.
(function (root) {
    "use strict";

    var ADAFRUIT_VENDOR_ID = 0x239A;
    var PROTOCOL_VERSION = 1;
    var REPORT_SIZE = 4;
    var VALUE_MASK = 0xFFFFFF;

    // id doubles as the register number. Flags are stored as 0 or 1.
    var SETTINGS = [
        { id: 0x01, key: "outputPeriodUs", label: "Output period", unit: "µs", initial: 8000, min: 2000, max: 100000 },
        { id: 0x02, key: "fullScaleCountsPerSecond", label: "Full scale", unit: "counts/s", initial: 8000, min: 1, max: 2000000 },
        { id: 0x03, key: "backlogPeriods", label: "Backlog", unit: "periods", initial: 6, min: 1, max: 64 },
        { id: 0x04, key: "invertX", label: "Invert X", flag: true, initial: 0, min: 0, max: 1 },
        { id: 0x05, key: "invertY", label: "Invert Y", flag: true, initial: 0, min: 0, max: 1 },
        { id: 0x06, key: "wheelEnabled", label: "Wheel buttons", flag: true, initial: 1, min: 0, max: 1 }
    ];

    var COMMANDS = {
        select: 0x00,
        save: 0x01,
        restoreDefaults: 0x02,
        reload: 0x03,
        clearCounters: 0x04,
        write: 0x80
    };

    var REGISTERS = {
        firmwareVersion: 0x10,
        protocolVersion: 0x11,
        mouseReports: 0x12,
        miceConnected: 0x13,
        backlogClipped: 0x14,
        status: 0x15,
        flashSaves: 0x16,
        error: 0xFF
    };

    var STATUS = { flashValid: 0x01, savePending: 0x02, unsaved: 0x04, saveFailed: 0x08 };

    var ERRORS = { unknownCommand: 1, unknownRegister: 2, outOfRange: 3 };
    var ERROR_MESSAGES = { 1: "unknown command", 2: "unknown register", 3: "value out of range" };

    // The joystick filter the tuner asks WebHID for: Adafruit's vendor ID and the
    // Generic Desktop / Joystick top-level collection.
    var HID_FILTER = { vendorId: ADAFRUIT_VENDOR_ID, usagePage: 0x01, usage: 0x04 };

    function settingById(id) {
        for (var i = 0; i < SETTINGS.length; i++) {
            if (SETTINGS[i].id === id) return SETTINGS[i];
        }
        return null;
    }

    function settingByKey(key) {
        for (var i = 0; i < SETTINGS.length; i++) {
            if (SETTINGS[i].key === key) return SETTINGS[i];
        }
        return null;
    }

    // Settings as the encoder takes them: numbers, with flags as booleans.
    function defaultSettings() {
        var out = {};
        SETTINGS.forEach(function (spec) {
            out[spec.key] = spec.flag ? spec.initial !== 0 : spec.initial;
        });
        return out;
    }

    function toRaw(spec, value) {
        if (spec.flag) return value ? 1 : 0;
        return value;
    }

    function fromRaw(spec, raw) {
        return spec.flag ? raw !== 0 : raw;
    }

    // Returns null when the value is acceptable, or a message.
    function validateSetting(key, value) {
        var spec = settingByKey(key);
        if (!spec) return "unknown setting " + key;
        var raw = toRaw(spec, value);
        if (typeof raw !== "number" || !Number.isInteger(raw)) return spec.label + " must be a whole number";
        if (raw < spec.min || raw > spec.max) {
            return spec.label + " must be " + spec.min + "–" + spec.max + (spec.unit ? " " + spec.unit : "");
        }
        return null;
    }

    function validateSettings(settings) {
        var errors = [];
        SETTINGS.forEach(function (spec) {
            var message = validateSetting(spec.key, settings[spec.key]);
            if (message) errors.push(message);
        });
        return errors;
    }

    function encodeCommand(command, value) {
        if (value === undefined) value = 0;
        if (!Number.isInteger(command) || command < 0 || command > 0xFF) throw new RangeError("command must be a byte");
        if (!Number.isInteger(value) || value < 0 || value > VALUE_MASK) throw new RangeError("value must fit in 24 bits");
        return new Uint8Array([command, value & 0xFF, (value >>> 8) & 0xFF, (value >>> 16) & 0xFF]);
    }

    function writeCommand(key, value) {
        var spec = settingByKey(key);
        if (!spec) throw new RangeError("unknown setting " + key);
        return encodeCommand(COMMANDS.write | spec.id, toRaw(spec, value));
    }

    function bytesOf(data) {
        if (data instanceof Uint8Array) return data;
        if (typeof ArrayBuffer !== "undefined" && ArrayBuffer.isView(data)) {
            return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
        }
        if (data instanceof ArrayBuffer) return new Uint8Array(data);
        if (Array.isArray(data)) return Uint8Array.from(data);
        throw new TypeError("expected bytes");
    }

    // GET_FEATURE answer. Some WebHID backends return the unnumbered report's
    // zero report ID as a leading byte, so a 5-byte answer drops it.
    function decodeResponse(data) {
        var bytes = bytesOf(data);
        if (bytes.length === REPORT_SIZE + 1) bytes = bytes.subarray(1);
        if (bytes.length < REPORT_SIZE) throw new RangeError("config report is " + bytes.length + " bytes, expected 4");
        return { register: bytes[0], value: bytes[1] | (bytes[2] << 8) | (bytes[3] << 16) };
    }

    function decodeError(value) {
        var code = value & 0xFF;
        return {
            code: code,
            command: (value >>> 8) & 0xFF,
            valueLow: (value >>> 16) & 0xFF,
            message: ERROR_MESSAGES[code] || "error " + code
        };
    }

    function decodeStatus(value) {
        return {
            flashValid: (value & STATUS.flashValid) !== 0,
            savePending: (value & STATUS.savePending) !== 0,
            unsaved: (value & STATUS.unsaved) !== 0,
            saveFailed: (value & STATUS.saveFailed) !== 0
        };
    }

    function formatVersion(value) {
        return ((value >>> 16) & 0xFF) + "." + ((value >>> 8) & 0xFF) + "." + (value & 0xFF);
    }

    // The 7-byte joystick input report (flight_stick_tinyusb layout, no report ID).
    function decodeJoystickReport(data) {
        var bytes = bytesOf(data);
        if (bytes.length < 7) throw new RangeError("joystick report is " + bytes.length + " bytes, expected 7");
        var low = (bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24)) >>> 0;
        return {
            x: low & 0x3FF,
            y: (low >>> 10) & 0x3FF,
            hat: (low >>> 20) & 0xF,
            twist: (low >>> 24) & 0xFF,
            buttonsA: bytes[4],
            slider: bytes[5],
            buttonsB: bytes[6] & 0x0F
        };
    }

    function encodeJoystickReport(report) {
        var low = ((report.x & 0x3FF) | ((report.y & 0x3FF) << 10) | ((report.hat & 0xF) << 20) |
            ((report.twist & 0xFF) << 24)) >>> 0;
        return new Uint8Array([low & 0xFF, (low >>> 8) & 0xFF, (low >>> 16) & 0xFF, (low >>> 24) & 0xFF,
            report.buttonsA & 0xFF, report.slider & 0xFF, report.buttonsB & 0x0F]);
    }

    var api = {
        ADAFRUIT_VENDOR_ID: ADAFRUIT_VENDOR_ID,
        PROTOCOL_VERSION: PROTOCOL_VERSION,
        REPORT_SIZE: REPORT_SIZE,
        VALUE_MASK: VALUE_MASK,
        HID_FILTER: HID_FILTER,
        SETTINGS: SETTINGS,
        COMMANDS: COMMANDS,
        REGISTERS: REGISTERS,
        STATUS: STATUS,
        ERRORS: ERRORS,
        settingById: settingById,
        settingByKey: settingByKey,
        defaultSettings: defaultSettings,
        toRaw: toRaw,
        fromRaw: fromRaw,
        validateSetting: validateSetting,
        validateSettings: validateSettings,
        encodeCommand: encodeCommand,
        writeCommand: writeCommand,
        decodeResponse: decodeResponse,
        decodeError: decodeError,
        decodeStatus: decodeStatus,
        formatVersion: formatVersion,
        decodeJoystickReport: decodeJoystickReport,
        encodeJoystickReport: encodeJoystickReport
    };
    if (typeof module === "object" && module.exports) module.exports = api;
    else root.mouse2xacProtocol = api;
}(this));
