// Config-channel client for a mouse2xac board over WebHID.
//
// Every exchange is a SET_FEATURE followed by a GET_FEATURE, so exchanges are
// queued: a second one must not slip between the two halves of the first.
//
// Classic script so index.html works from file://; CommonJS module in Node.
(function (root) {
    "use strict";

    var protocol = typeof module === "object" && module.exports ? require("./protocol.js") : root.mouse2xacProtocol;
    var COMMANDS = protocol.COMMANDS;
    var REGISTERS = protocol.REGISTERS;

    function ConfigError(message, detail) {
        var error = new Error(message);
        error.name = "ConfigError";
        error.detail = detail || null;
        return error;
    }

    function delay(ms) {
        return new Promise(function (resolve) { setTimeout(resolve, ms); });
    }

    // device: an opened HIDDevice, or anything with the same sendFeatureReport
    // and receiveFeatureReport methods.
    function ConfigClient(device) {
        this.device = device;
        this.queue = Promise.resolve();
    }

    ConfigClient.prototype.exchange = function (command, value) {
        var device = this.device;
        var run = function () {
            return device.sendFeatureReport(0, protocol.encodeCommand(command, value || 0))
                .then(function () { return device.receiveFeatureReport(0); })
                .then(function (view) { return protocol.decodeResponse(view); });
        };
        var result = this.queue.then(run, run);
        this.queue = result.catch(function () {});
        return result;
    };

    // Run a command and require the answer to come from `expected`.
    ConfigClient.prototype.expect = function (command, value, expected) {
        return this.exchange(command, value).then(function (answer) {
            if (answer.register === REGISTERS.error) {
                var error = protocol.decodeError(answer.value);
                throw ConfigError("device rejected command 0x" + command.toString(16) + ": " + error.message, error);
            }
            if (answer.register !== expected) {
                throw ConfigError("expected register 0x" + expected.toString(16) + ", got 0x" +
                    answer.register.toString(16));
            }
            return answer.value;
        });
    };

    ConfigClient.prototype.read = function (register) {
        return this.expect(COMMANDS.select, register, register);
    };

    ConfigClient.prototype.hello = function () {
        var self = this;
        return self.read(REGISTERS.protocolVersion).then(function (protocolVersion) {
            if (protocolVersion !== protocol.PROTOCOL_VERSION) {
                throw ConfigError("device speaks config protocol " + protocolVersion + ", the tuner speaks " +
                    protocol.PROTOCOL_VERSION);
            }
            return self.read(REGISTERS.firmwareVersion).then(function (firmware) {
                return { protocolVersion: protocolVersion, firmwareVersion: firmware,
                    firmware: protocol.formatVersion(firmware) };
            });
        });
    };

    ConfigClient.prototype.readSettings = function () {
        var self = this;
        var out = {};
        return protocol.SETTINGS.reduce(function (chain, spec) {
            return chain.then(function () {
                return self.read(spec.id).then(function (raw) { out[spec.key] = protocol.fromRaw(spec, raw); });
            });
        }, Promise.resolve()).then(function () { return out; });
    };

    // Write one setting; resolves with the value the device now holds.
    ConfigClient.prototype.write = function (key, value) {
        var spec = protocol.settingByKey(key);
        if (!spec) return Promise.reject(ConfigError("unknown setting " + key));
        var problem = protocol.validateSetting(key, value);
        if (problem) return Promise.reject(ConfigError(problem));
        return this.expect(COMMANDS.write | spec.id, protocol.toRaw(spec, value), spec.id).then(function (raw) {
            return protocol.fromRaw(spec, raw);
        });
    };

    // Write every setting in `settings` that the protocol knows.
    ConfigClient.prototype.writeSettings = function (settings) {
        var self = this;
        var errors = protocol.validateSettings(settings);
        if (errors.length) return Promise.reject(ConfigError(errors.join("; ")));
        return protocol.SETTINGS.reduce(function (chain, spec) {
            return chain.then(function () { return self.write(spec.key, settings[spec.key]); });
        }, Promise.resolve()).then(function () { return self.readSettings(); });
    };

    ConfigClient.prototype.readCounters = function () {
        var self = this;
        var out = {};
        var names = ["mouseReports", "miceConnected", "backlogClipped", "flashSaves", "status"];
        return names.reduce(function (chain, name) {
            return chain.then(function () {
                return self.read(REGISTERS[name]).then(function (value) { out[name] = value; });
            });
        }, Promise.resolve()).then(function () {
            out.statusFlags = protocol.decodeStatus(out.status);
            return out;
        });
    };

    ConfigClient.prototype.status = function () {
        return this.read(REGISTERS.status).then(protocol.decodeStatus);
    };

    // Ask for a flash save and wait for the firmware's output loop to finish it.
    ConfigClient.prototype.save = function (timeoutMs) {
        var self = this;
        var deadline = Date.now() + (timeoutMs || 3000);
        return self.expect(COMMANDS.save, 0, REGISTERS.status).then(function poll() {
            return self.status().then(function (status) {
                if (!status.savePending) {
                    if (status.saveFailed) throw ConfigError("flash save failed");
                    return status;
                }
                if (Date.now() > deadline) throw ConfigError("flash save did not finish");
                return delay(25).then(poll);
            });
        });
    };

    ConfigClient.prototype.restoreDefaults = function () {
        var self = this;
        return self.expect(COMMANDS.restoreDefaults, 0, REGISTERS.status).then(function () { return self.readSettings(); });
    };

    ConfigClient.prototype.reload = function () {
        var self = this;
        return self.expect(COMMANDS.reload, 0, REGISTERS.status).then(function () { return self.readSettings(); });
    };

    ConfigClient.prototype.clearCounters = function () {
        return this.expect(COMMANDS.clearCounters, 0, REGISTERS.mouseReports);
    };

    var api = { ConfigClient: ConfigClient };
    if (typeof module === "object" && module.exports) module.exports = api;
    else root.mouse2xacDevice = api;
}(this));
