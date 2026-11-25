// DOM wiring for the device page. The encoder, protocol, simulator and WebHID client
// live in their own scripts and have Node tests; this file only connects them to
// the page.
(function () {
    "use strict";

    var protocol = window.mouse2xacProtocol;
    var ui = window.mouse2xacUi;
    var $ = ui.$;
    var fmt = ui.fmt;
    var settingsForm = ui.settingsForm;
    var meters = ui.meters;
    var statsList = ui.statsList;
    var ConfigClient = window.mouse2xacDevice.ConfigClient;

    // --- Device --------------------------------------------------------------

    var deviceForm = settingsForm($("device-settings"), "device");
    // A link from the simulator carries its settings in the query. They stay in
    // the form until the user writes them or reads the board's.
    var linked = protocol.settingsFromQuery(location.search);
    deviceForm.set(linked ? linked.settings : protocol.defaultSettings());
    var deviceMeters = meters($("device-meters"), false);
    deviceMeters.update({ x: 512, y: 512, buttons: 0 });
    var counterEls = statsList($("device-counters"), [
        ["mouseReports", "Mouse reports"],
        ["miceConnected", "Mice connected"],
        ["backlogClipped", "Counts dropped by backlog cap"],
        ["flashSaves", "Saves completed since boot"],
        ["status", "Settings"],
        ["reportRate", "Joystick reports"]
    ]);

    var hid = navigator.hid;
    var device = null;
    var client = null;
    var busy = false;
    var pollTimer = null;
    var polling = false;
    var reportCount = 0;
    var reportRateStart = performance.now();
    var deviceButtons = ["device-read", "device-write", "device-save", "device-defaults", "device-reload",
        "device-clear"];

    function deviceStatus(text) { $("device-status").textContent = text; }
    function deviceError(text) { $("device-errors").textContent = text || ""; }

    function setDeviceEnabled() {
        var connected = !!client;
        deviceButtons.forEach(function (id) { $(id).disabled = !connected || busy; });
        $("device-settings").disabled = !connected;
        $("device-connect").disabled = !hid || connected || busy;
        $("device-disconnect").disabled = !connected;
    }

    if (!hid) {
        $("device-support").textContent = "WebHID is not available here. Use desktop Chrome or Edge, and open the page from a file, from http://localhost or from the hosted HTTPS page. The simulator page works without it.";
        setDeviceEnabled();
    } else if (!window.isSecureContext) {
        $("device-support").textContent = "WebHID needs a secure context. Open the page from a file, from http://localhost or over HTTPS.";
        hid = null;
        setDeviceEnabled();
    } else {
        $("device-support").textContent = "Plug the Feather into this computer, not the XAC, and press Connect.";
        setDeviceEnabled();
    }

    function describe(error) {
        return error && error.message ? error.message : String(error);
    }

    // Runs one device operation at a time and reports its outcome.
    function operation(label, work) {
        if (!client || busy) return Promise.resolve();
        busy = true;
        setDeviceEnabled();
        deviceError("");
        deviceStatus(label + "…");
        return work().then(function (message) {
            deviceStatus(message || label + " done.");
        }, function (error) {
            deviceError(label + " failed: " + describe(error));
            deviceStatus("Connected.");
        }).then(function () {
            busy = false;
            setDeviceEnabled();
        });
    }

    function showCounters(counters) {
        counterEls.mouseReports.textContent = fmt(counters.mouseReports);
        counterEls.miceConnected.textContent = fmt(counters.miceConnected);
        counterEls.backlogClipped.textContent = fmt(counters.backlogClipped);
        counterEls.flashSaves.textContent = fmt(counters.flashSaves);
        var flags = counters.statusFlags;
        var parts = [flags.unsaved ? "unsaved changes" : (flags.flashValid ? "saved in flash" : "defaults, flash empty")];
        if (flags.savePending) parts.push("saving");
        if (flags.saveFailed) parts.push("last save failed");
        counterEls.status.textContent = parts.join(", ");
    }

    function refreshCounters() {
        if (!client || polling) return Promise.resolve();
        polling = true;
        return client.readCounters().then(showCounters, function (error) {
            deviceError("Reading counters failed: " + describe(error));
        }).then(function () { polling = false; });
    }

    function onInputReport(event) {
        if (event.reportId !== 0) return;
        try {
            var report = protocol.decodeJoystickReport(event.data);
            deviceMeters.update({ x: report.x, y: report.y, buttons: report.buttonsA });
            reportCount++;
        } catch (error) {
            deviceError(describe(error));
        }
    }

    function updateReportRate() {
        var now = performance.now();
        var rate = reportCount * 1000 / Math.max(1, now - reportRateStart);
        counterEls.reportRate.textContent = fmt(rate) + "/s";
        reportCount = 0;
        reportRateStart = now;
    }

    function openDevice(candidate) {
        var opened = candidate.opened ? Promise.resolve() : candidate.open();
        busy = true;
        setDeviceEnabled();
        deviceError("");
        deviceStatus("Opening " + (candidate.productName || "device") + "…");
        return opened.then(function () {
            device = candidate;
            client = new ConfigClient(candidate);
            return client.hello();
        }).then(function (info) {
            $("device-info").textContent = (device.productName || "mouse2xac") + ", firmware " + info.firmware +
                ", protocol " + info.protocolVersion;
            return client.readSettings();
        }).then(function (settings) {
            device.addEventListener("inputreport", onInputReport);
            busy = false;
            setDeviceEnabled();
            if (linked) {
                linked = null;
                deviceStatus("Connected. The form still holds the simulator's settings: press Write to send them, or Read to load the board's.");
            } else {
                showSettings(settings);
                deviceStatus("Connected. Settings read from the board.");
            }
            pollTimer = setInterval(function () {
                updateReportRate();
                if (!busy) refreshCounters();
            }, 1000);
            return refreshCounters();
        }).catch(function (error) {
            deviceError("Could not use this device: " + describe(error) +
                ". It may run firmware without the config channel, or another program may hold it.");
            busy = false;
            closeDevice(false);
        });
    }

    function closeDevice(announce) {
        if (pollTimer) clearInterval(pollTimer);
        pollTimer = null;
        if (device) {
            device.removeEventListener("inputreport", onInputReport);
            if (device.opened) device.close().catch(function () {});
        }
        device = null;
        client = null;
        $("device-info").textContent = "";
        deviceMeters.update({ x: 512, y: 512, buttons: 0 });
        setDeviceEnabled();
        if (announce !== false) deviceStatus("No device connected.");
    }

    function isCandidate(d) {
        return d.vendorId === protocol.ADAFRUIT_VENDOR_ID && (d.collections || []).some(function (c) {
            return c.usagePage === protocol.HID_FILTER.usagePage && c.usage === protocol.HID_FILTER.usage;
        });
    }

    if (hid) {
        $("device-connect").addEventListener("click", function () {
            deviceError("");
            hid.requestDevice({ filters: [protocol.HID_FILTER] }).then(function (devices) {
                if (!devices.length) {
                    deviceStatus("No device chosen.");
                    return;
                }
                return openDevice(devices[0]);
            }, function (error) {
                deviceError("Could not ask for a device: " + describe(error));
            });
        });
        $("device-disconnect").addEventListener("click", function () { closeDevice(); });
        hid.addEventListener("disconnect", function (event) {
            if (event.device === device) {
                closeDevice(false);
                deviceStatus("The board was unplugged.");
            }
        });
        hid.addEventListener("connect", function (event) {
            if (!client && isCandidate(event.device)) openDevice(event.device);
        });
        // Reopen a board this page was already allowed to use.
        hid.getDevices().then(function (devices) {
            var known = devices.filter(isCandidate);
            if (known.length && !client) openDevice(known[0]);
        });
    }

    function deviceFormSettings() {
        var read = deviceForm.read();
        if (read.errors.length) throw new Error(read.errors.join("; "));
        return read.settings;
    }

    function writeSettings(settings) {
        return client.writeSettings(settings).then(function (stored) {
            showSettings(stored);
            return refreshCounters().then(function () {
                return "Written. Save to flash to keep them after a power cycle.";
            });
        });
    }

    $("device-read").addEventListener("click", function () {
        operation("Reading settings", function () {
            return client.readSettings().then(function (settings) {
                showSettings(settings);
                return refreshCounters().then(function () { return "Settings read from the board."; });
            });
        });
    });
    $("device-write").addEventListener("click", function () {
        operation("Writing settings", function () {
            return Promise.resolve().then(deviceFormSettings).then(writeSettings);
        });
    });
    $("device-save").addEventListener("click", function () {
        operation("Saving to flash", function () {
            return client.save().then(function () {
                return refreshCounters().then(function () { return "Saved to flash."; });
            });
        });
    });
    $("device-defaults").addEventListener("click", function () {
        operation("Restoring defaults", function () {
            return client.restoreDefaults().then(function (settings) {
                showSettings(settings);
                return refreshCounters().then(function () { return "Defaults restored in RAM. Save to flash to keep them."; });
            });
        });
    });
    $("device-reload").addEventListener("click", function () {
        operation("Reloading saved settings", function () {
            return client.reload().then(function (settings) {
                showSettings(settings);
                return refreshCounters().then(function () { return "Unsaved changes discarded."; });
            });
        });
    });
    function showSettings(settings) {
        deviceForm.set(settings);
        updateSimLink();
    }

    // Keeps the simulator link pointing at whatever the form holds.
    function updateSimLink() {
        var read = deviceForm.read();
        $("open-sim").href = "simulator.html" + (read.errors.length ? "" : "?" + protocol.settingsToQuery(read.settings));
    }
    $("device-form").addEventListener("input", updateSimLink);
    $("device-form").addEventListener("change", updateSimLink);
    $("device-form").addEventListener("submit", function (event) { event.preventDefault(); });
    updateSimLink();
    if (linked) {
        deviceStatus(linked.errors.length
            ? "Some settings in the link were invalid and use defaults: " + linked.errors.join("; ") + "."
            : "The form holds the simulator's settings. Connect, then press Write to send them to the board.");
    }
    $("device-clear").addEventListener("click", function () {
        operation("Clearing counters", function () {
            return client.clearCounters().then(function () {
                return refreshCounters().then(function () { return "Counters cleared."; });
            });
        });
    });
}());
