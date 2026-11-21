(function () {
    "use strict";
    var session;

    function open(broker, onFailure) {
        if (session && session.cleanup) return session.cleanup.then(function () { return open(broker, onFailure); });
        if (session) return session.ready;
        var storage = Windows.Storage, views = Windows.UI.ViewManagement;
        var run = Windows.Foundation.GuidHelper.createNewGuid();
        var file = storage.ApplicationData.current.localFolder.createFileAsync("big-picture-webview-report.txt",
            storage.CreationCollisionOption.replaceExisting);
        var report = Promise.resolve(file);
        var messages = [], last = "", servedGames, rendererError, memory = {};
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
            } else if (!refresh) return report;
            var text = "run_id=" + run + "\r\nstatus=" + stage + "\r\n" +
                (servedGames === undefined ? "" : "owned_games_served=" + servedGames + "\r\n") +
                (rendererError ? "renderer_error=" + rendererError + "\r\n" : "") +
                Object.keys(memory).map(function (key) { return memory[key]; }).join("") + messages.join("\r\n") + "\r\n";
            report = report.then(function (f) {
                return storage.FileIO.writeTextAsync(f, text).then(function () { return f; });
            });
            return report;
        }
        var host, viewId, mainId, visible = false, activeSwitch, timer, playing = false;
        function show(to, from) {
            function switchToView() { return views.ApplicationViewSwitcher.switchAsync(to, from); }
            return Promise.resolve(views.ApplicationViewSwitcher.tryShowAsStandaloneAsync(to)).then(switchToView, switchToView);
        }
        var playingApp = 0, presented = false, returnToGame, quitGame;
        function gameEvent(overlay) {
            if (!host || failed) return;
            var hosted = playingApp === 646570 || playingApp === 975370 || playingApp === 413150;
            host.reply(JSON.stringify({event: "xbox-game", appId: presented && hosted ? playingApp : 0, overlay: overlay}));
        }
        function showValve(from) {
            if (failed || !viewId || visible) return Promise.resolve();
            if (!activeSwitch) activeSwitch = show(viewId, from || mainId).then(function () {
                visible = true; activeSwitch = null;
            }, function (error) { activeSwitch = null; throw error; });
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
                        return show(mainId, viewId).then(function () {
                            visible = false;
                            sampleMemory("handoff");
                            return record("game-window-backgrounded");
                        });
                    }).then(function (outcome) {
                        if (outcome === "presented" || outcome === "window-ready") {
                            presented = playing && playingApp === appId && outcome === "presented";
                            sampleMemory("first_frame"); gameEvent(false);
                            return record("game-" + outcome).then(function () { return outcome; });
                        }
                        return resume().then(function () { return outcome; });
                    }, function (err) { return resume().then(function () { throw err; }); });
                };
            } else return respond(id, false, "unavailable");
            Promise.resolve().then(function () { if (!failed) return action(); }).then(function (result) {
                respond(id, true, result);
                if (request.method === "restart-client" && result === "restart" && !playing && !failed) {
                    finish("client-restart", true);
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
            finish(stage || "host-error:" + code, false);
        }
        function finish(stage, restart) {
            if (failed) return;
            failed = true;
            clearInterval(timer);
            clearTimeout(readinessTimer);
            record(stage).catch(function () {});
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
                    if (restart) {
                        open(broker, onFailure).catch(function (error) { if (onFailure) onFailure(error); });
                    } else if (wasReady && typeof onFailure === "function") onFailure(new Error(stage));
                });
            }
        }
        var lastNative;
        function poll() {
            try {
                var stage = host.status();
                if (stage !== lastNative) { record(stage); lastNative = stage; }
                if (broker && /-error(?:$|:)/.test(stage)) return fail(new Error(stage), stage);
                if (!viewId) {
                    viewId = host.viewId();
                    if (viewId) resume().catch(fail);
                }
                if (!settled && visible && (stage === "gamepad-ready" || stage === "setup-ready")) {
                    settled = true; clearTimeout(readinessTimer); resolveReady();
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
        }, 45000);
        sampleMemory("startup");
        var startup = record("starting").then(function () {
            mainId = views.ApplicationView.getForCurrentView().id;
            host = BigPictureView.Native.Renderer;
            return record("winrt-projected").then(function () {
                var current = host.status();
                return record("status-before-open:" + current);
            }).then(function () {
                host.open();
                return record("open-returned");
            }).then(function () {
                timer = setInterval(poll, 80);
                poll();
            });
        }).catch(fail);
        if (!broker) ready.catch(function () {});
        session = {resume: resume, showForGame: showForGame, ready: broker ? ready : startup};
        return session.ready;
    }
    window.steamBigPictureHost = {
        open: open,
        resume: function (appId) { return session ? session.resume(appId) : Promise.resolve(); },
        showForGame: function (appId, viewId, restore, requestExit) {
            return session ? session.showForGame(appId, viewId, restore, requestExit) : Promise.resolve(false);
        }
    };
}());
