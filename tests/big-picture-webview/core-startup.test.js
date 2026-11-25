const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require.resolve('../../ui/big-picture-bridge/xbox-core-startup.js'), 'utf8');
const drain = () => new Promise(resolve => setImmediate(resolve));
const first = '76561191111111111', second = '76561192222222222';
function fixture(answer) {
    const client = {}, calls = [];
    vm.runInNewContext(source + '\ninstallXboxCoreStartup(client,send);', {client, send(method) {
        calls.push(method); return answer(method);
    }});
    return {register: client.UI.RegisterForStartupFinished, calls};
}

test('the real empty owned library can confirm startup after account revalidation', async () => {
    let complete;
    const waiting = new Promise(resolve => { complete = resolve; });
    const f = fixture(method => method === 'session' ? {steamId:first} : waiting);
    const events = [];
    let a = f.register(() => { events.push('a'); a.unregister(); });
    const b = f.register(() => events.push('b'));
    b.unregister();
    await drain();
    assert.deepEqual(f.calls, ['session','library']);
    assert.deepEqual(events, []);
    complete([]);
    await drain();
    assert.deepEqual(f.calls, ['session','library','session']);
    assert.deepEqual(events, ['a']);
});

test('sign-out, invalid account, failed library and owner changes leave Valve waiting', async () => {
    for (const replies of [[{steamId:null}], [{steamId:'99'}], [{steamId:first}, Promise.resolve(null)],
        [{steamId:first}, new Error('offline')], [{steamId:first}, [], {steamId:second}]]) {
        const f = fixture(() => { const next = replies.shift(); if (next instanceof Error) throw next; return next; });
        let notified = false;
        f.register(() => { notified = true; });
        await drain();
        assert.equal(notified, false);
    }
});

test('a late subscriber revalidates the active session', async () => {
    let signedIn = true;
    const f = fixture(method => method === 'session' ? {steamId:signedIn ? first : null} : []);
    let calls = 0;
    f.register(() => { calls++; });
    await drain();
    signedIn = false;
    f.register(() => { calls++; });
    await drain();
    assert.equal(calls, 1);
});
