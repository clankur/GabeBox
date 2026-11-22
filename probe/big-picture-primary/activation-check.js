// This isolated identity contains no Steam code, account state, network capability, or broker.
(function () {
    "use strict";
    var label = document.getElementById("status");
    try {
        var storage = Windows.Storage;
        var version = Windows.ApplicationModel.Package.current.id.version;
        var receipt = "account_script=ready\nversion=" +
            [version.major, version.minor, version.build, version.revision].join(".") + "\n";
        storage.ApplicationData.current.localFolder.createFileAsync("big-picture-primary-proof.txt",
            storage.CreationCollisionOption.replaceExisting).then(function (file) {
                return storage.FileIO.writeTextAsync(file, receipt);
            }).then(function () {
                label.textContent = "The local XAML WebView loaded and wrote the projection receipt.";
            }, function () { label.textContent = "Local projection receipt failed."; });
    } catch (error) {
        label.textContent = "Local Windows Runtime projection unavailable.";
    }
})();
