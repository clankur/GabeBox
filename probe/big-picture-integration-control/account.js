(function () {
    "use strict";
    var folder = Windows.Storage.ApplicationData.current.localFolder;
    var name = "primary-renderer-control.txt";
    var host = BigPictureView.Native.Renderer;
    var report = ["run=" + Windows.Foundation.GuidHelper.createNewGuid()];
    var pending = Promise.resolve(), last, visible = false;
    var primary = Windows.UI.ViewManagement.ApplicationView.getForCurrentView().id;
    function record(value) {
        report.push(value);
        var text = report.join("\r\n") + "\r\n";
        pending = pending.then(function () {
            return folder.createFileAsync(name, Windows.Storage.CreationCollisionOption.openIfExists);
        }).then(function (file) { return Windows.Storage.FileIO.writeTextAsync(file, text); });
    }
    record("primary_view=" + primary);
    // This control package has no session component or real accounts. A reserved
    // fixture ID gives the actual renderer its normal per-owner storage path.
    window.addEventListener("load", function () {
        setTimeout(function () {
            host.openForPrimaryAccount("76561198000000000");
            var deadline = Date.now() + 60000;
            var timer = setInterval(function () {
                var stage = host.status();
                if (stage !== last) { last = stage; record("stage=" + stage); }
                if (stage === "document-loaded" && !visible) {
                    var same = host.viewId() === primary;
                    record("same_primary_view=" + same);
                    if (!same) { clearInterval(timer); return; }
                    host.reply(JSON.stringify({nativeWarmupComplete:true}));
                    visible = true;
                }
                for (var i = 0; i < 16; ++i) {
                    var message = host.read();
                    if (!message) break;
                    var data = JSON.parse(message);
                    if (data.method === "integration-control" && data.phase === "paint" && data.view === primary && visible) {
                        record("browser_frame_callback=received");
                        clearInterval(timer);
                        return;
                    }
                }
                if (stage.indexOf("-error") >= 0 || Date.now() >= deadline) clearInterval(timer);
            }, 100);
        }, 250);
    });
}());
