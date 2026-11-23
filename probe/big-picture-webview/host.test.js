const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const test = require("node:test");

const source = fs.readFileSync(require.resolve("./big-picture-webview.js"), "utf8");
const drain = () => new Promise(resolve => setImmediate(resolve));

function fixture({activationError = false, primary = false, xaml = false, warm = false, expected = 0, hostedApps = [], findRequest, openReport = async () => ({})} = {}) {
    let stage = "closed", poll, timeout;
    const reports = [], replies = [], incoming = [], switches = [];
    let opens = 0, warmOpens = 0, now = 100000;
    const memoryRequests = [], owners = [];
    const renderer = {
        open() { opens++; stage = "composition"; },
        viewId() { return stage === "closed" ? 0 : (primary ? 3 : 10); },
        status() {
            if (activationError) throw {number: -2147024703};
            return stage;
        },
        read() { return incoming.shift(); },
        reply(reply) { replies.push(JSON.parse(reply)); },
        close() { stage = "closed"; }
    };
    renderer.openForAccount = id => { owners.push(id); if (warm) warmOpens++; else opens++; stage = "composition"; };
    renderer.openForPrimaryAccount = id => { assert(xaml); owners.push(id); stage = "composition"; };
    const window = {steamSdlRuntimes: hostedApps.map(appId => ({appId}))}, memory = {appMemoryUsage: 262144, appMemoryUsageLimit: 1073741824,
        expectedAppMemoryUsageLimit: expected, trySetAppMemoryUsageLimit(bytes) { memoryRequests.push(bytes); return false; }};
    if (xaml) window.document = {documentElement:{getAttribute: key => key === "data-steam-primary-xaml" ? "true" : null}};
    vm.runInNewContext(source, {
        Date: {now: () => now}, window, BigPictureView: {Native: {Renderer: renderer}},
        Windows: {
            System: {MemoryManager: memory},
            Foundation: {GuidHelper: {createNewGuid: () => "test"}},
            Storage: {
                ApplicationData: {current: {localFolder: {createFileAsync: openReport, tryGetItemAsync: findRequest}}},
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
        start(broker, failure) {
            if (broker && !broker.session) broker.session = async () => ({steamId: "76561198000000001"});
            return window.steamBigPictureHost.open(broker, failure);
        },
        async advance(nextStage) {
            if (!warm && owners.length && (nextStage === "gamepad-ready" || nextStage === "setup-ready")) {
                stage = nextStage === "setup-ready" ? "warmup-setup-layout" : "warmup-gamepad-layout";
                poll(); await drain();
            }
            stage = nextStage; poll(); await drain();
        },
        expire() { timeout(); },
        elapse(ms) { now += ms; }, warmOpens: () => warmOpens, memoryRequests, memory,
        shell: window.steamBigPictureHost, opens: () => opens, owners, switches, incoming, reports, replies
    };
}

test("storage inventory may load during startup and only approved visible writes reach the broker", async () => {
    const app = fixture({warm: true});
    const calls = [];
    const start = app.start({storage(operation, argument) {
        calls.push([operation, argument]);
        return operation === "list" ? {defaultId: "D", folders: []} : {ok: true};
    }});
    const send = request => app.incoming.push(JSON.stringify(request));
    await drain();
    send({id: 1, method: "storage-list"});
    send({id: 2, method: "storage-select", argument: "E"});
    await app.advance("gamepad-window-created");
    assert.deepEqual(calls, [["list", ""]]);
    assert.deepEqual(app.replies.find(reply => reply.id === 1).result, {defaultId: "D", folders: []});
    assert.equal(app.replies.find(reply => reply.id === 2).error, "unavailable");

    await app.advance("warmup-gamepad-layout");
    await app.advance("gamepad-ready");
    await start;
    send({id: 3, method: "storage-select", argument: "E"});
    send({id: 4, method: "storage-move", argument: {appIds: [42], destination: "D"}});
    send({id: 5, method: "storage-select", argument: "C:\\"});
    send({id: 6, method: "storage-move", argument: {appIds: ["42"], destination: "D"}});
    await app.advance("gamepad-ready");
    assert.deepEqual(calls.map(([operation, argument]) => [operation, JSON.stringify(argument)]), [
        ["list", '""'], ["select", '"E"'], ["move", '{"appIds":[42],"destination":"D"}']
    ]);
    for (const id of [3, 4]) assert.deepEqual(app.replies.find(reply => reply.id === id).result, {ok: true});
    for (const id of [5, 6]) assert.equal(app.replies.find(reply => reply.id === id).error, "unavailable");
});

test("the signed host forwards Install for the selected game and reports download stages", async () => {
    const app = fixture();
    const calls = [];
    const start = app.start({install(appId, reportProgress) {
        calls.push(appId);
        reportProgress("queued");
        reportProgress("downloading", {downloaded: 512, downloadTotal: 2048});
        return Promise.resolve("installed");
    }});
    await drain();
    const appId = 1817070;
    app.incoming.push(JSON.stringify({id: 1, method: "install", appId}));
    await app.advance("gamepad-window-created");
    assert.deepEqual(calls, []);
    assert.deepEqual(app.replies.find(reply => reply.id === 1), {id: 1, error: "unavailable"});

    await app.advance("gamepad-ready");
    await start;
    app.incoming.push(JSON.stringify({id: 2, method: "install", appId}));
    await app.advance("gamepad-ready");
    assert.deepEqual(calls, [appId]);
    assert.deepEqual(app.replies.slice(-3), [
        {event: "xbox-install", appId, stage: "queued"},
        {event: "xbox-install", appId, stage: "downloading", progress: {downloaded: 512, downloadTotal: 2048}},
        {id: 2, result: "installed"}
    ]);
});

test("a signed warmup keeps QR foreground until a real layout and a fresh physical foreground check", async () => {
    const app = fixture({warm: true, expected: 5368709120});
    let ready = false, plays = 0;
    const start = app.start({play() { ++plays; }}).then(() => { ready = true; });
    await drain();
    assert.equal(app.warmOpens(), 1);
    app.incoming.push(JSON.stringify({id: 5, method: "play", appId: 42}));
    await app.advance("gamepad-window-created");
    await app.shell.resume();
    assert.equal(plays, 0);
    assert.deepEqual(app.switches, []);
    app.elapse(2200);
    await app.advance("warmup-gamepad-layout");
    assert.deepEqual(app.switches, [[10,3]]);
    assert(!ready);
    assert(app.replies.some(r => r.nativeWarmupComplete));
    assert.match(app.reports.at(-1), /warmup=warmup-gamepad-layout elapsed_ms=2200/);
    assert.deepEqual(app.memoryRequests, [1610612736]);
    assert.match(app.reports.at(-1), /expected_limit_bytes=5368709120/);
    await app.advance("warmup-gamepad-layout");
    assert(!ready);
    await app.advance("gamepad-ready");
    await start;
    assert(ready);
});

test("when hidden Steam stops making progress switch once and wait for visible setup", async () => {
    const app = fixture({warm: true});
    let ready = false;
    const start = app.start({}).then(() => { ready = true; });
    await drain();
    app.elapse(14001);
    await app.advance("composition");
    assert.deepEqual(app.switches, [[10,3]]);
    assert(!ready);
    assert.match(app.reports.at(-1), /warmup=no-layout-progress elapsed_ms=14001/);
    app.elapse(25000);
    await app.advance("gamepad-window-created");
    assert.deepEqual(app.switches, [[10,3]]);
    assert(!ready);
    await app.advance("setup-ready");
    await start;
    assert.deepEqual(app.memoryRequests, []);
});

test("observed portal progress extends warmup only up to its measured hard limit", async () => {
    const app = fixture({warm: true});
    const start = app.start({});
    await drain();
    for (const route of ["waiting-window", "waiting-portal", "library-selected"]) {
        app.elapse(10500);
        app.incoming.push(JSON.stringify({method: "startup-route", stage: route}));
        await app.advance("composition");
    }
    assert.deepEqual(app.switches, []);
    app.elapse(600);
    await app.advance("composition");
    assert.deepEqual(app.switches, [[10,3]]);
    assert.match(app.reports.at(-1), /warmup=time-limit elapsed_ms=32100/);
    await app.advance("gamepad-ready");
    await start;
});

test("a timed-out hidden layout keeps the Xbox allocation through a compilation spike", async () => {
    const app = fixture({warm: true});
    const started = app.start({});
    await drain();
    app.memory.appMemoryUsage = 2447257600; // Actual Xbox charge at the .20 fallback.
    app.elapse(33000);
    await app.advance("gamepad-window-created");
    assert.deepEqual(app.switches, []);
    assert.match(app.reports.at(-1), /warmup=waiting-for-memory time-limit/);
    app.elapse(1000);
    await app.advance("warmup-gamepad-layout");
    assert.deepEqual(app.switches, []);
    app.memory.appMemoryUsage = 960 * 1024 * 1024;
    app.elapse(500);
    await app.advance("warmup-gamepad-layout");
    app.memory.appMemoryUsage = 1200 * 1024 * 1024;
    app.elapse(1000);
    await app.advance("warmup-gamepad-layout");
    app.memory.appMemoryUsage = 944 * 1024 * 1024;
    app.elapse(500);
    await app.advance("warmup-gamepad-layout");
    app.elapse(1000);
    await app.advance("warmup-gamepad-layout");
    assert.deepEqual(app.switches, []);
    app.elapse(500);
    await app.advance("warmup-gamepad-layout");
    assert.deepEqual(app.switches, [[10,3]]);
    await app.advance("gamepad-ready");
    await started;
});

test("a renderer which remains over the Xbox budget releases its view at the startup deadline", async () => {
    const app = fixture({warm: true});
    const failed = assert.rejects(app.start({}), /warmup-memory-timeout/);
    await drain();
    app.memory.appMemoryUsage = 2447257600;
    app.elapse(33000);
    await app.advance("warmup-gamepad-layout");
    app.expire();
    await failed;
    assert.deepEqual(app.switches, []);
    assert.match(app.reports.at(-1), /memory_warm_timeout_usage_bytes=2447257600/);
});

test("warmup errors leave QR in foreground and unsigned smoke uses ordinary activation", async () => {
    const app = fixture({warm: true});
    const rejected = assert.rejects(app.start({}), /process-error/);
    await drain();
    await app.advance("process-error:owner:1:out-of-memory");
    await rejected;
    assert.deepEqual(app.switches, []);
    const smoke = fixture({warm: true});
    await smoke.start(null);
    assert.equal(smoke.warmOpens(), 0);
    assert.equal(smoke.opens(), 1);
});

test("an already-current Steam view does not request another foreground switch", async () => {
    const app = fixture({primary: true});
    const ready = app.start({});
    await drain();
    await app.advance("gamepad-ready");
    await ready;
    assert.equal(app.opens(), 1);
    assert.deepEqual(app.switches, []);
});

test("the named primary XAML host reveals Valve in the same Game window during a high memory charge", async () => {
    const app = fixture({primary:true, xaml:true, warm:true});
    const ready = app.start({});
    await drain();
    app.memory.appMemoryUsage = 2200 * 1024 * 1024;
    app.memory.appMemoryUsageLimit = 5368709120;
    await app.advance("warmup-gamepad-layout");
    assert(app.replies.some(r => r.nativeWarmupComplete));
    assert.deepEqual(app.switches, []);
    await app.advance("gamepad-ready");
    await ready;
    assert.deepEqual(app.owners, ["76561198000000001"]);
});

test("a local operator request captures the active console renderer only after startup", async () => {
    const files = [], removed = [];
    const app = fixture({primary: true, findRequest: async name => {
        files.push(name);
        return {deleteAsync: async () => { removed.push(name); }};
    }});
    const ready = app.start({});
    await drain();
    await app.advance("gamepad-window-created");
    assert.deepEqual(files, []);
    await app.advance("gamepad-ready");
    await ready;
    assert.deepEqual(files, ["big-picture-capture-library.request"]);
    assert.deepEqual(removed, files);
    assert.deepEqual(app.replies, [{nativeWarmupComplete: true}, {nativeCaptureLibrary: true}]);
    app.incoming.push(JSON.stringify({method: "capture-library-result", state: "saved", viewId: 3,
        browserPid: 99, width: 1920, height: 1080}));
    await app.advance("gamepad-ready");
    assert.match(app.reports.at(-1), /library_capture=saved width=1920 height=1080 view=3 browser_pid=99/);
});

test("surface sizing reports only the fixed numeric native and DOM measurements", async () => {
    const app = fixture(), ready = app.start({});
    await drain();
    app.incoming.push(JSON.stringify({method: "surface-sizing", values: ["private-field"]}));
    await app.advance("gamepad-ready");
    await ready;
    assert.doesNotMatch(app.reports.at(-1), /surface_sizing=|private-field/);
    app.incoming.push(JSON.stringify({method: "surface-sizing",
        values: [1920, 1080, 3840, 2160, 1500, 1920, 1080, 1000, 1920, 1080, 1000, 1000]}));
    await app.advance("gamepad-ready");
    assert.match(app.reports.at(-1), /bounds_width=1920 bounds_height=1080 raster_scale_milli=1000 css_width=1920/);
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
    assert.deepEqual(app.replies.filter(reply => reply.id), [{id: 11, result: {steamId: identity}}]);
    assert.ok(app.reports.every(report => !report.includes(identity)));

    const smoke = fixture();
    await smoke.start(null);
    smoke.incoming.push('{"id":12,"method":"session"}', '{"id":13,"method":"system-mode"}');
    await smoke.advance("document-loaded");
    assert.deepEqual(smoke.replies, [{id: 12, error: "signed-out"}, {id: 13, error: "signed-out"}]);
});

test("an account change closes the previous view and late library results never reach the new owner", async () => {
    const app = fixture();
    let replyLate, failures = 0;
    const first = {session: async () => ({steamId: "76561198000000010"}),
        library: () => new Promise(resolve => { replyLate = resolve; })};
    const opened = app.start(first, () => failures++);
    await drain(); await app.advance("gamepad-ready"); await opened;
    app.incoming.push(JSON.stringify({id: 8, method: "library"}));
    await app.advance("gamepad-ready");
    const second = {session: async () => ({steamId: "76561198000000011"}), signOut: async () => "signed-out"};
    const switched = app.start(second, () => failures++);
    await drain(); await app.advance("gamepad-ready"); await switched;
    assert.deepEqual(app.owners, ["76561198000000010", "76561198000000011"]);
    replyLate([{id: 101, name: "Private old account game"}]);
    app.incoming.push(JSON.stringify({id: 9, method: "sign-out"}));
    await app.advance("gamepad-ready");
    assert(!app.replies.some(reply => reply.id === 8));
    assert(app.replies.some(reply => reply.id === 9 && reply.result === "signed-out"));
    assert.deepEqual(app.switches.at(-1), [3, 10]);
    assert.equal(failures, 0);
});

test("a missing or invalid owner never opens a signed renderer profile", async () => {
    for (const steamId of [null, "../shared", "18446744073709551616"]) {
        const app = fixture();
        await assert.rejects(app.start({session: async () => ({steamId})}));
        assert.deepEqual(app.owners, []);
        assert.equal(app.opens(), 0);
    }
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
    const app = fixture({hostedApps: [1145360]});
    let signedIn = true, launches = 0, raises = 0;
    const ready = app.start({
        session: async () => ({steamId: signedIn ? "76561191111111111" : null}),
        async play(id, handoff) { assert.equal(id, 1145360); await handoff(); launches++; return "presented"; }
    });
    await drain();
    await app.advance("gamepad-ready");
    await ready;
    app.incoming.push('{"method":"play","id":1,"appId":1145360}');
    await app.advance("gamepad-ready");
    const restore = async view => { assert.equal(view, 10); raises++; return true; };
    assert.equal(await app.shell.showForGame(1, 90, restore), false);
    let quitRequests = 0;
    const quit = () => { quitRequests++; return true; };
    assert.equal(await app.shell.showForGame(1145360, 90, restore, quit), true);
    assert.deepEqual(app.switches.slice(-2), [[3,10],[10,90]]);
    signedIn = false;
    app.incoming.push('{"method":"resume-game","id":2,"appId":1145360}');
    await app.advance("gamepad-ready");
    assert.ok(app.replies.some(e => e.id === 2 && e.result === "signed-out"));
    signedIn = true;
    app.incoming.push('{"method":"resume-game","id":3,"appId":646570}', '{"method":"resume-game","id":4,"appId":1145360}');
    await app.advance("gamepad-ready");
    assert.ok(app.replies.some(e => e.id === 3 && e.result === "unavailable"));
    assert.ok(app.replies.some(e => e.id === 4 && e.result === "presented"));
    assert.equal(launches, 1); assert.equal(raises, 1);
    app.incoming.push('{"method":"quit-game","id":5,"appId":1145360}');
    await app.advance("gamepad-ready");
    assert.ok(app.replies.some(e => e.id === 5 && e.result === "unavailable"));
    assert.match(app.reports.at(-1), /game-quit-received/);
    assert.doesNotMatch(app.reports.at(-1), /game-quit-requested/);
    assert.equal(await app.shell.showForGame(1145360, 90, restore, quit), true);
    app.incoming.push('{"method":"quit-game","id":6,"appId":413150}', '{"method":"quit-game","id":7,"appId":1145360}');
    await app.advance("gamepad-ready");
    assert.ok(app.replies.some(e => e.id === 6 && e.result === "unavailable"));
    assert.ok(app.replies.some(e => e.id === 7 && e.result === "quit-requested"));
    assert.match(app.reports.at(-1), /game-quit-requested/);
    assert.equal(quitRequests, 1);
    await app.shell.resume(1145360);
    await drain();
    const events = app.replies.filter(e => e.event === "xbox-game").map(e => [e.appId,e.overlay]);
    const foreground = app.replies.filter(e => 'nativeGameForeground' in e).map(e => e.nativeGameForeground);
    assert.deepEqual(foreground, [true, false, true, false]);
    assert.deepEqual(events, [[1145360,false],[1145360,true],[1145360,false],[1145360,true],[0,false]]);
    assert.ok(app.reports.at(-1).includes("memory_first_frame_limit_bytes=1073741824"));
    assert.equal(app.opens(), 1);
    assert.equal(await app.shell.showForGame(1145360, 90, restore), false);
});

test("only fixed Store JS milestones persist and stay separate from native HTTP results", async () => {
    const app = fixture();
    const start = app.start({});
    await drain(); await app.advance("gamepad-ready"); await start;
    for (const stage of ["create","geometry","geometry","https://private.invalid/","visible","blank-complete","navigate"])
        app.incoming.push(JSON.stringify({method:"store-js-stage",stage}));
    await app.advance("gamepad-ready");
    assert.match(app.reports.at(-1), /store_js_stages=create,geometry,visible,blank-complete,navigate\r\n/);
    assert.doesNotMatch(app.reports.at(-1), /private\.invalid|public_store_navigation=/);
});
