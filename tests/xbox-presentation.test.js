const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const source = name => fs.readFileSync(path.join(__dirname, '../ui', name), 'utf8');

test('Xbox restricted presentation modes and unavailable host properties', () => {
    const check = vm.runInNewContext(source('signin/xbox-system-mode.js'), {window: {}});
    const host = (retail, kiosk) => ({System: {Profile: {RetailInfo: {isDemoModeEnabled: retail}},
        UserProfile: {AssignedAccessSettings: {getDefault: () => ({isEnabled: kiosk})}}}});
    assert.equal(check(host(false, false)), false);
    assert.equal(check(host(true, false)), true);
    assert.equal(check(host(false, true)), true);
    assert.equal(check(host(undefined, false)), false);
    assert.equal(check({}), true);
    assert.equal(check({get System() { throw Error(); }}), true);
});

test('Valve route descriptors retain format parameters, parent filters and safe HTTPS roots', async () => {
    const routes = {Hub: ['%community%/app/%p1%/%language%', 'community'], Front: ['%store%', 'store'],
        Missing: ['%applauncher%', ''], Query: ['%help%/x?q=How%20to%20play', ''],
        Unsafe: ['https://steamcommunity.com.invalid/', ''], Unmapped: ['%help%/', 'newfilter']};
    const client = {}, window = new EventTarget(), navigator = {language: 'pt-BR'};
    vm.runInNewContext(source('big-picture-bridge/xbox-url-roots.js') + '\n' +
        source('big-picture-bridge/xbox-url.js') + '\ninstallXboxUrls(client)', {client, xboxUrlTemplates: routes, navigator, window, URL});
    const urls = await client.URL.GetSteamURLList([...Object.keys(routes), 'StoreAppImages', 'toString']);
    assert.deepEqual(Object.keys(urls).sort(), ['Front', 'Hub', 'Query', 'StoreAppImages']);
    assert.equal(urls.Hub.url, 'https://steamcommunity.com/app/%p1%/brazilian');
    assert.equal(urls.Hub.feature, 2);
    assert.equal(urls.Front.url, 'https://store.steampowered.com/');
    assert.equal(urls.Query.url, 'https://help.steampowered.com/x?q=How%20to%20play');
    assert.equal(urls.StoreAppImages.url.endsWith('/steam/apps'), true);
    let calls = 0;
    const listener = client.URL.RegisterForSteamURLChanges(() => calls++);
    window.dispatchEvent(new Event('languagechange'));
    listener.unregister();
    window.dispatchEvent(new Event('languagechange'));
    assert.equal(calls, 1);
});

test('dispatches only supported screen routes to registered Valve listeners', () => {
    const client = {};
    vm.runInNewContext(source('big-picture-bridge/xbox-url.js') + '\ninstallXboxUrls(client)', {client});
    const seen = [];
    const token = client.URL.RegisterForRunSteamURL('open/settings', (...args) => seen.push(args));
    client.URL.ExecuteSteamURL('steam://open/settings/voice');
    assert.deepEqual(seen, [[0, 'steam://open/settings/voice']]);
    client.URL.RegisterForRunSteamURL('openurl_external', () => assert.fail('unexpected command'));
    for (const route of ['steam://openurl_external/https://example.com', 'steam://open/settings/../exit',
        'steam://open/settings?launch=1', 'https://example.com/']) {
        assert.throws(() => client.URL.ExecuteSteamURL(route), /Unsupported/);
    }
    token.unregister();
    assert.throws(() => client.URL.ExecuteSteamURL('steam://open/settings'), /unavailable/);
});

test('opens Valve running controls for the foregrounded game, including late registration', async () => {
    const client = {}, state = {app: 975370}, seen = [];
    const show = vm.runInNewContext(source('big-picture-bridge/xbox-url.js') +
        '\ninstallXboxUrls(client, () => state.app)', {client, state, queueMicrotask});
    show(state.app);
    const subscribe = () => client.URL.RegisterForRunSteamURL('open/apprunningcontrols', (...args) => seen.push(args));
    const sub = subscribe();
    await Promise.resolve();
    assert.deepEqual(seen, [[0, 'steam://open/apprunningcontrols/975370']]);
    assert.throws(() => client.URL.ExecuteSteamURL('steam://open/apprunningcontrols/413150'), /Unsupported/);
    sub.unregister();
    show(state.app);
    subscribe();
    state.app = 0;
    show(0);
    await Promise.resolve();
    assert.equal(seen.length, 1);
    assert.throws(() => client.URL.ExecuteSteamURL('steam://open/apprunningcontrols/975370'), /Unsupported/);
});

test('routes local Valve screen messages and honors listener revocation', async () => {
    const client = {};
    vm.runInNewContext(source('big-picture-bridge/xbox-messaging.js') + '\ninstallXboxMessaging(client)', {client, queueMicrotask, performance});
    const events = [], {Messaging: bus} = client;
    const listener = bus.RegisterForMessages('FriendsUI', (...args) => events.push(args));
    bus.PostMessage('FriendsUI', 'ChatTabActivated', '{"id":"test"}');
    await Promise.resolve();
    assert.deepEqual(events, [['FriendsUI', 'ChatTabActivated', '{"id":"test"}']]);
    bus.PostMessage('FriendsUI', 'NavigateToInvites', '{}');
    listener.unregister();
    await Promise.resolve();
    assert.equal(events.length, 1);
    assert.throws(() => bus.PostMessage('FriendsUI', 'AcceptedGameInvite', '{}'), /unavailable/);
    assert.throws(() => bus.PostMessage('Parental', 'PlaytimeExhausted', '{}'), /unavailable/);
});

test('Valve game actions track a real launch promise and clear on rejection', async () => {
    const client = {Apps: {}}, log = [];
    const run = vm.runInNewContext(source('big-picture-bridge/xbox-game-actions.js') + '\ninstallXboxGameActions(client)', {client, queueMicrotask});
    client.Apps.RegisterForGameActionStart((...args) => log.push(args));
    client.Apps.RegisterForGameActionEnd(id => log.push(id));
    let reject;
    const play = run(646570, 403, () => new Promise((_, fail) => { reject = fail; }));
    const actions = await client.Apps.GetActiveGameActions();
    assert.equal(actions[0].gameid, '646570');
    assert.deepEqual(log[0], [1, '646570', 'LaunchApp', 403]);
    await assert.rejects(run(413150, 403, () => assert.fail()), /in progress/);
    reject(new Error('not-owned'));
    await assert.rejects(play, /not-owned/);
    assert.equal((await client.Apps.GetActiveGameActions()).length, 0);
    assert.equal(log[1], 1);
});
