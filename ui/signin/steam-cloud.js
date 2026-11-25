var steamCloud = (function () {
    "use strict";

    function canRestore(appId) {
        return appId === 646570;
    }

    function supported(appId, trial) {
        return canRestore(appId) || (typeof appId === "number" && appId > 0 && appId === trial);
    }

    function trialApp(text, steamId) {
        var match = /^([1-9][0-9]{0,7}) ([1-9][0-9]{16,19})(?:\r?\n)?$/.exec(text);
        return match && match[0] === text && match[2] === steamId && +match[1] <= 0xffffff ? +match[1] : 0;
    }

    function message(status) {
        var restored = /^restored:([1-9][0-9]*)$/.exec(status);
        if (restored) return "Restored " + restored[1] + " Steam save files. Choose Play to use them.";
        var saved = /^snapshot:([1-9][0-9]*)$/.exec(status);
        if (saved) return "Saved a backup of " + saved[1] + " Steam save files on this Xbox.";
        switch (status) {
            case "empty": return "Steam Cloud has no save files for this game.";
            case "no-compatible-saves": return "No Steam Cloud saves match this game's Xbox save folders.";
            case "session-unavailable":
            case "sign-in": return "Sign in to Steam to download cloud saves.";
            case "not-installed": return "Install the game first.";
            case "busy": return "Wait for the current Steam activity to finish before downloading saves.";
            case "storage-error": return "The app could not save these files. Check the app's available storage and try again.";
            default: return "Steam Cloud could not deliver a verified copy of the saves. Try again.";
        }
    }

    function backups(status) {
        var count = /^snapshots:([0-9]+)$/.exec(status);
        if (!count) return "Saved backups are unavailable.";
        if (count[1] === "0") return "No backups saved on this Xbox.";
        return count[1] + (count[1] === "1" ? " backup saved on this Xbox." : " backups saved on this Xbox.");
    }

    return { canRestore: canRestore, supported: supported, trialApp: trialApp, message: message, backups: backups };
}());

if (typeof module === "object" && module.exports) module.exports = steamCloud;
