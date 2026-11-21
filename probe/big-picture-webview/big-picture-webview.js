(function () {
    "use strict";
    var session;

    function open(broker) {
        if (session) return session.resume();
        var storage = Windows.Storage, views = Windows.UI.ViewManagement;
        var run = Windows.Foundation.GuidHelper.createNewGuid();
        var file = storage.ApplicationData.current.localFolder.createFileAsync("big-picture-webview-report.txt",
            storage.CreationCollisionOption.replaceExisting);
        var report = Promise.resolve(file);
        var messages = [], last = "";
        function record(stage) {
            if (stage === last) return report;
            last = stage;
            messages.push(stage);
            if (messages.length > 24) messages.shift();
            var text = "run_id=" + run + "\r\nstatus=" + stage + "\r\n" + messages.join("\r\n") + "\r\n";
            report = report.then(function (f) {
                return storage.FileIO.writeTextAsync(f, text).then(function () { return f; });
            });
            return report;
        }
        var host, viewId, mainId, visible = false, activeSwitch, timer, playing = false;
        function show(to, from) {
            return Promise.resolve(views.ApplicationViewSwitcher.tryShowAsStandaloneAsync(to)).then(function () {
                return views.ApplicationViewSwitcher.switchAsync(to, from);
            });
        }
        function resume() {
            playing = false;
            if (!viewId || visible) return Promise.resolve();
            if (!activeSwitch) activeSwitch = show(viewId, mainId).then(function () {
                visible = true; activeSwitch = null;
            }, function (err) { activeSwitch = null; throw err; });
            return activeSwitch;
        }
        function respond(id, ok, value) {
            if (ok) host.reply(JSON.stringify({id: id, result: value}));
            else host.reply(JSON.stringify({id: id, error: value}));
        }
        function handle(raw) {
            var request;
            try { request = JSON.parse(raw); } catch (_) { return; }
            if (!request || typeof request !== "object") return;
            if (request.method === "renderer-error") {
                // Keep only the first short script failure. Exclude URLs or sign-in tokens.
                if (!messages.some(function (m) { return m.indexOf("renderer-error:") === 0; })) {
                    var detail = String(request.message || "script error").slice(0, 180)
                        .replace(/https?:\/\/\S+/gi, "[url]").replace(/[\r\n]/g, " ");
                    record("renderer-error:" + detail);
                }
                return;
            }
            var id = request.id, appId = request.appId;
            if (typeof id !== "number" || id <= 0 || id > 0x7fffffff || id % 1) return;
            if (!broker) return respond(id, false, "signed-out");
            var action;
            if (request.method === "library") action = function () { return broker.library(); };
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
            Promise.resolve().then(action).then(function (result) { respond(id, true, result); },
                function () { respond(id, false, "failed"); });
        }
        function fail(error) {
            clearInterval(timer);
            var code = typeof error.number === "number" ? "0x" + (error.number >>> 0).toString(16) : "unavailable";
            return record("host-error:" + code);
        }
        var lastNative;
        function poll() {
            try {
                var stage = host.status();
                if (stage !== lastNative) { record(stage); lastNative = stage; }
                if (!viewId) {
                    viewId = host.viewId();
                    if (viewId) resume().catch(fail);
                }
                for (var i = 0; i < 16; ++i) {
                    var request = host.read();
                    if (!request) break;
                    handle(request);
                }
            } catch (error) { fail(error); }
        }
        session = {resume: resume};
        return record("starting").then(function () {
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
    }
    window.steamBigPictureHost = {
        open: open,
        resume: function () { return session ? session.resume() : Promise.resolve(); }
    };
}());
