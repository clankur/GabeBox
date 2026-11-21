const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const test = require("node:test");

const source = fs.readFileSync(require.resolve("./big-picture-webview.js"), "utf8");
const drain = () => new Promise(resolve => setImmediate(resolve));

function fixture({activationError = false, primary = false, openReport = async () => ({})} = {}) {
    let stage = "closed", poll, timeout;
    const reports = [], replies = [], incoming = [], switches = [];
    let opens = 0;
    const renderer = {
        open() { opens++; stage = "composition"; },
        openInCurrentView() { opens++; stage = "composition"; },
        viewId() { return stage === "closed" ? 0 : (primary ? 3 : 10); },
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
            System: {MemoryManager: {appMemoryUsage: 262144, appMemoryUsageLimit: 1073741824}},
            Foundation: {GuidHelper: {createNewGuid: () => "test"}},
            Storage: {
                ApplicationData: {current: {localFolder: {createFileAsync: openReport}}},
                CreationCollisionOption: {openIfExists: 0},
                FileIO: {writeTextAsync: async (_, report) => { reports.push(report); }}
            },
            UI: {ViewManagement: {
                ApplicationView: {getForCurrentView: () => ({id: 3})},
                ApplicationViewSwitcher: {tryShowAsStandaloneAsync: async () => true, switchAsync: async (to, from) => { switches.push([to, from]); }}
            }}
        },
        setInterval(callback) { poll = callback; return 1; }, clearInterval() {},
        setTimeout(callback) { timeout = callback; return 2; }, clearTimeout() {}
    });
    return {
        start: (broker, failure) => window.steamBigPictureHost.open(broker, failure),
        async advance(nextStage) { stage = nextStage; poll(); await drain(); },
        expire() { timeout(); },
        shell: window.steamBigPictureHost, opens: () => opens, switches, incoming, reports, replies
    };
}

test("Steam can use the QR host's primary game view without a foreground switch", async () => {
    const app = fixture({primary: true});
    const ready = app.start({});
    await drain();
    await app.advance("gamepad-ready");
    await ready;
    assert.equal(app.opens(), 1);
    assert.deepEqual(app.switches, []);
});

test("opening Valve's window never waits on a locked startup report", async () => {
    let release;
    const app = fixture({openReport: () => new Promise(resolve => { release = resolve; })});
    const ready = app.start({});
    await drain();
    assert.equal(app.opens(), 1);
    await app.advance("gamepad-ready");
    await ready;
    assert.deepEqual(app.reports, []);
    release({});
    await drain();
    assert.match(app.reports.at(-1), /status=gamepad-ready/);
});

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

test("Valve can restart its foreground client after setup and retain the signed-in broker", async () => {
    const app = fixture();
    let failures = 0;
    const ready = app.start({session: async () => ({steamId: "76561198000000000"})}, () => failures++);
    await drain();
    await app.advance("setup-ready");
    await ready;
    app.incoming.push(JSON.stringify({id: 7, method: "restart-client"}));
    await app.advance("setup-ready");
    assert.deepEqual(app.replies.slice(-2), [{id: 7, result: "restart"}, {nativeRestartClient: true}]);
    assert.equal(app.opens(), 1);
    assert.equal(failures, 0);
    await app.advance("gamepad-ready");
    assert.match(app.reports.at(-1), /status=gamepad-ready/);
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

test("a visible Valve first-run screen keeps the session open during setup", async () => {
    const app = fixture();
    const ready = app.start({});
    await drain();
    await app.advance("setup-ready");
    await ready;
    assert.match(app.reports.at(-1), /status=setup-ready/);
    await app.advance("gamepad-ready");
    assert.match(app.reports.at(-1), /status=gamepad-ready/);
});

test("a game chord keeps both views alive and returns only to the owning game", async () => {
    const app = fixture();
    let signedIn = true, launches = 0, raises = 0;
    const ready = app.start({
        session: async () => ({steamId: signedIn ? "76561191111111111" : null}),
        async play(id, handoff) { assert.equal(id, 975370); await handoff(); launches++; return "presented"; }
    });
    await drain();
    await app.advance("gamepad-ready");
    await ready;
    app.incoming.push('{"method":"play","id":1,"appId":975370}');
    await app.advance("gamepad-ready");
    const restore = async view => { assert.equal(view, 10); raises++; return true; };
    assert.equal(await app.shell.showForGame(1, 90, restore), false);
    let quitRequests = 0;
    const quit = () => { quitRequests++; return true; };
    assert.equal(await app.shell.showForGame(975370, 90, restore, quit), true);
    assert.deepEqual(app.switches.slice(-2), [[3,10],[10,90]]);
    signedIn = false;
    app.incoming.push('{"method":"resume-game","id":2,"appId":975370}');
    await app.advance("gamepad-ready");
    assert.ok(app.replies.some(e => e.id === 2 && e.result === "signed-out"));
    signedIn = true;
    app.incoming.push('{"method":"resume-game","id":3,"appId":646570}', '{"method":"resume-game","id":4,"appId":975370}');
    await app.advance("gamepad-ready");
    assert.ok(app.replies.some(e => e.id === 3 && e.result === "unavailable"));
    assert.ok(app.replies.some(e => e.id === 4 && e.result === "presented"));
    assert.equal(launches, 1); assert.equal(raises, 1);
    app.incoming.push('{"method":"quit-game","id":5,"appId":975370}');
    await app.advance("gamepad-ready");
    assert.ok(app.replies.some(e => e.id === 5 && e.result === "unavailable"));
    assert.equal(await app.shell.showForGame(975370, 90, restore, quit), true);
    app.incoming.push('{"method":"quit-game","id":6,"appId":413150}', '{"method":"quit-game","id":7,"appId":975370}');
    await app.advance("gamepad-ready");
    assert.ok(app.replies.some(e => e.id === 6 && e.result === "unavailable"));
    assert.ok(app.replies.some(e => e.id === 7 && e.result === "quit-requested"));
    assert.equal(quitRequests, 1);
    await app.shell.resume(975370);
    await drain();
    const events = app.replies.filter(e => e.event === "xbox-game").map(e => [e.appId,e.overlay]);
    const foreground = app.replies.filter(e => 'nativeGameForeground' in e).map(e => e.nativeGameForeground);
    assert.deepEqual(foreground, [true, false, true, false]);
    assert.deepEqual(events, [[975370,false],[975370,true],[975370,false],[975370,true],[0,false]]);
    assert.ok(app.reports.at(-1).includes("memory_first_frame_limit_bytes=1073741824"));
    assert.equal(app.opens(), 1);
    assert.equal(await app.shell.showForGame(975370, 90, restore), false);
});
