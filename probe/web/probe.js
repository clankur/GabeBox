(function () {
    "use strict";

    var output = document.getElementById("result");
    function errorText(error) {
        var number = typeof error.number === "number" ? "0x" + (error.number >>> 0).toString(16) : "unavailable";
        return "hresult=" + number + "\r\nmessage=" + error.message + "\r\n";
    }

    try {
        var storage = Windows.Storage;
        var packageName = Windows.ApplicationModel.Package.current.id.name;
        var control = packageName === "XboxPortabilityProbe.ChildProcess.Control";
        var steamCore = packageName === "XboxPortabilityProbe.ChildProcess.SteamCore";
        var probeName = steamCore ? "packaged-steam-core" : "packaged-child-process";
        var header = "probe=" + (control ? "packaged-startup-control" : probeName) +
            "\r\nrun_id=" + Date.now() + "\r\n";
        storage.ApplicationData.current.localFolder.createFileAsync(
            "probe-report.txt", storage.CreationCollisionOption.replaceExisting
        ).then(function (file) {
            return storage.FileIO.writeTextAsync(file, header + "status=web-started\r\n").then(function () {
                function finish(inventory) {
                    var result;
                    try {
                        var probe = steamCore ? XboxProbe.Native.ProbeRunner.runSteamCore(inventory) :
                            (control ? "status=control-ready\r\n" : XboxProbe.Native.ProbeRunner.run());
                        result = probe;
                    } catch (error) {
                        result = "status=native-component-error\r\n" + errorText(error);
                    }
                    output.textContent = result;
                    return storage.FileIO.writeTextAsync(file, header + result);
                }

                if (!steamCore) return finish("");
                return Windows.ApplicationModel.Package.current.installedLocation
                    .tryGetItemAsync("steam-core-imports.txt").then(function (inventory) {
                        return inventory ? storage.FileIO.readTextAsync(inventory) : "";
                    }).then(finish);
            });
        }).done(null, function (error) {
            output.textContent = "Unable to write the LocalState report.\r\n" + errorText(error);
        });
    } catch (error) {
        output.textContent = "Unable to start the diagnostic.\r\n" + errorText(error);
    }
})();
