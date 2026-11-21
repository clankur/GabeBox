const assert = require("node:assert/strict");
const { readFileSync } = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const vm = require("node:vm");

const launchFile = path.join(__dirname, "../probe/signin/steam-game-launch.js");
const browser = vm.createContext({});
vm.runInContext(readFileSync(launchFile, "utf8"), browser, { filename: launchFile });
const dispatch = browser.steamGameLaunch;

function trial(automaticTrial = false) {
    return { file: {}, report: "run_id=123\r\n", automaticTrial, checkpoint: () => {} };
}

test("native Steam Play receives an unprofiled title and the original report", async () => {
    const request = trial();
    const calls = [];
    const runner = { launchSteamGame: (...args) => {
        calls.push(args);
        return Promise.resolve("steam_launch=window-ready\r\n");
    } };
    const hosted = { appId: 1, start: () => assert.fail("unmatched adapter started") };
    const launch = dispatch.forGame(runner, 2, [hosted]);
    assert.equal(launch.backend, "steam");
    assert.equal(await launch.start(request), "steam_launch=window-ready\r\n");
    assert.deepEqual(calls, [[2, request.file, request.report]]);
});

test("native Steam Play is still used when an adapter is omitted from the package", async () => {
    const runner = { launchSteamGame: (id) => "played-" + id };
    assert.equal(await dispatch.forGame(runner, 7760, []).start(trial()), "played-7760");
});

test("two packaged profiles use their own runtime only after authorization", async () => {
    const calls = [];
    const runner = { launchSteamGame: () => assert.fail("authorized adapter fell back to desktop Play") };
    const makeHosted = (appId, backend) => ({
        appId, backend,
        phaseField: "runtime_phase",
        authorizedLine: "ownership=verified",
        authorize: (received, title, attempt) => {
            assert.equal(received, runner);
            assert.equal(title, appId);
            assert.equal(attempt.report, "run_id=123\r\n");
            calls.push("authorized " + appId);
            return { then: resolve => resolve("ownership=verified\r\n") };
        },
        start: async (checkpoint, automaticTrial) => {
            calls.push("started " + appId + " " + automaticTrial);
            await checkpoint("starting");
            return "runtime=started\r\n";
        }
    });
    const hosted = [makeHosted(7760, "dosbox"), makeHosted(42, "test-runtime")];
    for (const [id, backend, automatic] of [[7760, "dosbox", true], [42, "test-runtime", false]]) {
        const launch = dispatch.forGame(runner, id, hosted);
        const request = trial(automatic);
        request.checkpoint = (progress) => {
            assert.equal(progress, "ownership=verified\r\nruntime_phase=starting\r\n");
        };
        assert.equal(launch.backend, backend);
        assert.equal(await launch.start(request), "ownership=verified\r\nruntime=started\r\n");
    }
    assert.deepEqual(calls, ["authorized 7760", "started 7760 true", "authorized 42", "started 42 false"]);
});

test("authorization errors and look-alike markers never load the runtime or fall back", async () => {
    const runner = { launchSteamGame: () => assert.fail("rejected adapter fell back to desktop Play") };
    const hosted = {
        appId: 7760,
        backend: "dosbox",
        authorizedLine: "steam_dos_install=verified",
        start: () => assert.fail("guest runtime started without authorization")
    };
    for (const denial of ["steam_launch=no-license\r\n", "fake_steam_dos_install=verified\r\n"]) {
        hosted.authorize = () => denial;
        assert.equal(await dispatch.forGame(runner, 7760, [hosted]).start(trial()), denial);
    }
    hosted.authorize = () => Promise.reject(new Error("session lost"));
    await assert.rejects(dispatch.forGame(runner, 7760, [hosted]).start(trial()), /session lost/);
});

test("both legacy and install-before-launch trial markers retain their meaning", () => {
    assert.equal(JSON.stringify(dispatch.parseTrial("7760\ninstall")),
        '{"appId":7760,"repeat":false,"install":true}');
    assert.equal(JSON.stringify(dispatch.parseTrial("427520\r\nrepeat\r\ninstall\r\n")),
        '{"appId":427520,"repeat":true,"install":true}');
    assert.equal(JSON.stringify(dispatch.parseTrial("646570")),
        '{"appId":646570,"repeat":false,"install":false}');
    for (const invalid of ["0", "16777216", "7760\nexec", "7760\ninstall\nrepeat"]) {
        assert.equal(dispatch.parseTrial(invalid), null);
    }
});
