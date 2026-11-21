(function () {
    "use strict";

    var screen = document.getElementById("sdl-game");
    var library = document.getElementById("library");
    var exit = document.getElementById("sdl-exit");
    var status = document.getElementById("sdl-status");
    var title = document.getElementById("sdl-game-title");
    var canvas = document.getElementById("sdl-frame");
    var cursor = document.getElementById("sdl-pointer");
    var context = canvas.getContext("2d");
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
            canvas.removeEventListener("keydown", setKey);
            canvas.removeEventListener("keyup", setKey);
            canvas.removeEventListener("pointermove", setPointer);
            canvas.removeEventListener("pointerdown", setPointer);
            canvas.removeEventListener("pointerup", setPointer);
            canvas.removeEventListener("pointercancel", setPointer);
            window.removeEventListener("blur", releaseKeys);
            document.removeEventListener("visibilitychange", releaseKeys);
            screen.hidden = true;
            library.hidden = false;
            if (previousInputMode !== undefined) navigator.gamepadInputEmulation = previousInputMode;
            if (previous && previous.focus) previous.focus();
            if (detail) complete(result + detail);
        }

        function quit() {
            if (closing || !ready) return;
            closing = true;
            releaseKeys();
            runner.quitSteamSdl();
            status.textContent = "Waiting for the game to quit…";
        }

        function setKey(event) {
            if (!directions[event.keyCode] && !keys[event.keyCode] && !clickCodes[event.keyCode]) return;
            event.preventDefault();
            held[event.keyCode] = event.type === "keydown";
        }

        function releaseKeys() {
            held = {};
            pointerHeld = false;
            if (ready) runner.sendSteamSdlInput(encodePointer(false), 0);
            lastPointer = -1;
        }

        function setPointer(event) {
            if (!ready || !firstFrame) return;
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

        function movePointer(elapsed) {
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
            var pads = navigator.getGamepads ? navigator.getGamepads() : [];
            for (var i = 0; i < pads.length; i++) {
                var pad = pads[i];
                if (!pad || !pad.connected || pad.mapping !== "standard") continue;
                dx += Math.abs(pad.axes[0]) > 0.2 ? pad.axes[0] : 0;
                dy += Math.abs(pad.axes[1]) > 0.2 ? pad.axes[1] : 0;
                break;
            }
            pointerX = Math.max(0, Math.min(1, pointerX + dx * elapsed / 1250));
            pointerY = Math.max(0, Math.min(1, pointerY + dy * elapsed / 1250));
            return { pointer: encodePointer(click), keys: keyMask };
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
                        if (trialSerial && frame.ack >= trialSerial && frame.events > trialEvents) {
                            trialComplete = true;
                            complete(result + "steam_sdl_host_frame=presented\r\nsteam_sdl_host_input=delivered\r\n");
                        } else if (now - firstFrame > 24000) {
                            trialComplete = true;
                            complete(result + "steam_sdl_host_frame=presented\r\nsteam_sdl_host_input=unconfirmed\r\n");
                        } else if (now >= trialRetry) {
                            // Some titles finish initializing their input devices after their first swap.
                            trialEvents = frame.events;
                            pointerX = pointerX === 0.64 ? 0.57 : 0.64;
                            lastPointer = encodePointer(false);
                            lastKeys = 0;
                            trialSerial = runner.sendSteamSdlInput(lastPointer, lastKeys);
                            trialRetry = now + 4000;
                        }
                    }
                }
                if (!closing) {
                    var input = movePointer(lastTime ? Math.min(50, now - lastTime) : 0);
                    if (input.pointer !== lastPointer || input.keys !== lastKeys) {
                        runner.sendSteamSdlInput(input.pointer, input.keys);
                        lastPointer = input.pointer;
                        lastKeys = input.keys;
                    }
                    cursor.style.left = pointerX * 100 + "%";
                    cursor.style.top = pointerY * 100 + "%";
                }
                lastTime = now;
                animation = requestAnimationFrame(tick);
            } catch (_) {
                runner.quitSteamSdl();
                stop("steam_sdl_host_frame=presentation-error\r\n");
            }
        }

        screen.hidden = false;
        library.hidden = true;
        title.textContent = titles[appId];
        status.textContent = "Starting " + titles[appId] + "…";
        exit.disabled = true;
        exit.addEventListener("click", quit);
        canvas.addEventListener("keydown", setKey);
        canvas.addEventListener("keyup", setKey);
        canvas.addEventListener("pointermove", setPointer);
        canvas.addEventListener("pointerdown", setPointer);
        canvas.addEventListener("pointerup", setPointer);
        canvas.addEventListener("pointercancel", setPointer);
        window.addEventListener("blur", releaseKeys);
        document.addEventListener("visibilitychange", releaseKeys);
        if (previousInputMode !== undefined) navigator.gamepadInputEmulation = "mouse";
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
