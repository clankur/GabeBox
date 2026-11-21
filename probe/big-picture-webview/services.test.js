const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const source = fs.readFileSync(require.resolve('../big-picture-bridge/xbox-services.js'), 'utf8');

const first = '76561191111111111', second = '76561192222222222';
function fixture(data, session) {
    const client = {};
    const window = {localStorage: {
        getItem: key => data.get(key) ?? null, setItem: (key, val) => data.set(key, val), removeItem: key => data.delete(key)
    }};
    vm.runInNewContext(source + '\ninstallXboxServices(client, send);', {client, window, send(method) {
        assert.equal(method, 'session'); return Promise.resolve({steamId:session.id});
    }});
    return client.Storage;
}

test('signed Steam preference writes and deletes are ordered with subsequent reads', async () => {
    const data = new Map(), prefs = fixture(data, {id: first});
    prefs.SetString('CurrentKeyboardTheme', '"DefaultTheme"');
    assert.equal(await prefs.GetString('CurrentKeyboardTheme'), '"DefaultTheme"');
    prefs.DeleteKey('CurrentKeyboardTheme');
    assert.equal(await prefs.GetString('CurrentKeyboardTheme'), null);
    const key = 'GetEquippedProfileItemsForUser' + first;
    await prefs.SetString(key, '{}');
    assert.equal(await prefs.GetString(key), '{}');
    assert.throws(() => prefs.GetString('session_token'), /Unsupported/);
    assert.throws(() => prefs.SetString('GetStartupMovies', 'a'.repeat(65537)), /Invalid/);
});

test('an account change or sign-out cannot read a previous user cache', async () => {
    const data = new Map(), owner = {id: first}, firstPrefs = fixture(data, owner);
    await firstPrefs.SetString('seen_clip_saved_hint', '1');
    owner.id = second;
    await assert.rejects(firstPrefs.GetString('seen_clip_saved_hint'), /unavailable/);
    const secondPrefs = fixture(data, owner);
    assert.equal(await secondPrefs.GetString('seen_clip_saved_hint'), null);
    await assert.rejects(secondPrefs.GetString('GetEquippedProfileItemsForUser' + first), /different account/);
    owner.id = null;
    await assert.rejects(secondPrefs.SetString('seen_clip_saved_hint', '1'), /unavailable/);
    assert.equal(data.size, 1);
});

test('Steam cannot migrate unscoped preferences left by an earlier user', async () => {
    const data = new Map([['CurrentKeyboardTheme', '"Old theme"']]);
    const prefs = fixture(data, {id: second});
    await assert.rejects(prefs.GetString('CurrentKeyboardTheme'), /no account owner/);
    await prefs.SetString('CurrentKeyboardTheme', '"My theme"');
    assert.equal(await prefs.GetString('CurrentKeyboardTheme'), '"My theme"');
});
