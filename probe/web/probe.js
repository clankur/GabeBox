(function () {
    "use strict";

    var output = document.getElementById("result");
    function errorText(error) {
        var number = typeof error.number === "number" ? "0x" + (error.number >>> 0).toString(16) : "unavailable";
        return "hresult=" + number + "\r\nmessage=" + error.message + "\r\n";
    }

    function writeReport(storage, name, report) {
        output.textContent = report;
        return storage.ApplicationData.current.localFolder.createFileAsync(
            name, storage.CreationCollisionOption.openIfExists
        ).then(function (file) {
            return storage.FileIO.writeTextAsync(file, report);
        });
    }

    function reportError(error) {
        output.textContent = "Unable to write the LocalState report.\r\n" + errorText(error);
    }

    function controlProbe(storage) {
        var wasVisible = false;
        function publishForeground() {
            var visible = !document.hidden;
            if (visible && !wasVisible) {
                writeReport(storage, "probe-report.txt", "probe=packaged-startup-control\r\n" +
                    "run_id=" + Date.now() + "\r\ncontrol_visible=yes\r\nstatus=control-ready\r\n"
                ).done(null, reportError);
            }
            wasVisible = visible;
        }
        document.addEventListener("visibilitychange", publishForeground);
        publishForeground();
    }

    function lifetimeProbe(storage) {
        var header = "probe=packaged-helper-lifetime\r\nrun_id=" + Date.now() + "\r\n";
        var app = Windows.UI.WebUI.WebUIApplication;
        var hidden = false;
        var background = false;
        var suspended = false;
        var resumed = false;
        var ready = false;
        var finished = false;

        function invoke(method) {
            try {
                return method(XboxProbe.Native.ProbeRunner);
            } catch (error) {
                return "lifetime=failed\r\nlifetime_stage=native-component\r\n" + errorText(error);
            }
        }

        function flag(value) {
            return value ? "yes" : "no";
        }

        function finishIfVisible() {
            if (!ready || finished || document.hidden || (!hidden && !background)) return;
            finished = true;
            var result = invoke(function (runner) { return runner.finishLifetime(); });
            var events = "observed_hidden=" + flag(hidden) + "\r\nobserved_background=" + flag(background) +
                "\r\nobserved_suspending=" + flag(suspended) + "\r\nobserved_resuming=" + flag(resumed) + "\r\n";
            writeReport(storage, "lifetime-report.txt", header + events + result).done(null, reportError);
        }

        document.addEventListener("visibilitychange", function () {
            if (document.hidden) hidden = true;
            else finishIfVisible();
        });
        app.addEventListener("enteredbackground", function () { background = true; });
        app.addEventListener("leavingbackground", function () { setTimeout(finishIfVisible, 0); });
        app.addEventListener("suspending", function () { suspended = true; });
        app.addEventListener("resuming", function () { resumed = true; });

        writeReport(storage, "lifetime-report.txt", header + "lifetime=web-started\r\n").then(function () {
            var result = invoke(function (runner) { return runner.startLifetime(); });
            return writeReport(storage, "lifetime-report.txt", header + result).then(function () {
                ready = result.indexOf("lifetime=ready\r\n") === 0;
            });
        }).done(null, reportError);
    }

    try {
        var storage = Windows.Storage;
        var packageName = Windows.ApplicationModel.Package.current.id.name;
        if (packageName === "XboxPortabilityProbe.ChildProcess.Control") {
            controlProbe(storage);
            return;
        }
        if (packageName === "XboxPortabilityProbe.ChildProcess.Lifetime") {
            lifetimeProbe(storage);
            return;
        }
        var header = "probe=packaged-child-process\r\nrun_id=" + Date.now() + "\r\n";
        storage.ApplicationData.current.localFolder.createFileAsync(
            "probe-report.txt", storage.CreationCollisionOption.replaceExisting
        ).then(function (file) {
            return storage.FileIO.writeTextAsync(file, header + "status=web-started\r\n").then(function () {
                var result;
                try {
                    result = XboxProbe.Native.ProbeRunner.run();
                } catch (error) {
                    result = "status=native-component-error\r\n" + errorText(error);
                }
                output.textContent = result;
                return storage.FileIO.writeTextAsync(file, header + result);
            });
        }).done(null, reportError);
    } catch (error) {
        output.textContent = "Unable to start the diagnostic.\r\n" + errorText(error);
    }
})();
