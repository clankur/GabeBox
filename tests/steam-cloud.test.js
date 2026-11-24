const test = require("node:test");
const assert = require("node:assert/strict");
const { canRestore, supported, trialApp, message, backups } = require("../ui/signin/steam-cloud.js");

test("only Slay the Spire has an Xbox cloud save destination", () => {
    assert.equal(supported(646570), true);
    for (const appId of [413150, 975370, "646570", undefined]) assert.equal(supported(appId), false);
});

test("verified restores enable Play while transport errors show a retry", () => {
    assert.match(message("restored:67"), /67 Steam save files/);
    assert.match(message("empty"), /no save files/);
    assert.match(message("unverified"), /could not deliver a verified copy/);
});

test("a signed trial exposes read-only backups only for its selected game and account", () => {
    const owner = "76561197960268001";
    const trial = "413150 " + owner;
    assert.equal(trialApp(trial + "\r\n", owner), 413150);
    assert.equal(trialApp(trial, "76561197960268002"), 0);
    assert.equal(trialApp(trial + "\n\n", owner), 0);
    assert.equal(trialApp("16777216 " + owner, owner), 0);
    assert.equal(supported(413150, 413150), true);
    assert.equal(supported(975370, 413150), false);
    assert.equal(canRestore(413150), false);
});

test("backups report a committed transfer, previous versions and empty results", () => {
    assert.match(message("snapshot:67"), /Saved a backup of 67 Steam save files/);
    assert.match(backups("snapshots:2"), /2 backups saved/);
    assert.match(backups("snapshots:0"), /No backups saved/);
    assert.match(backups("sign-in"), /unavailable/);
});
