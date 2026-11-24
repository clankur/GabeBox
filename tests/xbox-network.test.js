const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const {setImmediate: tick} = require('node:timers/promises');

function hostFixture() {
    const windows = {Networking: {Connectivity: {
        NetworkConnectivityLevel: {none: 0, localAccess: 1, constrainedInternetAccess: 2, internetAccess: 3},
        NetworkInformation: Object.assign(new EventTarget(), {getInternetConnectionProfile() { return state.profile; }})
    }}};
    const state = {windows, profile: null, changes: 0};
    const window = {};
    vm.runInNewContext(fs.readFileSync(require.resolve('../ui/signin/xbox-network.js'), 'utf8'), {window});
    const watcher = window.steamXboxNetwork(windows, () => ++state.changes);
    return {...watcher, state};
}

function profile(type, level) {
    return {getNetworkConnectivityLevel: () => level, networkAdapter: {ianaInterfaceType: type, networkAdapterId: 'test-' + type},
        isWlanConnectionProfile: type === 71, wlanConnectionProfileDetails: {getConnectedSsid: () => 'Fixture Wi-Fi'},
        getSignalBars: () => 5};
}

test('Windows preferred profile, interface changes and disconnection', () => {
    const {read, close, state} = hostFixture();
    assert.equal(read().level, 'offline');
    state.profile = profile(6, 3);
    const ethernet = read();
    assert.equal(ethernet.kind, 'ethernet');
    assert.equal(ethernet.level, 'internet');
    assert.equal(ethernet.ssid, undefined);
    state.profile = profile(71, 3);
    const wifi = read();
    assert.equal(wifi.kind, 'wifi');
    assert.equal(wifi.ssid, 'Fixture Wi-Fi');
    assert.equal(wifi.bars, 5);
    assert.notEqual(wifi.device, ethernet.device);
    assert.equal(read().accessPoint, wifi.accessPoint);
    const info = state.windows.Networking.Connectivity.NetworkInformation;
    info.dispatchEvent(new Event('networkstatuschanged'));
    assert.equal(state.changes, 1);
    state.profile = profile(71, 2);
    assert.equal(read().level, 'local');
    state.profile = profile(6, 1);
    assert.equal(read().level, 'local');
    state.profile = profile(6, 0);
    assert.equal(read().level, 'offline');
    state.profile = profile(6, 77);
    assert.equal(read().level, 'unknown');
    info.getInternetConnectionProfile = () => { throw Error('access denied'); };
    assert.equal(read().level, 'unknown');
    close();
    info.dispatchEvent(new Event('networkstatuschanged'));
    assert.equal(state.changes, 1);
});

test('an old successful query cannot overwrite disconnection; revocation stops callbacks', async () => {
    const host = new EventTarget(), window = new EventTarget(), client = {System: {}}, reads = [];
    const api = vm.runInNewContext(fs.readFileSync(require.resolve('../ui/big-picture-bridge/xbox-network.js'), 'utf8') +
        '\ninstallXboxNetwork', {window});
    const runtime = n => n === 78544 ? {co:{fromObject: value => ({serializeBinary: () => value})}} :
        n === 20887 ? {WO:{Wired:1, Wireless:2, Unknown:0}, $J:{Connected:5}} :
        {ckP:{k_EConnectivityTestResult_Unknown:0, k_EConnectivityTestResult_Connected:1, k_EConnectivityTestResult_NoLAN:5},
            jrQ:{k_ENetFakeLocalSystemState_Normal:0}};
    api(client, () => runtime, () => new Promise(resolve => reads.push(resolve)), host);
    const states = [];
    const sub = client.System.Network.RegisterForConnectivityTestChanges(value => states.push(value.eConnectivityTestResult));
    const changed = () => host.dispatchEvent(new MessageEvent('message', {data:{event:'xbox-network-changed'}}));
    changed();
    reads.shift()({level:'internet', kind:'ethernet', device:1});
    await tick();
    assert.deepEqual(states, []);
    reads.shift()({level:'offline'});
    await tick();
    assert.deepEqual(states, [5]);
    sub.unregister();
    changed();
    assert.equal(reads.length, 0);
    window.dispatchEvent(new Event('pagehide'));
});
