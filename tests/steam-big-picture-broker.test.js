const assert = require("node:assert/strict");
const fs = require("node:fs");
const test = require("node:test");
const vm = require("node:vm");

const context = vm.createContext({});
vm.runInContext(fs.readFileSync(require.resolve("../probe/signin/steam-big-picture-broker.js"), "utf8"), context);

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
    for (const outcome of ["ready", "missing", "failed", "lost"]) {
        const elements = new Map();
        const statuses = [];
        const document = {
            documentElement: { getAttribute: name => name === "data-steam-big-picture" ? "true" : null },
            getElementById(id) {
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
        let loseRenderer;
        if (outcome !== "missing") {
            window.steamBigPictureHost = { async open(broker, onFailure) {
                loseRenderer = onFailure;
                calls.push(await broker.library());
                if (outcome === "failed") throw Error("renderer startup");
            } };
        }
        const localFolder = { createFileAsync: () => Operation.resolve({}), tryGetItemAsync: () => Operation.resolve(null) };
        const globals = {
            window, document, navigator: outcome === "missing" ? {} : {gamepadInputEmulation: "mouse"},
            Promise: Operation, steamBigPictureBroker: context.steamBigPictureBroker, libraryArt: {schedule() {}},
            Windows: {
                Storage: {
                    ApplicationData: { current: { localFolder } },
                    CreationCollisionOption: { replaceExisting: 1 },
                    FileIO: { writeTextAsync: () => Operation.resolve(), readTextAsync: () => Operation.resolve("status=steam-library-rendering\r\n") }
                },
                Web: { Http: { Filters: { HttpBaseProtocolFilter: class {} }, HttpClient: class { close() {} } } }
            },
            XboxProbe: { Native: { ProbeRunner: { steamSessionSteamId: () => "76561197960268001", resumeSteamSignIn: () =>
                Operation.resolve(JSON.stringify([{ id: 12, name: "A", game: true }])) } } }
        };
        vm.runInNewContext(fs.readFileSync(require.resolve("../probe/signin/steam-signin.js"), "utf8"), globals);
        // The platform's first session lookup has not completed. Fresh QR
        // must already use focus navigation, even when no session is restored.
        assert.equal(globals.navigator.gamepadInputEmulation, outcome === "missing" ? undefined : "keyboard");
        await new Promise(resolve => setImmediate(resolve));
        assert.equal(document.getElementById("signin").hidden, true);
        assert.equal(document.getElementById("library").hidden, true);
        assert.equal(statuses.length, 1);
        if (outcome === "lost") {
            assert.equal(statuses[0].hidden, true);
            loseRenderer();
            assert.match(statuses[0].textContent, /Reopen/);
            assert.equal(statuses[0].hidden, false);
            assert.equal(document.getElementById("library").hidden, true);
        }
        if (outcome === "missing") assert.match(statuses[0].textContent, /Reinstall/);
        if (outcome === "failed") assert.match(statuses[0].textContent, /Reopen/);
        if (outcome === "ready") {
            assert.equal(statuses[0].hidden, true);
            assert.deepEqual(JSON.parse(JSON.stringify(calls)), [[{ id: 12, name: "A", installed: false, canPlay: false }]]);
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
