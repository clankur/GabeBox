(function () {
    "use strict";

    var activeSession = 0;

    function openMedia(host, path, events, nativePad, screen, mainId) {
        var video = document.createElement("video");
        var timer, url, closed = false, opened = false, shown = false;
        var sampleWindow = 0;
        var started = Date.now(), owner = ++activeSession, sampleStart, nextReport = 0, phase = "open";
        var capturePending = false;
        function resetSample() { sampleStart = null; }
        function close() {
            if (closed) return;
            closed = true;
            clearInterval(timer);
            document.removeEventListener("visibilitychange", resetSample);
            video.removeEventListener("error", failed);
            video.pause();
            video.removeAttribute("src");
            if (video.parentNode) video.parentNode.removeChild(video);
            if (opened) { try { host.closeMedia(); } catch (_) {} }
            if (url) URL.revokeObjectURL(url);
            screen.classList.remove("primary-game-video");
        }
        function failed(error) {
            if (closed) return;
            var code = error && (error.number || error.code) || video.error && video.error.code || 0;
            if (events.mediaError) events.mediaError(phase, code >>> 0);
            close();
            events.fallback();
        }
        function visible() {
            if (document.hidden || screen.hidden) return false;
            var core = Windows.UI.Core;
            if (core && core.CoreWindow && typeof core.CoreWindow.getForCurrentThread === "function") {
                var current = core.CoreWindow.getForCurrentThread();
                if (current && core.CoreWindowActivationMode && typeof current.activationMode === "number" &&
                    current.activationMode !== core.CoreWindowActivationMode.activatedInForeground) return false;
            }
            return !video.paused && video.readyState >= 2 &&
                video.videoWidth > 0 && video.videoHeight > 0 && video.getBoundingClientRect().width > 0;
        }
        function captureFrame() {
            var folder = Windows.Storage.ApplicationData.current.localFolder, temporary, requested = false, captureStats;
            var width = video.videoWidth, height = video.videoHeight;
            function currentVideo() {
                return !closed && owner === activeSession && events.running() && visible() &&
                    video.videoWidth === 1920 && video.videoHeight === 1080 &&
                    screen.contains(document.elementFromPoint(window.innerWidth / 2, window.innerHeight / 2));
            }
            if (capturePending || !events.mediaCapture || !folder.tryGetItemAsync || !shown) return;
            capturePending = true;
            Promise.resolve().then(function () {
                return folder.tryGetItemAsync("big-picture-capture-game.request");
            }).then(function (request) {
                if (!request || !currentVideo()) return;
                requested = true;
                return request.deleteAsync().then(function () {
                    if (!currentVideo()) throw new Error("game-hidden");
                    // Read only the video element that Windows is playing on this
                    // view. The resulting PNG is distinct from Xbox TV output.
                    var canvas = document.createElement("canvas");
                    canvas.width = width; canvas.height = height;
                    canvas.getContext("2d").drawImage(video, 0, 0);
                    captureStats = "capture_saved=1\r\nwidth=" + width + "\r\nheight=" + height +
                        "\r\nvisible=1\r\nprimary_media=1\r\nvideo_ms=" + Math.max(0, Math.round(video.currentTime * 1000)) +
                        "\r\n" + host.mediaStatus();
                    return new Promise(function (resolve, reject) {
                        function encoded(blob) {
                            if (!blob || blob.type !== "image/png" || blob.size > 12 * 1024 * 1024) {
                                reject(new Error("game-encode")); return;
                            }
                            var reader = new FileReader();
                            reader.onerror = function () { reject(new Error("game-read")); };
                            reader.onload = function () { resolve(new Uint8Array(reader.result)); };
                            reader.readAsArrayBuffer(blob);
                        }
                        if (canvas.msToBlob) encoded(canvas.msToBlob());
                        else canvas.toBlob(encoded, "image/png");
                    });
                }).then(function (bytes) {
                    if (!currentVideo()) throw new Error("game-hidden");
                    return folder.createFileAsync("big-picture-game-video.tmp", Windows.Storage.CreationCollisionOption.generateUniqueName)
                        .then(function (file) {
                            temporary = file;
                            var buffer = Windows.Security.Cryptography.CryptographicBuffer.createFromByteArray(bytes);
                            return Windows.Storage.FileIO.writeBufferAsync(file, buffer);
                        });
                }).then(function () {
                    if (!currentVideo()) throw new Error("game-hidden");
                    return temporary.renameAsync("big-picture-game-video.png", Windows.Storage.NameCollisionOption.replaceExisting);
                }).then(function () {
                    temporary = null;
                    if (currentVideo()) events.mediaCapture(captureStats);
                });
            }).catch(function () {
                if (requested && !closed) events.mediaCapture("capture_saved=0\r\nprimary_media=1");
                if (temporary) return Promise.resolve(temporary.deleteAsync()).catch(function () {});
            }).then(function () { capturePending = false; });
        }
        function poll() {
            if (closed) return;
            try {
                if (!events.running()) { close(); events.exited(); return; }
                var stats = host.mediaStatus();
                var error = /^error=([0-9]+)\r?$/m.exec(stats);
                if (error) { failed({code:Number(error[1])}); return; }
                if (events.mediaProgress && Date.now() >= nextReport) {
                    nextReport = Date.now() + 5000;
                    events.mediaProgress(stats);
                    captureFrame();
                }
                if (visible()) {
                    var count = /^presented=([0-9]+)\r?$/m.exec(stats);
                    var foreground = "visible=1\r\nprimary_media=1\r\n";
                    if (!shown && count && +count[1] > 0) {
                        shown = true;
                        events.presented(stats + "\r\n" + foreground);
                    }
                    if (shown && events.measured && count) {
                        var now = performance.now(), rendered = +count[1];
                        if (!sampleStart) sampleStart = {time: now, frames: rendered};
                        var elapsed = Math.round((now - sampleStart.time) * 1000);
                        var unique = rendered - sampleStart.frames;
                        if (elapsed >= 30000000) {
                            sampleStart = {time: now, frames: rendered};
                            ++sampleWindow;
                            var visibleSample = stats.replace(/^sample_(?:us|frames)=.*\r?$/gm, "") + "\r\n" +
                                "sample_window=" + sampleWindow + "\r\nsample_us=" + elapsed +
                                "\r\nsample_frames=" + unique + "\r\n" + foreground;
                            events.measured(unique, elapsed, visibleSample);
                        }
                    }
                } else resetSample();
                if (!shown && Date.now() - started > 30000) failed();
            } catch (_) { failed(); }
        }
        function resume(fromView) {
            if (closed || owner !== activeSession) return Promise.resolve(false);
            return Promise.resolve(fromView ? window.steamBigPictureHost.switchView(mainId, fromView) : undefined)
                .then(function () {
                    if (closed || owner !== activeSession) return false;
                    return Promise.resolve(video.play()).then(function () { return !closed; });
                });
        }
        try {
            var source = host.openMedia(path, nativePad !== false);
            opened = true;
            phase = "attach";
            url = URL.createObjectURL(source);
            video.className = "sdl-primary-video";
            video.autoplay = true;
            video.muted = true; // The running game already owns audio playback.
            video.setAttribute("aria-hidden", "true");
            video.addEventListener("error", failed);
            document.addEventListener("visibilitychange", resetSample);
            screen.classList.add("primary-game-video");
            screen.appendChild(video);
            video.src = url;
            phase = "playback";
            Promise.resolve(video.play()).catch(failed);
            timer = setInterval(poll, 100);
            if (window.steamBigPictureHost.samplePresenter) {
                window.steamBigPictureHost.samplePresenter("presenter_created");
                window.steamBigPictureHost.samplePresenter("presenter_foreground");
            }
        } catch (error) { failed(error); return null; }
        return {primary: true, close: close, resume: resume};
    }

    function open(report, events, nativePad, primaryScreen) {
        var mailbox = /^steam_sdl_mailbox=(sdl-host-[0-9]+-[0-9]+\.bin)\r?$/m.exec(report);
        var host = window.Fhd && window.Fhd.Native && window.Fhd.Native.RenderHost;
        if (!mailbox || !host) return null;

        var manager = Windows.UI.ViewManagement, switcher = manager.ApplicationViewSwitcher;
        var mainId = manager.ApplicationView.getForCurrentView().id;
        if (primaryScreen && typeof host.openMedia === "function") {
            var mediaPath = Windows.Storage.ApplicationData.current.localFolder.path + "\\" + mailbox[1];
            return openMedia(host, mediaPath, events, nativePad, primaryScreen, mainId);
        }
        var id = 0, timer, closed = false, switching = false, pendingShow, restored;
        var session = 0;
        var overlayOpen = false, opened = false, shown = false, ready = false, activated = false;
        var measured = false;
        var started = Date.now();
        function close() {
            if (closed) return restored;
            closed = true;
            clearInterval(timer);
            if (opened) {
                try { host.close(); } catch (_) {}
            }
            if (id && (shown || switching)) {
                // A show may finish after the game exits. Restore the main view after
                // that operation settles, unless a newer game has already taken over.
                function restore() {
                    if (session !== activeSession) return;
                    var steam = window.steamBigPictureHost;
                    if (steam && steam.switchView) return steam.switchView(mainId, id).catch(function () {});
                    function switchToLibrary() {
                        if (session === activeSession) return switcher.switchAsync(mainId, id);
                    }
                    return Promise.resolve().then(function () {
                        return switcher.tryShowAsStandaloneAsync(mainId);
                    }).then(switchToLibrary, switchToLibrary).catch(function () {});
                }
                restored = Promise.resolve(pendingShow).then(restore, restore);
            }
            return restored;
        }
        function failed() {
            if (closed) return;
            close();
            events.fallback();
        }
        function resume(fromView) {
            if (!id || closed) return Promise.resolve(false);
            if (switching) return pendingShow;
            if (overlayOpen) {
                try { host.start(); } catch (_) { failed(); return Promise.resolve(false); }
                overlayOpen = false;
            }
            switching = true;
            function switchToGame() {
                if (!closed) return switcher.switchAsync(id, fromView || mainId);
            }
            var steam = window.steamBigPictureHost;
            if (steam && steam.samplePresenter) steam.samplePresenter("presenter_created");
            if (activated && steam && steam.switchView) {
                pendingShow = steam.switchView(id, fromView || mainId);
            } else pendingShow = Promise.resolve().then(function () {
                // Register a newly created game view with the Xbox shell before
                // switching to it, as in the verified fullscreen FHD presenter.
                if (!closed) return switcher.tryShowAsStandaloneAsync(id);
            }).then(switchToGame, switchToGame);
            pendingShow = pendingShow.then(function () {
                switching = false;
                shown = !closed;
                activated = activated || shown;
                if (shown && steam && steam.samplePresenter) steam.samplePresenter("presenter_foreground");
                return shown;
            }, function () { failed(); return false; });
            return pendingShow;
        }
        function poll() {
            if (closed) return;
            try {
                if (!events.running()) {
                    close();
                    events.exited();
                    return;
                }
                var stats = host.status();
                if (/^(?:error=|status=(?:failed|error)\b)/m.test(stats)) return failed();
                if (!id) {
                    id = host.viewId();
                    if (id) resume();
                }
                var count = /^(?:presented|unique_present_calls)=([0-9]+)\r?$/m.exec(stats);
                if (!ready && shown && count && +count[1] > 0) {
                    ready = true;
                    events.presented(stats);
                }
                if (!ready && Date.now() - started > 12000) return failed();
                if (ready && !measured && events.measured) {
                    var duration = /^sample_us=([0-9]+)\r?$/m.exec(stats);
                    var frames = /^sample_frames=([0-9]+)\r?$/m.exec(stats);
                    if (duration && +duration[1] > 0 && frames) {
                        measured = true;
                        events.measured(+frames[1], +duration[1], stats);
                    }
                }
                var overlay = /^overlay_requested=([0-9]+)\r?$/m.exec(stats);
                if (overlay && +overlay[1] && !overlayOpen) {
                    overlayOpen = true;
                    shown = false;
                    events.menu(id);
                }
            } catch (_) { failed(); }
        }

        try {
            var root = Windows.Storage.ApplicationData.current.localFolder.path;
            host.open(root + "\\" + mailbox[1]);
            opened = true;
            session = ++activeSession;
            if (host.setNativeGamepad) host.setNativeGamepad(nativePad !== false);
            host.start();
            timer = setInterval(poll, 100);
        } catch (_) { close(); return null; }
        return { close: close, resume: resume };
    }

    window.steamSdlNativeView = { open: open };
}());
