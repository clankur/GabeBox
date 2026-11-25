// DOM wiring for the mouse2xac tuner. The encoder, protocol, simulator and
// WebHID client live in their own scripts and have Node tests; this file only
// connects them to the page.
(function () {
    "use strict";

    var protocol = window.mouse2xacProtocol;
    var encoderApi = window.mouse2xacEncoder;
    var Simulator = window.mouse2xacSim.Simulator;
    var ConfigClient = window.mouse2xacDevice.ConfigClient;
    var STICK_REACH = encoderApi.STICK_REACH;

    function $(id) { return document.getElementById(id); }

    function fmt(value, digits) {
        if (value === null || value === undefined || !isFinite(value)) return "—";
        return Number(value).toLocaleString(undefined, { maximumFractionDigits: digits === undefined ? 0 : digits });
    }

    function pct(part, whole) {
        return whole > 0 ? fmt(100 * part / whole, 1) + "%" : "—";
    }

    // --- Settings forms ------------------------------------------------------

    // Builds labelled inputs for the six firmware settings inside a fieldset.
    function settingsForm(fieldset, prefix) {
        var inputs = {};
        protocol.SETTINGS.forEach(function (spec) {
            var id = prefix + "-" + spec.key;
            var input = document.createElement("input");
            input.id = id;
            input.name = spec.key;
            if (spec.flag) {
                input.type = "checkbox";
                var check = document.createElement("label");
                check.className = "check";
                check.appendChild(input);
                check.appendChild(document.createTextNode(" " + spec.label));
                fieldset.appendChild(check);
            } else {
                input.type = "number";
                input.min = spec.min;
                input.max = spec.max;
                input.step = 1;
                input.inputMode = "numeric";
                var field = document.createElement("div");
                field.className = "field";
                var label = document.createElement("label");
                label.htmlFor = id;
                label.textContent = spec.label + ", " + spec.unit;
                var help = document.createElement("small");
                help.id = id + "-help";
                help.textContent = spec.min + "–" + spec.max;
                input.setAttribute("aria-describedby", help.id);
                field.appendChild(label);
                field.appendChild(input);
                field.appendChild(help);
                fieldset.appendChild(field);
            }
            inputs[spec.key] = input;
        });
        return {
            inputs: inputs,
            set: function (settings) {
                protocol.SETTINGS.forEach(function (spec) {
                    var input = inputs[spec.key];
                    if (spec.flag) input.checked = !!settings[spec.key];
                    else input.value = settings[spec.key];
                    input.removeAttribute("aria-invalid");
                });
            },
            // Returns {settings, errors}; marks invalid inputs.
            read: function () {
                var settings = {};
                var errors = [];
                protocol.SETTINGS.forEach(function (spec) {
                    var input = inputs[spec.key];
                    var value = spec.flag ? input.checked : (input.value === "" ? NaN : Number(input.value));
                    settings[spec.key] = value;
                    var problem = protocol.validateSetting(spec.key, value);
                    if (problem) errors.push(problem);
                    if (problem) input.setAttribute("aria-invalid", "true");
                    else input.removeAttribute("aria-invalid");
                });
                return { settings: settings, errors: errors };
            }
        };
    }

    // --- Meters --------------------------------------------------------------

    function meters(container, withBacklog) {
        container.innerHTML = "";
        var box = document.createElement("div");
        box.className = "stickbox";
        box.setAttribute("role", "img");
        var dot = document.createElement("div");
        dot.className = "dot";
        box.appendChild(dot);
        container.appendChild(box);

        var bars = document.createElement("div");
        bars.className = "bars";
        function bar(name, centered) {
            var row = document.createElement("div");
            row.className = "meter";
            var label = document.createElement("span");
            label.className = "name";
            label.textContent = name;
            var track = document.createElement("div");
            track.className = "track" + (centered ? " centered" : "");
            track.setAttribute("role", "meter");
            track.setAttribute("aria-label", name);
            var fill = document.createElement("div");
            fill.className = "fill";
            track.appendChild(fill);
            var value = document.createElement("span");
            value.className = "value";
            row.appendChild(label);
            row.appendChild(track);
            row.appendChild(value);
            bars.appendChild(row);
            return { track: track, fill: fill, value: value };
        }
        var x = bar("Stick X", true);
        var y = bar("Stick Y", true);
        var backlog = withBacklog ? bar("Backlog", false) : null;
        var side = document.createElement("div");
        side.className = "meter-side";
        side.appendChild(bars);
        container.appendChild(side);

        var chips = document.createElement("div");
        chips.className = "chips";
        chips.setAttribute("aria-label", "Joystick buttons 1 to 8");
        var names = ["L", "R", "M", "Back", "Fwd", "Wh↑", "Wh↓", "8"];
        var chipEls = names.map(function (name, i) {
            var chip = document.createElement("span");
            chip.className = "chip";
            chip.textContent = name;
            chip.title = "Button " + (i + 1);
            chips.appendChild(chip);
            return chip;
        });
        side.appendChild(chips);

        function setCentered(b, raw) {
            var units = raw - 512;
            var frac = Math.max(-1, Math.min(1, units / STICK_REACH));
            b.fill.style.left = (frac < 0 ? 50 + frac * 50 : 50) + "%";
            b.fill.style.width = Math.abs(frac) * 50 + "%";
            b.value.textContent = (units > 0 ? "+" : "") + units;
            b.track.setAttribute("aria-valuenow", String(units));
            b.track.setAttribute("aria-valuemin", "-511");
            b.track.setAttribute("aria-valuemax", "511");
        }

        return {
            update: function (state) {
                setCentered(x, state.x);
                setCentered(y, state.y);
                dot.style.left = (state.x / 1023 * 100) + "%";
                dot.style.top = (state.y / 1023 * 100) + "%";
                box.setAttribute("aria-label", "Stick at " + (state.x - 512) + ", " + (state.y - 512));
                if (backlog) {
                    var fill = state.backlog || 0;
                    backlog.fill.style.left = "0";
                    backlog.fill.style.width = fill * 100 + "%";
                    backlog.value.textContent = fmt(fill * 100) + "%";
                    backlog.track.setAttribute("aria-valuenow", fmt(fill * 100));
                }
                chipEls.forEach(function (chip, i) {
                    chip.classList.toggle("on", (state.buttons & (1 << i)) !== 0);
                });
            }
        };
    }

    function statsList(dl, rows) {
        dl.innerHTML = "";
        var out = {};
        rows.forEach(function (row) {
            var wrap = document.createElement("div");
            var dt = document.createElement("dt");
            dt.textContent = row[1];
            var dd = document.createElement("dd");
            dd.textContent = "—";
            wrap.appendChild(dt);
            wrap.appendChild(dd);
            dl.appendChild(wrap);
            out[row[0]] = dd;
        });
        return out;
    }

    // --- Simulator -----------------------------------------------------------

    var simForm = settingsForm($("sim-settings"), "sim");
    simForm.set(protocol.defaultSettings());
    var simSettings = protocol.defaultSettings();
    var simMeters = meters($("sim-meters"), true);
    var simStats = statsList($("sim-stats"), [
        ["path", "Mouse path"],
        ["inFlight", "In flight (delayed)"],
        ["backlog", "Dropped by backlog cap"],
        ["boot", "Lost to boot report limit"],
        ["saturated", "Periods at full deflection"],
        ["skipped", "Stick values never sampled"],
        ["lag", "Best-fit delay"],
        ["gesture", "Last gesture landed off by"]
    ]);

    var inputs = {
        cpi: $("cpi"),
        handSpeed: $("hand-speed"),
        reportRate: $("report-rate"),
        inputScale: $("input-scale"),
        clampBoot: $("clamp-boot"),
        consoleInterval: $("console-interval"),
        consoleJitter: $("console-jitter")
    };

    function num(input, min, max) {
        var value = Number(input.value);
        var ok = input.value !== "" && isFinite(value) && value >= min && value <= max;
        if (ok) input.removeAttribute("aria-invalid");
        else input.setAttribute("aria-invalid", "true");
        return ok ? value : null;
    }

    function simOptions() {
        return {
            cpi: num(inputs.cpi, 1, 100000),
            reportRateHz: Number(inputs.reportRate.value),
            inputScale: num(inputs.inputScale, 0.01, 100),
            clampBootReport: inputs.clampBoot.checked,
            consoleIntervalMs: num(inputs.consoleInterval, 1, 100),
            consoleJitterMs: num(inputs.consoleJitter, 0, 50)
        };
    }

    var sim = new Simulator({ settings: simSettings });

    function applySim() {
        var read = simForm.read();
        var options = simOptions();
        var errors = read.errors.slice();
        ["cpi", "inputScale", "consoleIntervalMs", "consoleJitterMs"].forEach(function (key) {
            if (options[key] === null) errors.push("Check " + key.replace(/[A-Z]/g, function (c) { return " " + c.toLowerCase(); }));
        });
        $("sim-errors").textContent = errors.join(". ");
        if (!read.errors.length) simSettings = read.settings;
        var next = { settings: simSettings, reportRateHz: options.reportRateHz, clampBootReport: options.clampBootReport };
        if (options.inputScale !== null) next.inputScale = options.inputScale;
        if (options.consoleIntervalMs !== null) next.consoleIntervalMs = options.consoleIntervalMs;
        if (options.consoleJitterMs !== null) next.consoleJitterMs = options.consoleJitterMs;
        sim.setOptions(next);
        renderContract();
    }

    // Full scale = CPI x hand speed. Editing either side keeps the other in step.
    function fullScaleFromSpeed() {
        var cpi = num(inputs.cpi, 1, 100000);
        var speed = num(inputs.handSpeed, 0.01, 1000);
        if (cpi === null || speed === null) return;
        simForm.inputs.fullScaleCountsPerSecond.value = Math.round(cpi * speed);
    }

    function speedFromFullScale() {
        var cpi = num(inputs.cpi, 1, 100000);
        var fullScale = Number(simForm.inputs.fullScaleCountsPerSecond.value);
        if (cpi === null || !(fullScale > 0)) return;
        inputs.handSpeed.value = Math.round(fullScale / cpi * 100) / 100;
        inputs.handSpeed.removeAttribute("aria-invalid");
    }

    function markPreset() {
        document.querySelectorAll("[data-cpi]").forEach(function (button) {
            button.setAttribute("aria-pressed", String(button.dataset.cpi === inputs.cpi.value));
        });
    }

    inputs.cpi.addEventListener("input", function () { fullScaleFromSpeed(); markPreset(); applySim(); });
    inputs.handSpeed.addEventListener("input", function () { fullScaleFromSpeed(); applySim(); });
    simForm.inputs.fullScaleCountsPerSecond.addEventListener("input", speedFromFullScale);
    document.querySelectorAll("[data-cpi]").forEach(function (button) {
        button.addEventListener("click", function () {
            inputs.cpi.value = button.dataset.cpi;
            fullScaleFromSpeed();
            markPreset();
            applySim();
        });
    });
    $("sim-form").addEventListener("input", applySim);
    $("sim-form").addEventListener("change", applySim);
    $("sim-form").addEventListener("submit", function (event) { event.preventDefault(); });

    function renderContract() {
        var options = simOptions();
        var cpi = options.cpi || 0;
        var c = encoderApi.contract(simSettings, { cpi: cpi, reportRateHz: options.reportRateHz });
        var inches = function (countsPerSecond) {
            return cpi ? " (" + fmt(countsPerSecond / cpi, 2) + " in/s)" : "";
        };
        var rows = [
            ["Counts per stick unit per period", fmt(c.countsPerUnit, 4) + " (" + fmt(c.unitsPerCount, 2) + " units per count)"],
            ["Full deflection", fmt(c.fullScaleCountsPerSecond) + " counts/s" + inches(c.fullScaleCountsPerSecond)],
            ["Smallest nonzero speed", fmt(c.smallestSpeedCountsPerSecond, 1) + " counts/s" + inches(c.smallestSpeedCountsPerSecond)],
            ["Full deflection per period", fmt(c.fullDeflectionCountsPerPeriod, 1) + " counts in " + fmt(c.periodMs, 2) + " ms"],
            ["Backlog", fmt(simSettings.backlogPeriods) + " periods = " + fmt(c.backlogMs, 1) + " ms, " +
                fmt(c.backlogCounts, 1) + " counts; drains in " + fmt(c.drainMs, 1) + " ms"],
            ["Boot report limit at " + fmt(options.reportRateHz) + " Hz", fmt(c.bootLimitCountsPerSecond) + " counts/s" +
                inches(c.bootLimitCountsPerSecond)]
        ];
        var body = $("contract").tBodies[0];
        body.innerHTML = "";
        rows.forEach(function (row) {
            var tr = document.createElement("tr");
            var th = document.createElement("th");
            th.scope = "row";
            th.textContent = row[0];
            var td = document.createElement("td");
            td.textContent = row[1];
            tr.appendChild(th);
            tr.appendChild(td);
            body.appendChild(tr);
        });

        var warnings = [];
        var longestGap = options.consoleIntervalMs + (options.consoleJitterMs || 0);
        if (options.consoleIntervalMs !== null && longestGap > c.periodMs) {
            warnings.push("Console samples can be up to " + fmt(longestGap, 2) + " ms apart, longer than the " +
                fmt(c.periodMs, 2) + " ms output period, so some stick values may never be read.");
        }
        if (c.unitsPerCount > 1) {
            warnings.push("One mouse count moves the stick " + fmt(c.unitsPerCount, 1) +
                " units per period, so slow motion is coarse. Raise the full scale or shorten the period.");
        }
        if (c.bootLimitCountsPerSecond < c.fullScaleCountsPerSecond) {
            warnings.push("A boot report at this rate cannot carry full-scale speed; the stick never reaches full deflection.");
        }
        var list = $("contract-warnings");
        list.innerHTML = "";
        warnings.forEach(function (text) {
            var li = document.createElement("li");
            li.textContent = text;
            list.appendChild(li);
        });
    }

    // Pointer capture --------------------------------------------------------

    var canvas = $("sim-canvas");
    var captured = false;
    var rawInput = null;
    var eventSource = "onpointerrawupdate" in window ? "pointerrawupdate" : "pointermove";
    var inputEvents = 0;
    var inputRate = 0;
    var wheelAccum = 0;

    function setInputStatus() {
        var text;
        if (!captured) {
            text = "Mouse not captured. Click the view to capture it.";
        } else {
            text = "Captured via " + eventSource + ". " +
                (rawInput === true ? "Raw counts (unadjustedMovement)." :
                    rawInput === false ? "Accelerated movement: this browser or OS refused unadjustedMovement, so counts include pointer acceleration." :
                        "Raw counts unknown.") +
                " Input events: " + fmt(inputRate) + "/s.";
        }
        $("input-status").textContent = text;
        $("capture-hint").hidden = captured;
    }

    function lock() {
        if (!canvas.requestPointerLock) {
            $("input-status").textContent = "Pointer Lock is not available in this browser.";
            return;
        }
        // Older browsers return nothing instead of a promise, so whether they
        // honored unadjustedMovement is unknown.
        var promised = true;
        var attempt = function (options) {
            try {
                var result = options ? canvas.requestPointerLock(options) : canvas.requestPointerLock();
                if (result && result.then) return result;
                promised = false;
                return Promise.resolve();
            } catch (error) {
                return Promise.reject(error);
            }
        };
        attempt({ unadjustedMovement: true }).then(function () {
            rawInput = promised ? true : null;
            setInputStatus();
        }, function () {
            return attempt(null).then(function () {
                rawInput = false;
                setInputStatus();
            }, function (error) {
                $("input-status").textContent = "Could not capture the mouse: " + (error && error.message || error);
            });
        });
    }

    canvas.addEventListener("click", function () { if (!captured) lock(); });
    canvas.addEventListener("keydown", function (event) {
        if (event.key === "Enter" || event.key === " ") {
            event.preventDefault();
            lock();
        }
    });
    document.addEventListener("pointerlockchange", function () {
        captured = document.pointerLockElement === canvas;
        if (!captured) sim.buttons(performance.now(), 0);
        setInputStatus();
    });
    document.addEventListener("pointerlockerror", function () {
        $("input-status").textContent = "The browser refused Pointer Lock. Click the view again.";
    });

    function onMove(event) {
        if (!captured) return;
        var events = event.type === "pointermove" && event.getCoalescedEvents ? event.getCoalescedEvents() : [];
        if (!events.length) events = [event];
        events.forEach(function (e) {
            inputEvents++;
            sim.move(e.timeStamp, e.movementX || 0, e.movementY || 0);
        });
    }
    canvas.addEventListener(eventSource, onMove);

    canvas.addEventListener("mousedown", function (event) {
        if (!captured) return;
        event.preventDefault();
        sim.buttons(event.timeStamp, event.buttons & 0x1F);
    });
    canvas.addEventListener("mouseup", function (event) {
        if (!captured) return;
        // Stops back and forward buttons from navigating.
        event.preventDefault();
        sim.buttons(event.timeStamp, event.buttons & 0x1F);
    });
    canvas.addEventListener("auxclick", function (event) { if (captured) event.preventDefault(); });
    canvas.addEventListener("contextmenu", function (event) { event.preventDefault(); });
    canvas.addEventListener("wheel", function (event) {
        if (!captured) return;
        event.preventDefault();
        // One detent is about 100 px or 3 lines in Chrome; positive deltaY scrolls
        // down, and a HID wheel counts up as positive.
        var step = event.deltaMode === 1 ? 3 : event.deltaMode === 2 ? 1 : 100;
        wheelAccum += event.deltaY / step;
        var detents = Math.trunc(wheelAccum);
        if (detents !== 0) {
            wheelAccum -= detents;
            sim.wheel(event.timeStamp, -detents);
        }
    }, { passive: false });

    $("sim-align").addEventListener("click", function () { sim.align(); });
    $("sim-reset").addEventListener("click", function () { sim.resetStats(); sim.align(); });
    $("zoom").addEventListener("input", function () { draw(); });

    // Drawing ---------------------------------------------------------------

    var ctx = canvas.getContext("2d");
    var colors = {};
    var camera = null;

    function readColors() {
        var style = getComputedStyle(document.documentElement);
        ["ref", "decoded", "grid", "muted", "canvas"].forEach(function (name) {
            colors[name] = style.getPropertyValue("--" + name).trim();
        });
    }
    readColors();
    if (window.matchMedia) {
        var scheme = window.matchMedia("(prefers-color-scheme: dark)");
        if (scheme.addEventListener) scheme.addEventListener("change", readColors);
    }

    function resizeCanvas() {
        var ratio = window.devicePixelRatio || 1;
        var rect = canvas.getBoundingClientRect();
        var w = Math.max(1, Math.round(rect.width * ratio));
        var h = Math.max(1, Math.round(rect.height * ratio));
        if (canvas.width !== w || canvas.height !== h) {
            canvas.width = w;
            canvas.height = h;
        }
        return ratio;
    }

    function niceStep(raw) {
        var power = Math.pow(10, Math.floor(Math.log10(raw)));
        var steps = [1, 2, 5, 10];
        for (var i = 0; i < steps.length; i++) {
            if (steps[i] * power >= raw) return steps[i] * power;
        }
        return 10 * power;
    }

    function draw() {
        var ratio = resizeCanvas();
        var w = canvas.width;
        var h = canvas.height;
        ctx.fillStyle = colors.canvas;
        ctx.fillRect(0, 0, w, h);

        // The view spans about half a second of full-scale motion, zoomable.
        var zoom = Math.pow(2, Number($("zoom").value));
        var spanCounts = Math.max(20, simSettings.fullScaleCountsPerSecond * 0.5) / zoom;
        var scale = w / spanCounts;
        var head = sim.ref;
        if (!camera) camera = { x: head.x, y: head.y };
        camera.x += (head.x - camera.x) * 0.2;
        camera.y += (head.y - camera.y) * 0.2;
        var toX = function (x) { return w / 2 + (x - camera.x) * scale; };
        var toY = function (y) { return h / 2 + (y - camera.y) * scale; };

        var gridCounts = niceStep(60 * ratio / scale);
        ctx.strokeStyle = colors.grid;
        ctx.lineWidth = 1;
        ctx.beginPath();
        var x0 = Math.floor((camera.x - w / 2 / scale) / gridCounts) * gridCounts;
        for (var gx = x0; toX(gx) <= w; gx += gridCounts) {
            ctx.moveTo(Math.round(toX(gx)) + 0.5, 0);
            ctx.lineTo(Math.round(toX(gx)) + 0.5, h);
        }
        var y0 = Math.floor((camera.y - h / 2 / scale) / gridCounts) * gridCounts;
        for (var gy = y0; toY(gy) <= h; gy += gridCounts) {
            ctx.moveTo(0, Math.round(toY(gy)) + 0.5);
            ctx.lineTo(w, Math.round(toY(gy)) + 0.5);
        }
        ctx.stroke();
        $("scale-label").textContent = "Grid: " + fmt(gridCounts) + " counts";

        function path(history, color, last) {
            ctx.strokeStyle = color;
            ctx.lineWidth = 2 * ratio;
            ctx.beginPath();
            history.forEach(function (p, i) {
                if (i === 0) ctx.moveTo(toX(p.x), toY(p.y));
                else ctx.lineTo(toX(p.x), toY(p.y));
            });
            ctx.lineTo(toX(last.x), toY(last.y));
            ctx.stroke();
            ctx.fillStyle = color;
            ctx.beginPath();
            ctx.arc(toX(last.x), toY(last.y), 4 * ratio, 0, Math.PI * 2);
            ctx.fill();
        }
        path(sim.refHistory, colors.ref, sim.ref);
        path(sim.decodedHistory, colors.decoded, sim.decoded);
    }

    // Frame loop ------------------------------------------------------------

    var lastFrame = performance.now();
    var lastStats = 0;
    var lastLag = 0;
    var lagValue = null;

    function renderStats(now) {
        var s = sim.stats;
        var period = sim.periodMs();
        var inFlight = sim.inFlight();
        var fullPerPeriod = STICK_REACH * sim.encoder.encoding.countsPerUnit;
        simStats.path.textContent = fmt(s.pathCounts) + " counts";
        simStats.inFlight.textContent = fmt(inFlight, 1) + " counts ≈ " + fmt(inFlight / fullPerPeriod * period, 1) + " ms";
        simStats.backlog.textContent = fmt(s.backlogClippedCounts) + " counts, " + pct(s.backlogClippedCounts, s.pathCounts);
        simStats.boot.textContent = fmt(s.bootClippedCounts) + " counts, " + pct(s.bootClippedCounts, s.pathCounts);
        simStats.saturated.textContent = pct(s.saturatedPeriods, s.movingPeriods) + " of moving periods";
        simStats.skipped.textContent = fmt(s.skippedValues) + " of " + fmt(s.periods);
        if (now - lastLag > 250) {
            lastLag = now;
            var lag = sim.estimateLag(1000, 150);
            if (lag !== null) lagValue = lag;
        }
        simStats.lag.textContent = lagValue === null ? "move the mouse" : fmt(lagValue) + " ms";
        simStats.gesture.textContent = s.lastGesture
            ? fmt(s.lastGesture.miss, 1) + " counts, " + fmt(s.lastGesture.missPercent, 1) + "% of its path"
            : "stop moving to score";
        inputRate = inputEvents * 1000 / Math.max(1, now - lastStats);
        inputEvents = 0;
        if (captured) setInputStatus();
    }

    function frame(now) {
        if (now - lastFrame > 1000) sim.rebase(now);
        lastFrame = now;
        sim.advance(now);
        simMeters.update({ x: sim.stick.x, y: sim.stick.y, buttons: sim.stick.buttonsA, backlog: sim.backlogFill() });
        draw();
        if (now - lastStats > 200) {
            renderStats(now);
            lastStats = now;
        }
        requestAnimationFrame(frame);
    }

    // For poking at the simulator from the browser console.
    window.mouse2xacTuner = { sim: sim };

    markPreset();
    fullScaleFromSpeed();
    applySim();
    setInputStatus();
    requestAnimationFrame(frame);

    // --- Device --------------------------------------------------------------

    var deviceForm = settingsForm($("device-settings"), "device");
    deviceForm.set(protocol.defaultSettings());
    var deviceMeters = meters($("device-meters"), false);
    deviceMeters.update({ x: 512, y: 512, buttons: 0 });
    var counterEls = statsList($("device-counters"), [
        ["mouseReports", "Mouse reports"],
        ["miceConnected", "Mice connected"],
        ["backlogClipped", "Counts dropped by backlog cap"],
        ["flashSaves", "Flash saves since boot"],
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
        "device-from-sim", "device-to-sim", "device-clear"];

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
        $("device-support").textContent = "WebHID is not available here. Use desktop Chrome or Edge, and open the page from a file or from http://localhost. The simulator works without it.";
        setDeviceEnabled();
    } else if (!window.isSecureContext) {
        $("device-support").textContent = "WebHID needs a secure context. Open the page from a file or from http://localhost.";
        hid = null;
        setDeviceEnabled();
    } else {
        $("device-support").textContent = "Plug the Feather into this computer, not the XAC, and press Connect. The console never sends feature reports.";
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
            deviceForm.set(settings);
            device.addEventListener("inputreport", onInputReport);
            busy = false;
            setDeviceEnabled();
            deviceStatus("Connected. Settings read from the board.");
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
            deviceForm.set(stored);
            return refreshCounters().then(function () {
                return "Written. Save to flash to keep them after a power cycle.";
            });
        });
    }

    $("device-read").addEventListener("click", function () {
        operation("Reading settings", function () {
            return client.readSettings().then(function (settings) {
                deviceForm.set(settings);
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
                deviceForm.set(settings);
                return refreshCounters().then(function () { return "Defaults restored in RAM. Save to flash to keep them."; });
            });
        });
    });
    $("device-reload").addEventListener("click", function () {
        operation("Reloading saved settings", function () {
            return client.reload().then(function (settings) {
                deviceForm.set(settings);
                return refreshCounters().then(function () { return "Unsaved changes discarded."; });
            });
        });
    });
    $("device-from-sim").addEventListener("click", function () {
        operation("Copying simulator settings", function () {
            deviceForm.set(simSettings);
            return writeSettings(simSettings);
        });
    });
    $("device-to-sim").addEventListener("click", function () {
        try {
            var settings = deviceFormSettings();
            simForm.set(settings);
            speedFromFullScale();
            applySim();
            deviceStatus("Simulator now uses the device settings.");
        } catch (error) {
            deviceError(describe(error));
        }
    });
    $("device-clear").addEventListener("click", function () {
        operation("Clearing counters", function () {
            return client.clearCounters().then(function () {
                return refreshCounters().then(function () { return "Counters cleared."; });
            });
        });
    });
}());
