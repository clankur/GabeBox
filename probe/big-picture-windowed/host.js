(function () {
    "use strict";
    try {
        WindowedControl.Native.Control.start();
    } catch (error) {
        var text = "stage=host-activation hresult=0x" + (Number(error.number) >>> 0).toString(16);
        document.getElementById("state").textContent = text;
        var storage = Windows.Storage;
        storage.ApplicationData.current.localFolder.createFileAsync("windowed-control.txt",
            storage.CreationCollisionOption.replaceExisting).then(function (file) {
                return storage.FileIO.writeTextAsync(file, text);
            });
    }
})();
