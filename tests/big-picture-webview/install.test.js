const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const test = require('node:test');
const vm = require('node:vm');

const source = readFileSync(require.resolve('../../ui/big-picture-bridge/xbox-installs.js'), 'utf8');
const states = {k_EInstallMgrStateNone: 0, k_EInstallMgrStateFailed: 15};
const statuses = {$g: 3, cM: 7, Cy: 22, Bp: 23};

function fixture() {
    let complete;
    let changes = 0;
    const client = {}, requests = [], wizard = [];
    const install = vm.runInNewContext(source + '\ninstallXboxInstalls(client, send, refresh, getRuntime);', {
        client,
        send(method, appId) {
            requests.push({method, appId});
            return new Promise(resolve => { complete = resolve; });
        },
        refresh() { changes++; },
        getRuntime: () => module => {
            assert.equal(module, 94601);
            return {H24: states, VIA: 0};
        }
    });
    client.Installs.RegisterForShowInstallWizard(state => wizard.push([state.eInstallState, state.currentAppID]));
    return {install, api: client.Installs, requests, wizard, changes: () => changes, complete: result => complete(result)};
}

test('the local stock Install action queues one AppID and projects only its reported stages', async () => {
    const {install, api, requests, wizard, changes, complete} = fixture();
    const id = 2868840;
    await api.OpenInstallWizard([id], '1');
    await api.OpenInstallWizard([String(id)], '0');
    await api.OpenInstallWizard([id, 976730], '0');
    assert.deepEqual(requests, []);
    const pending = api.OpenInstallWizard([id], '0');
    await api.OpenInstallWizard([id], '0');
    assert.deepEqual(requests, [{method:'install', appId:id}]);
    assert.equal(install.status(id, statuses), statuses.$g);
    install.update(976730, 'downloading');
    install.update(id, 'unknown');
    assert.equal(changes(), 1);
    install.update(id, 'queued');
    install.update(id, 'resuming'); // Same projected status, no repeated catalog reads.
    assert.equal(install.status(id, statuses), statuses.Bp);
    assert.equal(changes(), 2);
    install.update(id, 'downloading');
    assert.equal(install.status(id, statuses), statuses.cM);
    complete('installed');
    await pending;
    assert.equal(install.status(id, statuses), undefined);
    assert.equal(changes(), 4);
    assert.deepEqual(wizard, []);
});

test('a terminal download failure reaches the stock installer with a working dismissal', async () => {
    const {api, wizard, complete, requests} = fixture();
    const pending = api.OpenInstallWizard([976730], '0');
    complete('stalled');
    await pending;
    assert.deepEqual(wizard, [[states.k_EInstallMgrStateFailed, 976730]]);
    api.CancelInstall();
    assert.deepEqual(wizard, [[states.k_EInstallMgrStateFailed, 976730], [states.k_EInstallMgrStateNone, 976730]]);
    const retry = api.OpenInstallWizard([976730], '0');
    complete('signed-out');
    await retry;
    assert.equal(requests.length, 2);
    assert.equal(wizard.length, 2);
});
