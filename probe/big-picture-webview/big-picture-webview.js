(function () {
    "use strict";
    var session;

    function switchView(to, from) {
        if (to === from) return Promise.resolve();
        var views = Windows.UI.ViewManagement.ApplicationViewSwitcher;
        function replace() { return views.switchAsync(to, from); }
        // The FHD presenter and Valve are two views of the same Xbox game.
        // First request an in-place switch in the current foreground slot.
        return Promise.resolve().then(replace).catch(function () {
            return Promise.resolve(views.tryShowAsStandaloneAsync(to)).then(replace);
        });
    }

    function open(broker, onFailure) {
        if (session && session.cleanup) return session.cleanup.then(function () { return open(broker, onFailure); });
        if (session) return session.ready;
        var storage = Windows.Storage, views = Windows.UI.ViewManagement;
        var run = Windows.Foundation.GuidHelper.createNewGuid();
        var file, report = Promise.resolve();
        var messages = [], last = "", servedGames, rendererError, surfaceState, startupRoute, popupPhase, captures = {}, memory = {}, gameFrames = {};
        var surfaceSizing = "", warmReport = "", memoryRequest = "", triedMemory = false;
        var warming = false, warmStarted = 0, warmProgress = 0, warmSwitch = false;
        function sampleMemory(stage) {
            try {
                var manager = Windows.System.MemoryManager;
                var usage = manager.appMemoryUsage, limit = manager.appMemoryUsageLimit;
                if (typeof usage === "number" && isFinite(usage) && usage >= 0 &&
                    typeof limit === "number" && isFinite(limit) && limit > 0)
                    memory[stage] = "memory_" + stage + "_usage_bytes=" + usage + "\r\n" +
                        "memory_" + stage + "_limit_bytes=" + limit + "\r\n";
                var expected = manager.expectedAppMemoryUsageLimit;
                if (memory[stage] && Number.isSafeInteger(expected) && expected > 0)
                    memory[stage] += "memory_" + stage + "_expected_limit_bytes=" + expected + "\r\n";
                // Only a cold signed shell can make this one bounded request.
                // The platform has to advertise more headroom than the observed 1-GiB floor.
                var target = 1610612736;
                if (warming && !settled && !playing && !triedMemory && limit > 0 && limit < 1207959552 &&
                    Number.isSafeInteger(expected) && expected >= target && typeof manager.trySetAppMemoryUsageLimit === "function") {
                    triedMemory = true;
                    memoryRequest = "unavailable";
                    var granted = manager.trySetAppMemoryUsageLimit(target);
                    var currentLimit = manager.appMemoryUsageLimit;
                    memoryRequest = (granted ? "granted" : "declined") + " requested_bytes=" + target +
                        (Number.isSafeInteger(currentLimit) && currentLimit > 0 ? " current_bytes=" + currentLimit : "");
                }
            } catch (_) {}
        }
        var resolveReady, rejectReady, readinessTimer, settled = false, failed = false;
        var ready = new Promise(function (resolve, reject) {
            resolveReady = resolve; rejectReady = reject;
        });
        function record(stage, refresh) {
            if (stage !== last) {
                last = stage;
                messages.push(stage);
                if (messages.length > 24) messages.shift();
            } else if (!refresh) return;
            var text = "run_id=" + run + "\r\nstatus=" + stage + "\r\n" +
                (servedGames === undefined ? "" : "owned_games_served=" + servedGames + "\r\n") +
                (warmReport ? "warmup=" + warmReport + "\r\n" : "") +
                (memoryRequest ? "shell_memory_request=" + memoryRequest + "\r\n" : "") +
                (surfaceSizing ? "surface_sizing=" + surfaceSizing + "\r\n" : "") +
                (surfaceState ? "valve_surface=" + surfaceState + "\r\n" : "") +
                (popupPhase ? "valve_popup=" + popupPhase + "\r\n" : "") +
                (startupRoute ? "valve_startup=" + startupRoute + "\r\n" : "") +
                Object.keys(captures).map(function (kind) { return kind + "_capture=" + captures[kind] + "\r\n"; }).join("") +
                Object.keys(gameFrames).map(function (key) { return "game_frame_" + key + "=" + gameFrames[key] + "\r\n"; }).join("") +
                (rendererError ? "renderer_error=" + rendererError + "\r\n" : "") +
                Object.keys(memory).map(function (key) { return memory[key]; }).join("") + messages.join("\r\n") + "\r\n";
            // Device Portal may be reading the report during a cold launch. Its
            // file lifetime must never gate Steam startup, input or foregrounding.
            report = report.then(function () {
                return file || storage.ApplicationData.current.localFolder.createFileAsync("big-picture-webview-report.txt",
                    storage.CreationCollisionOption.openIfExists);
            }).then(function (opened) {
                file = opened;
                return storage.FileIO.writeTextAsync(file, text);
            }).catch(function () { file = null; });
        }
        var host, viewId, mainId, visible = false, activeSwitch, timer, playing = false;
        function show(to, from) {
            return switchView(to, from);
        }
        var playingApp = 0, presented = false, returnToGame, quitGame;
        function recordGameFrame(appId, stage, stats) {
            if (failed || !playing || appId !== playingApp || typeof stats !== "string" ||
                (stage !== "first" && stage !== "sample")) return;
            var fields = stats.split(/\r?\n/).filter(function (line) {
                return /^(width|height|visible|activation_mode|presented|gpu_presented|cpu_presented|sample_us|sample_frames)=[0-9]{1,16}$/.test(line);
            });
            if (!fields.length) return;
            gameFrames[stage] = "appid=" + appId + " " + fields.join(" ");
            record(last, true);
        }
        var gameForeground = false;
        var firstActivation = true;
        function setGameForeground(value) {
            if (!host || failed || value === gameForeground) return;
            gameForeground = value;
            host.reply(JSON.stringify({nativeGameForeground: value}));
        }
        function gameEvent(overlay) {
            if (!host || failed) return;
            var hosted = playingApp === 646570 || playingApp === 975370 || playingApp === 413150;
            host.reply(JSON.stringify({event: "xbox-game", appId: presented && hosted ? playingApp : 0, overlay: overlay}));
        }
        function showValve(from) {
            if (failed || !viewId || visible) return Promise.resolve();
            var wasGame = gameForeground;
            setGameForeground(false);
            // The initial Steam window needs its own Xbox foreground allocation.
            // Replacing the QR view directly can retire the primary WWA host.
            if (!activeSwitch) activeSwitch = (firstActivation && viewId !== mainId ?
                Promise.resolve(views.ApplicationViewSwitcher.tryShowAsStandaloneAsync(viewId)).then(function (shown) {
                    if (!shown) throw new Error("Xbox could not show the Steam window");
                    return views.ApplicationViewSwitcher.switchAsync(viewId, mainId);
                }) : show(viewId, from || mainId)).then(function () {
                firstActivation = false;
                sampleMemory("steam_view");
                record(last, true);
                visible = true; activeSwitch = null;
            }, function (error) { activeSwitch = null; if (wasGame) setGameForeground(true); throw error; });
            return activeSwitch;
        }
        function handoffWarmup(reason) {
            if (warmSwitch || failed || !viewId) return;
            warmSwitch = true;
            warmReport = reason + " elapsed_ms=" + Math.max(0, Date.now() - warmStarted);
            sampleMemory("warm_handoff");
            record(last, true);
            showValve(mainId).then(function () {
                if (failed) return;
                host.reply(JSON.stringify({nativeWarmupComplete: true}));
            }).catch(fail);
        }
        function showForGame(appId, from, restore, requestExit) {
            if (!played(appId) || !from || typeof restore !== "function")
                return Promise.resolve(false);
            returnToGame = restore; quitGame = requestExit;
            return showValve(from).then(function () {
                if (!playing || playingApp !== appId) return false;
                sampleMemory("overlay"); gameEvent(true); record("game-switch-to-steam");
                return true;
            });
        }
        function raiseGame(appId) {
            if (!visible || !played(appId) || !returnToGame) return Promise.resolve("unavailable");
            var restore = returnToGame;
            returnToGame = null;
            return Promise.resolve().then(function () { return restore(viewId); }).then(function (shown) {
                if (!shown || !played(appId)) return "unavailable";
                visible = false;
                setGameForeground(true);
                sampleMemory("resume_game"); gameEvent(false); record("game-switch-to-game");
                return "presented";
            }, function () {
                if (played(appId)) returnToGame = restore;
                return "failed";
            });
        }
        function played(appId) { return !failed && playing && presented && appId === playingApp; }
        function resume(exitedApp) {
            if (warming && !settled) return Promise.resolve();
            if (exitedApp && playingApp && exitedApp !== playingApp) return Promise.resolve();
            var returning = playing;
            playing = presented = false;
            playingApp = 0; returnToGame = quitGame = null;
            return showValve(mainId).then(function () {
                if (returning && !playing && !failed) {
                    sampleMemory("return"); gameEvent(false); return record("game-returned-to-steam");
                }
            });
        }
        function respond(id, ok, value) {
            if (failed) return;
            if (ok) host.reply(JSON.stringify({id: id, result: value}));
            else host.reply(JSON.stringify({id: id, error: value}));
        }
        function handle(raw) {
            if (failed) return;
            var request;
            try { request = JSON.parse(raw); } catch (_) { return; }
            if (!request || typeof request !== "object") return;
            if (request.method === "capture-library-result" || request.method === "capture-surface-result") {
                var kind = request.method === "capture-library-result" ? "library" : "surface";
                if (captures[kind] !== "requested") return;
                if (request.state === "saved" && request.viewId === viewId && Number.isInteger(request.browserPid) &&
                    request.browserPid > 0 && Number.isInteger(request.width) && request.width > 0 && request.width <= 16384 &&
                    Number.isInteger(request.height) && request.height > 0 && request.height <= 16384) {
                    captures[kind] = "saved width=" + request.width + " height=" + request.height +
                        " view=" + viewId + " browser_pid=" + request.browserPid;
                } else if (request.state === "error" || request.state === "not-visible") captures[kind] = request.state;
                else return;
                record(last, true);
                return;
            }
            if (request.method === "renderer-error") {
                // Keep only the first short script failure. Exclude URLs or sign-in tokens.
                if (!rendererError) {
                    rendererError = String(request.message || "script error").slice(0, 180)
                        .replace(/https?:\/\/\S+/gi, "[url]").replace(/[\r\n]/g, " ");
                    // Independent stores can fail while the gamepad window goes on to render.
                    record(last, true);
                }
                return;
            }
            if (request.method === "surface-sizing") {
                var labels = ["view_width", "view_height", "screen_width", "screen_height", "display_scale_milli",
                    "bounds_width", "bounds_height", "raster_scale_milli", "css_width", "css_height", "dpr_milli", "backing_scale_milli"];
                if (Array.isArray(request.values) && request.values.length === labels.length &&
                    request.values.every(function (n) { return Number.isInteger(n) && n >= 0 && n <= 16384; })) {
                    surfaceSizing = labels.map(function (label, i) { return label + "=" + request.values[i]; }).join(" ");
                    record(last, true);
                }
                return;
            }
            if (request.method === "surface-state" && Number.isInteger(request.state) && request.state >= 1 && request.state <= 8) {
                surfaceState = ["", "setup", "library-or-home", "no-portal", "empty-portal", "error-boundary", "grid", "content", "empty-content"][request.state];
                record(last, true);
                return;
            }
            if (request.method === "startup-popup" && /^(?:[01]-){4}(?:[0-9]{1,2}-){4}[01]-[01]-[01]$/.test(request.phase)) {
                if (warming && request.phase !== popupPhase) warmProgress = Date.now();
                popupPhase = request.phase;
                record(last, true);
                return;
            }
            if (request.method === "startup-route" && /^(waiting-user|waiting-store|waiting-window|waiting-portal|selecting-library|library-selected|selection-pending)$/.test(request.stage)) {
                if (warming && request.stage !== startupRoute) warmProgress = Date.now();
                startupRoute = request.stage;
                if (request.stage === "library-selected") sampleMemory("library_selected");
                record(last, true);
                return;
            }
            var id = request.id, appId = request.appId;
            if (typeof id !== "number" || !isFinite(id) || id <= 0 || id > 0x7fffffff || id % 1) return;
            if (!broker) return respond(id, false, "signed-out");
            var action;
            if (request.method === "library") action = function () { return broker.library(); };
            else if (request.method === "session") action = function () { return broker.session(); };
            else if (request.method === "restart-client") action = function () {
                return broker.session().then(function (user) {
                    if (!user || !user.steamId) return "signed-out";
                    return settled && visible && !playing && !failed ? "restart" : "busy";
                });
            };
            else if (request.method === "system-mode") action = function () { return window.steamXboxSystemMode(Windows); };
            else if ((request.method === "play" || request.method === "install" || request.method === "resume-game" || request.method === "quit-game") &&
                     typeof appId === "number" && appId > 0 && appId <= 0xffffffff && appId % 1 === 0) {
                if (!settled) return respond(id, false, "unavailable");
                if (request.method === "resume-game" || request.method === "quit-game") action = function () {
                    return broker.session().then(function (user) {
                        if (!user || !user.steamId) return "signed-out";
                        if (request.method === "resume-game") return raiseGame(appId);
                        if (!visible || !played(appId) || !quitGame) return "unavailable";
                        if (!quitGame()) return "unavailable";
                        quitGame = null;
                        return "quit-requested";
                    });
                };
                else if (request.method === "install") action = function () { return broker.install(appId); };
                else action = function () {
                    if (playing) {
                        if (!played(appId) || !returnToGame) return "busy";
                        return broker.session().then(function (user) { return user && user.steamId ? raiseGame(appId) : "signed-out"; });
                    }
                    playing = true; playingApp = appId; gameFrames = {};
                    // Make the expensive view handoff only after the broker has
                    // verified this account, the title and its local installation.
                    return broker.play(appId, function () {
                        // The QR host creates the game's fullscreen presenter.
                        return show(mainId, viewId).then(function () {
                            visible = false;
                            setGameForeground(true);
                            sampleMemory("handoff");
                            return record("game-window-backgrounded");
                        });
                    }).then(function (outcome) {
                        if (outcome === "presented" || outcome === "window-ready") {
                            presented = playing && playingApp === appId && outcome === "presented";
                            sampleMemory("first_frame"); gameEvent(false);
                            record("game-" + outcome);
                            return outcome;
                        }
                        return resume().then(function () { return outcome; });
                    }, function (err) { return resume().then(function () { throw err; }); });
                };
            } else return respond(id, false, "unavailable");
            Promise.resolve().then(function () { if (!failed) return action(); }).then(function (result) {
                respond(id, true, result);
                if (request.method === "restart-client" && result === "restart" && !playing && !failed) {
                    record("client-restart");
                    host.reply(JSON.stringify({nativeRestartClient: true}));
                    return;
                }
                if (!failed && request.method === "library" && Array.isArray(result) && servedGames !== result.length) {
                    if (warming) warmProgress = Date.now();
                    servedGames = result.length;
                    record(last, true);
                }
            }, function () { respond(id, false, "failed"); });
        }
        function fail(error, stage) {
            var code = typeof error.number === "number" ? "0x" + (error.number >>> 0).toString(16) : "unavailable";
            finish(stage || "host-error:" + code);
        }
        function finish(stage) {
            if (failed) return;
            failed = true;
            clearInterval(timer);
            clearTimeout(readinessTimer);
            record(stage);
            var wasReady = settled;
            if (!settled) { settled = true; rejectReady(new Error(stage)); }
            if (broker) {
                var restore = activeSwitch || Promise.resolve();
                var current = session;
                current.cleanup = restore.catch(function () {}).then(function () {
                    if (visible && viewId) return show(mainId, viewId);
                }).catch(function () {}).then(function () {
                    if (!host) return;
                    try { host.close(); } catch (_) { return; }
                    // Closing the CoreWindow is dispatched to its UI thread.
                    var deadline = Date.now() + 2500;
                    function check() {
                        if (!host.viewId() || Date.now() >= deadline) return;
                        return new Promise(function (resolve) { setTimeout(resolve, 50); }).then(check);
                    }
                    return check();
                }).catch(function () {}).then(function () {
                    if (session === current) session = null;
                    if (wasReady && typeof onFailure === "function") onFailure(new Error(stage));
                });
            }
        }
        var lastNative, nextCaptureCheck = 0, lookingForCapture = false;
        function checkCaptureRequest() {
            if (!broker || !settled || !visible || failed || lookingForCapture || Date.now() < nextCaptureCheck) return;
            if (captures.library === "requested" || captures.surface === "requested") return;
            nextCaptureCheck = Date.now() + 4000;
            var folder = storage.ApplicationData.current.localFolder;
            if (!folder.tryGetItemAsync) return;
            lookingForCapture = true;
            var kind = "library";
            Promise.resolve().then(function () {
                return folder.tryGetItemAsync("big-picture-capture-library.request");
            }).then(function (request) {
                if (request) return request;
                kind = "surface";
                return folder.tryGetItemAsync("big-picture-surface.request");
            }).then(function (request) {
                if (!request || failed || !visible) return;
                return request.deleteAsync().then(function () {
                    if (failed || !visible) return;
                    captures[kind] = "requested";
                    host.reply(JSON.stringify(kind === "library" ? {nativeCaptureLibrary: true} : {nativeCaptureSurface: true}));
                    record(last, true);
                });
            }).then(function () { lookingForCapture = false; }, function () { lookingForCapture = false; });
        }
        function poll() {
            try {
                var stage = host.status();
                if (stage !== lastNative) {
                    if (warming) warmProgress = Date.now();
                    if (stage === "gamepad-window-created") sampleMemory("steam_popup");
                    if (stage === "gamepad-ready") sampleMemory("steam_ready");
                    record(stage);
                    lastNative = stage;
                }
                if (broker && /-error(?:$|:)/.test(stage)) return fail(new Error(stage), stage);
                if (!viewId) {
                    viewId = host.viewId();
                    if (viewId) {
                        if (warming) {
                            warmStarted = warmProgress = Date.now();
                            sampleMemory("warm_view");
                            warmReport = "preparing";
                            record(last, true);
                        } else resume().catch(fail);
                    }
                }
                if (!settled && visible && (stage === "gamepad-ready" || stage === "setup-ready")) {
                    sampleMemory("steam_foreground");
                    settled = true; clearTimeout(readinessTimer);
                    warming = false;
                    record(last, true);
                    resolveReady();
                }
                for (var i = 0; i < 16; ++i) {
                    var request = host.read();
                    if (!request) break;
                    handle(request);
                }
                if (warming && viewId && !warmSwitch) {
                    if (stage === "warmup-gamepad-layout" || stage === "warmup-setup-layout") handoffWarmup(stage);
                    else if (Date.now() - warmStarted >= 32000) handoffWarmup("time-limit");
                    else if (Date.now() - warmProgress >= 14000) handoffWarmup("no-layout-progress");
                }
                checkCaptureRequest();
            } catch (error) { fail(error); }
        }
        if (broker) readinessTimer = setTimeout(function () {
            fail(new Error("Timed out waiting for Valve's gamepad window"), "gamepad-timeout:" + (lastNative || "starting"));
        }, 120000);
        sampleMemory("startup");
        record("starting");
        var startup = Promise.resolve().then(function () {
            mainId = views.ApplicationView.getForCurrentView().id;
            host = BigPictureView.Native.Renderer;
            record("winrt-projected");
            record("status-before-open:" + host.status());
            warming = !!broker && typeof host.openForWarmup === "function";
            if (warming) host.openForWarmup();
            else host.open();
            record("open-returned");
            timer = setInterval(poll, 80);
            poll();
        }).catch(fail);
        if (!broker) ready.catch(function () {});
        session = {resume: resume, showForGame: showForGame, sampleMemory: sampleMemory, recordGameFrame: recordGameFrame,
            ready: broker ? ready : startup};
        return session.ready;
    }
    window.steamBigPictureHost = {
        open: open,
        switchView: switchView,
        recordGameFrame: function (appId, stage, stats) { if (session) session.recordGameFrame(appId, stage, stats); },
        samplePresenter: function (stage) {
            if (session && (stage === "presenter_created" || stage === "presenter_foreground")) session.sampleMemory(stage);
        },
        resume: function (appId) { return session ? session.resume(appId) : Promise.resolve(); },
        showForGame: function (appId, viewId, restore, requestExit) {
            return session ? session.showForGame(appId, viewId, restore, requestExit) : Promise.resolve(false);
        }
    };
}());
