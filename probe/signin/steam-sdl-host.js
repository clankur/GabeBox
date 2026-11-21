(function () {
    "use strict";

    var screen = document.getElementById("sdl-game");
    var library = document.getElementById("library");
    var exit = document.getElementById("sdl-exit");
    var resume = document.getElementById("sdl-resume");
    var menu = document.getElementById("sdl-menu");
    var status = document.getElementById("sdl-status");
    var title = document.getElementById("sdl-game-title");
    var canvas = document.getElementById("sdl-frame");
    var cursor = document.getElementById("sdl-pointer");
    var context = canvas.getContext("2d");
    var gamepad = window.steamSdlGamepad;
    var padButtons = gamepad.buttons;
    var directions = {
        37: [-1, 0], 140: [-1, 0], 205: [-1, 0], 214: [-1, 0],
        38: [0, -1], 138: [0, -1], 203: [0, -1], 211: [0, -1],
        39: [1, 0], 141: [1, 0], 206: [1, 0], 213: [1, 0],
        40: [0, 1], 139: [0, 1], 204: [0, 1], 212: [0, 1]
    };
    var keys = { 27: 32, 32: 64, 143: 32, 196: 32 };
    var clickCodes = { 13: true, 142: true, 195: true };
    var titles = { 975370: "Dwarf Fortress", 413150: "Stardew Valley", 646570: "Slay the Spire" };

    function start(runner, appId, attempt) {
        var previous = document.activeElement;
        var previousInputMode = navigator.gamepadInputEmulation;
        var nativePad = appId !== 975370;
        var view, savedBounds, enteredFullscreen;
        var foreground = true, chordStarted = null, chordUsed = false, lastButtons = 0, lastPad;
        var layoutWidth = 0, layoutHeight = 0;
        var held = {};
        var pointerHeld = false;
        var pointerX = 0.5, pointerY = 0.5;
        var lastPointer = -1, lastKeys = -1;
        var animation, image, imageBytes, firstFrame, trialSerial = 0, trialEvents = 0, trialRetry = 0;
        var trialComplete = false;
        var lastTime = 0, lastFrameTime = 0;
        var ready = false, closing = false, stopped = false;
        var result = "";
        var complete;
        var finished = new Promise(function (resolve) { complete = resolve; });

        function stop(detail) {
            if (stopped) return;
            stopped = true;
            ready = false;
            cancelAnimationFrame(animation);
            exit.removeEventListener("click", quit);
            resume.removeEventListener("click", hideMenu);
            window.removeEventListener("keydown", setKey, true);
            window.removeEventListener("keyup", setKey, true);
            canvas.removeEventListener("pointermove", setPointer);
            canvas.removeEventListener("pointerdown", setPointer);
            canvas.removeEventListener("pointerup", setPointer);
            canvas.removeEventListener("pointercancel", setPointer);
            window.removeEventListener("blur", onBlur);
            window.removeEventListener("focus", onFocus);
            document.removeEventListener("visibilitychange", onVisibility);
            menu.hidden = true;
            screen.hidden = true;
            library.hidden = false;
            if (previousInputMode !== undefined) navigator.gamepadInputEmulation = previousInputMode;
            if (view) {
                if (savedBounds !== undefined) view.setDesiredBoundsMode(savedBounds);
                if (enteredFullscreen) view.exitFullScreenMode();
            }
            if (previous && previous.focus) previous.focus();
            if (detail) complete(result + detail);
        }

        function quit() {
            if (closing || !ready) return;
            closing = true;
            releaseKeys();
            runner.quitSteamSdl();
            menu.hidden = true;
            status.textContent = "Waiting for the game to quit…";
        }

        function hideMenu() {
            menu.hidden = true;
            canvas.focus();
        }

        function showMenu() {
            releaseKeys();
            menu.hidden = false;
            resume.focus();
        }

        function setKey(event) {
            if (event.keyCode === 121 && event.type === "keydown" && !event.repeat) {
                event.preventDefault();
                if (menu.hidden) showMenu(); else hideMenu();
                return;
            }
            // Read Xbox buttons from Gamepad. Suppress the browser's duplicate navigation and Back actions.
            if ((event.keyCode >= 136 && event.keyCode <= 143) || (event.keyCode >= 195 && event.keyCode <= 218)) {
                event.preventDefault();
                return;
            }
            if (!menu.hidden) return;
            if (!directions[event.keyCode] && !keys[event.keyCode] && !clickCodes[event.keyCode]) return;
            event.preventDefault();
            held[event.keyCode] = event.type === "keydown";
        }

        function releaseKeys() {
            held = {};
            pointerHeld = false;
            if (ready) runner.sendSteamSdlInput(encodePointer(false), 0);
            if (ready && nativePad) neutralPad();
            lastPointer = -1;
        }

        function onBlur() { foreground = false; releaseKeys(); }
        function onFocus() { foreground = true; }
        function onVisibility() { if (document.hidden) onBlur(); else onFocus(); }

        function setPointer(event) {
            if (!ready || !firstFrame || !menu.hidden) return;
            var bounds = canvas.getBoundingClientRect();
            if (!bounds.width || !bounds.height) return;
            pointerX = Math.max(0, Math.min(1, (event.clientX - bounds.left) / bounds.width));
            pointerY = Math.max(0, Math.min(1, (event.clientY - bounds.top) / bounds.height));
            pointerHeld = !!(event.buttons & 1);
            if (event.type === "pointerdown") {
                canvas.focus();
                if (canvas.setPointerCapture) canvas.setPointerCapture(event.pointerId);
            }
        }

        function encodePointer(click) {
            return Math.round(pointerX * 4095) | (Math.round(pointerY * 4095) << 12) | (click ? 1 << 24 : 0);
        }

        function movePointer(elapsed, pad) {
            var dx = 0, dy = 0, keyMask = 0, click = pointerHeld;
            Object.keys(held).forEach(function (code) {
                if (!held[code]) return;
                if (directions[code]) {
                    dx += directions[code][0];
                    dy += directions[code][1];
                }
                keyMask |= keys[code] || 0;
                click = click || !!clickCodes[code];
            });
            if (!nativePad) {
                dx += (Math.abs(pad[1]) > 6500 ? pad[1] / 32768 : 0) +
                    Number(!!(pad[0] & padButtons.right)) - Number(!!(pad[0] & padButtons.left));
                dy += (Math.abs(pad[2]) > 6500 ? pad[2] / 32768 : 0) +
                    Number(!!(pad[0] & padButtons.down)) - Number(!!(pad[0] & padButtons.up));
                click = click || !!(pad[0] & padButtons.a);
                if (pad[0] & padButtons.b) keyMask |= 32;
            }
            pointerX = Math.max(0, Math.min(1, pointerX + dx * elapsed / 1250));
            pointerY = Math.max(0, Math.min(1, pointerY + dy * elapsed / 1250));
            return { pointer: encodePointer(click), keys: keyMask };
        }

        function sendPad(pad) {
            if (lastPad && pad.every(function (value, i) { return value === lastPad[i]; })) return 0;
            lastPad = pad;
            return runner.sendSteamSdlGamepad(pad[0], pad[1], pad[2], pad[3], pad[4], pad[5], pad[6]);
        }

        function neutralPad() {
            sendPad([(lastPad ? lastPad[0] : 0) & gamepad.connected, 0, 0, 0, 0, -32768, -32768]);
        }

        function fitFrame() {
            var width = screen.clientWidth, height = screen.clientHeight;
            if (!width || !height) return;
            layoutWidth = width;
            layoutHeight = height;
            var scale = Math.min(width / canvas.width, height / canvas.height);
            canvas.style.width = Math.round(canvas.width * scale) + "px";
            canvas.style.height = Math.round(canvas.height * scale) + "px";
        }

        function draw() {
            var buffer = runner.steamSdlFrame();
            if (!buffer) return null;
            var reader = Windows.Storage.Streams.DataReader.fromBuffer(buffer);
            try {
                reader.byteOrder = Windows.Storage.Streams.ByteOrder.littleEndian;
                var width = reader.readUInt32(), height = reader.readUInt32();
                var sequence = reader.readUInt32(), ack = reader.readUInt32(), events = reader.readUInt32();
                if (!width || !height || width > 1920 || height > 1080 || reader.unconsumedBufferLength !== width * height * 4) {
                    throw new Error("Unexpected SDL frame dimensions");
                }
                if (!image || canvas.width !== width || canvas.height !== height) {
                    canvas.width = width;
                    canvas.height = height;
                    image = context.createImageData(width, height);
                    imageBytes = new Uint8Array(image.data.buffer);
                    fitFrame();
                }
                reader.readBytes(imageBytes);
                context.putImageData(image, 0, 0);
                return { sequence: sequence, ack: ack, events: events };
            } finally {
                reader.close();
            }
        }

        function tick(now) {
            if (stopped) return;
            try {
                if (!runner.steamSdlRunning()) {
                    stop("steam_sdl_host_frame=exited\r\n");
                    return;
                }
                if (now - lastFrameTime >= 45) {
                    var frame = draw();
                    lastFrameTime = now;
                    if (frame && !firstFrame) {
                        firstFrame = now;
                        status.textContent = "";
                        if (!attempt.automaticTrial) {
                            complete(result + "steam_sdl_host_frame=presented\r\n");
                        }
                    }
                    if (frame && attempt.automaticTrial && !trialComplete) {
                        var delivered = nativePad ? runner.steamSdlGamepadAck() === trialSerial :
                            frame.ack >= trialSerial && frame.events > trialEvents;
                        if (trialSerial && delivered) {
                            trialComplete = true;
                            complete(result + "steam_sdl_host_frame=presented\r\nsteam_sdl_host_input=delivered\r\n" +
                                (nativePad ? "steam_sdl_host_gamepad=delivered\r\n" : ""));
                        } else if (now - firstFrame > 24000) {
                            trialComplete = true;
                            complete(result + "steam_sdl_host_frame=presented\r\nsteam_sdl_host_input=unconfirmed\r\n");
                        } else if (now >= trialRetry) {
                            // Some titles finish initializing their input devices after their first swap.
                            if (nativePad) {
                                var rightX = lastPad && lastPad[3] === 8192 ? -8192 : 8192;
                                trialSerial = sendPad([gamepad.connected, 0, 0, rightX, 0, -32768, -32768]);
                            } else {
                                trialEvents = frame.events;
                                pointerX = pointerX === 0.64 ? 0.57 : 0.64;
                                lastPointer = encodePointer(false);
                                lastKeys = 0;
                                trialSerial = runner.sendSteamSdlInput(lastPointer, lastKeys);
                            }
                            trialRetry = now + 4000;
                        }
                    }
                }
                if (image && (screen.clientWidth !== layoutWidth || screen.clientHeight !== layoutHeight)) fitFrame();
                var pad = foreground ? gamepad.read(window.Windows, navigator) : gamepad.idle;
                var chord = padButtons.start | padButtons.back;
                if ((pad[0] & chord) === chord) {
                    if (chordStarted === null) chordStarted = now;
                    if (!chordUsed && now - chordStarted >= 750) {
                        chordUsed = true;
                        if (menu.hidden) showMenu(); else hideMenu();
                    }
                    pad = pad.slice();
                    pad[0] &= ~chord;
                } else {
                    chordStarted = null;
                    chordUsed = false;
                }
                var pressed = pad[0] & ~lastButtons;
                lastButtons = pad[0];
                if (!menu.hidden) {
                    if (pressed & padButtons.b) hideMenu();
                    if (pressed & padButtons.a) quit();
                } else if (!closing && foreground) {
                    if (nativePad && (!attempt.automaticTrial || trialComplete)) sendPad(pad);
                    var input = movePointer(lastTime ? Math.min(50, now - lastTime) : 0, pad);
                    if (input.pointer !== lastPointer || input.keys !== lastKeys) {
                        if (!nativePad || pointerHeld || lastPointer !== -1) {
                            runner.sendSteamSdlInput(input.pointer, input.keys);
                        }
                        lastPointer = input.pointer;
                        lastKeys = input.keys;
                    }
                    if (!nativePad) {
                        cursor.style.left = pointerX * 100 + "%";
                        cursor.style.top = pointerY * 100 + "%";
                    }
                }
                lastTime = now;
                animation = requestAnimationFrame(tick);
            } catch (_) {
                runner.quitSteamSdl();
                stop("steam_sdl_host_frame=presentation-error\r\n");
            }
        }

        try {
            var management = Windows.UI.ViewManagement;
            view = management.ApplicationView.getForCurrentView();
            savedBounds = view.desiredBoundsMode;
            enteredFullscreen = !view.isFullScreenMode && view.tryEnterFullScreenMode();
            view.setDesiredBoundsMode(management.ApplicationViewBoundsMode.useCoreWindow);
        } catch (_) {
            // The fixed display still covers the available viewport.
        }
        screen.hidden = false;
        cursor.hidden = nativePad;
        menu.hidden = true;
        library.hidden = true;
        title.textContent = titles[appId];
        status.textContent = "Starting " + titles[appId] + "…";
        exit.disabled = true;
        exit.addEventListener("click", quit);
        resume.addEventListener("click", hideMenu);
        window.addEventListener("keydown", setKey, true);
        window.addEventListener("keyup", setKey, true);
        canvas.addEventListener("pointermove", setPointer);
        canvas.addEventListener("pointerdown", setPointer);
        canvas.addEventListener("pointerup", setPointer);
        canvas.addEventListener("pointercancel", setPointer);
        window.addEventListener("blur", onBlur);
        window.addEventListener("focus", onFocus);
        document.addEventListener("visibilitychange", onVisibility);
        if (previousInputMode !== undefined) navigator.gamepadInputEmulation = "keyboard";
        canvas.focus();
        Promise.resolve(runner.launchSteamSdlGame(appId, attempt.file, attempt.report)).then(function (report) {
            result = report;
            if (report.split(/\r?\n/).indexOf("steam_sdl_host=ready") < 0) {
                stop();
                complete(report);
                return;
            }
            ready = true;
            exit.disabled = false;
            animation = requestAnimationFrame(tick);
        }, function () { stop("steam_sdl_host=launch-error\r\n"); });
        return finished;
    }

    var apps = document.getElementById("steam-sdl-profiles").getAttribute("data-apps").split(" ");
    window.steamSdlRuntimes = apps.map(Number).filter(function (appId) { return !!titles[appId]; })
        .map(function (appId) { return { appId: appId, backend: "sdl-host", launch: start }; });
}());
