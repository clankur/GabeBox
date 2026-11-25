// DOM wiring for the simulator page. The encoder, protocol, simulator and WebHID client
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
    var pct = ui.pct;
    var encoderApi = window.mouse2xacEncoder;
    var Simulator = window.mouse2xacSim.Simulator;
    var STICK_REACH = encoderApi.STICK_REACH;

    // --- Simulator -----------------------------------------------------------

    // A link from the device page carries the board's settings in the query.
    var linked = protocol.settingsFromQuery(location.search);
    var simForm = settingsForm($("sim-settings"), "sim");
    var simSettings = linked ? linked.settings : protocol.defaultSettings();
    simForm.set(simSettings);
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
        $("open-device").href = "./?" + protocol.settingsToQuery(simSettings);
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
    // Linked settings keep their full scale; otherwise it follows CPI x hand speed.
    if (linked) speedFromFullScale();
    else fullScaleFromSpeed();
    applySim();
    if (linked) {
        $("linked-status").textContent = linked.errors.length
            ? "Some settings in the link were invalid and use defaults: " + linked.errors.join("; ") + "."
            : "Using the settings from the device page.";
    }
    setInputStatus();
    requestAnimationFrame(frame);
}());
