// Run with node tests/steam-big-picture-shell.cjs <staged ValveClient/steamui>.
// Set NODE_PATH for playwright and CHROMIUM_PATH for an existing Chromium binary.
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const {chromium} = require('playwright');

const origin = 'https://steamloopback.host';
const assets = path.resolve(process.argv[2] || 'ValveClient/steamui');
const bridge = path.resolve(__dirname, '../probe/big-picture-bridge/bridge.js');

async function localPage(browser) {
    const context = await browser.newContext({offline: true, serviceWorkers: 'block'});
    await context.route('**/*', async route => {
        const url = new URL(route.request().url());
        const file = path.resolve(assets, '.' + decodeURIComponent(url.pathname));
        if (url.origin !== origin || !file.startsWith(assets + path.sep)) return route.abort();
        try { await route.fulfill({path: file}); }
        catch { await route.fulfill({status: 404, body: ''}); }
    });
    const page = await context.newPage();
    page.setDefaultTimeout(10000);
    return {page, context};
}

async function verifyAdapter(browser) {
    const {page, context} = await localPage(browser);
    try {
        const fixture = () => {
            // Authenticated library and the return from the hosted presenter are
            // fixtures only. All React, webpack and protobuf code is Valve's.
            const events = new EventTarget();
            window.testGames = [{id: 42, name: 'Fixture A', installed: true, canPlay: true},
                {id: 73, name: 'Fixture B', installed: false, canPlay: false}];
            window.testRequests = [];
            (window.chrome ||= {}).webview = {
                addEventListener: events.addEventListener.bind(events),
                postMessage(request) {
                    window.testRequests.push(request);
                    const reply = request.method === 'library' ? {result: window.testGames} :
                        request.appId === 42 ? {result: 'presented'} : {error: 'not-installed'};
                    queueMicrotask(() => events.dispatchEvent(new MessageEvent('message', {data: {id: request.id, ...reply}})));
                }
            };
        };
        await page.addInitScript({content: `(${fixture.toString()})();\n${await fs.readFile(bridge, 'utf8')}`});
        const blocker = page.waitForEvent('pageerror', {predicate: error => error.message.includes('IsSteamInTournamentMode')});
        await page.goto(origin + '/index.html');
        await blocker;
        const result = await page.evaluate(async () => {
            let require;
            window.webpackChunksteamui.push([['xbox-protobuf-check'], {}, r => { require = r; }]);
            const codec = require(76142).bs;
            let subscription;
            const next = () => new Promise(resolve => {
                subscription = SteamClient.Apps.RegisterForAppOverviewChanges(bytes => resolve(codec.deserializeBinary(bytes).toObject()));
            });
            const snapshot = await Promise.race([next(), new Promise((_, reject) => setTimeout(() => reject(new Error('No app overview delivered')), 5000))]);
            subscription.unregister();
            await SteamClient.Apps.RunGame('42', '', -1, 0);
            let refused = '';
            try { await SteamClient.Apps.RunGame('73', '', -1, 0); } catch (error) { refused = error.message; }
            return {snapshot, refused, calls: window.testRequests.map(call => [call.method, call.appId || 0]),
                title: document.title, body: document.getElementById('root').textContent};
        });
        assert.equal(result.title, 'SharedJSContext');
        assert.equal(result.body, '');
        assert.equal(result.snapshot.full_update, true);
        assert.equal(result.snapshot.update_complete, true);
        assert.deepEqual(result.snapshot.app_overview.map(game => [game.appid, game.display_name, game.subscribed_to,
            game.visible_in_game_list, game.per_client_data[0].installed, game.per_client_data[0].display_status]),
        [[42, 'Fixture A', true, true, true, 16], [73, 'Fixture B', true, true, false, 12]]);
        assert.equal(result.refused, 'not-installed');
        assert.deepEqual(result.calls, [['library', 0], ['play', 42], ['play', 73]]);
        assert.equal(context.pages().length, 1);
        console.log('Intact entry stopped at System.IsSteamInTournamentMode. Valve decoded both app overviews. The broker accepted one Play and refused the other.');
    } finally { await context.close(); }
}

async function verifyTransportBoundary(browser) {
    const {page, context} = await localPage(browser);
    try {
        // Exercise the pre-login sequence under a conventional, non-tournament
        // fixture. No users, gamepads, messages, service ports or tokens are fabricated.
        await page.addInitScript(() => {
            const events = new EventTarget();
            function registration(event) {
                return callback => {
                    events.addEventListener(event, callback);
                    return {unregister: () => events.removeEventListener(event, callback)};
                };
            }
            window.SteamClient = {
                System: {IsSteamInTournamentMode: async () => false},
                Input: {RegisterForUnboundControllerListChanges: registration('unbound'),
                    RegisterForDualSenseUpdateNotification: registration('dualsense')},
                User: {RegisterForLoginStateChange: registration('login'), RegisterForCurrentUserChanges: registration('user')}
            };
        });
        const missing = ['GetTransportInfo', 'AllocateSharedConnection', 'GetSteamURLList'];
        const observed = Promise.all(missing.map(method => page.waitForEvent('console', {
            predicate: message => message.type() === 'error' && message.text().includes(method)
        })));
        await page.goto(origin + '/index.html?IN_CLIENT=true');
        await observed;
        assert.equal(context.pages().length, 1);
        console.log('Before login, Valve requested WebUI transport credentials, a shared connection handle, and its URL catalog. The SP BPM window did not open.');
    } finally { await context.close(); }
}

(async () => {
    await fs.access(path.join(assets, 'sp.js'));
    const browser = await chromium.launch({headless: true, chromiumSandbox: true,
        executablePath: process.env.CHROMIUM_PATH, args: ['--host-resolver-rules=MAP * ~NOTFOUND', '--disable-background-networking']});
    try { await verifyAdapter(browser); await verifyTransportBoundary(browser); }
    finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
