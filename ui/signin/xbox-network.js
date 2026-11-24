// Read the active Windows connection profile. Its Internet assessment describes
// OS reachability; it provides no evidence of an authenticated Steam connection.
window.steamXboxNetwork = function (windows, changed) {
    var connectivity = windows.Networking && windows.Networking.Connectivity;
    var info = connectivity && connectivity.NetworkInformation;
    var identifiers = Object.create(null), serial = 0, watching = false;
    function id(value) {
        // Keep private adapter GUIDs and the SSID out of broker IDs and diagnostics.
        if (!Object.prototype.hasOwnProperty.call(identifiers, value)) identifiers[value] = ++serial;
        return identifiers[value];
    }
    function read() {
        if (!info) return {level: "unknown"};
        try {
            var profile = info.getInternetConnectionProfile();
            if (!profile) return {level: "offline"};
            var level = profile.getNetworkConnectivityLevel();
            var levels = connectivity.NetworkConnectivityLevel;
            if (level === levels.none) return {level: "offline"};
            if (level !== levels.internetAccess && level !== levels.localAccess && level !== levels.constrainedInternetAccess)
                return {level: "unknown"};
            var adapter = profile.networkAdapter;
            if (!adapter) return {level: "unknown"};
            var type = adapter.ianaInterfaceType;
            var kind = profile.isWlanConnectionProfile || type === 71 ? "wifi" : type === 6 ? "ethernet" : "other";
            var snapshot = {level: level === levels.internetAccess ? "internet" : "local", kind: kind,
                device: id("adapter:" + String(adapter.networkAdapterId))};
            if (kind === "wifi") {
                var ssid = "";
                try { ssid = profile.wlanConnectionProfileDetails.getConnectedSsid(); } catch (_) {}
                if (typeof ssid === "string" && ssid.length > 0 && ssid.length <= 32) snapshot.ssid = ssid;
                snapshot.accessPoint = id("wifi:" + snapshot.device + ":" + (snapshot.ssid || ""));
                try {
                    var bars = profile.getSignalBars();
                    if (typeof bars === "number" && bars >= 0 && bars <= 5 && bars % 1 === 0) snapshot.bars = bars;
                } catch (_) {}
            }
            return snapshot;
        } catch (_) { return {level: "unknown"}; }
    }
    if (info) try { info.addEventListener("networkstatuschanged", changed); watching = true; } catch (_) {}
    return {read: read, close: function () {
        if (watching) info.removeEventListener("networkstatuschanged", changed);
        watching = false;
    }};
};
