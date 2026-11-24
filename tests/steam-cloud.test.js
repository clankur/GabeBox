const test = require("node:test");
const assert = require("node:assert/strict");
const { supported, message } = require("../ui/signin/steam-cloud.js");

test("only Slay the Spire has an Xbox cloud save destination", () => {
    assert.equal(supported(646570), true);
    for (const appId of [413150, 975370, "646570", undefined]) assert.equal(supported(appId), false);
});

test("verified restores enable Play while transport errors show a retry", () => {
    assert.match(message("restored:67"), /67 Steam save files/);
    assert.match(message("empty"), /no save files/);
    assert.match(message("unverified"), /could not deliver a verified copy/);
});
