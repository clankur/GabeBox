const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const vm = require("node:vm");

const adapterSource = fs.readFileSync(path.join(__dirname, "../probe/signin/xcom-dos.js"), "utf8");
const publishedConfig = [
    "[dosbox]", " machine = VGA ", "memsize=32", "[cpu]", "cycles=auto limit 8000",
    "[sdl]", "fullscreen=true", "[autoexec]", "@ECHO OFF", "mount C XCOM -freesize 1024",
    "C:", "cd XCOM", "call ufocd.bat", "exit"
].join("\r\n");

function element() {
    return {
        children: [],
        handlers: {},
        appendChild(child) { this.children.push(child); child.parentNode = this; },
        removeChild(child) { this.children = this.children.filter(item => item !== child); },
        setAttribute(key, value) { this[key] = value; },
        addEventListener(event, callback) { this.handlers[event] = callback; },
        focus() { this.focused = true; },
        toDataURL() { return "data:image/png;base64,QUJD"; }
    };
}

function fixture(config, options = {}) {
    const foldersRead = [];
    const filesRead = [];
    const copies = new Map();
    const dirs = [];
    const frames = [];
    const delays = [];
    const launches = [];
    const body = element();
    const library = element();
    const playButton = element();
    const document = {
        body, createElement: element, querySelector: () => library, getElementById: () => playButton,
        documentElement: {contains: () => true}
    };
    function file(name) { return {name, isOfType: type => type === "file"}; }
    function folder(name, children = []) {
        return {
            name, isOfType: type => type === "folder",
            async getItemsAsync() {
                foldersRead.push(name);
                return {size: children.length, getAt: index => children[index]};
            },
            async getFolderAsync(childName) {
                const child = children.find(item => item.name === childName && item.isOfType("folder"));
                assert.ok(child, "Unexpected mount path " + childName);
                return child;
            },
            async getFileAsync(childName) {
                assert.equal(name, "XCom UFO Defense");
                assert.equal(childName, "dosbox.conf");
                return file(childName);
            }
        };
    }
    const guest = folder("XCOM", [
        folder("DATA", [file("LEVEL.DAT")]), folder("GAME_10", [file("PRIVATE.SAV")]),
        file("dosbox.exe"), file("UFO Defense_Patched.exe"), file("SDL.DLL")
    ]);
    const common = folder("common", [folder("XCom UFO Defense", [guest])]);
    const local = folder("LocalState", [folder("SteamLibrary", [folder("steamapps", [common])])]);
    local.createFileAsync = async name => { frames.push(name); return file(name); };
    const storage = {
        ApplicationData: {current: {localFolder: local}},
        StorageItemTypes: {file: "file", folder: "folder"}, CreationCollisionOption: {failIfExists: 0},
        FileIO: {
            readTextAsync: async () => config,
            readBufferAsync: async source => {
                filesRead.push(source.name);
                return options.readBuffer ? options.readBuffer() : {length: 3};
            },
            writeBufferAsync: async () => {}
        },
        Streams: {DataReader: {fromBuffer: () => ({readBytes: bytes => bytes.set([0, 128, 255]), close() {}})}}
    };
    const runtimeFs = {
        fs: {mkdir: dir => dirs.push(dir)},
        createFile(name, bytes) { copies.set(name, bytes); }
    };
    const game = {exit() { this.exited = true; }};
    const window = {exports: {WDOSBOX() {}}};
    const navigator = options.noPointerMode ? {} : {gamepadInputEmulation: "keyboard"};
    let now = 25000;
    vm.runInNewContext(adapterSource, {
        window, document, navigator, Promise, Uint8Array, Date: {now: () => now},
        setTimeout(callback, delay) { delays.push(delay); now += delay; callback(); },
        Windows: {Storage: storage, Security: {Cryptography: {CryptographicBuffer: {decodeFromBase64String: value => value}}}},
        Dos: async () => ({fs: runtimeFs, main: async args => { launches.push(Array.from(args)); return game; }})
    });
    const start = (automatic, checkpoint = () => Promise.resolve()) => window.xcomDosRuntime.start(checkpoint, automatic);
    return {adapter: window.xcomDosRuntime, start, dirs, foldersRead, filesRead, copies, launches, frames, delays, game, body, library, playButton, navigator};
}

test("a pointer-driven title enables Xbox mouse mode until it returns to the library", async () => {
    const sample = fixture(publishedConfig);
    await sample.start(false);
    assert.equal(sample.navigator.gamepadInputEmulation, "mouse");
    sample.body.children[0].children[1].handlers.click();
    assert.equal(sample.navigator.gamepadInputEmulation, "keyboard");

    const withoutPointerMode = fixture(publishedConfig, {noPointerMode: true});
    await withoutPointerMode.start(false);
    assert.equal("gamepadInputEmulation" in withoutPointerMode.navigator, false);
});

test("X-COM profile mounts only installed DOS data and preserves the verified Play contract", async () => {
    const host = fixture(publishedConfig);
    assert.equal(host.adapter.appId, 7760);
    assert.match(await host.start(false), /steam_dos_files=1\r\nsteam_dos=guest-started/);
    assert.deepEqual(host.dirs, ["/XCOM", "/XCOM/DATA", "/XCOM/GAME_10"]);
    assert.deepEqual(host.foldersRead, ["XCOM", "DATA"]);
    assert.deepEqual(host.filesRead, ["LEVEL.DAT"]);
    assert.deepEqual(Array.from(host.copies.get("/XCOM/DATA/LEVEL.DAT")), [0, 128, 255]);
    assert.equal(host.copies.get("/XCOMDOS.CONF"), "[dosbox]\r\nmachine=vgaonly\r\nmemsize=16\r\n[cpu]\r\ncycles=auto limit 8000\r\n");
    assert.deepEqual(host.launches[0], [
        "-conf", "XCOMDOS.CONF", "-c", "z:", "-c", "mount -u c", "-c", "mount c /XCOM -freesize 1024",
        "-c", "c:", "-c", "call UFOCD.BAT", "-c", "exit"
    ]);
    assert.deepEqual(host.frames, []);
    assert.match(await host.start(false), /already-running/);
    host.body.children[0].children[1].handlers.click();
    assert.equal(host.game.exited, true);
    assert.equal(host.library.hidden, false);
    assert.equal(host.playButton.focused, true);
});

test("changed vendor autoexec cannot select a different mount or entry point", async () => {
    for (const config of [
        publishedConfig.replace("mount C XCOM", "mount C OTHER"),
        publishedConfig.replace("call ufocd.bat", "call OTHER.BAT")
    ]) {
        const host = fixture(config);
        assert.match(await host.start(true), /steam_dos_phase=reading-config/);
        assert.equal(host.launches.length, 0);
        assert.equal(host.filesRead.length, 0);
        assert.equal(host.frames.length, 0);
    }
});

test("automatic trial captures retain their names and eight- and eighteen-second timing", async () => {
    const host = fixture(publishedConfig);
    const report = await host.start(true);
    assert.deepEqual(host.delays, [8000, 10000]);
    assert.deepEqual(host.frames, ["xcom-trial-25000-8s.png", "xcom-trial-25000-18s.png"]);
    for (const name of host.frames) assert.ok(report.includes("=" + name + "\r\n"));
});

function deferred() {
    let resolve;
    const promise = new Promise(done => { resolve = done; });
    return {promise, resolve};
}

test("a closed launch cannot finish an old file read or start a guest after a new Play", async () => {
    const startedRead = deferred();
    const oldBuffer = deferred();
    let readCount = 0;
    const host = fixture(publishedConfig, {readBuffer() {
        if (++readCount === 1) {
            startedRead.resolve();
            return oldBuffer.promise;
        }
        return {length: 3};
    }});
    const oldPhases = [];
    const oldRun = host.start(true, async phase => { oldPhases.push(phase); });
    await startedRead.promise;
    host.body.children[0].children[1].handlers.click();
    assert.match(await oldRun, /returned-to-library/);
    assert.match(await host.start(false), /guest-started/);
    oldBuffer.resolve({length: 3});
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(oldPhases, ["reading-config", "starting-emulator", "copying-installed-files"]);
    assert.equal(host.launches.length, 1);
    assert.deepEqual(host.frames, []);
});

test("a new Play waits for a previously submitted checkpoint to finish", async () => {
    const host = fixture(publishedConfig);
    const startedReport = deferred();
    const finishReport = deferred();
    const oldRun = host.start(false, phase => {
        if (phase === "starting-emulator") {
            startedReport.resolve();
            return finishReport.promise;
        }
    });
    await startedReport.promise;
    host.body.children[0].children[1].handlers.click();
    assert.match(await host.start(false), /already-running/);
    finishReport.resolve();
    assert.match(await oldRun, /returned-to-library/);
    assert.match(await host.start(false), /guest-started/);
    assert.equal(host.launches.length, 1);
});
