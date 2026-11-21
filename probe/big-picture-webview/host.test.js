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
        viewId() { return stage === "closed" ? 0 : 10; },
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
        start: (broker, failure) => window.steamBigPictureHost.open(broker, failure),
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

test("an optional script failure is recorded while Valve continues opening its gamepad window", async () => {
    const app = fixture();
    const opened = app.start({});
    await drain();
    app.incoming.push(JSON.stringify({method: "renderer-error", message: "optional service failed at https://example.com/personal"}));
    await app.advance("document-loaded");
    await app.advance("gamepad-window-created");
    await app.advance("gamepad-ready");
    await opened;
    const report = app.reports.at(-1);
    assert.match(report, /status=gamepad-ready/);
    assert.match(report, /renderer_error=optional service failed at \[url\]/);
    assert.doesNotMatch(report, /example.com/);
});

test("timeout retains the first script failure and last native milestone", async () => {
    const app = fixture();
    const deadline = assert.rejects(app.start({}), /gamepad-timeout:gamepad-window-created/);
    await drain();
    app.incoming.push(JSON.stringify({method: "renderer-error", message: "First failure"}));
    app.incoming.push(JSON.stringify({method: "renderer-error", message: "Second failure"}));
    await app.advance("document-loaded");
    await app.advance("gamepad-window-created");
    app.expire();
    await deadline;
    await drain();
    const report = app.reports.at(-1);
    assert.match(report, /status=gamepad-timeout:gamepad-window-created/);
    assert.match(report, /renderer_error=First failure/);
    assert.doesNotMatch(report, /Second failure/);
});

test("the uncredentialed smoke host never dispatches a broker operation", async () => {
    const testApp = fixture();
    await testApp.start(null);
    testApp.incoming.push('{"id":1,"method":"play","appId":10}');
    await testApp.advance("document-loaded");
    assert.deepEqual(testApp.replies, [{id: 1, error: "signed-out"}]);
});

test("a failed startup can be retried and a later process failure reaches the trusted shell", async () => {
    const app = fixture();
    const rejected = assert.rejects(app.start({}), /environment-error/);
    await drain();
    await app.advance("environment-error:-1");
    await rejected;
    let failure;
    const retried = app.start({}, error => { failure = error; });
    await drain();
    await app.advance("gamepad-ready");
    await retried;
    await app.advance("process-error:3");
    await drain();
    assert.match(failure.message, /process-error:3/);
});

test("session and system policy requests require a signed-in broker and keep account data out of reports", async () => {
    const app = fixture();
    const identity = "76561199999999999";
    const startup = app.start({session: async () => ({steamId: identity})});
    await drain();
    app.incoming.push('{"id":11,"method":"session"}');
    await app.advance("gamepad-ready");
    await startup;
    assert.deepEqual(app.replies, [{id: 11, result: {steamId: identity}}]);
    assert.ok(app.reports.every(report => !report.includes(identity)));

    const smoke = fixture();
    await smoke.start(null);
    smoke.incoming.push('{"id":12,"method":"session"}', '{"id":13,"method":"system-mode"}');
    await smoke.advance("document-loaded");
    assert.deepEqual(smoke.replies, [{id: 12, error: "signed-out"}, {id: 13, error: "signed-out"}]);
});
