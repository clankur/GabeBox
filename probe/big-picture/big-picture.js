(function () {
    "use strict";

    var storage = Windows.Storage;
    var runId = Windows.Foundation.GuidHelper.createNewGuid().toString();
    var prefix = "run_id=" + runId + "\r\nprobe=native-steam-big-picture\r\nstatus=";
    var pid;
    function hex(value) {
        return "0x" + ("00000000" + (value >>> 0).toString(16)).slice(-8);
    }
    function report(file, status, detail) {
        var processId = pid ? "\r\nclient_pid=" + hex(pid) : "";
        return storage.FileIO.writeTextAsync(file, prefix + status + processId + (detail || "") + "\r\n");
    }
    function errorReport(file, status, error) {
        var detail = typeof error.number === "number" ? "\r\nhresult=" + hex(error.number) : "";
        document.body.textContent = "Steam Big Picture could not start or be observed. " + status + detail;
        return report(file, status, detail);
    }
    function storageError() {
        document.body.textContent = "Steam Big Picture could not save the launch report.";
    }
    storage.ApplicationData.current.localFolder.createFileAsync("big-picture-report.txt",
        storage.CreationCollisionOption.replaceExisting).then(function (file) {
        return report(file, "launching").then(function () {
            var launcher;
            try {
                launcher = BigPicture.Native.Launcher;
            } catch (error) {
                return errorReport(file, "native-component-error", error);
            }
            try {
                pid = launcher.start();
            } catch (error) {
                return errorReport(file, "client-launch-error", error);
            }
            var started = Date.now();
            var recordedRunning = false;
            function poll() {
                var exitCode;
                try {
                    exitCode = launcher.pollExitCode();
                } catch (error) {
                    return errorReport(file, "observation-error", error);
                }
                if (exitCode >= 0) return report(file, "client-exited", "\r\nexit_code=" + hex(exitCode));
                if (!recordedRunning && Date.now() - started >= 10000) {
                    recordedRunning = true;
                    return report(file, "client-running").then(poll);
                }
                setTimeout(function () { Promise.resolve().then(poll).catch(storageError); }, 500);
            }
            return report(file, "client-created").then(poll);
        });
    }).then(null, storageError);
}());
