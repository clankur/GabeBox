(function () {
    "use strict";

    var output = document.getElementById("result");
    var storage = Windows.Storage;
    var header = "probe=uwp-mouse-source\r\nrun_id=" + Date.now() + "\r\n";
    var counts = {callbacks: 0, motion: 0, pointer_moves: 0, pointer_down: 0, pointer_wheel: 0};
    var diagnostics = {};
    var currentView, mouse, originalCursor;
    var lostVisibility = false;
    var cleanedUp = false;
    var pointerEvents = ["pointermove", "pointerdown", "wheel"];

    function failure(error) {
        return typeof error.number === "number" ? "0x" + (error.number >>> 0).toString(16) : "unavailable";
    }

    function attempt(key, operation) {
        try {
            var result = operation();
            diagnostics[key] = "0x00000000";
            return result;
        } catch (error) {
            diagnostics[key] = failure(error);
        }
    }

    function activation() {
        return currentView && currentView.activationMode !== undefined ? currentView.activationMode : "unavailable";
    }

    function report(status, fields) {
        var message = header + "status=" + status + "\r\n";
        [diagnostics, fields || {}].forEach(function (group) {
            Object.keys(group).forEach(function (key) { message += key + "=" + group[key] + "\r\n"; });
        });
        output.textContent = message;
        return storage.ApplicationData.current.localFolder.createFileAsync("probe-report.txt",
            storage.CreationCollisionOption.openIfExists).then(function (file) {
            return storage.FileIO.writeTextAsync(file, message);
        });
    }

    function relative(event) {
        counts.callbacks++;
        var delta = event.mouseDelta;
        if (delta && (delta.x || delta.y)) counts.motion++;
    }

    function observePointer(event) {
        if (document.hidden || (event.pointerType && event.pointerType !== "mouse")) return;
        if (event.type === "pointermove") counts.pointer_moves++;
        if (event.type === "pointerdown") counts.pointer_down++;
        if (event.type === "wheel" && (event.deltaX || event.deltaY)) counts.pointer_wheel++;
    }

    function onVisibility() {
        if (document.hidden) lostVisibility = true;
    }

    function capture() {
        if (!currentView || !mouse) return;
        originalCursor = attempt("read_cursor_hresult", function () { return currentView.pointerCursor; });
        attempt("capture_hresult", function () { currentView.setPointerCapture(); });
        if (diagnostics.read_cursor_hresult === "0x00000000") {
            attempt("hide_cursor_hresult", function () { currentView.pointerCursor = null; });
        }
    }

    function cleanup() {
        if (cleanedUp) return;
        cleanedUp = true;
        if (mouse) attempt("remove_mouse_moved_hresult", function () { mouse.removeEventListener("mousemoved", relative); });
        if (diagnostics.capture_hresult === "0x00000000") {
            attempt("release_capture_hresult", function () { currentView.releasePointerCapture(); });
        }
        if (diagnostics.hide_cursor_hresult === "0x00000000") {
            attempt("restore_cursor_hresult", function () { currentView.pointerCursor = originalCursor; });
        }
        document.removeEventListener("visibilitychange", onVisibility);
        pointerEvents.forEach(function (name) { document.removeEventListener(name, observePointer); });
    }

    function finish() {
        cleanup();
        diagnostics.activation_end = activation();
        diagnostics.document_visible = lostVisibility || document.hidden ? "interrupted" : "yes";
        diagnostics.relative_motion = counts.motion ? "observed" : "absent";
        diagnostics.pointer_input = counts.pointer_moves || counts.pointer_down || counts.pointer_wheel ? "observed" : "absent";
        return report("mouse-sample-complete", counts);
    }

    function reportError(error) {
        cleanup();
        output.textContent = "Unable to write the LocalState report. " + failure(error);
    }

    function start() {
        currentView = attempt("core_window_hresult", function () {
            var view = Windows.UI.Core.CoreWindow.getForCurrentThread();
            if (!view) throw new Error("CoreWindow unavailable");
            return view;
        });
        mouse = attempt("mouse_subscription_hresult", function () {
            var device = Windows.Devices.Input.MouseDevice.getForCurrentView();
            device.addEventListener("mousemoved", relative);
            return device;
        });
        var capabilities = attempt("mouse_capabilities_hresult", function () {
            return new Windows.Devices.Input.MouseCapabilities();
        });
        if (capabilities) {
            diagnostics.mouse_present = capabilities.mousePresent;
            diagnostics.mouse_button_count = capabilities.numberOfButtons;
        }
        diagnostics.activation_start = activation();
        document.addEventListener("visibilitychange", onVisibility);
        pointerEvents.forEach(function (name) {
            document.addEventListener(name, observePointer);
        });
        report("waiting-for-mouse").then(function () {
            setTimeout(function () {
                diagnostics.uncaptured_motion = counts.motion;
                if (!counts.motion) capture();
                var status = diagnostics.capture_hresult === "0x00000000" ? "waiting-for-captured-mouse" : "waiting-for-mouse";
                report(status).then(function () {
                    setTimeout(function () { finish().then(null, reportError); }, 15000);
                }, reportError);
            }, 10000);
        }, reportError);
    }

    if (!document.hidden) start();
    else document.addEventListener("visibilitychange", function ready() {
        if (document.hidden) return;
        document.removeEventListener("visibilitychange", ready);
        start();
    });
}());
