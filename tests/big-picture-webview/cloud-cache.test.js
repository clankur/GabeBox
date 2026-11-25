const assert = require('node:assert/strict');
const fs = require('node:fs');
const test = require('node:test');
const vm = require('node:vm');

const source = fs.readFileSync(require.resolve('../../ui/big-picture-bridge/xbox-cloud-cache.js'), 'utf8');
const account = '76561198000000001';
const index = 'cloud-storage-namespaces';
const address = '/cloudstorage/' + index + '.json';

function fixture(saved = new Map(), clock = {setTimeout, clearTimeout}) {
    const requests = [], receipts = [], client = {};
    const originalWaits = [];
    let owner = account, connected = false;
    const state = {
        m_CMInterface: {BIsConnected: () => connected},
        WaitForSync(namespace) { return new Promise(resolve => originalWaits.push({namespace, resolve})); }
    };
    const db = {transaction(table, mode) {
        assert.equal(table, 'documents');
        assert.equal(mode, 'readonly');
        const txn = {objectStore() {
            return {get(pair) {
                const request = {result: saved.get(pair.join(':'))};
                requests.push(Array.from(pair));
                queueMicrotask(() => txn.oncomplete());
                return request;
            }};
        }};
        return txn;
    }};
    const indexedDB = {open() {
        const request = {result: db};
        queueMicrotask(() => request.onsuccess());
        return request;
    }};
    class AxiosError extends Error {
        constructor(message, code, config, request, response) {
            super(message);
            Object.assign(this, {code, config, request, response});
        }
    }
    const interceptors = [];
    const axios = {AxiosError, interceptors: {request: {use(fn) { interceptors.push(fn); }}}};
    vm.runInNewContext(source + '\nconst cache = installXboxCloudCache(client, send);' +
        '\ncache.installLocalReads(axios); cache.observeStartup(); window.cloudStorageInternalState = state;', {
        client, send: async () => ({steamId: owner}), axios, indexedDB, state, ...clock,
        window: {chrome: {webview: {addEventListener() {}, postMessage({label, status, method, namespace}) {
            receipts.push(namespace === undefined ? [label, status] : [method, namespace]);
        }}}}
    });
    assert.equal(interceptors.length, 1);
    return {requests, receipts, state, originalWaits, changeOwner(id) { owner = id; },
        setConnected(value) { connected = value; }, intercept: interceptors[0]};
}

function timedFixture() {
    const deadlines = new Set();
    const app = fixture(new Map(), {
        setTimeout(callback, delay) { assert.equal(delay, 750); deadlines.add(callback); return callback; },
        clearTimeout(callback) { deadlines.delete(callback); }
    });
    return {...app, deadlines, async expire() {
        for (const fire of [...deadlines]) fire();
        await new Promise(resolve => setImmediate(resolve));
    }};
}

test('a verified offline UI can use local settings while original namespace sync remains pending', async () => {
    const app = timedFixture();
    const wait = app.state.WaitForSync(3);
    assert.deepEqual(app.originalWaits.map(item => item.namespace), [3]);
    await app.expire();
    assert.equal(await wait, undefined);
    assert.deepEqual(app.receipts, [['startup-cloud-local-fallback', 3]]);
    assert.equal(app.deadlines.size, 0);
    app.originalWaits[0].resolve('Steam completed later');
});

test('original UI wait wins on fast sync, connection recovery, account change and other namespaces', async () => {
    const app = timedFixture();
    const early = app.state.WaitForSync(1);
    app.originalWaits[0].resolve('fast Steam sync');
    assert.equal(await early, 'fast Steam sync');
    assert.equal(app.deadlines.size, 0);

    const reconnecting = app.state.WaitForSync(3);
    app.setConnected(true);
    const connected = app.state.WaitForSync(4);
    await app.expire();
    app.originalWaits[1].resolve('connected before fallback');
    app.originalWaits[2].resolve('stayed connected');
    assert.equal(await reconnecting, 'connected before fallback');
    assert.equal(await connected, 'stayed connected');

    app.setConnected(false);
    const fallback = app.state.WaitForSync(1);
    await app.expire();
    await fallback;
    app.changeOwner('76561198000000002');
    const revoked = app.state.WaitForSync(4);
    await app.expire();
    app.originalWaits[4].resolve('account changed');
    assert.equal(await revoked, 'account changed');

    const other = app.state.WaitForSync(2);
    assert.equal(app.deadlines.size, 0);
    app.originalWaits[5].resolve('ordinary namespace');
    assert.equal(await other, 'ordinary namespace');
    assert.deepEqual(app.receipts, [['startup-cloud-local-fallback', 1]]);
});

test('Axios reads saved Steam UI JSON and revalidates the renderer account', async () => {
    const app = fixture(new Map([[account + ':' + index, '[[3,"6"]]']]));
    const config = app.intercept({url: address, method: 'get', withCredentials: false});
    const response = await config.adapter(config);
    assert.equal(response.status, 200);
    assert.equal(JSON.stringify(response.data), '[[3,"6"]]');
    assert.deepEqual(app.requests, [[account, index]]);
    assert.deepEqual(app.receipts, [['index', 200]]);

    app.changeOwner('76561198000000002');
    await assert.rejects(config.adapter(config), error => error.response.status === 403);
    assert.deepEqual(app.requests, [[account, index]]);
});

test('unknown records, authenticated URLs, other methods and other cloud calls retain their adapter', async () => {
    const app = fixture();
    const unlike = [
        {url: '/cloudstorage/cloud-storage-namespace-2.json'},
        {url: 'https://steamloopback.host' + address},
        {url: address + '?version=0'},
        {url: '/cloudstorage/game-saves.json'},
        {url: address, method: 'post'},
        {url: address, withCredentials: true},
        {url: address, auth: {}},
        {url: address, params: {version: 1}},
        {url: address, baseURL: 'https://example.invalid'}
    ];
    const adapter = () => {};
    for (const config of unlike) {
        assert.equal(app.intercept({method: 'get', withCredentials: false, adapter, ...config}).adapter, adapter);
    }
    const missing = app.intercept({url: address, method: 'get', withCredentials: false});
    await assert.rejects(missing.adapter(missing), error => error.response.status === 404);
    assert.deepEqual(app.receipts, [['index', 404]]);
});
