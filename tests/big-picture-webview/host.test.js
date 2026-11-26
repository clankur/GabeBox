const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const test = require("node:test");

const source = fs.readFileSync(require.resolve("../../ui/big-picture-webview/big-picture-webview.js"), "utf8");
const drain = () => new Promise(resolve => setImmediate(resolve));

function fixture({hostedApps = [], activationError = false, primary = false, xaml = false, warm = false, earlyLibrary = false,
                  expected = 0, findRequest, findInstalled = async () => null, openReport = async () => ({})} = {}) {
    let stage = "closed", poll, timeout, timerId = 2;
    const timers = new Map();
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
    if (xaml || earlyLibrary) window.document = {documentElement: {getAttribute(key) {
        if ((xaml && key === "data-steam-primary-xaml") || (earlyLibrary && key === "data-steam-early-library-reveal"))
            return "true";
        return null;
    }}};
    vm.runInNewContext(source, {
        Date: {now: () => now}, window, BigPictureView: {Native: {Renderer: renderer}},
        Windows: {
            ApplicationModel: {Package: {current: {installedLocation: {tryGetItemAsync: findInstalled}}}},
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
        setTimeout(callback, delay) { timeout = callback; timers.set(++timerId, {callback, delay}); return timerId; },
        clearTimeout(id) { timers.delete(id); }
    });
    return {
        start(broker, failure, timing) {
            if (broker && !broker.session) broker.session = async () => ({steamId: "76561198000000001"});
            return window.steamBigPictureHost.open(broker, failure, timing);
        },
        async advance(nextStage) {
            if (!warm && owners.length && (nextStage === "gamepad-ready" || nextStage === "setup-ready")) {
                stage = nextStage === "setup-ready" ? "warmup-setup-layout" : "warmup-gamepad-layout";
                poll(); await drain();
            }
            stage = nextStage; poll(); await drain();
        },
        expire() { timeout(); },
        // Runs each pending timer with this delay that was not cleared.
        fire(delay) {
            for (const [id, timer] of [...timers]) if (timer.delay === delay) { timers.delete(id); timer.callback(); }
        },
        pending: delay => [...timers.values()].some(timer => timer.delay === delay),
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

test("cached account timing reports shell milestones while keeping early Play unavailable", async () => {
    const app = fixture({primary: true, xaml: true, warm: true});
    let elapsed = 4100, played = false;
    const timing = {
        marks: {cache_check_finished: 4000},
        mark(stage) { this.marks[stage] ??= elapsed; }
    };
    const ready = app.start({play() { played = true; }}, undefined, timing);
    await drain();
    elapsed = 5100;
    app.incoming.push(JSON.stringify({method: "startup-route", stage: "library-selected"}),
        JSON.stringify({id: 5, method: "play", appId: 42}));
    await app.advance("gamepad-window-created");
    assert.equal(played, false);
    assert.match(app.reports.at(-1), /startup_elapsed_ms=cache_check_finished:4000,shell_open:4100/);
    assert.match(app.reports.at(-1), /\bbrowser_gamepad_window_created:5100,shell_library_selected:5100\b/);
    assert.doesNotMatch(app.reports.at(-1), /shell_gamepad_ready:/);

    timing.mark("catalog_enriched");
    app.shell.catalogChanged();
    assert.deepEqual(app.replies.at(-1), {event: "xbox-catalog-changed"});
    app.incoming.push(JSON.stringify({method: "startup-cloud-local-fallback", namespace: 3}),
        JSON.stringify({method: "startup-cloud-local-fallback", namespace: 2}));
    await app.advance("gamepad-window-created");
    assert.match(app.reports.at(-1), /catalog_enriched:5100,cloud_local_namespace_3:5100/);
    assert.doesNotMatch(app.reports.at(-1), /cloud_local_namespace_2/);

    elapsed = 5300;
    await app.advance("warmup-gamepad-layout");
    elapsed = 5500;
    await app.advance("gamepad-ready");
    await ready;
    assert.match(app.reports.at(-1), /\bshell_warmup_handoff:5300,shell_revealed:5300,shell_gamepad_ready:5500\b/);
});

test("the early Library trial reveals the same primary window and waits for native readiness", async () => {
    const app = fixture({primary: true, xaml: true, warm: true, earlyLibrary: true});
    let ready = false, plays = 0;
    let serveLibrary;
    const library = new Promise(resolve => { serveLibrary = resolve; });
    const started = app.start({library() { return library; }, play() { ++plays; }}).then(() => { ready = true; });
    await drain();
    app.memory.appMemoryUsage = 2400 * 1024 * 1024;
    app.memory.appMemoryUsageLimit = 5368709120;
    app.elapse(900);
    app.incoming.push(JSON.stringify({method: "startup-route", stage: "library-selected"}),
        JSON.stringify({id: 1, method: "library"}));
    await app.advance("gamepad-window-created");
    assert.deepEqual(app.replies, []);
    app.elapse(600);
    serveLibrary([{id: 42, name: "Game"}]);
    await drain();
    await app.advance("gamepad-window-created");
    assert.match(app.reports.at(-1), /warmup=primary-library-served elapsed_ms=1500/);
    assert.deepEqual(app.switches, []);
    assert.equal(app.replies.filter(reply => reply.nativeWarmupComplete).length, 1);
    app.incoming.push(JSON.stringify({id: 5, method: "play", appId: 42}));
    await app.advance("gamepad-window-created");
    assert(!ready);
    assert.equal(plays, 0);
    await app.advance("gamepad-ready");
    await started;
    assert(ready);
});

test("the early Library option never advances secondary view activation", async () => {
    const app = fixture({warm: true, earlyLibrary: true});
    const started = app.start({library: async () => [{id: 42, name: "Game"}]});
    await drain();
    app.elapse(1500);
    app.incoming.push(JSON.stringify({id: 1, method: "library"}));
    await app.advance("gamepad-window-created");
    await app.advance("gamepad-window-created");
    assert.deepEqual(app.switches, []);
    assert(!app.replies.some(reply => reply.nativeWarmupComplete));
    app.elapse(1500);
    await app.advance("warmup-gamepad-layout");
    assert.deepEqual(app.switches, [[10, 3]]);
    await app.advance("gamepad-ready");
    await started;
});

test("the startup movie override records a single bounded outcome", async () => {
    const app = fixture({primary: true, xaml: true, warm: true});
    const started = app.start({});
    await drain();
    app.incoming.push(JSON.stringify({method: "startup-movie-override-js", state: "untrusted text"}),
        JSON.stringify({method: "startup-movie-override-js", state: "absent-404"}));
    await app.advance("gamepad-window-created");
    assert.match(app.reports.at(-1), /startup_movie_override_js=absent-404/);
    assert.doesNotMatch(app.reports.at(-1), /untrusted text/);
    await app.advance("warmup-gamepad-layout");
    await app.advance("gamepad-ready");
    await started;
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
    assert.match(report, /renderer_error=First failure\r\n/);
    // The latest failure is kept beside the first, with the stage it arrived in.
    assert.match(report, /renderer_error_count=2\r\nrenderer_error_latest=document-loaded Second failure\r\n/);
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

async function playingPrimaryGame(appId, broker = {}) {
    const app = fixture({primary: true, xaml: true, hostedApps: [appId]});
    let failure;
    const ready = app.start({async play(_id, handoff) { await handoff(); return "presented"; }, ...broker},
        error => { failure = error; });
    await drain();
    await app.advance("gamepad-ready");
    await ready;
    app.incoming.push(JSON.stringify({method: "play", id: 1, appId}));
    await app.advance("gamepad-ready");
    assert.deepEqual(app.replies.find(reply => reply.id === 1), {id: 1, result: "presented"});
    return {app, failure: () => failure};
}
const recoveryLines = report => report.split("\r\n").filter(line => line.startsWith("webview_recovery_"));

test("an unresponsive renderer under the game overlay returns to the game and reopens Big Picture after it exits", async () => {
    const appId = 2868840, {app, failure} = await playingPrimaryGame(appId);
    const restores = [];
    assert.equal(await app.shell.showForGame(appId, 3, async view => { restores.push(view); return true; }, () => true), true);
    await drain();
    assert.match(app.reports.at(-1), /status=game-switch-to-steam/);
    await app.advance("process-error:owner:2:unresponsive:259:none");
    await drain();
    assert.deepEqual(restores, [3]);
    assert.equal(failure(), undefined);
    assert.deepEqual(recoveryLines(app.reports.at(-1)), [
        "webview_recovery_1=game-webview-lost appid=2868840 covered=1 cause=process-error:owner:2:unresponsive:259:none",
        "webview_recovery_2=game-restore-started appid=2868840",
        "webview_recovery_3=game-restored appid=2868840"
    ]);
    // The chord now returns straight to the game until it exits.
    assert.equal(await app.shell.showForGame(appId, 3, async () => true, () => true), false);
    await drain();
    assert.match(app.reports.at(-1), /webview_recovery_4=game-overlay-unavailable appid=2868840/);
    assert.equal(app.owners.length, 1);
    const returning = app.shell.resume(appId);
    await drain();
    await app.advance("gamepad-ready");
    await returning;
    assert.equal(app.owners.length, 2);
    assert.match(app.reports.at(-1), /webview_recovery_5=webview-restart-after-game appid=2868840\r\n/);
    assert.match(app.reports.at(-1), /status=gamepad-ready/);
    assert.equal(failure(), undefined);
});

test("an overlay whose renderer never draws hands the game back with its input", async () => {
    // Slay the Spire 2 on 1.0.246.2: the chord switched to Steam, the overlay stayed white,
    // A did nothing, and the owner renderer was reported unresponsive only ~75 s later.
    const appId = 2868840, {app, failure} = await playingPrimaryGame(appId);
    const restores = [];
    let confirms = 0;
    const confirm = async () => { confirms++; return {ok: true}; };
    assert.equal(await app.shell.showForGame(appId, 3, async view => { restores.push(view); return true; }, () => true, confirm), true);
    await drain();
    assert.ok(app.pending(10000));
    // A confirmation for another game does not count.
    app.incoming.push(JSON.stringify({method: "overlay-ready", appId: 42}));
    await app.advance("gamepad-ready");
    assert.ok(app.pending(10000));
    app.fire(10000);
    for (let i = 0; i < 4; i++) await drain();
    assert.deepEqual(restores, [3]);
    assert.equal(confirms, 1);
    assert.equal(failure(), undefined);
    assert.deepEqual(recoveryLines(app.reports.at(-1)), [
        "webview_recovery_1=game-webview-lost appid=2868840 covered=1 cause=overlay-unresponsive",
        "webview_recovery_2=game-restore-started appid=2868840",
        "webview_recovery_3=game-restored appid=2868840"
    ]);
    // A chord while Big Picture is down stays with the game and is recorded.
    assert.equal(app.shell.overlayAvailable(appId), false);
    await drain();
    assert.match(app.reports.at(-1), /webview_recovery_4=game-overlay-unavailable appid=2868840/);
    assert.equal(app.owners.length, 1);
});

test("an overlay that draws in time stays open for Resume", async () => {
    const appId = 2868840, {app} = await playingPrimaryGame(appId);
    let raises = 0;
    assert.equal(app.shell.overlayAvailable(appId), true);
    assert.equal(await app.shell.showForGame(appId, 3, async () => { raises++; return true; }, () => true), true);
    app.incoming.push(JSON.stringify({method: "overlay-ready", appId}));
    await app.advance("gamepad-ready");
    assert.equal(app.pending(10000), false);
    assert.match(app.reports.at(-1), /\r\ngame-overlay-ready\r\n/);
    app.fire(10000);
    await drain();
    assert.deepEqual(recoveryLines(app.reports.at(-1)), []);
    app.incoming.push(JSON.stringify({method: "resume-game", id: 2, appId}));
    await app.advance("gamepad-ready");
    assert.deepEqual(app.replies.find(reply => reply.id === 2), {id: 2, result: "presented"});
    assert.equal(raises, 1);
});

test("a Resume whose hosted input stays paused is recorded", async () => {
    const appId = 2868840, {app} = await playingPrimaryGame(appId);
    const confirm = async () => ({ok: false, reason: "input-paused"});
    assert.equal(await app.shell.showForGame(appId, 3, async () => true, () => true, confirm), true);
    app.incoming.push(JSON.stringify({method: "resume-game", id: 2, appId}));
    await app.advance("gamepad-ready");
    await drain();
    assert.deepEqual(app.replies.find(reply => reply.id === 2), {id: 2, result: "presented"});
    assert.match(app.reports.at(-1), /\r\ngame-input-resume-failed:input-paused\r\n/);
});

test("recovery that shows frames without input retries once and then records why", async () => {
    const appId = 42, {app, failure} = await playingPrimaryGame(appId);
    let restores = 0;
    const confirm = async () => ({ok: false, reason: "input-background"});
    assert.equal(await app.shell.showForGame(appId, 3, async () => { restores++; return true; }, () => true, confirm), true);
    await app.advance("process-error:owner:2:unresponsive:259:none");
    await drain();
    assert.equal(restores, 1);
    app.fire(1000);
    for (let i = 0; i < 4; i++) await drain();
    assert.equal(restores, 2);
    assert.equal(failure(), undefined);
    assert.deepEqual(recoveryLines(app.reports.at(-1)), [
        "webview_recovery_1=game-webview-lost appid=42 covered=1 cause=process-error:owner:2:unresponsive:259:none",
        "webview_recovery_2=game-restore-started appid=42",
        "webview_recovery_3=game-restore-retry appid=42",
        "webview_recovery_4=game-input-resume-failed appid=42 reason=input-background"
    ]);
    // The game stays on screen: Big Picture is not restarted over it.
    assert.equal(app.owners.length, 1);
});

test("recovery records the live input state that confirmed it, and retries a window without key focus", async () => {
    // Slay the Spire 2 on 1.0.250.3: game-restored was recorded while key events never
    // reached the window again. The confirmation now also requires the view's window to hold
    // Win32 keyboard focus, and the report keeps the fields read after the restore.
    const appId = 2868840, {app, failure} = await playingPrimaryGame(appId);
    const answers = [
        {ok: false, reason: "window-unfocused", detail: "visible=1,paused=0,active=1,window_focus=2,keys=8"},
        {ok: true, detail: "visible=1,paused=0,active=1,window_focus=1,keys=8"}
    ];
    let restores = 0;
    const confirm = async () => answers.shift();
    assert.equal(await app.shell.showForGame(appId, 3, async () => { restores++; return true; }, () => true, confirm), true);
    await drain();
    app.fire(10000);
    for (let i = 0; i < 4; i++) await drain();
    assert.equal(restores, 1);
    app.fire(1000);
    for (let i = 0; i < 4; i++) await drain();
    assert.equal(restores, 2);
    assert.equal(failure(), undefined);
    assert.deepEqual(recoveryLines(app.reports.at(-1)), [
        "webview_recovery_1=game-webview-lost appid=2868840 covered=1 cause=overlay-unresponsive",
        "webview_recovery_2=game-restore-started appid=2868840",
        "webview_recovery_3=game-restore-retry appid=2868840",
        "webview_recovery_4=game-restored appid=2868840 input=visible=1,paused=0,active=1,window_focus=1,keys=8"
    ]);
});

test("the game frame report keeps the panel's focus, resume sample and composition counters", async () => {
    // clankur/GabeBox#89: a white flicker over a game's menus. The panel status counts hides by
    // cause, shows, reorders and presents of an already presented frame.
    const appId = 2868840, {app} = await playingPrimaryGame(appId);
    app.shell.recordGameFrame(appId, "media", "primary_panel=1\r\nvisible=1\r\npanel_hidden=3\r\n" +
        "panel_hidden_shell=2\r\npanel_hidden_panel=0\r\npanel_hidden_window=1\r\npanel_uncovered=4\r\n" +
        "panel_reordered=0\r\nrepeat_presents=12\r\nnative_window_focus=1\r\nnative_window_focus_taken=2\r\n" +
        "native_pad_resume_serial=22\r\nnative_pad_ack=22\r\npanel_flash=1\r\n");
    await drain();
    assert.match(app.reports.at(-1), new RegExp("game_frame_media=appid=2868840 primary_panel=1 visible=1 panel_hidden=3 " +
        "panel_hidden_shell=2 panel_hidden_panel=0 panel_hidden_window=1 panel_uncovered=4 panel_reordered=0 " +
        "repeat_presents=12 native_window_focus=1 native_window_focus_taken=2 native_pad_resume_serial=22 " +
        "native_pad_ack=22\r\n"));
});

test("the game frame report keeps the shared-texture exchange counters", async () => {
    // clankur/GabeBox#89: the panel status shows whether the ready fence held back a frame
    // whose copy was still running, and the last drawn fence values.
    const appId = 2868840, {app} = await playingPrimaryGame(appId);
    app.shell.recordGameFrame(appId, "media", "primary_panel=1\r\nvisible=1\r\ngpu_wait_producer=5\r\n" +
        "gpu_held=2\r\ngpu_released_unshown=1\r\ngpu_drawn=1800\r\ngpu_ready_fence=1803\r\n" +
        "gpu_done_fence=1803\r\ngpu_fence_handle=7\r\n");
    await drain();
    assert.match(app.reports.at(-1), new RegExp("game_frame_media=appid=2868840 primary_panel=1 visible=1 " +
        "gpu_wait_producer=5 gpu_held=2 gpu_released_unshown=1 gpu_drawn=1800 gpu_ready_fence=1803 " +
        "gpu_done_fence=1803\r\n"));
});

test("the game frame report keeps the white-flash probe counters", async () => {
    // clankur/GabeBox#89: the opt-in probe's counters and the first white frame's time reach
    // the report; a malformed time does not.
    const appId = 2868840, {app} = await playingPrimaryGame(appId);
    app.shell.recordGameFrame(appId, "media", "primary_panel=1\r\nflash_probe=1\r\nflash_producer=1\r\n" +
        "flash_frames=1800\r\nflash_game_frames=1795\r\nwhite_frames=2\r\nwhite_runs=1\r\n" +
        "flash_mismatched=0\r\nflash_captures=3\r\nwhite_first_utc=2026-11-26T09:36:55.123Z\r\n" +
        "white_first_utc=yesterday\r\nflash_secret=1\r\n");
    await drain();
    assert.match(app.reports.at(-1), new RegExp("game_frame_media=appid=2868840 primary_panel=1 flash_probe=1 " +
        "flash_producer=1 flash_frames=1800 flash_game_frames=1795 white_frames=2 white_runs=1 flash_mismatched=0 " +
        "flash_captures=3 white_first_utc=2026-11-26T09:36:55\\.123Z\r\n"));
});

test("a confirmation detail outside the bounded field list is not written to the report", async () => {
    const appId = 42, {app} = await playingPrimaryGame(appId);
    const confirm = async () => ({ok: true, detail: "token=abc\r\nstatus=forged"});
    assert.equal(await app.shell.showForGame(appId, 3, async () => true, () => true, confirm), true);
    await app.advance("process-error:owner:2:unresponsive:259:none");
    for (let i = 0; i < 4; i++) await drain();
    assert.deepEqual(recoveryLines(app.reports.at(-1)).at(-1), "webview_recovery_3=game-restored appid=42");
});

test("a renderer failure while the game is in the foreground leaves it there", async () => {
    const appId = 42, {app, failure} = await playingPrimaryGame(appId);
    await app.advance("process-error:owner:1:crashed:1:none");
    await drain();
    assert.equal(failure(), undefined);
    assert.deepEqual(recoveryLines(app.reports.at(-1)), [
        "webview_recovery_1=game-webview-lost appid=42 covered=0 cause=process-error:owner:1:crashed:1:none",
        "webview_recovery_2=game-kept-foreground appid=42"
    ]);
    const returning = app.shell.resume(appId);
    await drain();
    await app.advance("gamepad-ready");
    await returning;
    assert.equal(app.owners.length, 2);
});

test("a game that cannot be shown again is handed to a restarted Big Picture with Resume and Exit Game", async () => {
    const appId = 42, {app, failure} = await playingPrimaryGame(appId);
    let attempts = 0, quits = 0;
    const restore = async view => { assert.equal(view, 3); return ++attempts > 2; };
    assert.equal(await app.shell.showForGame(appId, 3, restore, () => { quits++; return true; }), true);
    await app.advance("process-error:owner:2:unresponsive:259:none");
    await drain();
    assert.equal(attempts, 1);
    assert.match(app.reports.at(-1), /webview_recovery_3=game-restore-retry appid=42/);
    app.expire();
    await drain();
    assert.equal(attempts, 2);
    assert.equal(app.owners.length, 2);
    await app.advance("gamepad-ready");
    assert.equal(failure(), undefined);
    const report = app.reports.at(-1);
    assert.match(report, /webview_recovery_4=game-restore-failed appid=42\r\nwebview_recovery_5=webview-restart-for-game appid=42\r\n/);
    assert.match(report, /webview_recovery_6=game-adopted appid=42/);
    assert.deepEqual(app.replies.filter(e => e.event === "xbox-game").at(-1), {event: "xbox-game", appId, overlay: true});
    app.incoming.push(JSON.stringify({method: "resume-game", id: 2, appId}));
    await app.advance("gamepad-ready");
    assert.deepEqual(app.replies.find(reply => reply.id === 2), {id: 2, result: "presented"});
    assert.equal(attempts, 3);
    assert.equal(await app.shell.showForGame(appId, 3, restore, () => { quits++; return true; }), true);
    app.incoming.push(JSON.stringify({method: "quit-game", id: 3, appId}));
    await app.advance("gamepad-ready");
    assert.deepEqual(app.replies.find(reply => reply.id === 3), {id: 3, result: "quit-requested"});
    assert.equal(quits, 1);
    await app.shell.resume(appId);
    await drain();
    assert.match(app.reports.at(-1), /status=game-returned-to-steam/);
});

// Plays a game, covers it with Valve's overlay, fails the renderer, and lets both restore
// attempts fail, so Big Picture restarts for the game. Stops while the new session warms.
async function restartingForGame(appId, running) {
    const played = await playingPrimaryGame(appId), {app} = played;
    let attempts = 0;
    const restore = async () => { attempts++; return false; };
    assert.equal(await app.shell.showForGame(appId, 3, restore, () => true, undefined, running), true);
    await app.advance("process-error:owner:2:unresponsive:259:none");
    await drain();
    app.expire();
    await drain();
    assert.equal(attempts, 2);
    assert.equal(app.owners.length, 2);
    assert.match(app.reports.at(-1), /webview_recovery_5=webview-restart-for-game appid=42\r\n/);
    return {...played, overlayEvents: () => app.replies.filter(e => e.event === "xbox-game" && e.overlay).length};
}

test("a game that exits while Big Picture restarts for it is not adopted and leaves the library usable", async () => {
    const appId = 42, {app, failure, overlayEvents} = await restartingForGame(appId);
    const overlays = overlayEvents();
    // The exit reaches the warming session, which cannot return to Steam yet.
    await app.shell.resume(appId);
    await drain();
    await app.advance("gamepad-ready");
    assert.equal(failure(), undefined);
    const report = app.reports.at(-1);
    assert.match(report, /webview_recovery_6=game-exited-before-adopt appid=42\r\n/);
    assert.doesNotMatch(report, /game-adopted/);
    assert.match(report, /\r\ngame-exited-before-adopt\r\n/);
    assert.equal(overlayEvents(), overlays, "Valve is never told the exited game is running");
    app.incoming.push(JSON.stringify({method: "resume-game", id: 2, appId}));
    await app.advance("gamepad-ready");
    assert.deepEqual(app.replies.find(reply => reply.id === 2), {id: 2, result: "unavailable"});
    assert.equal(await app.shell.playConfiguredTrial(43), "presented");
});

test("an exit reported before the restarted session starts warming still prevents adoption", async () => {
    const appId = 42, app = fixture({primary: true, xaml: true, hostedApps: [appId]});
    let releaseIdentity, attempts = 0, sessions = 0;
    const ready = app.start({
        session: () => ++sessions < 2 ? Promise.resolve({steamId: "76561198000000001"}) :
            new Promise(resolve => { releaseIdentity = () => resolve({steamId: "76561198000000001"}); }),
        async play(_id, handoff) { await handoff(); return "presented"; }
    });
    await drain();
    await app.advance("gamepad-ready");
    await ready;
    app.incoming.push(JSON.stringify({method: "play", id: 1, appId}));
    await app.advance("gamepad-ready");
    assert.equal(await app.shell.showForGame(appId, 3, async () => { attempts++; return false; }, () => true), true);
    await app.advance("process-error:owner:2:unresponsive:259:none");
    await drain();
    app.expire();
    await drain();
    assert.equal(attempts, 2);
    assert.equal(app.owners.length, 1, "the restarted session is still verifying the account");
    await app.shell.resume(appId);
    releaseIdentity();
    await drain();
    assert.equal(app.owners.length, 2);
    await app.advance("gamepad-ready");
    assert.match(app.reports.at(-1), /game-exited-before-adopt appid=42/);
    assert.doesNotMatch(app.reports.at(-1), /game-adopted/);
});

test("a restarted Big Picture checks the game is alive before adopting it", async () => {
    let alive = true;
    const appId = 42, {app, overlayEvents} = await restartingForGame(appId, () => alive);
    const overlays = overlayEvents();
    // No exit is reported, but the process is gone by the time Valve is ready.
    alive = false;
    await app.advance("gamepad-ready");
    assert.match(app.reports.at(-1), /webview_recovery_6=game-exited-before-adopt appid=42\r\n/);
    assert.equal(overlayEvents(), overlays);
    assert.equal(await app.shell.playConfiguredTrial(43), "presented");
});

test("an adopted game that dies without an exit report does not hold the library busy", async () => {
    let alive = true;
    const appId = 42, {app} = await restartingForGame(appId, () => alive);
    await app.advance("gamepad-ready");
    assert.match(app.reports.at(-1), /webview_recovery_6=game-adopted appid=42\r\n/);
    assert.equal(await app.shell.playConfiguredTrial(43), "busy");
    alive = false;
    assert.equal(await app.shell.playConfiguredTrial(43), "presented");
    const report = app.reports.at(-1);
    assert.match(report, /webview_recovery_7=game-exited-unreported appid=42\r\n/);
    assert.match(report, /\r\ngame-returned-to-steam\r\n/);
    assert.deepEqual(app.replies.filter(e => e.event === "xbox-game").at(-1), {event: "xbox-game", appId: 0, overlay: false});
});

test("a signed hosted profile releases the primary renderer and reopens it for the owning game", async () => {
    const appId = 42, lookups = [];
    const app = fixture({primary: true, xaml: true, async findInstalled(name) {
        lookups.push(name);
        return name === "steam-win32-release-renderer-42.txt" ? {} : null;
    }});
    let launches = 0;
    const ready = app.start({async play(_id, handoff) { await handoff(); launches++; return "presented"; }});
    await drain();
    await app.advance("gamepad-ready");
    await ready;
    const play = app.shell.playConfiguredTrial(appId);
    await drain();
    assert.deepEqual(lookups, ["steam-win32-release-renderer-42.txt"]);
    assert.equal(launches, 0);
    assert.match(app.reports.at(-1), /status=game-renderer-released/);
    app.expire();
    assert.equal(await play, "presented");
    await app.shell.resume();
    await app.shell.resume(7);
    assert.equal(app.owners.length, 1);
    const returning = app.shell.resume(appId);
    assert.equal(app.shell.resume(appId), returning);
    await drain();
    await app.advance("gamepad-ready");
    await returning;
    assert.equal(app.owners.length, 2);
    assert.equal(launches, 1);
});

test("a failure after renderer release restores the library before reporting Play failed", async () => {
    const app = fixture({primary: true, xaml: true, findInstalled: async () => ({})});
    const ready = app.start({async play(_id, handoff) { await handoff(); return "failed"; }});
    await drain();
    await app.advance("gamepad-ready");
    await ready;
    const play = app.shell.playConfiguredTrial(42);
    await drain();
    app.expire();
    await drain();
    await app.advance("gamepad-ready");
    assert.equal(await play, "failed");
    assert.equal(app.owners.length, 2);
});

test("a declined or rejected Resume preserves the running game's next attempt", async () => {
    const appId = 42, app = fixture({hostedApps: [appId]});
    const ready = app.start({async play(_id, handoff) { await handoff(); return "presented"; }});
    await drain();
    await app.advance("gamepad-ready");
    await ready;
    app.incoming.push(JSON.stringify({method: "play", id: 1, appId}));
    await app.advance("gamepad-ready");
    let attempts = 0;
    assert.equal(await app.shell.showForGame(appId, 90, async view => {
        assert.equal(view, 10);
        attempts++;
        if (attempts === 1) return false;
        if (attempts === 2) throw new Error("Presenter failed to switch");
        return true;
    }), true);
    for (const [id, result] of [[2, "unavailable"], [3, "failed"], [4, "presented"]]) {
        app.incoming.push(JSON.stringify({method: "resume-game", id, appId}));
        await app.advance("gamepad-ready");
        assert.deepEqual(app.replies.find(reply => reply.id === id), {id, result});
    }
    const foreground = app.replies.filter(e => 'nativeGameForeground' in e).map(e => e.nativeGameForeground);
    assert.deepEqual(foreground, [true, false, true]);
    const milestones = app.reports.at(-1).split('\r\n').filter(stage => /^game-(resume-|switch-to-game)/.test(stage));
    assert.deepEqual(milestones, ["game-resume-declined", "game-resume-failed", "game-switch-to-game"]);
});

test("an exit during a pending Resume restores the library after the handoff settles", async () => {
    const appId = 42, app = fixture({hostedApps: [appId]});
    const ready = app.start({async play(_id, handoff) { await handoff(); return "presented"; }});
    await drain();
    await app.advance("gamepad-ready");
    await ready;
    const send = (method, id) => app.incoming.push(JSON.stringify({method, id, appId}));
    send("play", 1);
    await app.advance("gamepad-ready");
    let complete, attempts = 0;
    const switching = new Promise(resolve => { complete = resolve; });
    assert.equal(await app.shell.showForGame(appId, 90, () => { attempts++; return switching; }), true);
    send("resume-game", 2);
    send("resume-game", 3);
    await app.advance("gamepad-ready");
    assert.equal(attempts, 1);
    assert.deepEqual(app.replies.find(reply => reply.id === 3), {id: 3, result: "unavailable"});
    const returning = app.shell.resume(appId);
    complete(true);
    await returning;
    await drain();
    assert.deepEqual(app.replies.find(reply => reply.id === 2), {id: 2, result: "unavailable"});
    assert.deepEqual(app.switches.at(-1), [10, 3]);
    assert.deepEqual(app.replies.filter(e => e.event === "xbox-game").map(e => [e.appId,e.overlay]),
        [[appId,false],[appId,true],[0,false]]);
});

test("a configured launch waits for Valve, uses the owned Play path, and keeps the explicit game exit", async () => {
    const app = fixture({hostedApps: [2868840]});
    const brokerContext = {};
    vm.runInNewContext(fs.readFileSync(require.resolve("../../ui/signin/steam-big-picture-broker.js"), "utf8"), brokerContext);
    let installed = false, launches = 0, quits = 0;
    const broker = brokerContext.steamBigPictureBroker.create({
        current: () => true,
        steamId: () => "76561198000000001",
        games: [{id: 2868840}],
        busy: () => false,
        isInstalled: async () => installed,
        play: async () => { launches++; return "presented"; }
    });
    const ready = app.start(broker);
    await drain();
    assert.equal(await app.shell.playConfiguredTrial(2868840), "unavailable");
    await app.advance("gamepad-ready");
    await ready;
    app.incoming.push(JSON.stringify({id: 1, method: "playConfiguredTrial", appId: 2868840}));
    await app.advance("gamepad-ready");
    assert.ok(app.replies.some(reply => reply.id === 1 && reply.error === "unavailable"));
    assert.equal(await app.shell.playConfiguredTrial(646570), "not-owned");
    assert.equal(await app.shell.playConfiguredTrial(2868840), "not-installed");
    assert.deepEqual(app.switches, [[10,3]]);
    assert.equal(launches, 0);
    installed = true;
    assert.equal(await app.shell.playConfiguredTrial(2868840), "presented");
    assert.equal(await app.shell.playConfiguredTrial(2868840), "unavailable");
    assert.deepEqual(app.switches, [[10,3],[3,10]]);
    assert.equal(launches, 1);
    assert.equal(quits, 0);
    assert.equal(await app.shell.showForGame(2868840, 90, async () => true, () => { quits++; return true; }), true);
    app.incoming.push(JSON.stringify({id: 2, method: "quit-game", appId: 2868840}));
    await app.advance("gamepad-ready");
    assert.ok(app.replies.some(reply => reply.id === 2 && reply.result === "quit-requested"));
    assert.equal(quits, 1);
    await app.shell.resume(2868840);
    await drain();
    assert.match(app.reports.at(-1), /game-returned-to-steam/);
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

test("the foreground Valve library relays authorized native install milestones without a game handoff", async () => {
    const app = fixture();
    const requests = [];
    let finishInstall;
    const started = app.start({install(id, progress) {
        requests.push(id);
        progress("queued");
        return new Promise(resolve => { finishInstall = resolve; });
    }});
    await drain();
    app.incoming.push('{"method":"install","id":11,"appId":2868840}');
    await app.advance("gamepad-window-created");
    assert.deepEqual(requests, []);
    await app.advance("gamepad-ready");
    await started;
    const switches = [...app.switches];
    app.incoming.push('{"method":"install","id":12,"appId":2868840}');
    await app.advance("gamepad-ready");
    assert.deepEqual(requests, [2868840]);
    assert(app.replies.some(reply => reply.event === "xbox-install" && reply.appId === 2868840 && reply.stage === "queued"));
    assert(!app.replies.some(reply => reply.id === 12));
    finishInstall("installed");
    await drain();
    assert(app.replies.some(reply => reply.id === 12 && reply.result === "installed"));
    assert.deepEqual(app.switches, switches);
});

test("only ready foreground Big Picture requests a game-scoped Cloud backup", async () => {
    const app = fixture();
    const calls = [];
    const started = app.start({
        cloudBackup(id, download) {
            calls.push([id, download]);
            return download ? "snapshot:67" : "snapshots:2";
        },
        cloudRestore(id) {
            calls.push([id, "restore"]);
            return "restored:67";
        }
    });
    await drain();
    app.incoming.push('{"method":"cloud-backup-status","id":1,"appId":646570}');
    await app.advance("gamepad-window-created");
    assert.deepEqual(calls, []);
    await app.advance("gamepad-ready");
    await started;
    app.incoming.push('{"method":"cloud-backup-status","id":2,"appId":646570}',
        '{"method":"cloud-backup-download","id":3,"appId":646570}',
        '{"method":"cloud-backup-download","id":4,"appId":"646570"}',
        '{"method":"cloud-backup-upload","id":5,"appId":646570}',
        '{"method":"cloud-backup-restore","id":6,"appId":646570}');
    await app.advance("gamepad-ready");
    assert.deepEqual(calls, [[646570, false], [646570, true], [646570, "restore"]]);
    assert(app.replies.some(reply => reply.id === 2 && reply.result === "snapshots:2"));
    assert(app.replies.some(reply => reply.id === 3 && reply.result === "snapshot:67"));
    assert(app.replies.some(reply => reply.id === 6 && reply.result === "restored:67"));
    for (const id of [1, 4, 5]) assert(app.replies.some(reply => reply.id === id && reply.error === "unavailable"));
});
