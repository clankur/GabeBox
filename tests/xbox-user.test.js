const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const source = fs.readFileSync(require.resolve("../ui/big-picture-bridge/xbox-user.js"), "utf8");
const settle = () => new Promise(resolve => setImmediate(resolve));
const steamId = "76561197960268001";

function setup(send, runtime) {
    let refresh;
    const context = vm.createContext({
        setInterval(callback, ms) { assert.equal(ms, 5000); refresh = callback; return 1; },
        clearInterval() { refresh = null; }
    });
    vm.runInContext(source, context);
    const client = {};
    context.installXboxUser(client, send, () => runtime);
    return { user: client.User, poll() { assert.ok(refresh); refresh(); }, active: () => !!refresh };
}

test("verified session drives both Valve observers; only public identity leaves the broker", async () => {
    let result = { steamId, credential: "private" };
    let reads = 0;
    const { user, poll, active } = setup(method => {
        assert.equal(method, "session"); reads++; return result;
    });
    const states = [], users = [];
    const loginSub = user.RegisterForLoginStateChange((...args) => states.push(args));
    const userSub = user.RegisterForCurrentUserChanges(user => users.push(JSON.parse(JSON.stringify(user))));
    await settle();
    assert.equal(reads, 1);
    assert.deepEqual(states, [["", 5, 1, 0, 100, ""]]);
    assert.deepEqual(users, [{ strSteamID: steamId }]);
    poll(); await settle();
    assert.equal(states.length, 1);
    result = { steamId: null }; poll(); await settle();
    assert.deepEqual(states[1], ["", 1, 0, 0, 0, ""]);
    assert.deepEqual(users[1], {});
    loginSub.unregister();
    result = { steamId: "76561197960268002" }; poll(); await settle();
    assert.equal(states.length, 2);
    assert.deepEqual(users[2], { strSteamID: result.steamId });
    userSub.unregister();
    assert.equal(active(), false);
});

test("Valve can start login before the verified Xbox session reaches its listeners", async () => {
    let completeSession, reads = 0;
    const { user } = setup(method => {
        assert.equal(method, "session");
        reads++;
        return new Promise(resolve => { completeSession = resolve; });
    });
    assert.equal(user.StartLogin(), undefined);
    assert.equal(user.StartLogin(), undefined);
    const states = [];
    const sub = user.RegisterForLoginStateChange((...args) => states.push(args));
    await settle();
    assert.equal(reads, 1);
    assert.deepEqual(states, []);
    completeSession({ steamId });
    await settle();
    assert.deepEqual(states, [["", 5, 1, 0, 100, ""]]);
    sub.unregister();
});

test("failed or invalid native reads never produce a login or a logout event", async () => {
    let result = () => { throw Error("unavailable"); };
    const { user, poll } = setup(() => result());
    const states = [];
    const sub = user.RegisterForLoginStateChange((...args) => states.push(args));
    await settle();
    for (const value of [null, {}, {steamId: 123}, {steamId: "18446744073709551616"}, {steamId: "private"}]) {
        result = () => value; poll(); await settle();
    }
    assert.deepEqual(states, []);
    result = () => ({ steamId }); poll(); await settle();
    assert.equal(states.length, 1);
    result = () => Promise.reject(Error("unavailable")); poll(); await settle();
    assert.equal(states.length, 1);
    sub.unregister();
});

test("unregistered pending callbacks stay revoked", async () => {
    let resolve;
    const { user, active } = setup(() => new Promise(done => { resolve = done; }));
    const sub = user.RegisterForLoginStateChange(() => assert.fail("unregistered"));
    await settle(); sub.unregister();
    resolve({ steamId }); await settle();
    assert.equal(active(), false);
});


test("public user config follows the verified session without changing CM state", async () => {
    const config = {steamid: "", accountid: 0, logged_in: false, account_name: "", token: undefined};
    class SteamID {
        constructor(value) { this.value = BigInt(value); }
        GetAccountID() { return Number(this.value & 0xffffffffn); }
    }
    const runtime = id => ({30610: {iA: config}, 55324: {b: SteamID}})[id];
    let result = {steamId};
    const {user, poll} = setup(() => result, runtime);
    const sub = user.RegisterForCurrentUserChanges(() => {});
    await settle();
    assert.equal(config.steamid, steamId);
    result = {steamId: "76561197960268002"};
    poll(); await settle();
    assert.equal(config.steamid, result.steamId);
    assert.equal(config.accountid, Number(BigInt(result.steamId) & 0xffffffffn));
    result = {steamId: null};
    poll(); await settle();
    assert.deepEqual(config, {steamid: "", accountid: 0, logged_in: false, account_name: "", token: undefined});
    sub.unregister();
});
