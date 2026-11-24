const assert = require('node:assert/strict');
const fs = require('node:fs');
const test = require('node:test');
const vm = require('node:vm');

const source = fs.readFileSync(require.resolve('../../ui/big-picture-bridge/xbox-cloud-cache.js'), 'utf8');
const account = '76561198000000001';
const index = 'cloud-storage-namespaces';
const address = '/cloudstorage/' + index + '.json';

function fixture(saved = new Map()) {
    const requests = [], receipts = [], client = {};
    let owner = account;
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
    vm.runInNewContext(source + '\ninstallXboxCloudCache(client, send)(axios);', {
        client, send: async () => ({steamId: owner}), axios, indexedDB, setTimeout, clearTimeout,
        window: {chrome: {webview: {addEventListener() {}, postMessage({label, status}) { receipts.push([label, status]); }}}}
    });
    assert.equal(interceptors.length, 1);
    return {requests, receipts, changeOwner(id) { owner = id; }, intercept: interceptors[0]};
}

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
