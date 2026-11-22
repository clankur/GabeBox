(function () {
    "use strict";
    var storage = Windows.Storage;
    var local = storage.ApplicationData.current.localFolder;
    var observations = ["run_id=" + Windows.Foundation.GuidHelper.createNewGuid().toString()];
    function check(name, fn) {
        try {
            var value = fn();
            observations.push(name + "=" + String(value).replace(/[\r\n]/g, " ").slice(0, 80));
        } catch (error) {
            observations.push(name + "_error=" + (typeof error.number === "number"
                ? "0x" + (error.number >>> 0).toString(16) : error.name));
        }
    }
    // Exercise the two packaged WinRT components independently. A projection
    // appearing in JS does not establish that its DLL can be activated.
    check("renderer_metadata", function () {
        return Windows.Foundation.Metadata.ApiInformation.isTypePresent("BigPictureView.Native.Renderer");
    });
    check("renderer_status", function () { return BigPictureView.Native.Renderer.status(); });
    check("webview_version", function () {
        return Microsoft.Web.WebView2.Core.CoreWebView2Environment.getAvailableBrowserVersionString();
    });
    local.createFileAsync("big-picture-activation.txt", storage.CreationCollisionOption.replaceExisting)
        .then(function (file) { return storage.FileIO.writeTextAsync(file, observations.join("\r\n") + "\r\n"); });
    window.steamBigPictureHost.open(null);
}());
