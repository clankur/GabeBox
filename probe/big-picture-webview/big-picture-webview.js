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
        var messages = [], last = "", servedGames, rendererError;
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
                (rendererError ? "renderer_error=" + rendererError + "\r\n" : "") + messages.join("\r\n") + "\r\n";
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
        function resume() {
            playing = false;
            if (failed || !viewId || visible) return Promise.resolve();
            if (!activeSwitch) activeSwitch = show(viewId, mainId).then(function () {
                visible = true; activeSwitch = null;
            }, function (err) { activeSwitch = null; throw err; });
            return activeSwitch;
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
            else if (request.method === "system-mode") action = function () { return window.steamXboxSystemMode(Windows); };
            else if ((request.method === "play" || request.method === "install") &&
                     typeof appId === "number" && appId > 0 && appId <= 0xffffffff && appId % 1 === 0) {
                if (request.method === "install") action = function () { return broker.install(appId); };
                else action = function () {
                    if (playing) return "busy";
                    playing = true;
                    return show(mainId, viewId).then(function () {
                        visible = false;
                        return broker.play(appId);
                    }).then(function (outcome) {
                        if (outcome === "presented" || outcome === "window-ready") return outcome;
                        return resume().then(function () { return outcome; });
                    }, function (err) { return resume().then(function () { throw err; }); });
                };
            } else return respond(id, false, "unavailable");
            Promise.resolve().then(function () { if (!failed) return action(); }).then(function (result) {
                respond(id, true, result);
                if (!failed && request.method === "library" && Array.isArray(result) && servedGames !== result.length) {
                    servedGames = result.length;
                    record(last, true);
                }
            }, function () { respond(id, false, "failed"); });
        }
        function fail(error, stage) {
            if (failed) return;
            failed = true;
            clearInterval(timer);
            clearTimeout(readinessTimer);
            var code = typeof error.number === "number" ? "0x" + (error.number >>> 0).toString(16) : "unavailable";
            stage = stage || "host-error:" + code;
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
                    if (wasReady && typeof onFailure === "function") onFailure(new Error(stage));
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
                if (!settled && visible && stage === "gamepad-ready") {
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
        session = {resume: resume, ready: broker ? ready : startup};
        return session.ready;
    }
    window.steamBigPictureHost = {
        open: open,
        resume: function () { return session ? session.resume() : Promise.resolve(); }
    };
}());
