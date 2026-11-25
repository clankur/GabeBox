// Runs in the trusted package view. Retail demo and assigned access are the
// host's restricted presentation modes. Steam account restrictions are separate.
window.steamXboxSystemMode = function (windows) {
    var observed = [];
    function observe(read) {
        try {
            var value = read();
            if (typeof value === "boolean") observed.push(value);
        } catch (_) {}
    }
    observe(function () { return windows.System.Profile.RetailInfo.isDemoModeEnabled; });
    observe(function () { return windows.System.UserProfile.AssignedAccessSettings.getDefault().isEnabled; });
    return !observed.length || observed.indexOf(true) >= 0;
};
