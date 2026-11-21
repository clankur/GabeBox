var steamCloud = (function () {
    "use strict";

    function remoteStorageUrl(appId) {
        if (typeof appId !== "number" || Math.floor(appId) !== appId || appId < 1 || appId > 4294967295) {
            return null;
        }
        return "https://store.steampowered.com/account/remotestorageapp/?appid=" + appId;
    }

    return { remoteStorageUrl: remoteStorageUrl };
}());

if (typeof module === "object" && module.exports) module.exports = steamCloud;
