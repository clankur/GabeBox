(function () {
    "use strict";

    var activeSession = 0;

    function open(report, events, nativePad) {
        var mailbox = /^steam_sdl_mailbox=(sdl-host-[0-9]+-[0-9]+\.bin)\r?$/m.exec(report);
        var host = window.Fhd && window.Fhd.Native && window.Fhd.Native.RenderHost;
        if (!mailbox || !host) return null;

        var manager = Windows.UI.ViewManagement, switcher = manager.ApplicationViewSwitcher;
        var mainId = manager.ApplicationView.getForCurrentView().id;
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
