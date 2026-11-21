var steamCloud = (function () {
    "use strict";

    function supported(appId) {
        return appId === 646570;
    }

    function message(status) {
        var restored = /^restored:([1-9][0-9]*)$/.exec(status);
        if (restored) return "Restored " + restored[1] + " Steam save files. Choose Play to use them.";
        switch (status) {
            case "empty": return "Steam Cloud has no save files for this game.";
            case "no-compatible-saves": return "No Steam Cloud saves match this game's Xbox save folders.";
            case "session-unavailable":
            case "sign-in": return "Sign in to Steam to download cloud saves.";
            case "not-installed": return "Install the game first.";
            case "busy": return "Close the running game before restoring its saves.";
            case "storage-error": return "The app could not restore these saves. Check the app's available storage and try again.";
            default: return "Steam Cloud could not deliver a verified copy of the saves. Try again.";
        }
    }

    return { supported: supported, message: message };
}());

if (typeof module === "object" && module.exports) module.exports = steamCloud;
