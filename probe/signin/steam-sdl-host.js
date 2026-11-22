(function () {
    "use strict";

    var screen = document.getElementById("sdl-game");
    var library = document.getElementById("library");
    var exit = document.getElementById("sdl-exit");
    var resume = document.getElementById("sdl-resume");
    var menu = document.getElementById("sdl-menu");
    var keyboard = document.getElementById("sdl-keyboard");
    var textForm = document.getElementById("sdl-text");
    var textValue = document.getElementById("sdl-text-value");
    var textSend = document.getElementById("sdl-text-send");
    var textCancel = document.getElementById("sdl-text-cancel");
    var textStatus = document.getElementById("sdl-text-status");
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
    // SDL scancodes follow USB HID usage IDs. The virtual-key fallback is for older Xbox HTML hosts.
    var scans = {
        Enter: 40, Escape: 41, Backspace: 42, Tab: 43, Space: 44, Minus: 45, Equal: 46,
        BracketLeft: 47, BracketRight: 48, Backslash: 49, Semicolon: 51, Quote: 52,
        Backquote: 53, Comma: 54, Period: 55, Slash: 56, CapsLock: 57, Insert: 73,
        Home: 74, PageUp: 75, Delete: 76, End: 77, PageDown: 78, ArrowRight: 79,
        ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82, NumpadEnter: 88, NumpadDecimal: 99,
        NumpadDivide: 84, NumpadMultiply: 85, NumpadSubtract: 86, NumpadAdd: 87,
        ControlLeft: 224, ShiftLeft: 225, AltLeft: 226, MetaLeft: 227,
        ControlRight: 228, ShiftRight: 229, AltRight: 230, MetaRight: 231
    };
    var virtualKeys = {
        8: "Backspace", 9: "Tab", 13: "Enter", 16: "ShiftLeft", 17: "ControlLeft",
        18: "AltLeft", 20: "CapsLock", 27: "Escape", 32: "Space", 33: "PageUp",
        34: "PageDown", 35: "End", 36: "Home", 37: "ArrowLeft", 38: "ArrowUp",
        39: "ArrowRight", 40: "ArrowDown", 45: "Insert", 46: "Delete", 106: "NumpadMultiply",
        107: "NumpadAdd", 109: "NumpadSubtract", 110: "NumpadDecimal", 111: "NumpadDivide",
        186: "Semicolon", 187: "Equal", 188: "Comma", 189: "Minus", 190: "Period",
        191: "Slash", 192: "Backquote", 219: "BracketLeft", 220: "Backslash",
        221: "BracketRight", 222: "Quote"
    };
    function scanCode(event) {
        var code = event.code, key = event.keyCode;
        if (/^Key[A-Z]$/.test(code)) return code.charCodeAt(3) - 65 + 4;
        if (/^Digit[0-9]$/.test(code)) return 30 + (Number(code[5]) + 9) % 10;
        if (/^Numpad[0-9]$/.test(code)) return 89 + (Number(code[6]) + 9) % 10;
        if (/^F([1-9]|1[0-2])$/.test(code)) return 57 + Number(code.slice(1));
        if (scans[code]) return scans[code];
        if (key >= 65 && key <= 90) return key - 65 + 4;
        if (key >= 48 && key <= 57) return 30 + (key - 48 + 9) % 10;
        if (key >= 96 && key <= 105) return 89 + (key - 96 + 9) % 10;
        if (key >= 112 && key <= 123) return key - 112 + 58;
        return scans[virtualKeys[key]];
    }

    function inputPane() {
        try { return Windows.UI.ViewManagement.InputPane.getForCurrentView(); }
        catch (_) { return null; }
    }

    function start(runner, appId, attempt) {
        var bigPictureMode = attempt.bigPicture === true;
        var previous = document.activeElement;
        var previousInputMode = navigator.gamepadInputEmulation;
        var nativePad = appId !== 975370;
        var view, savedBounds, enteredFullscreen;
        var foreground = true, chordStarted = null, chordUsed = false, lastButtons = 0, lastPad;
        var blockedMenuButtons = bigPictureMode ? padButtons.a | padButtons.b : 0;
        var keyboardReady = false;
        var layoutWidth = 0, layoutHeight = 0;
        var held = {};
        var pointerHeld = false;
        var pointerX = 0.5, pointerY = 0.5;
        var lastPointer = -1, lastKeys = -1;
        var animation, image, imageBytes, firstFrame, trialSerial = 0, trialEvents = 0, trialRetry = 0;
        var trialComplete = false;
        var lastTime = 0;
        var ready = false, closing = false, stopped = false;
        var result = "";
        var timing = attempt.automaticTrial && window.steamSdlTiming && window.steamSdlTiming.create();
        var trialReport, timingReport, nativeView;
        var complete;
        var finished = new Promise(function (resolve) { complete = resolve; });

        function stop(detail) {
            if (stopped) return;
            stopped = true;
            var restored = nativeView && nativeView.close();
            ready = false;
            cancelAnimationFrame(animation);
            exit.removeEventListener("click", quit);
            resume.removeEventListener("click", hideMenu);
            keyboard.removeEventListener("click", openText);
            textValue.removeEventListener("click", showInputPane);
            textForm.removeEventListener("submit", sendText);
            textCancel.removeEventListener("click", closeText);
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
            textForm.hidden = true;
            textValue.value = "";
            screen.hidden = true;
            library.hidden = bigPictureMode;
            if (previousInputMode !== undefined) navigator.gamepadInputEmulation = previousInputMode;
            if (view) {
                if (savedBounds !== undefined) view.setDesiredBoundsMode(savedBounds);
                if (enteredFullscreen) view.exitFullScreenMode();
            }
            if (previous && previous.focus) previous.focus();
            if (detail) complete(result + detail);
            if (attempt.onExit) Promise.resolve(restored).then(attempt.onExit);
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
            blockedMenuButtons = lastButtons & (padButtons.a | padButtons.b);
            menu.hidden = true;
            if (nativeView) nativeView.resume();
            canvas.focus();
        }

        function showMenu(gameViewId) {
            releaseKeys();
            if (bigPictureMode) {
                var steam = window.steamBigPictureHost;
                if (!gameViewId && (!nativeView || nativeView.primary) && view) gameViewId = view.id;
                if (steam && gameViewId) {
                    if (nativeView && nativeView.primary) nativeView.pause();
                    steam.showForGame(appId, gameViewId, function (steamViewId) {
                        if (!runner.steamSdlRunning()) return false;
                        return nativeView ? nativeView.resume(steamViewId) :
                            steam.switchView(gameViewId, steamViewId).then(function () { return !stopped; });
                    }, function () {
                        if (closing || !ready || !runner.steamSdlRunning()) return false;
                        quit();
                        return true;
                    }).then(function (shown) {
                        if (!shown && nativeView && runner.steamSdlRunning()) nativeView.resume();
                    }, function () {
                        if (nativeView && runner.steamSdlRunning()) nativeView.resume();
                    });
                } else if (nativeView) nativeView.resume();
                return;
            }
            menu.hidden = false;
            resume.focus();
        }

        function showInputPane() {
            var pane = inputPane();
            try { if (pane && pane.tryShow) pane.tryShow(); }
            catch (_) { textValue.focus(); }
        }

        function openText() {
            if (!keyboardReady) return;
            menu.hidden = true;
            textForm.hidden = false;
            textValue.value = "";
            textStatus.textContent = "";
            textValue.focus();
            showInputPane();
        }

        function closeText() {
            var pane = inputPane();
            try { if (pane && pane.tryHide) pane.tryHide(); }
            catch (_) { /* Returning focus to the game also dismisses the input pane. */ }
            textForm.hidden = true;
            textValue.value = "";
            hideMenu();
        }

        function sendText(event) {
            if (event) event.preventDefault();
            if (!textValue.value || runner.sendSteamSdlText(textValue.value)) closeText();
            else textStatus.textContent = "The game could not accept the text. Try again.";
        }

        function moveMenuFocus(items, pressed) {
            var index = items.indexOf(document.activeElement);
            if (pressed & (padButtons.up | padButtons.left)) items[Math.max(0, index - 1)].focus();
            if (pressed & (padButtons.down | padButtons.right)) items[Math.min(items.length - 1, index + 1)].focus();
        }

        function setKey(event) {
            if (event.keyCode === 121 || event.code === "F10") {
                event.preventDefault();
                if (event.type === "keydown" && !event.repeat) {
                    if (!textForm.hidden) closeText();
                    else if (menu.hidden) showMenu(); else hideMenu();
                }
                return;
            }
            // Read Xbox buttons from Gamepad. Suppress the browser's duplicate navigation and Back actions.
            if ((event.keyCode >= 136 && event.keyCode <= 143) || (event.keyCode >= 195 && event.keyCode <= 218)) {
                event.preventDefault();
                return;
            }
            if (!menu.hidden || !textForm.hidden || !foreground || closing) return;
            if (keyboardReady) {
                var scan = scanCode(event);
                if (!scan) return;
                event.preventDefault();
                var down = event.type === "keydown";
                if (runner.sendSteamSdlKey(scan, down) && down &&
                        !event.isComposing && !event.ctrlKey && !event.altKey && !event.metaKey &&
                        typeof event.key === "string" && Array.from(event.key).length === 1) {
                    runner.sendSteamSdlText(event.key);
                }
                return;
            }
            if (!directions[event.keyCode] && !keys[event.keyCode] && !clickCodes[event.keyCode]) return;
            event.preventDefault();
            held[event.keyCode] = event.type === "keydown";
        }

        function releaseKeys() {
            held = {};
            pointerHeld = false;
            if (ready) runner.sendSteamSdlInput(encodePointer(false), 0);
            if (ready && nativePad) neutralPad();
            if (keyboardReady) runner.resetSteamSdlKeyboard();
            lastPointer = -1;
        }

        function onBlur() {
            foreground = false;
            if (timing) timing.reset();
            releaseKeys();
        }
        function onFocus() {
            foreground = true;
            if (bigPictureMode) blockedMenuButtons = padButtons.a | padButtons.b;
        }
        function onVisibility() { if (document.hidden) onBlur(); else onFocus(); }

        function setPointer(event) {
            if (!ready || !firstFrame || !menu.hidden || !textForm.hidden) return;
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

        var imageWords, appliedSequence = 0;
        var useRgbDelta = appId === 646570 && !!runner.steamSdlRgbDeltaFrame;
        function drawRgb() {
            var began = timing && performance.now();
            var text = runner.steamSdlRgbDeltaFrame(appliedSequence);
            if (!text) return null;
            var acquired = timing && performance.now();
            var headerEnd = text.indexOf("\n");
            var fields = text.slice(0, headerEnd).split(",");
            var width = +fields[0], height = +fields[1];
            var count = width * height, offset = headerEnd + 1;
            var delta = fields[5] === "D";
            if (fields.length !== 6 || !width || !height || width > 1920 || height > 1080 ||
                    (!delta && text.length - offset !== count * 2)) throw new Error("Unexpected SDL RGB frame");
            if (!image || canvas.width !== width || canvas.height !== height) {
                canvas.width = width;
                canvas.height = height;
                image = context.createImageData(width, height);
                imageWords = new Uint32Array(image.data.buffer);
                fitFrame();
            }
            var i = 0;
            if (delta) {
                while (offset < text.length) {
                    var code = text.charCodeAt(offset++);
                    switch (code >> 12) {
                    case 4: i += (code & 4095) + 1; break;
                    case 2:
                        imageWords[i] += (code & 15) - 8 + ((((code >> 4) & 15) - 8) << 8) +
                            ((((code >> 8) & 15) - 8) << 16);
                        i++;
                        break;
                    case 1:
                        imageWords[i++] = 0xff000000 | ((text.charCodeAt(offset++) & 4095) << 12) | (code & 4095);
                        break;
                    default: throw new Error("Invalid SDL RGB delta");
                    }
                }
                if (i !== count || offset !== text.length) throw new Error("Incomplete SDL RGB delta");
            } else {
                for (; i < count; i++, offset += 2) {
                    imageWords[i] = 0xff000000 | ((text.charCodeAt(offset + 1) & 4095) << 12) |
                        (text.charCodeAt(offset) & 4095);
                }
            }
            appliedSequence = +fields[2];
            var copied = timing && performance.now();
            context.putImageData(image, 0, 0);
            var frame = { sequence: +fields[2], ack: +fields[3], events: +fields[4] };
            if (timing) {
                frame.nativeMs = acquired - began;
                frame.readMs = copied - acquired;
                frame.uploadMs = performance.now() - copied;
            }
            return frame;
        }

        function draw() {
            if (useRgbDelta) {
                try { return drawRgb(); }
                catch (_) {
                    // Resume with a complete frame if the Xbox projection cannot allocate a delta.
                    useRgbDelta = false;
                    image = imageWords = null;
                }
            }
            var began = timing && performance.now();
            var buffer = runner.steamSdlFrame();
            if (!buffer) return null;
            var acquired = timing && performance.now();
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
                var copied = timing && performance.now();
                context.putImageData(image, 0, 0);
                var frame = { sequence: sequence, ack: ack, events: events };
                if (timing) {
                    frame.nativeMs = acquired - began;
                    frame.readMs = copied - acquired;
                    frame.uploadMs = performance.now() - copied;
                }
                return frame;
            } finally {
                reader.close();
            }
        }

        function finishTrial(report) {
            trialReport = report;
            if (!timing || timingReport) complete(report + (timingReport || ""));
        }

        function tick(now) {
            if (stopped) return;
            // The secondary view owns rendering and live input until the user opens a player overlay.
            if (nativeView && !nativeView.primary && menu.hidden && textForm.hidden) {
                animation = requestAnimationFrame(tick);
                return;
            }
            try {
                if (!runner.steamSdlRunning()) {
                    stop("steam_sdl_host_frame=exited\r\n");
                    return;
                }
                var frame = nativeView ? null : draw();
                if (timing && foreground && firstFrame !== undefined && now - firstFrame >= 8000) {
                    timingReport = timing.observe(performance.now(), frame);
                    if (timingReport) {
                        timingReport += "steam_sdl_canvas=" + canvas.width + "x" + canvas.height + "\r\n" +
                            "steam_sdl_viewport=" + screen.clientWidth + "x" + screen.clientHeight + "\r\n";
                        timing = null;
                        if (trialReport) complete(trialReport + timingReport);
                    }
                }
                if (frame) {
                    if (!firstFrame) {
                        firstFrame = now;
                        status.textContent = "";
                        if (nativePad && attempt.automaticTrial) {
                            sendPad([gamepad.connected, 0, 0, 0, 0, -32768, -32768]);
                            trialRetry = now + 12000;
                        }
                        if (!attempt.automaticTrial) {
                            complete(result + "steam_sdl_host_frame=presented\r\n");
                        }
                    }
                    if (attempt.automaticTrial && !trialComplete) {
                        var delivered = nativePad ? runner.steamSdlGamepadAck() === trialSerial :
                            frame.ack >= trialSerial && frame.events > trialEvents;
                        if (trialSerial && delivered) {
                            trialComplete = true;
                            finishTrial(result + "steam_sdl_host_frame=presented\r\nsteam_sdl_host_input=delivered\r\n" +
                                (nativePad ? "steam_sdl_host_gamepad=delivered\r\n" : ""));
                        } else if (now - firstFrame > 24000) {
                            trialComplete = true;
                            finishTrial(result + "steam_sdl_host_frame=presented\r\nsteam_sdl_host_input=unconfirmed\r\n");
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
                if (firstFrame && !keyboardReady && runner.steamSdlKeyboardReady && runner.steamSdlKeyboardReady()) {
                    keyboardReady = true;
                    keyboard.hidden = false;
                    releaseKeys();
                }
                var pad = foreground ? gamepad.read(window.Windows, navigator) : gamepad.idle;
                var chord = padButtons.start | padButtons.back;
                if ((pad[0] & chord) === chord) {
                    if (chordStarted === null) chordStarted = now;
                    if (!chordUsed && (bigPictureMode || now - chordStarted >= 750)) {
                        chordUsed = true;
                        if (!textForm.hidden) closeText();
                        else if (menu.hidden) showMenu(); else hideMenu();
                    }
                    pad = pad.slice();
                    pad[0] &= ~chord;
                } else {
                    chordStarted = null;
                    chordUsed = false;
                }
                var pressed = pad[0] & ~lastButtons;
                lastButtons = pad[0];
                if (!textForm.hidden) {
                    var pane = inputPane();
                    if (!pane || !pane.occludedRect || !pane.occludedRect.height) {
                        moveMenuFocus([textValue, textSend, textCancel], pressed);
                        if (pressed & padButtons.b) closeText();
                        else if (pressed & padButtons.a) {
                            if (document.activeElement === textSend) sendText();
                            else if (document.activeElement === textCancel) closeText();
                            else showInputPane();
                        }
                    }
                } else if (!menu.hidden) {
                    moveMenuFocus(keyboardReady ? [resume, keyboard, exit] : [resume, exit], pressed);
                    if (pressed & padButtons.b) hideMenu();
                    else if (pressed & padButtons.a) {
                        if (document.activeElement === exit) quit();
                        else if (document.activeElement === keyboard) openText();
                        else hideMenu();
                    }
                } else if (!closing && foreground) {
                    blockedMenuButtons &= pad[0];
                    if (blockedMenuButtons) {
                        pad = pad.slice();
                        pad[0] &= ~blockedMenuButtons;
                    }
                    if (nativePad && (!attempt.automaticTrial || trialComplete)) {
                        if (attempt.automaticTrial && pad === gamepad.idle) neutralPad();
                        else sendPad(pad);
                    }
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
        keyboard.hidden = true;
        textForm.hidden = true;
        library.hidden = true;
        title.textContent = titles[appId];
        status.textContent = "Starting " + titles[appId] + "…";
        exit.disabled = true;
        exit.addEventListener("click", quit);
        resume.addEventListener("click", hideMenu);
        keyboard.addEventListener("click", openText);
        textValue.addEventListener("click", showInputPane);
        textForm.addEventListener("submit", sendText);
        textCancel.addEventListener("click", closeText);
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
            if (window.steamSdlNativeView) {
                nativeView = window.steamSdlNativeView.open(report, {
                    running: function () { return runner.steamSdlRunning(); },
                    menu: showMenu,
                    exited: function () { stop("steam_sdl_host_frame=exited\r\n"); },
                    fallback: function () { nativeView = null; },
                    mediaError: function (phase, code) {
                        var steam = window.steamBigPictureHost;
                        if (steam && steam.recordGameFrame) steam.recordGameFrame(appId, "fallback",
                            "media_phase=" + phase + "\r\nmedia_error=" + code + "\r\n");
                    },
                    mediaProgress: function (stats) {
                        var steam = window.steamBigPictureHost;
                        if (steam && steam.recordGameFrame) steam.recordGameFrame(appId, "media", stats);
                    },
                    mediaCapture: function (stats) {
                        var steam = window.steamBigPictureHost;
                        if (steam && steam.recordGameFrame) steam.recordGameFrame(appId, "media_capture", stats);
                    },
                    measured: function (frames, microseconds, stats) {
                        if (window.steamBigPictureHost && window.steamBigPictureHost.recordGameFrame)
                            window.steamBigPictureHost.recordGameFrame(appId, "sample", stats);
                        if (!attempt.automaticTrial) return;
                        var padAcknowledged = nativePad && runner.steamSdlGamepadAck() > 0;
                        complete(result + "steam_sdl_host_frame=presented\r\nsteam_sdl_presentation=native\r\n" +
                            "steam_sdl_sample_frames=" + frames + "\r\nsteam_sdl_sample_us=" + microseconds + "\r\n" +
                            "steam_sdl_host_input=" + (padAcknowledged ? "delivered" : "unconfirmed") + "\r\n" +
                            (padAcknowledged ? "steam_sdl_host_gamepad=delivered\r\n" : ""));
                    },
                    presented: function (stats) {
                        if (window.steamBigPictureHost && window.steamBigPictureHost.recordGameFrame)
                            window.steamBigPictureHost.recordGameFrame(appId, "first", stats);
                        firstFrame = performance.now();
                        status.textContent = "";
                        keyboardReady = !!(runner.steamSdlKeyboardReady && runner.steamSdlKeyboardReady());
                        keyboard.hidden = !keyboardReady;
                        if (!attempt.automaticTrial) complete(report + "steam_sdl_host_frame=presented\r\n");
                    }
                }, nativePad, bigPictureMode ? screen : null);
            }
            animation = requestAnimationFrame(tick);
        }, function () { stop("steam_sdl_host=launch-error\r\n"); });
        return finished;
    }

    var apps = document.getElementById("steam-sdl-profiles").getAttribute("data-apps").split(" ");
    window.steamSdlRuntimes = apps.map(Number).filter(function (appId) { return !!titles[appId]; })
        .map(function (appId) { return { appId: appId, backend: "sdl-host", launch: start }; });
}());
