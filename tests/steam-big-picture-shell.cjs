// node tests/steam-big-picture-shell.cjs <staged ValveClient/steamui> [public/url_list.txt] [--require-gamepad] [--single-webview] [--accounts] [--network] [--storage] [--store] [--home] [--install] [--downloads] [--startup-history] [--empty-account [--hold-tour]]
// Set NODE_PATH for playwright and CHROMIUM_PATH for an existing Chromium binary.
const assert = require('node:assert/strict');
const {execFileSync} = require('node:child_process');
const fs = require('node:fs/promises');
const path = require('node:path');
const {chromium} = require('playwright');

const origin = 'https://steamloopback.host';
const entry = origin + '/index.html?IN_CLIENT=true';
const args = process.argv.slice(2).filter(arg => !['--require-gamepad', '--native-startup', '--single-webview', '--accounts', '--empty-account', '--hold-tour', '--network', '--storage', '--store', '--home', '--install', '--downloads', '--startup-history'].includes(arg));
const accounts = process.argv.includes('--accounts');
const emptyOnly = process.argv.includes('--empty-account');
const networkOnly = process.argv.includes('--network');
const storageOnly = process.argv.includes('--storage');
const storeOnly = process.argv.includes('--store');
const homeOnly = process.argv.includes('--home');
const installOnly = process.argv.includes('--install');
const downloadsOnly = process.argv.includes('--downloads');
const installAppId = 1817070;
const startupHistory = process.argv.includes('--startup-history');
const framedGamepad = startupHistory || installOnly || downloadsOnly || homeOnly || storeOnly || networkOnly || storageOnly || accounts || emptyOnly || process.argv.includes('--single-webview');
const requireGamepad = process.argv.includes('--require-gamepad');
const assets = path.resolve(args[0] || 'ValveClient/steamui');
const catalog = path.resolve(args[1] || path.join(assets, '../public/url_list.txt'));
const bundle = path.resolve(__dirname, '../probe/big-picture-bridge/bundle.py');
const contentTypes = {'.css': 'text/css', '.js': 'application/javascript', '.html': 'text/html',
    '.json': 'application/json', '.webm': 'video/webm', '.svg': 'image/svg+xml', '.png': 'image/png',
    '.jpg': 'image/jpeg', '.woff2': 'font/woff2', '.woff': 'font/woff', '.ttf': 'font/ttf'};
const fixtureAccount = '76561198000000000';
const secondAccount = '76561198000000001';
const emptyAccount = '76561198000000002';
const catalogs = {
    [emptyAccount]: [],
    [fixtureAccount]: [{id: 42, name: 'Fixture A', installed: true, canPlay: true},
        {id: installOnly || downloadsOnly ? installAppId : 73, name: 'Fixture B', installed: false, canPlay: false}],
    [secondAccount]: [{id: 81, name: 'Second Atlas', installed: false, canPlay: false},
        {id: 93, name: 'Second Lantern', installed: true, canPlay: true}]
};

// This fixture stands in for the outer, authenticated Xbox host. The production
// bundle installs SteamClient inside Valve's unmodified document.
function setupHost(steamId, windowState, catalogs) {
    const state = {...windowState};
    const gb = 1024 ** 3;
    window.fixtureStorage = {defaultId: 'D', folders: [
        {id: 'D', name: 'Xbox Development Storage', path: 'D:\\DevelopmentFiles\\SteamLibrary', mounted: true,
            totalBytes: 100 * gb, freeBytes: 90 * gb, apps: []},
        {id: 'E', name: 'External Storage', path: 'E:\\SteamLibrary', mounted: true,
            totalBytes: 500 * gb, freeBytes: 400 * gb, apps: []},
        {id: 'local', name: 'App Storage', path: 'LocalState\\SteamLibrary', mounted: true,
            totalBytes: 30 * gb, freeBytes: 24 * gb, apps: [{appId: 42, sizeBytes: 2 * gb}]}
    ]};
    const events = new EventTarget();
    window.fixtureCalls = [];
    window.fixtureNativePad = {slot: 0, bits: 0, lt: 0, rt: 0, lx: 0, ly: 0, rx: 0, ry: 0};
    window.fixtureSteamId = sessionStorage.getItem('fixture-next-account') || steamId;
    window.fixtureReads = new Map();
    window.hostEvent = data => {
        if (data.event === 'xbox-game') window.fixtureRunningApp = data.appId;
        events.dispatchEvent(new MessageEvent('message', {data}));
    };
    (window.chrome ||= {}).webview = {
        addEventListener: events.addEventListener.bind(events),
        removeEventListener: events.removeEventListener.bind(events),
        postMessage(request) {
            if (state.framedGamepad && typeof request === 'string' &&
                ['xbox-window-frame-open', 'xbox-window-frame-closed', 'xbox-window-show', 'xbox-window-hide'].includes(request)) {
                if (request === 'xbox-window-frame-open') state.popupOpen = true;
                if (request === 'xbox-window-frame-closed') state.popupOpen = false;
                state.popupVisible = state.popupOpen && request !== 'xbox-window-hide';
                const snapshot = {...state};
                queueMicrotask(() => hostEvent({event: 'xbox-window', state: snapshot}));
                return;
            }
            if (request === 'xbox-read-gamepads') {
                queueMicrotask(() => window.hostEvent({event: 'xbox-gamepads', pads: [{...window.fixtureNativePad}]}));
                return;
            }
            if (request.method === 'cloud-cache-result') {
                const callback = window.fixtureReads.get(request.token);
                if (callback) { window.fixtureReads.delete(request.token); callback(request); }
                return;
            }
            window.fixtureCalls.push(request);
            if (typeof request === 'string') return;
            let reply;
            const games = catalogs[window.fixtureSteamId];
            switch (request.method) {
                case 'store-browser': return; // The Store test drives isolated browser receipts explicitly.
                case 'network-state': reply = {result: window.fixtureNetwork || {level: 'offline'}}; break;
                case 'storage-list':
                    reply = window.fixtureStorage && games ? {result: {...window.fixtureStorage,
                        folders: window.fixtureStorage.folders.map(folder => ({...folder,
                            apps: folder.apps.filter(app => games.some(game => game.id === app.appId))}))}} :
                        {error: 'storage-unavailable'};
                    break;
                case 'storage-select':
                    window.fixtureStorage.defaultId = request.argument;
                    reply = {result: {ok: true}};
                    break;
                case 'storage-move': {
                    const {appIds, destination} = request.argument;
                    const target = window.fixtureStorage.folders.find(folder => folder.id === destination && folder.mounted);
                    const from = window.fixtureStorage.folders.find(folder => folder !== target &&
                        folder.apps.some(app => app.appId === appIds[0]));
                    if (target && from) target.apps.push(...from.apps.splice(from.apps.findIndex(app => app.appId === appIds[0]), 1));
                    reply = {result: {ok: Boolean(target && from)}};
                    break;
                }
                case 'library': reply = games ? {result: games} : {error: 'signed-out'}; break;
                case 'session': reply = {result: {steamId: window.fixtureSteamId}}; break;
                case 'sign-out':
                    window.fixtureSteamId = null;
                    reply = {result: 'signed-out'};
                    break;
                case 'restart-client': reply = {result: window.fixtureSteamId ? 'restart' : 'signed-out'}; break;
                case 'system-mode': reply = {result: false}; break;
                case 'play': reply = games?.some(g => g.id === request.appId && g.canPlay) ? {result: 'presented'} : {error: games ? 'not-installed' : 'signed-out'}; break;
                case 'install': {
                    const game = games?.find(entry => entry.id === request.appId);
                    if (!game) { reply = {result: games ? 'not-owned' : 'signed-out'}; break; }
                    window.fixtureFinishInstall = result => {
                        if (result === 'installed') game.installed = game.canPlay = true;
                        hostEvent({id: request.id, result});
                    };
                    return;
                }
                case 'quit-game':
                    reply = request.appId === window.fixtureRunningApp && games?.some(g => g.id === request.appId)
                        ? {result: 'quit-requested'} : {error: 'unavailable'};
                    break;
                case 'resume-game': reply = window.fixtureSteamId === steamId && games?.some(g => g.id === request.appId && g.canPlay) ? {result: 'presented'} : {error: 'unavailable'}; break;
                default: reply = {error: 'unavailable'};
            }
            queueMicrotask(() => window.hostEvent({id: request.id, ...reply}));
        }
    };
}

// Exercise Valve's own menu, gateway and history router. The test browser stays
// offline. Its separate page provides a live, empty initial document, and
// intercepts the public Store request before any response bytes are returned.
async function checkStore(browser, page, popup) {
    await pressNative(page, 3);
    const menu = popup.frameLocator('iframe[name="MainMenu_uid2"]');
    await page.evaluate(() => {
        const create = SteamClient.BrowserView.Create;
        window.fixtureStoreCreate = [];
        SteamClient.BrowserView.Create = options => {
            try { return create(options); }
            catch (error) {
                fixtureStoreCreate.push({keys:Object.keys(options), error:error.message});
                throw error;
            }
        };
    });
    const store = menu.getByText('Store', {exact: true});
    await store.waitFor({state:'visible'});
    const focus = () => store.evaluate(element => {
        const doc = element.ownerDocument;
        const selected = [...doc.querySelectorAll('.gpfocus')].findLast(row =>
            row instanceof doc.defaultView.HTMLElement && row.getBoundingClientRect().width > 0);
        return [doc.activeElement, selected].some(row => row && row !== doc.body &&
            (element.contains(row) || row.contains(element)) && row.innerText.trim() === 'Store');
    });
    for (let step = 0; step < 10 && !(await focus()); step++) await pressNative(page,128);
    assert(await focus(), 'Stock Store row did not gain Valve controller focus');
    await pressNative(page,4);
    const request = action => page.waitForFunction(action =>
        window.fixtureCalls.find(call => call?.method === 'store-browser' && call.action === action), action);
    const create = await request('create').catch(async error => { throw new Error(error.message + ' ' +
        JSON.stringify(await page.evaluate(() => fixtureStoreCreate)) + ' ' +
        (await popup.locator('#popup_target').innerText()).slice(0, 500)); });
    const {id} = await create.jsonValue();
    assert(!await page.evaluate(() => fixtureCalls.some(c => c?.method === 'store-browser' && c.action === 'navigate')));
    const isolated = await browser.newContext();
    try {
        const remote = await isolated.newPage();
        const initial = await remote.evaluate(() => ({
            url:location.href, ready:document.readyState === 'complete', empty:document.body?.childElementCount === 0,
            opener:!!window.opener, children:window.frames.length
        }));
        assert.deepEqual(initial,{url:'about:blank',ready:true,empty:true,opener:false,children:0});
        await page.evaluate(({id, url}) => hostEvent({event:'store-browser', id, kind:'initial-document', url}),
            {id, url: initial.url});
        const nav = await (await request('navigate')).jsonValue();
        assert.equal(nav.id, id);
        assert.equal(nav.url, 'https://store.steampowered.com/');
        const requested = remote.waitForRequest(nav.url);
        await isolated.route('**/*', route => route.abort('internetdisconnected'));
        await remote.goto(nav.url).catch(() => {});
        assert.equal((await requested).url(), nav.url);
        await page.evaluate(({id,url}) => {
            hostEvent({event:'store-browser', id, kind:'start', url});
            hostEvent({event:'store-browser', id, kind:'complete', url, success:false, code:1});
        }, nav);
        await page.waitForFunction(() => {
            let r; webpackChunksteamui.push([['fixture-store-route'], {}, f => { r=f; }]);
            return r(5757).oy.WindowStore.GamepadUIMainWindowInstance.History.location.pathname === '/steamweb';
        });
        console.log('Original Store menu requested the public HTTPS Store from /steamweb; network intentionally blocked.');
    } finally { await isolated.close(); }
}

async function checkHome(page, popup) {
    await pressNative(page, 3);
    const menu = popup.frameLocator('iframe[name="MainMenu_uid2"]');
    await menu.getByText('Home', {exact: true}).click();
    const target = popup.locator('#popup_target');
    await popup.getByText('Recent Games', {exact: true}).first().waitFor({state:'visible', timeout:10000})
        .catch(async error => { throw new Error(error.message + '\nHome content: ' + (await target.innerText()).slice(0,1200)); });
    assert(!/Something went wrong while displaying this content/i.test(await target.innerText()));
    const cache = await page.evaluate(() => {
        let r; webpackChunksteamui.push([['fixture-home-cache'], {}, require => { r = require; }]);
        const store = r(90544).tw;
        const mark = 'steam-library-asset-cache-miss';
        performance.clearMarks(mark);
        const missing = store.BHasCachedLibraryAsset(81, undefined, 1);
        const notice = performance.getEntriesByName(mark).map(entry => entry.detail);
        performance.clearMarks(mark);
        const repeated = store.BHasCachedLibraryAsset(81, undefined, 1);
        const present = store.BHasCachedLibraryAsset(42, 1 << 2, 2);
        const newNotices = performance.getEntriesByName(mark).length;
        return {missing, notice, repeated, present, newNotices};
    });
    assert.deepEqual(cache, {missing:false, notice:[{assetType:1}], repeated:false, present:true, newNotices:0});
    console.log('Valve Home opened with offline artwork. The original AppStore retained its cache miss, deduplicated its report and recognized a genuine cache bitmap hit.');
    const search = popup.locator('#header input[type=search]');
    await search.waitFor({state:'visible'});
    const searchFocused = () => search.evaluate(e => e.matches(':focus,.gpfocus'));
    for (let move = 0; move < 6 && !await searchFocused(); move++) await pressNative(page, 64);
    assert(await searchFocused(), 'Controller did not reach the original header Search field');
    await pressNative(page, 4);
    const keys = popup.locator('[data-key][data-key-row]');
    await keys.first().waitFor({state:'visible', timeout:8000}).catch(async error => {
        throw new Error(error.message + ' ' + (await target.innerText()).slice(0,900));
    });
    const focusedKey = popup.locator('[data-key].gpfocus');
    await focusedKey.waitFor({state:'visible'});
    const firstKey = await focusedKey.getAttribute('data-key');
    await pressNative(page, 512);
    const letter = await focusedKey.getAttribute('data-key');
    assert.match(letter, /^[a-z]$/i);
    assert.notEqual(letter, firstKey);
    await pressNative(page, 4);
    assert.equal((await search.inputValue()).toLowerCase(), letter.toLowerCase());
    await pressNative(page, 8);
    await focusedKey.waitFor({state:'hidden',timeout:5000});
    await pressNative(page, 3);
    await menu.getByText('Library', {exact:true}).waitFor({state:'visible'});
    assert(!/Something went wrong while displaying this content/i.test(await target.innerText()));
    console.log('Xbox controller opened Valve search, typed with the original on-screen keyboard, dismissed it and reopened the Steam menu.');
}

async function checkStartupHistory(page, popup, reopen) {
    await pressNative(page, 3);
    const menu = popup.frameLocator('iframe[name="MainMenu_uid2"]');
    await menu.getByText('Store', {exact: true}).click();
    await page.waitForFunction(() => fixtureCalls.some(c => c.method === 'store-browser' && c.action === 'create'));
    await page.close();
    // Keep the browser profile and completed tour. Create an actual new owner document.
    page = await reopen();
    await page.waitForFunction(() => window.App?.GetServicesInitialized(), undefined, {timeout:50000});
    const target = page.frameLocator('#xbox-steam-gamepad-surface');
    await stockTile(target, 'Fixture A').waitFor({state:'visible', timeout:15000});
    const tab = target.getByRole('tab', {name:/^all games/i});
    assert.equal(await tab.getAttribute('aria-selected'), 'true');
    const storeOpened = () => page.evaluate(() => fixtureCalls.some(c => c.method === 'store-browser' && c.action === 'create'));
    // On Xbox the blank Store appeared six seconds after the cold ready receipt.
    // Observe the route without any input, for twice that interval.
    await page.waitForTimeout(12000);
    assert.equal(await tab.getAttribute('aria-selected'), 'true');
    assert.equal(await storeOpened(), false);
    const tile = stockTile(target, 'Fixture A');
    await tile.press('F7'); // Valve's BUMPER_RIGHT binding on its focused Library.
    await target.getByRole('tab', {name:/^installed/i, selected:true}).waitFor({state:'visible'})
        .catch(async error => { throw new Error(error.message + '\n' + (await target.locator('#popup_target').innerText()).slice(0,700)); });
    const route = await page.evaluate(() => {
        let r; webpackChunksteamui.push([['fixture-installed-route'],{}, get => { r = get; }]);
        return r(5757).oy.WindowStore.GamepadUIMainWindowInstance.History.location.pathname;
    });
    assert.equal(route, '/library/tab/Installed');
    assert.equal(await storeOpened(), false);
    console.log('Reopened the same account after Store; cold All Games stayed visible, and F7 opened the original Installed tab.');
}

async function checkNetwork(page, popup) {
    await page.waitForFunction(() => window.SystemNetworkStore && !SystemNetworkStore.isAwaitingInitialNetworkState);
    async function update(snapshot) {
        await page.evaluate(snapshot => {
            window.fixtureNetwork = snapshot;
            hostEvent({event: 'xbox-network-changed'});
        }, snapshot);
        await page.waitForFunction(snapshot => {
            const store = SystemNetworkStore;
            const active = store.presentAccessPoints.find(ap => ap.isDefaultRoute);
            return store.hasInternetConnection === (snapshot.level === 'internet') &&
                (snapshot.kind ? active?.type === (snapshot.kind === 'wifi' ? 2 : 1) : !active);
        }, snapshot);
    }
    const ethernet = {level: 'internet', kind: 'ethernet', device: 1};
    const wifi = {level: 'internet', kind: 'wifi', device: 2, accessPoint: 3, ssid: 'Fixture Wi-Fi', bars: 5};
    const headerSelector = await page.evaluate(() => {
        let r; webpackChunksteamui.push([['fixture-network-css'], {}, require => { r = require; }]);
        return '.' + CSS.escape(r(30557).WirelessIcon);
    });
    const header = popup.locator(headerSelector).first();
    const shape = () => header.locator('svg').evaluate(svg => svg.innerHTML);
    await header.waitFor({state:'visible'});
    const offline = await shape();
    await update(ethernet);
    await popup.waitForFunction(([selector, original]) => {
        const svg = document.querySelector(selector + ' svg');
        return !svg || svg.innerHTML !== original;
    }, [headerSelector, offline]);
    const wired = await header.isVisible() ? await shape() : null;
    await update(wifi);
    await header.waitFor({state:'visible'});
    const wireless = await shape();
    assert.notEqual(wireless, offline);
    assert.notEqual(wireless, wired);
    assert.equal(await page.evaluate(() => SystemNetworkStore.hasSteamConnection), false);
    await pressNative(page, 3);
    const menu = popup.frameLocator('iframe[name="MainMenu_uid2"]');
    await menu.getByText('Settings', {exact: true}).click();
    await popup.getByText('Internet', {exact: true}).first().click({timeout: 8000});
    const wirelessRow = popup.getByText('Fixture Wi-Fi', {exact:true});
    const wiredRow = popup.getByText('Wired Network', {exact:true});
    await wirelessRow.waitFor({state:'visible'});
    await update(ethernet);
    await wiredRow.waitFor({state:'visible'});
    await wirelessRow.waitFor({state:'hidden'});
    await update({...ethernet, level:'local'});
    assert.equal(await page.evaluate(() => SystemNetworkStore.hasNetworkConnection && !SystemNetworkStore.hasInternetConnection), true);
    await update({level:'offline'});
    await wiredRow.waitFor({state:'hidden'});
    await update(wifi);
    await wirelessRow.waitFor({state:'visible'});
    assert.equal(await page.evaluate(() => SystemNetworkStore.hasSteamConnection), false);
    console.log('Valve header SVG changed between offline, wired and Wi-Fi. Settings updated live from Wi-Fi to wired, local-only, offline and back to Wi-Fi. CM stayed disconnected.');
}

async function checkStorage(page, popup) {
    await pressNative(page, 3);
    const menu = popup.frameLocator('iframe[name="MainMenu_uid2"]');
    await menu.getByText('Settings', {exact: true}).click();
    await popup.getByText('Storage', {exact: true}).first().click({timeout: 8000});
    const pane = popup.getByRole('tabpanel', {name: 'Storage', exact: true});
    const developer = pane.getByRole('tabpanel', {name: 'Xbox Development Storage (D:)', exact: true});
    await developer.getByText('D:\\DevelopmentFiles\\SteamLibrary', {exact: true}).waitFor({state: 'visible'})
        .catch(async error => { throw new Error(error.message + '\n' + (await popup.locator('#popup_target').innerText()).slice(0, 1200)); });
    await page.waitForFunction(() => installFolderStore.MountedInstallFolders.length === 3);
    assert.equal(await page.evaluate(() => installFolderStore.GetInstallFolder(0).bIsDefaultFolder), true);
    await pane.getByRole('tab', {name: /External Storage/}).press('F3'); // Valve's secondary action sets the default.
    await page.waitForFunction(() => installFolderStore.GetInstallFolder(1).bIsDefaultFolder);
    await pane.getByRole('tab', {name: /Xbox Development Storage/}).press('F3');
    await page.waitForFunction(() => installFolderStore.GetInstallFolder(0).bIsDefaultFolder);

    await pane.getByRole('tab', {name: /App Storage/}).click();
    const game = pane.getByRole('tabpanel', {name: 'App Storage', exact: true}).locator('[data-appid="42"]');
    await game.press('F4').catch(async error => { throw new Error(error.message + '\n' + await pane.innerText()); });
    const moved = popup.getByText('All content moved successfully', {exact: true});
    await popup.getByRole('button', {name: 'Move', exact: true}).click();
    await moved.waitFor({state: 'visible'});
    await page.waitForFunction(() => installFolderStore.GetInstallFolder(0).vecApps.some(app => app.nAppID === 42));
    await popup.getByRole('button', {name: 'Close', exact: true}).click();
    await pane.getByRole('tab', {name: /Xbox Development Storage/}).click();
    await developer.locator('[data-appid="42"]').waitFor({state: 'visible'});
    await page.evaluate(() => {
        window.fixtureStorage.folders.find(folder => folder.id === 'E').mounted = false;
        SteamClient.InstallFolder.RefreshFolders();
    });
    await page.waitForFunction(() => !installFolderStore.GetInstallFolder(1).bIsMounted);
    await page.evaluate(() => {
        window.fixtureStorage = null;
        SteamClient.InstallFolder.RefreshFolders();
    });
    await popup.getByText('Storage unavailable', {exact: true}).first().waitFor({state: 'visible'});
    console.log('Opened Valve Storage and checked the D default, E selection, move completion, and unavailable view.');
}

async function checkAppAdapter(page) {
    const result = await page.evaluate(async () => {
        let require;
        window.webpackChunksteamui.push([['xbox-protobuf-check'], {}, r => { require = r; }]);
        const codec = require(76142).bs;
        const firstCall = window.fixtureCalls.length;
        let subscription;
        const snapshot = await new Promise((resolve, reject) => {
            const timeout = setTimeout(() => reject(new Error('No app overview delivered')), 5000);
            subscription = SteamClient.Apps.RegisterForAppOverviewChanges(bytes => {
                clearTimeout(timeout);
                resolve(codec.deserializeBinary(bytes).toObject());
            });
        });
        subscription.unregister();
        async function receiveStatus(appId) {
            window.hostEvent({event: 'xbox-game', appId, overlay: false});
            let sub;
            try {
                return await new Promise(resolve => {
                    sub = SteamClient.Apps.RegisterForAppOverviewChanges(bytes =>
                        resolve(codec.deserializeBinary(bytes).toObject().app_overview[0].per_client_data[0].display_status));
                });
            } finally { sub?.unregister(); }
        }
        const running = await receiveStatus(42);
        const exited = await receiveStatus(0);
        await SteamClient.Apps.RunGame('42', '', -1, 0);
        let refused;
        try { await SteamClient.Apps.RunGame('73', '', -1, 0); } catch (error) { refused = error.message; }
        return {snapshot, running, exited, refused, plays: window.fixtureCalls.slice(firstCall).filter(call => call.method === 'play').map(call => call.appId)};
    });
    assert.equal(result.snapshot.full_update, true);
    assert.equal(result.snapshot.update_complete, true);
    assert.equal(result.running, 4);
    assert.equal(result.exited, 11);
    assert.deepEqual(result.snapshot.app_overview.map(game => [game.appid, game.display_name, game.subscribed_to,
        game.visible_in_game_list, game.per_client_data[0].installed, game.per_client_data[0].display_status]),
    [[42, 'Fixture A', true, true, true, 11], [73, 'Fixture B', true, true, false, 9]]);
    assert.equal(result.refused, 'not-installed');
    assert.deepEqual(result.plays, [42, 73]);
}

async function checkRunningControls(page, popup) {
    if (accounts) await page.evaluate(() => {
        let runtime;
        webpackChunksteamui.push([['fixture-overlay-history'], {}, value => { runtime = value; }]);
        const history = runtime(5757).oy.WindowStore.GamepadUIMainWindowInstance.History;
        const route = runtime(72500).BV.GamepadUI.AppOverlay.Root(42);
        window.fixtureOverlayPushes = 0;
        window.fixtureStopObservingHistory = history.listen((location, action) => {
            if (action === 'PUSH' && location.pathname === route) fixtureOverlayPushes++;
        });
    });
    await page.evaluate(() => hostEvent({event: 'xbox-game', appId: 42, overlay: true}));
    let resume = popup.getByText('Resume game', {exact: true});
    await resume.waitFor({state: 'visible'});
    assert(await popup.getByText('Exit game', {exact: true}).isVisible());
    if (accounts) {
        const menu = popup.frameLocator('iframe[name="MainMenu_uid2"]');
        await pressNative(page, 3);
        await menu.getByText('Library', {exact: true}).click({timeout: 7000});
        const predicates = await nativeLibraryPredicates();
        await popup.waitForFunction(predicates.library, undefined, {timeout: 12000});
        const running = await page.evaluate(() => ({
            app: fixtureRunningApp, quit: fixtureCalls.filter(c => c.method === 'quit-game').length,
            resume: fixtureCalls.filter(c => c.method === 'resume-game').length
        }));
        assert.deepEqual(running, {app: 42, quit: 0, resume: 0});
        console.log('Original All Games is in the foreground with the same game still running.');
        await stockTile(popup, 'Fixture A').click();
        resume = popup.getByText('Resume', {exact: true});
        await resume.waitFor({state: 'visible', timeout: 6500});
    }
    const before = await page.evaluate(() => fixtureCalls.filter(c => c.method === 'resume-game').length);
    await resume.click();
    await page.waitForFunction(count => fixtureCalls.filter(c => c.method === 'resume-game' && c.appId === 42).length > count, before);
    if (accounts) {
        await page.evaluate(() => hostEvent({event: 'xbox-game', appId: 42, overlay: true}));
        const exit = popup.getByText('Exit game', {exact: true});
        const confirm = popup.getByText('Confirm', {exact: true});
        const cancel = popup.getByText('Cancel', {exact: true});
        await exit.click();
        await confirm.waitFor({state: 'visible'});
        assert.equal(await confirm.getAttribute('type'), 'submit');
        assert.equal(await cancel.getAttribute('type'), 'button');
        assert.equal(await page.evaluate(() => {
            fixtureStopObservingHistory();
            return fixtureOverlayPushes;
        }), 2, 'Returning from the Library opens the controls once, and resuming preserves their Back destination');
        await cancel.click();
        await confirm.waitFor({state: 'hidden'});
        assert.equal(await page.evaluate(() => fixtureCalls.filter(c => c.method === 'quit-game').length), 0);
        await exit.click();
        await confirm.click();
        await page.waitForFunction(() => fixtureCalls.filter(c => c.method === 'quit-game' && c.appId === 42).length === 1,
            undefined, {timeout: 5000});
        await confirm.waitFor({state: 'hidden'});
        console.log('The original Exit dialog canceled without quitting, then confirmed one graceful quit for the active game.');
    }
    await page.evaluate(() => hostEvent({event: 'xbox-game', appId: 0, overlay: false}));
    if (accounts) {
        const predicates = await nativeLibraryPredicates();
        await popup.waitForFunction(predicates.library, undefined, {timeout: 12000}).catch(async error => {
            const route = await page.evaluate(() => {
                let r; webpackChunksteamui.push([['fixture-exit-route'], {}, value => { r = value; }]);
                return r(5757).oy.WindowStore.GamepadUIMainWindowInstance?.History.location.pathname;
            });
            const ui = await popup.evaluate(() => {
                const visible = el => el.checkVisibility({visibilityProperty:true}) && el.getBoundingClientRect().width > 0;
                const grid = document.querySelector('[role=tabpanel][id$="AllGames_Content"] [role=grid]');
                const tile = grid?.querySelector('[role=link]');
                const rect = tile?.getBoundingClientRect();
                const blockers = [...document.querySelectorAll('[role=dialog],[aria-modal=true],dialog[open],._1yLBJnGBard9ajB79vr-6_,._16IUnAiPD_1s1KQ0g8vJpq,iframe')]
                    .filter(visible).map(el=>[el.tagName,el.name,el.className]);
                return {text:document.getElementById('popup_target')?.innerText.slice(0,500), blockers, tile:tile?.innerText,
                    hit:rect && document.elementFromPoint(rect.x+rect.width/2,rect.y+rect.height/2)?.className};
            });
            throw new Error(error.message + '\nExit route ' + route + ' ' + JSON.stringify(ui));
        });
        await stockTile(popup, 'Fixture A').waitFor({state: 'visible'});
    }
}

async function checkSettingsAdapter(page) {
    const result = await page.evaluate(async () => {
        let runtime;
        window.webpackChunksteamui.push([['xbox-settings-check'], {}, r => { runtime = r; }]);
        const codec = runtime(29303).Ne;
        const messages = [];
        const sub = SteamClient.Settings.RegisterForSettingsArrayChanges(bytes => {
            messages.push(codec.deserializeBinary(bytes).toObject());
        });
        const encode = value => btoa(String.fromCharCode(...codec.fromObject(value).serializeBinary()));
        await SteamClient.Settings.SetSetting(encode({oobe_completed: true}));
        await SteamClient.Settings.SetSetting(encode({oobe_stage_2_completed: false}));
        const data = messages.at(-1);
        const count = messages.length;
        let rejected;
        try {
            await SteamClient.Settings.SetSetting(encode({oobe_test_mode_enabled: true, oobe_completed: false}));
        } catch (error) { rejected = error.message; }
        sub.unregister();
        return {first: data.oobe_completed, second: data.oobe_stage_2_completed, rejected, count,
            unchanged: messages.length === count};
    });
    assert.equal(result.first, true);
    assert.equal(result.second, false);
    assert(result.count >= 2 && result.unchanged);
    assert.match(result.rejected, /Unsupported/);
}

async function checkPlatform(page) {
    const actual = await page.evaluate(async () => {
        const settings = SteamClient.Settings;
        const languages = await settings.GetAvailableLanguages(), language = await settings.GetCurrentLanguage();
        const zones = await settings.GetAvailableTimeZones(), zone = await settings.GetTimeZone();
        return {info: await SteamClient.System.GetSystemInfo(), debug: SteamClient._internal.IsDebuggingEnabled(),
            languageAvailable: languages.some(item => item.strShortName === language && Number.isInteger(item.language)),
            zonesAvailable: zones.some(item => item.timezoneID === zone)};
    });
    assert.equal(actual.info.sOSVariantId, 'Windows.Xbox');
    assert.equal(actual.info.sOSVersionId, '10.0.26100.2');
    assert.equal(actual.info.nSteamVersion, 1794536281);
    assert.equal(actual.debug, false);
    assert(actual.languageAvailable && actual.zonesAvailable);
}

async function checkLocalCloudCache(page) {
    const actual = await page.evaluate(async () => {
        const key = 'cloud-storage-namespace-4.modified';
        const url = '/cloudstorage/' + key + '.json';
        const rows = JSON.stringify(['fixture UI preference', 'layout']);
        await SteamClient.CloudStorage.WriteKey(key, rows);
        const received = await (await fetch(url)).text();
        let denied = false;
        try { await SteamClient.CloudStorage.WriteKey('outside-file', rows); } catch { denied = true; }
        window.fixtureSteamId = '76561198000000001';
        const changedOwner = (await fetch(url)).status;
        let writeDenied = false;
        try { await SteamClient.CloudStorage.WriteKey(key, '[]'); } catch { writeDenied = true; }
        window.fixtureSteamId = null;
        const signedOut = (await fetch(url)).status;
        return {received, rows, denied, changedOwner, writeDenied, signedOut};
    });
    assert.equal(actual.received, actual.rows);
    assert(actual.denied && actual.writeDenied);
    assert.equal(actual.changedOwner, 403);
    assert.equal(actual.signedOut, 403);
    const readCache = () => page.evaluate(async () => {
        const response = await fetch('/cloudstorage/cloud-storage-namespace-4.modified.json');
        return {status: response.status, text: await response.text()};
    });
    await page.evaluate(() => sessionStorage.setItem('fixture-next-account', '76561198000000001'));
    await page.goto('about:blank');
    await page.goto(entry, {waitUntil: 'domcontentloaded'});
    assert.equal((await readCache()).status, 404);
    await page.evaluate(() => sessionStorage.removeItem('fixture-next-account'));
    await page.goto('about:blank');
    await page.goto(entry, {waitUntil: 'domcontentloaded'});
    assert.deepEqual(await readCache(), {status: 200, text: actual.rows});
}

async function connectStandardPad(page, popup) {
    await page.evaluate(() => {
        window.setupPad = {index: 0, id: 'Xbox Controller', connected: true, mapping: 'standard',
            buttons: Array.from({length: 17}, () => ({pressed: false})), axes: [0, 0, 0, 0]};
        Object.defineProperty(navigator, 'getGamepads', {configurable: true, value: () => [setupPad]});
    });
    await popup.evaluate(() => Object.defineProperty(navigator, 'getGamepads', {
        configurable: true, value: () => [window.opener.setupPad]
    }));
    await popup.waitForTimeout(600);
}
async function pressPad(page, popup, button) {
    await page.evaluate(index => { setupPad.buttons[index].pressed = true; }, button);
    await popup.waitForTimeout(180);
    await page.evaluate(index => { setupPad.buttons[index].pressed = false; }, button);
    await popup.waitForTimeout(500);
}
async function openStockLibrary(page, popup) {
    const title = stockTile(popup, 'Fixture A');
    await title.waitFor({state: 'visible'});
    await stockTile(popup, 'Fixture B').waitFor({state: 'visible'});
    if (requireGamepad) {
        assert(await popup.evaluate(visibleShell), 'Original Steam library layout is missing or offscreen.');
        assert(!(await popup.locator('#popup_target').innerText()).includes('Error Reference:'),
            'The visible Steam library contains a render error.');
    }
    await title.click();
    console.log('After clicking an owned title:', (await popup.locator('#popup_target').innerText()).slice(0, 450));
    const play = popup.getByText(/^play$/i).first();
    await play.waitFor({state: 'visible', timeout: 6000});
    const count = await page.evaluate(() => fixtureCalls.filter(c => c.method === 'play' && c.appId === 42).length);
    await play.click();
    await page.waitForFunction(before => fixtureCalls.filter(c => c.method === 'play' && c.appId === 42).length > before, count);
}
async function checkController(page, popup) {
    // Supply a W3C gamepad reading at the browser boundary. No SteamClient method
    // or Valve controller/navigation module is replaced in this test.
    await page.evaluate(() => {
        const pad = {index: 0, id: 'Fixture Standard', connected: true, mapping: 'standard',
            buttons: Array.from({length: 17}, () => ({pressed: false})), axes: [0, 0, 0, 0]};
        const unbound = {index: 1, id: 'Fixture Unmapped', connected: true, mapping: '', buttons: [], axes: []};
        const state = window.controllerFixture = {pad, pads: [pad, unbound], digital: [], analog: [], guide: [], unbound: [],
            changes: 0};
        Object.defineProperty(navigator, 'getGamepads', {configurable: true, value: () => state.pads});
    });
    if (popup) await popup.evaluate(() => Object.defineProperty(navigator, 'getGamepads', {
        configurable: true, value: () => window.opener.controllerFixture.pads
    }));
    await page.evaluate(() => {
        const state = window.controllerFixture;
        let runtime;
        window.webpackChunksteamui.push([['xbox-controller-check'], {}, r => { runtime = r; }]);
        const svc = runtime(744).f_;
        state.readInventory = async () => (await svc.GetControllerList({})).Body().toObject().controllers;
        state.subs = [
            svc.RegisterForNotifyControllerListChanged(() => { state.changes++; }),
            SteamClient.Input.RegisterForControllerInputMessages((...e) => state.digital.push(e)),
            SteamClient.Input.RegisterForControllerAnalogInputMessages((...e) => state.analog.push(e)),
            SteamClient.System.UI.RegisterForSystemKeyEvents(e => state.guide.push(e)),
            SteamClient.Input.RegisterForUnboundControllerListChanges(list => state.unbound.push(list))
        ];
        SteamClient.Input.EnableControllerAnalogInputMessages(true);
        for (const button of [0, 12, 16]) state.pad.buttons[button].pressed = true;
    });
    await page.waitForFunction(() => controllerFixture.guide.length && controllerFixture.unbound.at(-1)?.length);
    const pairing = await page.evaluate(async () => {
        let r; webpackChunksteamui.push([['pairing-service-check'], {}, runtime => { r = runtime; }]);
        const reply = await r(744).f_.ShouldTritonPairInOobe({});
        return [reply.BSuccess(), reply.Hdr().eresult()];
    });
    assert.deepEqual(pairing, [false, 20]);
    const connected = await page.evaluate(() => controllerFixture.readInventory());
    assert.deepEqual(connected.map(p => [p.controller_index, p.name]), [[0, 'Fixture Standard'], [1, 'Fixture Unmapped']]);
    await page.evaluate(() => {
        controllerFixture.pad.axes[1] = -0.8; // overlap the held D-pad direction
        controllerFixture.pad.buttons[12].pressed = false;
    });
    await page.waitForFunction(() => controllerFixture.analog.some(args => args[4] === -0.8));
    await page.evaluate(() => { controllerFixture.pads = []; });
    await page.waitForFunction(() => controllerFixture.changes > 0 && controllerFixture.digital.some(args => args[1] === 20 && !args[2]));
    assert.deepEqual(await page.evaluate(() => controllerFixture.readInventory()), []);
    const value = await page.evaluate(() => {
        const {pad, pads, subs, readInventory, ...value} = controllerFixture;
        subs.forEach(sub => sub.unregister());
        return value;
    });
    assert.deepEqual(value.digital, [[0, 0, true, 0, 0], [0, 20, true, 0, 0], [0, 0, false, 0, 0], [0, 20, false, 0, 0]]);
    assert.deepEqual(value.guide, [{eKey: 0, nControllerIndex: 0}]);
    assert.deepEqual(value.analog, [[0, 24, false, 0, -0.8], [0, 24, false, 0, 0]]);
    assert.deepEqual(value.unbound, [[{strName: 'Fixture Unmapped'}], []]);
}

function visibleShell() {
    const target = document.getElementById('popup_target'), shell = document.getElementById('Header.Main.Footer');
    const content = shell?.querySelector('.ZBBhezbBR4VJYysmVhF3_, ._2_cRkGdbegoIbiRsy_-I7');
    const grid = target?.querySelector('#GamepadUI_Full_Root._3LYP1SIxyoky-yW9Ug_1fq #Main [role=tabpanel][id$="AllGames_Content"] [role=grid]');
    const tile = grid?.querySelector('[role=gridcell] [role=link]');
    if (tile?.getBoundingClientRect().width > 0 && tile.getBoundingClientRect().height > 0 &&
        getComputedStyle(tile).visibility === 'visible') return true;
    return !!(target?.contains(shell) && shell.classList.contains('_3LYP1SIxyoky-yW9Ug_1fq') && content &&
        content.getBoundingClientRect().width && content.getBoundingClientRect().height &&
        getComputedStyle(content).visibility === 'visible');
}

const tourPrompts = ['Welcome to Big Picture', 'Steam Menu', 'Quick Access Menu', 'Set up Parental Controls', 'Have fun'];
async function pressNative(page, bits) {
    await page.evaluate(value => { fixtureNativePad.bits = value; }, bits);
    await page.waitForTimeout(200);
    await page.evaluate(() => { fixtureNativePad.bits = 0; });
    await page.waitForTimeout(350);
}

async function completeTour(page, popup, advance = () => pressNative(page, 4)) {
    for (const step of tourPrompts) {
        const heading = popup.getByText(new RegExp('^' + step + '[!.]?$', 'i')).first();
        await heading.waitFor({state: 'visible', timeout: 6000}).catch(async error => {
            throw new Error(error.message + '\n' + (await popup.locator('#popup_target').innerText()).slice(0, 700));
        });
        await advance();
        await heading.waitFor({state: 'hidden', timeout: 6000});
    }
}

function stockTile(popup, name) {
    return popup.locator('[role=tabpanel]:visible [role=gridcell] [role=link]').filter({hasText:name}).first();
}

async function checkInstalledTab(popup, installed, other) {
    const tab = popup.getByRole('tab', {name:/^installed/i});
    await tab.click({timeout:10000});
    const panelId = await tab.getAttribute('aria-controls');
    const panel = popup.locator('[role=tabpanel][id=' + JSON.stringify(panelId) + ']');
    const tile = panel.getByRole('link', {name:installed, exact:true}).first();
    await tile.waitFor({state:'visible'});
    assert.equal(await tab.getAttribute('aria-selected'), 'true');
    assert.equal(await panel.getByRole('link', {name:other, exact:true}).count(), 0);
    return tile;
}

async function checkInstall(page, popup) {
    const tile = stockTile(popup, 'Fixture B');
    await tile.waitFor({state: 'visible'});
    await tile.click();
    const requests = () => page.evaluate(() => fixtureCalls.filter(request =>
        request.method === 'install').map(request => request.appId));
    const button = popup.getByText(/^install$/i).first();
    await button.click();
    await page.waitForFunction(() => typeof fixtureFinishInstall === 'function');
    assert.deepEqual(await requests(), [installAppId]);
    await page.evaluate(appId => hostEvent({event: 'xbox-install', appId, stage: 'downloading'}), installAppId);
    await button.waitFor({state:'hidden'});
    await page.evaluate(() => fixtureFinishInstall('stalled'));
    const failureMessage = /An unexpected error occurred while installing Fixture B|Steam is unable to install content when in Offline Mode/i;
    const failure = popup.getByText(failureMessage);
    await failure.waitFor({state:'visible'}).catch(async error => {
        throw new Error(error.message + '\n' + (await popup.locator('#popup_target').innerText()).slice(0, 700));
    });
    await popup.getByRole('dialog').filter({hasText: failureMessage}).getByText(/^close$/i).click();
    await failure.waitFor({state:'hidden'});
    await button.click();
    await page.waitForFunction(() => fixtureCalls.filter(request => request.method === 'install').length === 2);
    await page.evaluate(() => fixtureFinishInstall('installed'));
    await popup.getByText(/^play$/i).first().waitFor({state:'visible'});
    assert.deepEqual(await requests(), [installAppId, installAppId]);
    assert.equal(await page.evaluate(() => fixtureCalls.filter(request => request.method === 'play').length), 0);
    console.log('Valve installed an owned title through the Xbox broker, displayed a Steam error, and offered Play after retry without reopening.');
}
async function checkDownloads(page, popup) {
    async function menuItem(name) {
        await pressNative(page, 3);
        await popup.frameLocator('iframe[name="MainMenu_uid2"]').getByText(name, {exact: true}).first().click();
    }
    await menuItem('Downloads');
    const empty = popup.getByText(/no downloads/i).first();
    await empty.waitFor({state: 'visible', timeout: 12000}).catch(async error => {
        throw new Error(error.message + '\n' + (await popup.locator('#popup_target').innerText()).slice(0, 1200));
    });
    await menuItem('Library');
    await stockTile(popup, 'Fixture B').click();
    await popup.getByText(/^install$/i).first().click();
    await page.waitForFunction(() => typeof fixtureFinishInstall === 'function');
    await page.evaluate(appId => hostEvent({event: 'xbox-install', appId, stage: 'downloading',
        progress: {downloaded: 8 * 1024 ** 2, downloadTotal: 32 * 1024 ** 2}}), installAppId);
    await menuItem('Downloads');
    const game = popup.getByText('Fixture B', {exact: true}).first();
    await game.waitFor({state: 'visible', timeout: 12000});
    assert.deepEqual(await page.evaluate(() => {
        let runtime;
        webpackChunksteamui.push([['fixture-download-state'], {}, value => { runtime = value; }]);
        const store = runtime(55426).hj;
        const overview = store.LocalDownloadOverview;
        const bytes = overview.progress[runtime(94601).tgE.k_EAppUpdateProgress_Download];
        return {queue: store.QueuedTransfers.map(item => item.appid), appId: overview.update_appid,
            state: overview.update_state, percent: overview.overall_percent_complete,
            downloaded: bytes.bytes_in_progress, total: bytes.bytes_total};
    }), {queue: [installAppId], appId: installAppId, state: 'Downloading', percent: 25,
        downloaded: 8 * 1024 ** 2, total: 32 * 1024 ** 2});
    await page.evaluate(() => fixtureFinishInstall('installed'));
    await page.waitForFunction(appId => {
        let runtime;
        webpackChunksteamui.push([['fixture-download-complete-' + crypto.randomUUID()], {}, value => { runtime = value; }]);
        const store = runtime(55426).hj;
        return store.CompletedTransfers.some(item => item.appid === appId) && store.QueuedTransfers.length === 0;
    }, installAppId);
    assert.equal(await game.isVisible(), true);
    console.log('Valve Downloads opened empty, showed Spider-Man byte progress, and moved the finished install to Completed.');
}

async function readSeen(page) {
    return page.evaluate(async () => {
        let r; webpackChunksteamui.push([['fixture-tour-read-' + crypto.randomUUID()], {}, value => { r = value; }]);
        return r(14255).TT();
    });
}

async function readMembership(page) {
    const membership = await page.evaluate(async () => {
        let runtime;
        webpackChunksteamui.push([['fixture-library-membership'], {}, value => { runtime = value; }]);
        let subscription;
        const change = await new Promise(resolve => {
            subscription = SteamClient.Apps.RegisterForAppOverviewChanges(bytes => {
                resolve(runtime(76142).bs.deserializeBinary(bytes).toObject());
            });
        });
        subscription.unregister();
        const store = runtime(90544).tw;
        return {identity: runtime(30610).iA.steamid, initialized: !!store.m_bIsInitialized,
            store: store.allApps.map(app => app.appid).sort((a, b) => a - b),
            full: change.full_update, complete: change.update_complete,
            overviews: change.app_overview.map(app => [app.appid, app.display_name, app.per_client_data[0].installed])};
    });
    assert(membership.initialized && membership.full && membership.complete, 'Expected a complete original Steam app overview');
    return membership;
}

async function checkOtherAccount(page) {
    const popup = page.frameLocator('#xbox-steam-gamepad-surface');
    const welcome = popup.getByText('Welcome to Big Picture', {exact:true}).first();
    await welcome.waitFor({state:'visible', timeout:60000});
    await page.waitForFunction(() => App.GetServicesInitialized(), undefined, {timeout:50000});
    const expected = catalogs[secondAccount];
    const membership = await readMembership(page);
    assert.equal(membership.identity, secondAccount);
    assert.deepEqual(membership.store, expected.map(g=>g.id));
    assert.deepEqual(membership.overviews, expected.map(g=>[g.id,g.name,g.installed]));
    for (const prior of catalogs[fixtureAccount]) assert.equal(await popup.getByText(prior.name,{exact:true}).count(), 0);
    await completeTour(page, popup);
    await pressNative(page, 3);
    await popup.frameLocator('iframe[name="MainMenu_uid2"]').getByText('Library', {exact:true}).click();
    for (const game of expected) await popup.getByText(game.name, {exact:true}).first().waitFor({state:'visible'});
    const installed = await checkInstalledTab(popup, 'Second Lantern', 'Second Atlas');
    await installed.click();
    await popup.getByText(/^play$/i).first().click();
    await page.waitForFunction(() => fixtureCalls.some(c => c.method === 'play' && c.appId === 93));
    console.log('Fresh B completed its own tour, saw only its two owned games, filtered its installed title and played its own AppID.');
}

async function nativeLibraryPredicates() {
    const header = await fs.readFile(path.resolve(__dirname, '../probe/big-picture-webview/library-capture.h'), 'utf8');
    const entries = [...header.matchAll(/constexpr wchar_t (\w+)\[\] = LR"JS\(([\s\S]*?)\)JS"/g)];
    const predicates = Object.fromEntries(entries.map(match => [match[1], match[2]]));
    const renderer = await fs.readFile(path.resolve(__dirname, '../probe/big-picture-webview/renderer.cpp'), 'utf8');
    predicates.warmLayout = renderer.match(/fire_and_forget observeGamepad[\s\S]*?wchar_t check\[\] = LR"JS\(([\s\S]*?)\)JS"/)?.[1];
    for (const name of ['emptyLibrary', 'emptyForeground', 'library', 'warmLayout']) assert(predicates[name]);
    return predicates;
}

async function checkEmptyLibrary(page) {
    const root = page.frameLocator('#xbox-steam-gamepad-surface');
    const popup = await (await page.locator('#xbox-steam-gamepad-surface').elementHandle()).contentFrame();
    const predicates = await nativeLibraryPredicates();
    const membership = await readMembership(page);
    assert.equal(membership.identity, emptyAccount);
    assert.deepEqual(membership.store, []);
    assert.deepEqual(membership.overviews, []);
    const welcome = root.getByText(tourPrompts[0], {exact: true}).first();
    await welcome.waitFor({state: 'visible', timeout: 20000});
    const underTour = await popup.evaluate(({warmLayout, emptyLibrary, emptyForeground}) => ({
        warm: eval(warmLayout), empty: eval(emptyLibrary), foreground: eval(emptyForeground)
    }), predicates);
    assert.equal(underTour.warm, 1, 'The original tour supplies an actionable native warm-up layout');
    assert.equal(underTour.foreground, false, 'The welcome covers the Library');
    if (process.argv.includes('--hold-tour')) {
        await page.waitForTimeout(111000); // Longer than the normal startup routing deadline.
        assert(await welcome.isVisible(), 'Steam must wait for user input for the entire tour');
    }
    await completeTour(page, root);
    await popup.waitForFunction(predicates.emptyLibrary, undefined, {timeout: 20000});
    await popup.waitForFunction(predicates.emptyForeground, undefined, {timeout: 15000});
    assert(await popup.evaluate(predicates.emptyLibrary));
    assert.equal(await popup.evaluate(predicates.library), false);
    const panel = root.locator('#Main [role=tabpanel][id$="AllGames_Content"]');
    const selected = root.locator('#Main [role=tab][aria-selected=true][aria-controls$="AllGames_Content"]');
    assert.equal(await selected.getAttribute('aria-controls'), await panel.getAttribute('id'));
    const grid = panel.locator('.rz1mrOnj9WN6UErES7qsq');
    assert.equal(await grid.locator('[role=gridcell] [role=link]').count(), 0);
    assert.equal((await grid.innerText()).trim(), '');
    console.log('Fresh empty account passed native setup while the tour was visible and strict empty Library after dismissal.', underTour);
}

async function checkSignedOutShell(page) {
    await page.waitForFunction(() => window.App?.BFinishedInitBeforeLogin(), undefined, {timeout:50000});
    const sample = await page.evaluate(async () => {
        let r; webpackChunksteamui.push([['fixture-signed-out'], {}, value => { r = value; }]);
        const store = r(90544).tw;
        let blocked = false;
        try { await SteamClient.Apps.RunGame('93', '', -1, 0); } catch (_) { blocked = true; }
        return {user:App.BHasCurrentUser(), after:App.m_bStartedInitAfterLogin,
            games:store.m_bIsInitialized ? store.allApps.map(app => app.appid) : [], blocked};
    });
    assert(!sample.user && !sample.after && !sample.games.length && sample.blocked);
    for (const frame of page.frames()) for (const group of Object.values(catalogs))
        for (const game of group) assert.equal(await frame.getByText(game.name, {exact:true}).count(), 0);
    console.log('Signed-out original shared shell has no current user, catalog, or allowed launch.');
}

async function verifyStartup(browser, source, steamId, nativeGamepads = false) {
    const context = await browser.newContext({offline: true, serviceWorkers: 'block'});
    try {
        const sourceError = value => {
            const text = String(value), frame = text.match(/(?:library|sp)\.js[^\s)]*?:\d+:\d+/);
            return text.split('\n')[0] + (frame ? ' [' + frame[0] + ']' : '');
        };
        const files = new Set(), missing = new Set(), errors = new Set(), blocked = new Set(), failures = new Set();
        let cloudToken = 0;
        context.on('requestfailed', request => {
            const url = new URL(request.url());
            failures.add(url.pathname + ' ' + request.failure()?.errorText);
        });
        // Chromium pauses requests made during the about:blank popup's initial
        // document.write before Playwright can associate them with a page. Serve
        // directly on each DevTools session, including the initial owner target.
        async function serve(target) {
            const session = await context.newCDPSession(target);
            const ownerId = (await session.send('Page.getFrameTree')).frameTree.frame.id;
            session.on('Fetch.requestPaused', async ({requestId, request, resourceType, frameId}) => {
                if (framedGamepad && resourceType === 'Document' && (request.url !== entry || frameId !== ownerId)) {
                    await session.send('Fetch.failRequest', {requestId, errorReason: 'BlockedByClient'}).catch(() => {});
                    return;
                }
                const url = new URL(request.url);
                const file = path.resolve(assets, '.' + decodeURIComponent(url.pathname));
                if (url.origin !== origin || !file.startsWith(assets + path.sep)) {
                    blocked.add(url.origin);
                    await session.send('Fetch.failRequest', {requestId, errorReason: 'BlockedByClient'}).catch(() => {});
                    return;
                }
                if (url.pathname.startsWith('/cloudstorage/')) {
                    const key = /^\/cloudstorage\/(cloud-storage-(?:namespaces|namespace-[134](?:\.modified)?))\.json$/.exec(url.pathname)?.[1];
                    let result = {status: 403};
                    const referer = Object.entries(request.headers).find(([name]) => name.toLowerCase() === 'referer')?.[1];
                    const ownerAddress = value => value === entry || (typeof value === 'string' && value.length <= 2048 &&
                        /^https:\/\/steamloopback\.host\/routes\/[A-Za-z0-9_/-]*$/.test(value) && !value.slice(origin.length).includes('//'));
                    if (key && !url.search && request.method === 'GET' && target === page && ownerAddress(page.url()) && ownerAddress(referer)) {
                        result = await page.evaluate(({key, token}) => new Promise(resolve => {
                            const timer = setTimeout(() => { fixtureReads.delete(token); resolve({status: 503}); }, 5000);
                            fixtureReads.set(token, reply => { clearTimeout(timer); resolve(reply); });
                            hostEvent({event: 'xbox-cloud-cache-get', key, token});
                        }), {key, token: ++cloudToken}).catch(() => ({status: 503}));
                    }
                    await session.send('Fetch.fulfillRequest', {requestId, responseCode: result.status,
                        responseHeaders: [{name: 'Content-Type', value: 'application/json'}, {name: 'Cache-Control', value: 'no-store'}],
                        body: result.body ? Buffer.from(result.body).toString('base64') : undefined}).catch(() => {});
                    return;
                }
                const responseHeaders = [
                    {name: 'Content-Type', value: contentTypes[path.extname(file)] || 'application/octet-stream'},
                    {name: 'Cache-Control', value: 'public, max-age=60'}
                ];
                let responseCode = 200, body;
                try {
                    let data;
                    try { data = await fs.readFile(file); }
                    catch (error) {
                        const glyph = /^\/steaminputglyphs\/([\w-]+\.svg)$/.exec(url.pathname);
                        if (!glyph || error.code !== 'ENOENT') throw error;
                        // Raw desktop layouts contain the source of stage-glyphs.py's packaged copies.
                        data = await fs.readFile(path.join(assets, '../controller_base/images/api/knockout', glyph[1]));
                    }
                    body = data.toString('base64');
                    files.add(url.pathname);
                } catch {
                    responseCode = 404;
                    missing.add(url.pathname);
                }
                await session.send('Fetch.fulfillRequest', {requestId, responseCode, responseHeaders, body}).catch(() => {});
            });
            await session.send('Fetch.enable', {patterns: [{urlPattern: '*'}]});
        }
        const state = {nativeGamepads, framedGamepad, deviceFamily: 'Windows.Xbox', deviceFamilyVersion: String((10n << 48n) | (26100n << 16n) | 2n),
            keyboardPresent: 0,
            devToolsEnabled: false, viewId: 1, pid: 100, ownerId: 1, popupId: 2, visible: true, popupOpen: false,
            popupVisible: false, x: 0, y: 0, width: 1280, height: 720, screenWidth: 1280, screenHeight: 720,
            usableX: 0, usableY: 0, usableWidth: 1280, usableHeight: 720, scale: 1};
        let page, openingOwner = false;
        function observe(target) {
            target.on('pageerror', error => errors.add(sourceError(error.stack || error.message)));
            target.on('console', msg => { if (msg.type() === 'error') errors.add(sourceError(msg.text())); });
        }
        context.on('page', child => {
            if (openingOwner) return;
            serve(child).catch(error => errors.add(error.message));
            observe(child);
            page.evaluate(seed => hostEvent({event: 'xbox-window', state: {...seed, popupOpen: true, popupVisible: true}}), state)
                .catch(error => errors.add('Fixture window notification: ' + error.message));
        });
        async function openOwner(account = steamId) {
            openingOwner = true;
            try { page = await context.newPage(); } finally { openingOwner = false; }
            await serve(page);
            observe(page);
            page.setDefaultTimeout(10000);
            await page.addInitScript({content: 'if(window===top){ const xboxNativeWindow=Object.freeze(' + JSON.stringify(state) + ');\n' +
                '(' + setupHost.toString() + ')(' + JSON.stringify(account) + ', xboxNativeWindow, ' + JSON.stringify(catalogs) + ');\n' + source + '\n}'});
            await page.goto(entry);
        }
        await openOwner();
        await page.waitForFunction(() => window.App && document.title === 'SharedJSContext').catch(error => {
            console.error('Valve bootstrap errors', [...errors], 'served', [...files]);
            throw error;
        });
        await page.waitForFunction(() => App.BFinishedInitBeforeLogin(), undefined, {timeout: 5000}).catch(() => {});
        // Valve waits up to ten seconds for each cloud namespace and for FriendsUI.
        if (steamId) await page.waitForFunction(() => App.GetServicesInitialized(), undefined, {timeout: 35000}).catch(() => {});
        else await page.waitForTimeout(1200);
        assert(files.has('/sp.js') && files.has('/index.html'));
        const boot = await page.evaluate(() => {
            let runtime;
            window.webpackChunksteamui.push([['xbox-window-check'], {}, r => { runtime = r; }]);
            const popups = [...runtime(16724).Of.GetPopups()].map(p => ({name: p.GetName(), created: p.m_bCreated,
                waitingForCreate: !!p.m_onCreateRender, pendingStyles: p.m_renderWhenReady?.m_rgLoadingLinks.map(l => l.href)}));
            const gamepad = runtime(5757).oy.WindowStore.GamepadUIMainWindowInstance;
            return {calls: window.fixtureCalls.map(call => call.method || call), finishedBeforeLogin: App.BFinishedInitBeforeLogin(),
                identity: [runtime(30610).iA.steamid, runtime(30610).iA.accountid, runtime(30610).iA.logged_in],
                ownedAppStore: runtime(90544).tw.m_bIsInitialized ? runtime(90544).tw.allApps.map(app => [app.appid, app.display_name]) : [],
                currentUser: App.BHasCurrentUser(), beganAfterLogin: App.m_bStartedInitAfterLogin,
                servicesInitialized: App.GetServicesInitialized(), route: gamepad?.History?.location?.pathname, popups};
        });
        const surfaces = framedGamepad ? page.frames().filter(frame => !frame.parentFrame() || frame.parentFrame() === page.mainFrame()) : context.pages();
        const windows = await Promise.all(surfaces.map(async w => ({
            ...await w.evaluate(() => {
                const target = document.getElementById('popup_target'), box = target?.getBoundingClientRect();
                return {name: window.name, title: document.title, viewport: [innerWidth, innerHeight],
                    sheets: [...document.querySelectorAll('link[rel=stylesheet]')].map(link => [link.href, !!link.sheet]),
                    target: box ? {width: box.width, height: box.height, children: target.childElementCount,
                        text: target.innerText.slice(0, 160), className: target.firstElementChild?.className} : null};
            }), visibleGamepad: await w.evaluate(visibleShell)
        })));
        console.log(JSON.stringify({session: steamId ? 'signed-in fixture' : 'signed-out fixture', boot,
            windows, errors: [...errors], missingFiles: [...missing], blockedOrigins: [...blocked], failures: [...failures]}, null, 2));
        assert(boot.calls.includes('system-mode'));
        assert(boot.calls.includes('session'));
        assert(boot.finishedBeforeLogin);
        assert.equal(boot.beganAfterLogin, !!steamId);
        if (steamId) {
            assert.deepEqual(boot.identity, [steamId, Number(BigInt(steamId) & 0xffffffffn), false]);
            assert(boot.servicesInitialized);
            assert(boot.calls.includes('xbox-app-initialized'));
            if (steamId === emptyAccount) {
                assert.deepEqual(boot.ownedAppStore, []);
                await checkEmptyLibrary(page);
                return;
            }
            assert.deepEqual(boot.ownedAppStore, catalogs[steamId].map(game => [game.id, game.name]));
            await checkPlatform(page);
            const gamepad = windows.findIndex(win => /^(SP BPM)(?:$|_uid\d+$)/.test(win.name));
            const popup = surfaces[gamepad];
            if (framedGamepad) {
                assert.equal(context.pages().length, 1);
                assert(await popup.evaluate(() => opener === parent && parent.origin === origin && !window.chrome?.webview));
            }
            assert(popup, 'Expected Valve to create its Big Picture window');
            if (!framedGamepad) await popup.getByText('Fixture A', {exact: true}).first().waitFor({state: 'visible', timeout: 15000}).catch(async error => {
                const route = await page.evaluate(() => {
                    let r; webpackChunksteamui.push([['startup-route'], {}, require => { r = require; }]);
                    return r(5757).oy.WindowStore.GamepadUIMainWindowInstance?.History.location.pathname;
                });
                throw new Error(error.message + '\nValve route ' + route + '\n' + (await popup.locator('#popup_target').innerText()).slice(0, 600) +
                    '\n' + [...errors].slice(-8).join('\n'));
            });
            await connectStandardPad(page, popup);
            if (nativeGamepads) {
                // Chromium exposes an idle controller. All actual button transitions
                // come through the native Windows.Gaming.Input reply.
                const welcome = popup.getByText('Welcome to Big Picture', {exact: true}).first();
                if (framedGamepad) await welcome.waitFor({state:'visible', timeout:20000}).catch(async err => {
                    throw new Error(err.message + '\nGenuine fresh tour state: ' + JSON.stringify(await page.evaluate(async () => {
                        let r; webpackChunksteamui.push([['tour-observation'],{}, v=>{r=v;}]);
                        return {seen: await r(14255).TT(), session: r(14255).sp.IsGuidedTourSession(),
                            pathname: r(5757).oy.WindowStore.GamepadUIMainWindowInstance?.History?.location?.pathname,
                            prefs: Object.keys(localStorage).filter(k=>k.endsWith('Deck_GuidedTourVersionSeen')).map(k=>localStorage.getItem(k))};
                    })));
                });
                const hasTour = await welcome.isVisible();
                if (hasTour) await completeTour(page, popup);
                if (startupHistory) {
                    await checkStartupHistory(page, popup, async () => { await openOwner(); return page; });
                    return;
                }
                if (installOnly) { await checkInstall(page, popup); return; }
                if (downloadsOnly) {
                    await checkDownloads(page, popup).catch(error => {
                        throw new Error(error.message + '\n' + [...errors].slice(-8).join('\n'));
                    });
                    return;
                }
                if (homeOnly) { await checkHome(page, popup); return; }
                if (storeOnly) { await checkStore(browser, page, popup); return; }
                if (networkOnly) { await checkNetwork(page, popup); return; }
                if (storageOnly) {
                    await checkStorage(page, popup).catch(error => { throw new Error(error.message + '\n' + [...errors].slice(-8).join('\n')); });
                    return;
                }
                if (accounts) {
                    const predicates = await nativeLibraryPredicates();
                    await popup.waitForFunction(predicates.library);
                    assert.equal(await popup.evaluate(predicates.emptyLibrary), false);
                }
                await pressNative(page, 3); // View + Menu opens Valve's own main menu.
                const menu = popup.frameLocator('iframe[name="MainMenu_uid2"]');
                await menu.getByText('Library', {exact: true}).waitFor({state: 'visible', timeout: 7000}).catch(async error => {
                    throw new Error(error.message + '\n' + (await popup.locator('#popup_target').innerText()).slice(0, 450));
                });
                await pressNative(page, 8); // B closes the menu and returns to the library.
                await menu.getByText('Library', {exact: true}).waitFor({state: 'hidden'});
                if (framedGamepad) {
                    await pressNative(page, 3);
                    await menu.getByText('Library', {exact: true}).click();
                    await popup.getByText('All Games', {exact: true}).first().waitFor({state: 'visible'});
                    await openStockLibrary(page, popup);
                    await checkRunningControls(page, popup);
                    const tourKey = 'xbox-steamui:1:' + steamId + ':Deck_GuidedTourVersionSeen';
                    await page.waitForFunction(key => localStorage.getItem(key) === '1', tourKey);
                    console.log('Account A completed the original tour and Steam recorded its preference.');
                    await page.close();
                    await openOwner();
                    const reopened = page.frameLocator('#xbox-steam-gamepad-surface');
                    const tile = stockTile(reopened, 'Fixture A');
                    await tile.waitFor({state:'visible', timeout:60000});
                    await page.waitForFunction(() => App.GetServicesInitialized(), undefined, {timeout:45000});
                    assert.equal(await readSeen(page), true);
                    assert(!await reopened.getByText('Welcome to Big Picture', {exact:true}).first().isVisible());
                    const installed = await checkInstalledTab(reopened, 'Fixture A', 'Fixture B');
                    await installed.click({timeout:10000});
                    await reopened.getByText(/^play$/i).first().waitFor({state:'visible'});
                    assert(!(await reopened.getByText('Welcome to Big Picture', {exact:true}).isVisible()));
                    assert.equal(await readSeen(page),true);
                    console.log('Reopened A, Steam reports tour seen and original game details accept input.');
                    if (accounts) {
                        await page.close();
                        await openOwner(secondAccount);
                        await checkOtherAccount(page);
                        await page.evaluate(() => SteamClient.User.SignOutAndRestart());
                        assert.equal(await page.evaluate(() => fixtureSteamId), null);
                        await page.close();
                        await openOwner(null);
                        await checkSignedOutShell(page);
                    }
                }
                console.log('Native View + Menu opened Steam and B returned to the library. Tour shown:', hasTour);
                return;
            }
            const welcome = popup.getByText(tourPrompts[0], {exact: true}).first();
            await welcome.waitFor({state: 'visible', timeout: 15000});
            await completeTour(page, popup, () => pressPad(page, popup, 0));
            await openStockLibrary(page, popup);
            await checkAppAdapter(page);
            await checkRunningControls(page, popup);
            await checkController(page, popup);
            await checkSettingsAdapter(page); // intentional fixture choices after observing the untouched initial route
            await checkLocalCloudCache(page);
        }
    } finally { await context.close(); }
}

(async () => {
    await fs.access(path.join(assets, 'sp.js'));
    const source = execFileSync('python3', [bundle, catalog], {encoding: 'utf8'});
    const browser = await chromium.launch({headless: true, chromiumSandbox: true,
        executablePath: process.env.CHROMIUM_PATH, args: ['--host-resolver-rules=MAP * ~NOTFOUND', '--disable-background-networking']});
    try {
        if (emptyOnly) await verifyStartup(browser, source, emptyAccount, true);
        else if (framedGamepad || process.argv.includes('--native-startup')) await verifyStartup(browser, source, fixtureAccount, true);
        else {
            await verifyStartup(browser, source, null);
            await verifyStartup(browser, source, fixtureAccount);
        }
    } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
