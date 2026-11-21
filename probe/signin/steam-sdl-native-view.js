(function () {
    "use strict";

    function open(report, events) {
        var mailbox = /^steam_sdl_mailbox=(sdl-host-[0-9]+-[0-9]+\.bin)\r?$/m.exec(report);
        var host = window.Fhd && window.Fhd.Native && window.Fhd.Native.RenderHost;
        if (!mailbox || !host) return null;

        var manager = Windows.UI.ViewManagement, switcher = manager.ApplicationViewSwitcher;
        var mainId = manager.ApplicationView.getForCurrentView().id;
        var id = 0, timer, closed = false, switching = false;
        var previousOverlay = 0, opened = false, shown = false, ready = false;
        var started = Date.now();
        function close() {
            if (closed) return;
            closed = true;
            clearInterval(timer);
            if (opened) host.close();
        }
        function failed() {
            if (closed) return;
            close();
            events.fallback();
        }
        function resume() {
            if (!id || switching || closed) return;
            switching = true;
            function switchToGame() {
                if (!closed) return switcher.switchAsync(id, mainId);
            }
            Promise.resolve(switcher.tryShowAsStandaloneAsync(id)).then(switchToGame, switchToGame).then(function () {
                switching = false;
                shown = true;
            }, failed);
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
                    events.presented();
                }
                if (!ready && Date.now() - started > 12000) return failed();
                var overlay = /^overlay_requested=([0-9]+)\r?$/m.exec(stats);
                if (overlay && +overlay[1] > previousOverlay) {
                    previousOverlay = +overlay[1];
                    shown = false;
                    events.menu();
                }
            } catch (_) { failed(); }
        }

        try {
            var root = Windows.Storage.ApplicationData.current.localFolder.path;
            host.open(root + "\\" + mailbox[1]);
            opened = true;
            host.start();
            timer = setInterval(poll, 100);
        } catch (_) { close(); return null; }
        return { close: close, resume: resume };
    }

    window.steamSdlNativeView = { open: open };
}());
