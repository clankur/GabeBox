const assert = require('node:assert/strict');
const fs = require('node:fs');
const test = require('node:test');
const vm = require('node:vm');

const source = fs.readFileSync(require.resolve('../../ui/big-picture-bridge/xbox-startup-movie.js'), 'utf8');
const override = '/uioverrides/movies/bigpicture_startup.webm';

function fixture(missingOverride, deviceFamily = 'Windows.Xbox') {
    const calls = [], receipts = [];
    const window = {fetch(...args) {
        calls.push({owner: this, args});
        return Promise.resolve(new Response(null, {status: 200}));
    }};
    const host = {postMessage(message) { receipts.push(message); }};
    vm.runInNewContext(source + '\ninstallXboxStartupMovieProbe(host, missingOverride);',
        {window, host, missingOverride, xboxNativeWindow: {deviceFamily}, Response});
    return {window, calls, receipts};
}

test('an absent packaged override completes only the pinned Xbox HEAD probe', async () => {
    const {window, calls, receipts} = fixture(true);
    assert.equal((await window.fetch(override, {method: 'HEAD'})).status, 404);
    assert.equal((await window.fetch(override, {method: 'HEAD'})).status, 404);
    assert.deepEqual(calls, []);
    assert.equal(receipts.length, 1);
    assert.equal(receipts[0].method, 'startup-movie-override-js');
    assert.equal(receipts[0].state, 'absent-404');

    const other = {method: 'HEAD', cache: 'reload'};
    assert.equal((await window.fetch(override, other)).status, 200);
    assert.equal((await window.fetch(override)).status, 200);
    const stock = '/movies/bigpicture_startup.webm';
    assert.equal((await window.fetch(stock, {method: 'HEAD'})).status, 200);
    assert.equal(calls[0].owner, window);
    assert.deepEqual(calls.map(call => call.args), [[override, other], [override], [stock, {method: 'HEAD'}]]);
});

test('the existing override and other platform keep normal resource handling', async () => {
    for (const app of [fixture(false), fixture(true, 'Windows.Desktop')]) {
        assert.equal((await app.window.fetch(override, {method: 'HEAD'})).status, 200);
        assert.equal(app.calls.length, 1);
        assert.deepEqual(app.receipts, []);
    }
});
