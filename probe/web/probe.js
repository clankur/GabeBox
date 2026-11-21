(function () {
    "use strict";

    var output = document.getElementById("result");
    function errorText(error) {
        var number = typeof error.number === "number" ? "0x" + (error.number >>> 0).toString(16) : "unavailable";
        return "hresult=" + number + "\r\nmessage=" + error.message + "\r\n";
    }

    try {
        var storage = Windows.Storage;
        var control = Windows.ApplicationModel.Package.current.id.name === "XboxPortabilityProbe.ChildProcess.Control";
        var header = "probe=" + (control ? "packaged-startup-control" : "packaged-child-process") +
            "\r\nrun_id=" + Date.now() + "\r\n";
        storage.ApplicationData.current.localFolder.createFileAsync(
            "probe-report.txt", storage.CreationCollisionOption.replaceExisting
        ).then(function (file) {
            return storage.FileIO.writeTextAsync(file, header + "status=web-started\r\n").then(function () {
                var result;
                try {
                    result = control ? "status=control-ready\r\n" : XboxProbe.Native.ProbeRunner.run();
                } catch (error) {
                    result = "status=native-component-error\r\n" + errorText(error);
                }
                output.textContent = result;
                return storage.FileIO.writeTextAsync(file, header + result);
            });
        }).done(null, function (error) {
            output.textContent = "Unable to write the LocalState report.\r\n" + errorText(error);
        });
    } catch (error) {
        output.textContent = "Unable to start the diagnostic.\r\n" + errorText(error);
    }
})();
