// Page helpers shared by the device page (index.html) and the simulator page
// (simulator.html): settings forms, stick meters and stat lists.
(function () {
    "use strict";

    var protocol = window.mouse2xacProtocol;
    var STICK_REACH = window.mouse2xacEncoder.STICK_REACH;

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

    window.mouse2xacUi = {
        $: $,
        fmt: fmt,
        pct: pct,
        settingsForm: settingsForm,
        meters: meters,
        statsList: statsList
    };
}());
