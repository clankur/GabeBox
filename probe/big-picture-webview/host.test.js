const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const test = require("node:test");

const source = fs.readFileSync(require.resolve("./big-picture-webview.js"), "utf8");
const drain = () => new Promise(resolve => setImmediate(resolve));

function fixture({activationError = false} = {}) {
    let stage = "closed", poll, timeout;
    const reports = [], replies = [], incoming = [];
    const renderer = {
        open() { stage = "composition"; },
        viewId() { return 10; },
        status() {
            if (activationError) throw {number: -2147024703};
            return stage;
        },
        read() { return incoming.shift(); },
        reply(reply) { replies.push(JSON.parse(reply)); },
        close() { stage = "closed"; }
    };
    const window = {};
    vm.runInNewContext(source, {
        window, BigPictureView: {Native: {Renderer: renderer}},
        Windows: {
            Foundation: {GuidHelper: {createNewGuid: () => "test"}},
            Storage: {
                ApplicationData: {current: {localFolder: {createFileAsync: async () => ({})}}},
                CreationCollisionOption: {replaceExisting: 0},
                FileIO: {writeTextAsync: async (_, report) => { reports.push(report); }}
            },
            UI: {ViewManagement: {
                ApplicationView: {getForCurrentView: () => ({id: 3})},
                ApplicationViewSwitcher: {tryShowAsStandaloneAsync: async () => true, switchAsync: async () => {}}
            }}
        },
        setInterval(callback) { poll = callback; return 1; }, clearInterval() {},
        setTimeout(callback) { timeout = callback; return 2; }, clearTimeout() {}
    });
    return {
        start: broker => window.steamBigPictureHost.open(broker),
        async advance(nextStage) { stage = nextStage; poll(); await drain(); },
        expire() { timeout(); },
        incoming, reports, replies
    };
}

test("activation failure rejects signed-in startup and preserves the HRESULT", async () => {
    const testApp = fixture({activationError: true});
    await assert.rejects(testApp.start({}), /host-error:0x800700c1/);
    await drain();
    assert.ok(testApp.reports.at(-1).endsWith("\r\n"));
    assert.match(testApp.reports.at(-1), /winrt-projected/);
});

test("a loaded document and an empty gamepad popup cannot resolve startup", async () => {
    const testApp = fixture();
    let ready = false;
    const startup = testApp.start({}).then(() => { ready = true; });
    await drain();
    await testApp.advance("document-loaded");
    await testApp.advance("gamepad-window-created");
    assert.equal(ready, false);
    await testApp.advance("gamepad-ready");
    await startup;
    assert.equal(ready, true);
});

test("the signed-in caller receives the renderer error or bounded timeout", async () => {
    const failed = fixture();
    const rejection = assert.rejects(failed.start({}), /SteamClient is not defined/);
    await drain();
    failed.incoming.push(JSON.stringify({method: "renderer-error", message: "SteamClient is not defined"}));
    await failed.advance("document-loaded");
    await rejection;

    const stalled = fixture();
    const deadline = assert.rejects(stalled.start({}), /gamepad-timeout:document-loaded/);
    await drain();
    await stalled.advance("document-loaded");
    stalled.expire();
    await deadline;
});

test("the uncredentialed smoke host never dispatches a broker operation", async () => {
    const testApp = fixture();
    await testApp.start(null);
    testApp.incoming.push('{"id":1,"method":"play","appId":10}');
    await testApp.advance("document-loaded");
    assert.deepEqual(testApp.replies, [{id: 1, error: "signed-out"}]);
});
