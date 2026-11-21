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
        var messages = [], last = "", servedGames, rendererError, surfaceState, startupRoute, popupPhase, memory = {};
        function sampleMemory(stage) {
            try {
                var manager = Windows.System.MemoryManager;
                var usage = manager.appMemoryUsage, limit = manager.appMemoryUsageLimit;
                if (typeof usage === "number" && isFinite(usage) && usage >= 0 &&
                    typeof limit === "number" && isFinite(limit) && limit > 0)
                    memory[stage] = "memory_" + stage + "_usage_bytes=" + usage + "\r\n" +
                        "memory_" + stage + "_limit_bytes=" + limit + "\r\n";
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
                (surfaceState ? "valve_surface=" + surfaceState + "\r\n" : "") +
                (popupPhase ? "valve_popup=" + popupPhase + "\r\n" : "") +
                (startupRoute ? "valve_startup=" + startupRoute + "\r\n" : "") +
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
            if (request.method === "surface-state" && Number.isInteger(request.state) && request.state >= 1 && request.state <= 8) {
                surfaceState = ["", "setup", "library-or-home", "no-portal", "empty-portal", "error-boundary", "grid", "content", "empty-content"][request.state];
                record(last, true);
                return;
            }
            if (request.method === "startup-popup" && /^(?:[01]-){4}(?:[0-9]{1,2}-){4}[01]-[01]-[01]$/.test(request.phase)) {
                popupPhase = request.phase;
                record(last, true);
                return;
            }
            if (request.method === "startup-route" && /^(waiting-user|waiting-store|waiting-window|waiting-portal|selecting-library|library-selected|selection-pending)$/.test(request.stage)) {
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
                    playing = true; playingApp = appId;
                    // Make the expensive view handoff only after the broker has
                    // verified this account, the title and its local installation.
                    return broker.play(appId, function () {
                        // Slay exceeded the foreground budget on the separate FHD view.
                        // Keep its game's swapchain in the existing Valve CoreWindow.
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
        var lastNative;
        function poll() {
            try {
                var stage = host.status();
                if (stage !== lastNative) {
                    if (stage === "gamepad-window-created") sampleMemory("steam_popup");
                    if (stage === "gamepad-ready") sampleMemory("steam_ready");
                    record(stage);
                    lastNative = stage;
                }
                if (broker && /-error(?:$|:)/.test(stage)) return fail(new Error(stage), stage);
                if (!viewId) {
                    viewId = host.viewId();
                    if (viewId) resume().catch(fail);
                }
                if (!settled && visible && (stage === "gamepad-ready" || stage === "setup-ready")) {
                    settled = true; clearTimeout(readinessTimer);
                    sampleMemory("steam_foreground");
                    record(last, true);
                    resolveReady();
                }
                for (var i = 0; i < 16; ++i) {
                    var request = host.read();
                    if (!request) break;
                    handle(request);
                }
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
            if (broker) host.openInCurrentView();
            else host.open();
            record("open-returned");
            timer = setInterval(poll, 80);
            poll();
        }).catch(fail);
        if (!broker) ready.catch(function () {});
        session = {resume: resume, showForGame: showForGame, sampleMemory: sampleMemory, ready: broker ? ready : startup};
        return session.ready;
    }
    window.steamBigPictureHost = {
        open: open,
        switchView: switchView,
        samplePresenter: function (stage) {
            if (session && (stage === "presenter_created" || stage === "presenter_foreground")) session.sampleMemory(stage);
        },
        resume: function (appId) { return session ? session.resume(appId) : Promise.resolve(); },
        showForGame: function (appId, viewId, restore, requestExit) {
            return session ? session.showForGame(appId, viewId, restore, requestExit) : Promise.resolve(false);
        }
    };
}());
