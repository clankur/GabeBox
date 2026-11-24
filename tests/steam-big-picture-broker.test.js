const assert = require("node:assert/strict");
const fs = require("node:fs");
const test = require("node:test");
const vm = require("node:vm");

const context = vm.createContext({});
vm.runInContext("delete Promise.prototype.finally", context);
vm.runInContext(fs.readFileSync(require.resolve("../ui/signin/steam-big-picture-broker.js"), "utf8"), context);

function fixture() {
    const calls = [];
    const session = {
        current: () => true,
        steamId: () => "76561197960268001",
        busy: () => false,
        games: [{ id: 12, name: "A", game: true, privateData: "internal" }, { id: 23, name: "B", game: true }],
        installedGames: async () => new Set([12]),
        isInstalled: async (game) => game.id === 12,
        play: async (game) => { calls.push(["play", game]); return "presented"; },
        install: async (game) => { calls.push(["install", game]); return "installed"; }
    };
    return { calls, session, broker: context.steamBigPictureBroker.create(session) };
}

test("lists every owned game, with only its public fields and observed install state", async () => {
    const { session, broker } = fixture();
    const games = await broker.library();
    assert.deepEqual(JSON.parse(JSON.stringify(games)), [
        { id: 12, name: "A", installed: true, canPlay: true },
        { id: 23, name: "B", installed: false, canPlay: false }
    ]);
    games[0].id = 99;
    assert.equal(session.games[0].id, 12);
    assert.equal(await broker.play(99), "not-owned");
});

test("only numeric owned AppIDs can reach the real play or install action", async () => {
    const { session, broker, calls } = fixture();
    for (const invalid of [true, NaN, Infinity, 0, -12, 0x1000000, 12.5, "12", { id: 12 }]) {
        assert.equal(await broker.play(invalid), "invalid-app");
        assert.equal(await broker.install(invalid), "invalid-app");
    }
    assert.equal(await broker.play(5321), "not-owned");
    assert.equal(await broker.install(5321), "not-owned");
    assert.equal(await broker.play(23), "not-installed");
    assert.equal(await broker.install(12), "already-installed");
    assert.equal(await broker.play(12), "presented");
    assert.equal(await broker.install(23), "installed");
    assert.deepEqual(calls, [["play", session.games[0]], ["install", session.games[1]]]);
});

test("cloud backups are exposed and dispatched only for a supported installed game under the current account", async () => {
    const {session, broker, calls} = fixture();
    session.supportsCloudBackup = id => id === 12 || id === 23;
    let finish;
    session.cloudBackup = async (id, download) => {
        calls.push([id, download]);
        if (download) return new Promise(resolve => { finish = resolve; });
        return "snapshots:0";
    };
    const games = await broker.library();
    assert.equal(games[0].cloudBackup, true);
    assert.equal(games[1].cloudBackup, undefined);
    assert.equal(await broker.cloudBackup("12", true), "invalid-app");
    assert.equal(await broker.cloudBackup(13, true), "not-owned");
    assert.equal(await broker.cloudBackup(23, false), "not-installed");
    session.supportsCloudBackup = () => false;
    assert.equal(await broker.cloudBackup(12, true), "unsupported");
    session.supportsCloudBackup = id => id === 12;
    assert.equal(await broker.cloudBackup(12, false), "snapshots:0");
    const pending = broker.cloudBackup(12, true);
    assert.equal(await broker.cloudBackup(12, true), "busy");
    assert.equal(await broker.play(12), "busy");
    assert.equal(await broker.signOut(), "busy");
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(calls, [[12, false], [12, true]]);
    session.steamId = () => "76561197960268002";
    finish("snapshot:67");
    assert.equal(await pending, "signed-out");
    assert.equal(await broker.cloudBackup(12, false), "signed-out");
});

test("Cloud restore requires its own support and shares the download and Play lock", async () => {
    const {session, broker, calls} = fixture();
    session.supportsCloudBackup = () => true;
    session.cloudBackup = async () => "snapshot:67";
    session.supportsCloudRestore = id => id === 12;
    let finish;
    session.cloudRestore = id => new Promise(resolve => {
        calls.push(id);
        finish = resolve;
    });
    assert.equal((await broker.library())[0].cloudRestore, true);
    assert.equal((await broker.library())[1].cloudRestore, undefined);
    assert.equal(await broker.cloudRestore(23), "unsupported");
    session.isInstalled = async () => false;
    assert.equal(await broker.cloudRestore(12), "not-installed");
    session.isInstalled = async () => true;
    const restore = broker.cloudRestore(12);
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(calls, [12]);
    assert.equal(await broker.cloudBackup(12, true), "busy");
    assert.equal(await broker.play(12), "busy");
    finish("restored:67");
    assert.equal(await restore, "restored:67");
    assert.equal(await broker.play(12), "presented");
});

test("only the current idle owner can sign out using the stock menu", async () => {
    const {session, broker} = fixture();
    let calls = 0;
    session.signOut = async () => { calls++; return "signed-out"; };
    session.busy = () => true;
    assert.equal(await broker.signOut(), "busy");
    session.busy = () => false;
    const switched = broker.signOut();
    session.steamId = () => "76561197960268002";
    assert.equal(await switched, "session-changed");
    assert.equal(calls, 0);
    assert.equal(await broker.signOut(), "session-changed");
    session.steamId = () => "76561197960268001";
    assert.equal(await broker.signOut(), "signed-out");
    assert.equal(calls, 1);
});

test("signing out while an install check or catalog request is pending revokes the broker", async () => {
    const { session, broker, calls } = fixture();
    let installed, catalog;
    session.isInstalled = () => new Promise(resolve => { installed = resolve; });
    session.installedGames = () => new Promise(resolve => { catalog = resolve; });
    const action = broker.play(12);
    const library = broker.library();
    assert.equal(await broker.install(23), "busy");
    session.current = () => false;
    installed(true);
    catalog(new Set([12]));
    assert.equal(await action, "signed-out");
    await assert.rejects(library, /signed-out/);
    assert.equal(await broker.play(12), "signed-out");
    assert.deepEqual(calls, []);
});

test("a completed or rejected storage action releases Play in the Xbox host", async () => {
    const {session, broker} = fixture();
    let finish;
    session.storage = () => new Promise(resolve => { finish = resolve; });
    const migration = broker.storage("migrate", "");
    assert.equal(await broker.play(12), "busy");
    finish({ok: true});
    assert.equal((await migration).ok, true);
    assert.equal(await broker.play(12), "presented");
    session.storage = async () => { throw new Error("storage unavailable"); };
    await assert.rejects(broker.storage("move", {}), /storage unavailable/);
    assert.equal(await broker.play(12), "presented");
});

test("pending native actions cannot overlap and failures leave the broker usable", async () => {
    const { session, broker } = fixture();
    let reject;
    session.play = () => new Promise((resolve, fail) => { reject = fail; });
    const first = broker.play(12);
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(await broker.play(12), "busy");
    reject(new Error("private native detail"));
    assert.equal(await first, "failed");
    session.busy = () => true;
    assert.equal(await broker.install(23), "busy");
    session.busy = () => false;
    assert.equal(await broker.install(23), "installed");
});

test("integrated sign-in goes directly to the Valve host, including when startup fails", async () => {
    class Operation extends Promise {
        done(resolve, reject) { this.then(resolve, reject); }
    }
    for (const outcome of ["ready", "missing", "failed", "lost", "configured", "configured-consumed",
        "cold-hosted-trial", "mounted-storage", "native-unavailable", "prerequisite-restart",
        "prerequisite-other-account", "prerequisite-request", "early-catalog", "early-catalog-other-account",
        "primary-activation", "startpage-activation-unavailable"]) {
        const elements = new Map();
        const statuses = [];
        const rootAttributes = {"data-steam-big-picture": "true"};
        if (outcome === "primary-activation") rootAttributes["data-steam-primary-xaml"] = "true";
        const document = {
            documentElement: {
                getAttribute: name => rootAttributes[name] ?? null,
                setAttribute(name, value) { rootAttributes[name] = value; }
            },
            getElementById(id) {
                if (id === "xcom-game") return null;
                if (!elements.has(id)) elements.set(id, {
                    hidden: true, value: "", addEventListener(type, listener) { this[type] = listener; },
                    appendChild() {}, setAttribute() {}, focus() {}
                });
                return elements.get(id);
            },
            createElement() { return { setAttribute() {} }; },
            createDocumentFragment() { return {}; },
            body: { appendChild: item => statuses.push(item) }
        };
        const window = { steamBigPictureBroker: context.steamBigPictureBroker };
        const calls = [];
        const configured = outcome.startsWith("configured");
        const earlyCatalog = outcome.startsWith("early-catalog");
        const coldTrial = outcome === "cold-hosted-trial";
        const configuredAppIds = [];
        const marker = {name: coldTrial ? "steam-launch-app.txt" : "steam-launch-via-big-picture.txt"};
        let closeTrial, latestReport;
        if (coldTrial) window.steamSdlRuntimes = [{appId: 12, backend: "sdl-host", launch(_, appId, attempt) {
            assert.equal(appId, 12);
            closeTrial = attempt.onExit;
            return "steam_sdl_host_frame=presented\r\n";
        }}];
        const settings = new Map();
        const markerKey = "steamBigPictureLaunchProbePackage";
        const packageId = "signed-host-1";
        if (outcome === "configured-consumed") settings.set(markerKey, packageId);
        const restartKey = "steamPrerequisitePlayActivation";
        if (outcome === "prerequisite-restart" || outcome === "prerequisite-other-account") {
            const account = outcome === "prerequisite-restart" ? "76561197960268001" : "76561197960268002";
            settings.set(restartKey, "steam-prerequisite-play " + account + " 12 123");
        }
        let finishWarmup;
        const warmup = new Promise(resolve => { finishWarmup = resolve; });
        let loseRenderer, activeBroker, firstManifest, inventory, finishCatalog;
        let activeOwner = "76561197960268001", catalogChanges = 0;
        if (outcome === "prerequisite-request") firstManifest = {name: "appmanifest_12.acf"};
        if (outcome !== "missing") {
            window.steamBigPictureHost = { async open(broker, onFailure) {
                loseRenderer = onFailure;
                activeBroker = broker;
                calls.push(await broker.library());
                if (outcome === "failed") throw Error("renderer startup");
                if (configured) await warmup;
            }, playConfiguredTrial(appId) { configuredAppIds.push(appId); }, resume() {},
            catalogChanged() { ++catalogChanges; } };
        }
        const manifests = {
            getFilesAsync: () => Operation.resolve({size: 1, getAt: () => firstManifest}),
            tryGetItemAsync: () => Operation.resolve(firstManifest)
        };
        const steamLibrary = {tryGetItemAsync: () => Operation.resolve(manifests)};
        const localFolder = {createFileAsync: name => Operation.resolve({name}), tryGetItemAsync: () =>
            Operation.resolve(firstManifest ? steamLibrary : null)};
        let installPolls = 0, sessionLookups = 0;
        const qrRequests = [];
        const restartRequests = [], downloadReports = [];
        const globals = {
            window, document, navigator: outcome === "missing" ? {} : {gamepadInputEmulation: "mouse"},
            setTimeout, clearTimeout, btoa,
            Promise: Operation, steamBigPictureBroker: context.steamBigPictureBroker, libraryArt: {schedule() {}},
            Windows: {
                ApplicationModel: { Package: { current: { id: {fullName: packageId}, installedLocation: {
                    tryGetItemAsync: name => Operation.resolve(
                        outcome === "ready" && name === "steam-library-inventory.txt" ? {} :
                            (configured || coldTrial) && name === marker.name ? marker : null)
                } } }, Core: { CoreApplication: { requestRestartAsync(args) {
                    restartRequests.push(args);
                    return Operation.resolve("unavailable");
                } } } },
                System: { Profile: { AnalyticsInfo: { versionInfo: {
                    deviceFamily: outcome === "missing" ? "Windows.Desktop" : "Windows.Xbox"
                } } } },
                Storage: {
                    Streams: { UnicodeEncoding: {utf8: 0} },
                    ApplicationData: { current: { localFolder, localSettings: {values: {
                        hasKey: key => settings.has(key),
                        lookup: key => settings.get(key),
                        insert: (key, value) => settings.set(key, value),
                        remove: key => settings.delete(key)
                    }} } },
                    CreationCollisionOption: { replaceExisting: 1 },
                    FileIO: {
                        writeTextAsync(file, text) {
                            if (file.name === "steam-library-inventory.json") inventory = JSON.parse(text);
                            if (file.name === "steam-download-report.txt") downloadReports.push(text);
                            latestReport = text;
                            return Operation.resolve();
                        },
                        readTextAsync: file => Operation.resolve(
                            file === firstManifest ? '"StateFlags" "4"\r\n' :
                                file === marker ? "12" : "status=steam-library-rendering\r\n")
                    }
                },
                Foundation: { Uri: class { constructor(url) { this.url = url; } } },
                Web: { Http: {
                    Filters: { HttpBaseProtocolFilter: class {} },
                    HttpStringContent: class { close() {} },
                    HttpClient: class {
                        close() {}
                        postAsync(uri) {
                            qrRequests.push(uri.url);
                            return Operation.reject(Error("offline QR control"));
                        }
                    }
                } }
            },
            XboxProbe: { Native: { ProbeRunner: {
                steamSessionSteamId: () => activeOwner,
                launchSteamGame() {
                    assert.equal(outcome, "prerequisite-request");
                    return Operation.resolve("steam_launch_redistributables_manifest=complete\r\n" +
                        "steam_launch=prerequisite-restart-required\r\n");
                },
                resumeSteamSignIn() {
                    ++sessionLookups;
                    assert(!earlyCatalog, "Use the native progressive restore for Big Picture");
                    if (outcome === "native-unavailable") {
                        throw Object.assign(Error("local activation details"), {number: 0x80040154 | 0});
                    }
                    return Operation.resolve(JSON.stringify([{ id: 12, name: "A", game: true }]));
                },
                queueSteamDownload: appId => {
                    assert.equal(appId, 12);
                    return Operation.resolve("downloading:1290\r\nsteam_download_measurement=unavailable\r\n" +
                        "steam_download_active=other\r\nsteam_download_other_active_app=23\r\n" +
                        "steam_download_depot_activity=observed\r\nsteam_download_bytes_downloaded=0\r\n" +
                        "steam_download_bytes_to_download=2048\r\n");
                },
                steamDownloadStatus: () => {
                    if (installPolls++ === 0) return "downloading:1290\r\nsteam_download_measurement=steam\r\n" +
                        "steam_download_bytes_downloaded=1024\r\nsteam_download_bytes_to_download=2048\r\n" +
                        "steam_download_active=selected\r\nsteam_download_other_active_app=0\r\n" +
                        "steam_download_network_bytes_per_second=512\r\nsteam_download_disk_bytes_per_second=0\r\n";
                    firstManifest = {name: "appmanifest_12.acf"};
                    return "installed:4\r\nsteam_download_measurement=unavailable\r\n" +
                        "steam_download_active=none\r\nsteam_download_other_active_app=0\r\n";
                }
            } } }
        };
        let activationRegistrations = 0;
        if (outcome === "primary-activation" || outcome === "startpage-activation-unavailable") {
            globals.Windows.UI = {WebUI: {WebUIApplication: {addEventListener() {
                ++activationRegistrations;
                throw new Error("Access is denied");
            }}}};
        }
        if (earlyCatalog) {
            Object.assign(globals.XboxProbe.Native.ProbeRunner, {
                resumeSteamBigPictureSignIn: () => Operation.resolve(JSON.stringify([{id: 12, name: "A", game: true}])),
                completeSteamCatalog: () => new Operation(resolve => { finishCatalog = resolve; })
            });
        }
        let driveMounted = true;
        const nativeStorageCalls = [];
        if (outcome === "mounted-storage") {
            globals.XboxProbe.Native.ProbeRunner.steamLibraryCommand = (operation, argument) => {
                nativeStorageCalls.push([operation, argument]);
                return Operation.resolve(JSON.stringify({
                    folders: [{id: "D", mounted: true, apps: []},
                        {id: "E", mounted: driveMounted, apps: [{appId: 12}]}]
                }));
            };
        }
        const signInPage = vm.createContext(globals);
        vm.runInContext(fs.readFileSync(require.resolve("../ui/signin/steam-game-launch.js"), "utf8"), signInPage);
        vm.runInContext(fs.readFileSync(require.resolve("../ui/signin/steam-signin.js"), "utf8"), signInPage);
        assert.equal(activationRegistrations, outcome === "startpage-activation-unavailable" ? 1 : 0);
        // The platform's first session lookup has not completed. Fresh QR
        // must already use focus navigation, even when no session is restored.
        assert.equal(globals.navigator.gamepadInputEmulation, outcome === "missing" ? undefined : "keyboard");
        const qr = elements.get("signin");
        assert.equal(qr.click, undefined);
        await new Promise(resolve => setImmediate(resolve));
        if (outcome === "native-unavailable") {
            assert.match(latestReport, /steam_session_restore=unavailable\r\nsteam_session_restore_hresult=0x80040154/);
            assert.doesNotMatch(latestReport, /steam_qr_challenge|local activation details/);
            assert.match(document.getElementById("status").textContent, /runtime could not open/);
            assert.deepEqual(calls, []);
            const retry = elements.get("retry");
            retry.tagName = "BUTTON";
            assert.equal(retry.hidden, false);
            retry.click({detail: 1});
            retry.click({detail: 2});
            await new Promise(resolve => setImmediate(resolve));
            assert.equal(sessionLookups, 1);
            assert.equal(qrRequests.length, 1);
            assert.match(qrRequests[0], /\/BeginAuthSessionViaQR\//);
            assert.match(latestReport, /steam_qr_challenge=failed/);
            const press = {target: retry, keyCode: 195, preventDefault() { this.defaultPrevented = true; }};
            qr.keydown({...press, repeat: true});
            qr.keydown(press);
            assert.equal(press.defaultPrevented, true);
            await new Promise(resolve => setImmediate(resolve));
            assert.equal(sessionLookups, 1);
            assert.equal(qrRequests.length, 2);
            continue;
        }
        assert.equal(document.getElementById("signin").hidden, true);
        assert.equal(document.getElementById("library").hidden, true);
        assert.equal(statuses.length, 1);
        if (earlyCatalog) {
            assert.equal(statuses[0].hidden, true);
            assert.deepEqual(Array.from(calls[0], game => game.id), [12]);
            if (outcome === "early-catalog-other-account") activeOwner = "76561197960268002";
            finishCatalog(JSON.stringify([{id: 23, name: "B", game: true}]));
            await new Promise(resolve => setImmediate(resolve));
            if (outcome === "early-catalog") {
                assert.equal(catalogChanges, 1);
                assert.deepEqual(Array.from(await activeBroker.library(), game => game.id), [12, 23]);
            } else {
                assert.equal(catalogChanges, 0);
                await assert.rejects(activeBroker.library(), /signed-out/);
            }
        }
        if (outcome === "prerequisite-restart" || outcome === "prerequisite-other-account") {
            assert.deepEqual(configuredAppIds, outcome === "prerequisite-restart" ? [12] : []);
            assert.equal(settings.has(restartKey), false);
        }
        if (outcome === "prerequisite-request") {
            assert.equal(await activeBroker.play(12), "prerequisite-restart-required");
            assert.equal(restartRequests.length, 1);
            assert.match(restartRequests[0], /^steam-prerequisite-play 76561197960268001 12 \d+$/);
            assert.match(latestReport, /^steam_launch_prerequisite_restart_request=unavailable\r?$/m);
        }
        if (coldTrial) {
            assert.match(latestReport, /steam_sdl_host_frame=presented/);
            assert.deepEqual(calls, [], "Steam cannot start while the cold game owns the primary window");
            closeTrial();
            await new Promise(resolve => setImmediate(resolve));
            assert.equal(calls.length, 1);
        }
        if (configured) {
            assert.deepEqual(configuredAppIds, []);
            finishWarmup();
            await new Promise(resolve => setImmediate(resolve));
            assert.equal(statuses[0].hidden, true);
            assert.equal(settings.get(markerKey), packageId);
            assert.deepEqual(configuredAppIds, outcome === "configured" ? [12] : []);
        }
        if (outcome === "lost") {
            assert.equal(statuses[0].hidden, true);
            loseRenderer();
            assert.match(statuses[0].textContent, /Reopen/);
            assert.equal(statuses[0].hidden, false);
            assert.equal(document.getElementById("library").hidden, true);
        }
        if (outcome === "missing") assert.match(statuses[0].textContent, /Reinstall/);
        if (outcome === "failed") assert.match(statuses[0].textContent, /Reopen/);
        if (outcome === "mounted-storage") {
            assert.equal(calls[0][0].installed, true);
            driveMounted = false;
            assert.equal((await activeBroker.library())[0].installed, false);
            assert.equal(await activeBroker.play(12), "not-installed");
            assert.deepEqual(nativeStorageCalls, [["list", ""], ["list", ""], ["list", ""]]);
        }
        if (outcome === "ready") {
            assert.equal(statuses[0].hidden, true);
            assert.deepEqual(inventory, [{id: 12, name: "A"}]);
            assert.deepEqual(JSON.parse(JSON.stringify(calls)), [[{ id: 12, name: "A", installed: false, canPlay: false }]]);
            const stages = [];
            globals.setTimeout = callback => setTimeout(callback, 0);
            assert.equal(await activeBroker.install(12, (stage, progress) => {
                stages.push(progress ? [stage, {...progress}] : [stage]);
            }), "installed");
            const measured = {measurement: "steam", downloaded: 1024, downloadTotal: 2048,
                networkBytesPerSecond: 512, diskBytesPerSecond: 0};
            assert.deepEqual(stages, [["starting"], ["downloading", {measurement: "unavailable", depotActivity: true}],
                ["downloading", measured], ["installed", measured]]);
            assert.ok(downloadReports.some(report =>
                report.includes("steam_download_active=other\r\nsteam_download_other_active_app=23\r\n") &&
                report.includes("status=steam-download-in-progress")));
            assert.ok(downloadReports.some(report =>
                report.includes("steam_download_active=selected\r\nsteam_download_other_active_app=0\r\n") &&
                report.includes("status=steam-download-in-progress")));
            assert.match(downloadReports.at(-1),
                /steam_download_active=none\r\nsteam_download_other_active_app=0[\s\S]+status=steam-download-complete/);
            assert.equal((await activeBroker.library())[0].installed, true);
            assert.equal(await activeBroker.install(12), "already-installed");
            globals.navigator.gamepadInputEmulation = "mouse";
            globals.XboxProbe.Native.ProbeRunner.signOutSteam = () => Operation.resolve(true);
            assert.equal(await elements.get("sign-out").click(), "signed-out");
            assert.equal(elements.get("signin").hidden, false);
            assert.equal(globals.navigator.gamepadInputEmulation, "keyboard");
        }
    }
});

test("session only exposes a validated public SteamID from current native handles", async () => {
    const { session, broker } = fixture();
    session.steamId = () => "76561197960268001";
    assert.deepEqual(JSON.parse(JSON.stringify(await broker.session())), {steamId: "76561197960268001"});
    for (const value of [undefined, null, {steamId: "76561197960268001", secret: "private"}, "private", "18446744073709551616"]) {
        session.steamId = () => value;
        await assert.rejects(broker.session(), /session-unavailable/);
    }
    session.steamId = () => "";
    assert.equal((await broker.session()).steamId, null);
    session.steamId = () => assert.fail("revoked native query");
    session.current = () => false;
    assert.equal((await broker.session()).steamId, null);
});

test("a changed native account or dropped connection revokes the prior library and its actions", async () => {
    const { session, broker, calls } = fixture();
    for (const id of ["", "76561197960268999"]) {
        session.steamId = () => id;
        assert.equal((await broker.session()).steamId, null);
        await assert.rejects(broker.library(), /signed-out/);
        assert.equal(await broker.play(12), "signed-out");
        assert.equal(await broker.install(23), "signed-out");
        assert.deepEqual(calls, []);
    }
});
