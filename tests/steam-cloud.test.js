const test = require("node:test");
const assert = require("node:assert/strict");
const { remoteStorageUrl } = require("../probe/signin/steam-cloud.js");

test("cloud saves opens Valve's per-game page without authentication data", () => {
    for (const appId of [413150, 975370, 646570, 4294967295]) {
        assert.equal(remoteStorageUrl(appId),
            `https://store.steampowered.com/account/remotestorageapp/?appid=${appId}`);
    }
});

test("cloud URLs require the numeric AppID format from the native library", () => {
    for (const appId of [null, undefined, "413150", "413150&token=value", true, NaN, Infinity, 0, -1, 1.5, 4294967296]) {
        assert.equal(remoteStorageUrl(appId), null);
    }
});
