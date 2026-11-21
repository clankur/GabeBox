// node tests/steam-big-picture-shell.cjs <staged ValveClient/steamui> [public/url_list.txt] [--require-gamepad]
// Set NODE_PATH for playwright and CHROMIUM_PATH for an existing Chromium binary.
const assert = require('node:assert/strict');
const {execFileSync} = require('node:child_process');
const fs = require('node:fs/promises');
const path = require('node:path');
const {chromium} = require('playwright');

const origin = 'https://steamloopback.host';
const entry = origin + '/index.html?IN_CLIENT=true';
const args = process.argv.slice(2).filter(arg => !['--require-gamepad', '--native-startup'].includes(arg));
const requireGamepad = process.argv.includes('--require-gamepad');
const assets = path.resolve(args[0] || 'ValveClient/steamui');
const catalog = path.resolve(args[1] || path.join(assets, '../public/url_list.txt'));
const bundle = path.resolve(__dirname, '../probe/big-picture-bridge/bundle.py');
const contentTypes = {'.css': 'text/css', '.js': 'application/javascript', '.html': 'text/html',
    '.json': 'application/json', '.webm': 'video/webm', '.svg': 'image/svg+xml', '.png': 'image/png',
    '.jpg': 'image/jpeg', '.woff2': 'font/woff2', '.woff': 'font/woff', '.ttf': 'font/ttf'};
const fixtureAccount = '76561198000000000';

// This fixture stands in for the outer, authenticated Xbox host. The production
// bundle installs SteamClient inside Valve's unmodified document.
function setupHost(steamId) {
    const events = new EventTarget();
    const games = [{id: 42, name: 'Fixture A', installed: true, canPlay: true},
        {id: 73, name: 'Fixture B', installed: false, canPlay: false}];
    window.fixtureCalls = [];
    window.fixtureNativePad = {slot: 0, bits: 0, lt: 0, rt: 0, lx: 0, ly: 0, rx: 0, ry: 0};
    window.fixtureSteamId = sessionStorage.getItem('fixture-next-account') || steamId;
    window.fixtureReads = new Map();
    window.hostEvent = data => events.dispatchEvent(new MessageEvent('message', {data}));
    (window.chrome ||= {}).webview = {
        addEventListener: events.addEventListener.bind(events),
        postMessage(request) {
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
            switch (request.method) {
                case 'library': reply = {result: games}; break;
                case 'session': reply = {result: {steamId: window.fixtureSteamId}}; break;
                case 'restart-client': reply = {result: window.fixtureSteamId ? 'restart' : 'signed-out'}; break;
                case 'system-mode': reply = {result: false}; break;
                case 'play': reply = request.appId === 42 ? {result: 'presented'} : {error: 'not-installed'}; break;
                case 'resume-game': reply = request.appId === 42 ? {result: 'presented'} : {error: 'unavailable'}; break;
                default: reply = {error: 'unavailable'};
            }
            queueMicrotask(() => window.hostEvent({id: request.id, ...reply}));
        }
    };
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
    await page.evaluate(() => hostEvent({event: 'xbox-game', appId: 42, overlay: true}));
    const resume = popup.getByText('Resume game', {exact: true});
    await resume.waitFor({state: 'visible'});
    assert(await popup.getByText('Exit game', {exact: true}).isVisible());
    const before = await page.evaluate(() => fixtureCalls.filter(c => c.method === 'resume-game').length);
    await resume.evaluate(button => button.click());
    await page.waitForFunction(count => fixtureCalls.filter(c => c.method === 'resume-game' && c.appId === 42).length > count, before);
    await page.evaluate(() => hostEvent({event: 'xbox-game', appId: 0, overlay: false}));
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
    const title = popup.getByText('Fixture A', {exact: true}).first();
    await title.waitFor({state: 'visible'});
    assert(await popup.getByText('Fixture B', {exact: true}).first().isVisible());
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
            session.on('Fetch.requestPaused', async ({requestId, request}) => {
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
        const state = {nativeGamepads, deviceFamily: 'Windows.Xbox', deviceFamilyVersion: String((10n << 48n) | (26100n << 16n) | 2n),
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
        async function openOwner() {
            openingOwner = true;
            try { page = await context.newPage(); } finally { openingOwner = false; }
            await serve(page);
            observe(page);
            page.setDefaultTimeout(10000);
            await page.addInitScript({content: 'const xboxNativeWindow=Object.freeze(' + JSON.stringify(state) + ');\n' +
                '(' + setupHost.toString() + ')(' + JSON.stringify(steamId) + ');\n' + source});
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
        const windows = await Promise.all(context.pages().map(async w => ({
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
            assert.deepEqual(boot.ownedAppStore, [[42, 'Fixture A'], [73, 'Fixture B']]);
            await checkPlatform(page);
            const gamepad = windows.findIndex(win => /^(SP BPM)(?:$|_uid\d+$)/.test(win.name));
            let popup = gamepad < 0 ? undefined : context.pages()[gamepad];
            assert(popup, 'Expected Valve to create its Big Picture window');
            await popup.getByText('Fixture A', {exact: true}).first().waitFor({state: 'visible', timeout: 15000}).catch(async error => {
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
                async function pressNative(bits) {
                    await page.evaluate(value => { fixtureNativePad.bits = value; }, bits);
                    await popup.waitForTimeout(200);
                    await page.evaluate(() => { fixtureNativePad.bits = 0; });
                    await popup.waitForTimeout(350);
                }
                const tour = ['Welcome to Big Picture', 'Steam Menu', 'Quick Access Menu', 'Set up Parental Controls', 'Have fun'];
                const welcome = popup.getByText('Welcome to Big Picture', {exact: true}).first();
                const hasTour = await welcome.isVisible();
                for (const step of hasTour ? tour : []) {
                    const heading = popup.getByText(new RegExp('^' + step + '[!.]?$', 'i')).first();
                    await heading.waitFor({state: 'visible', timeout: 4000}).catch(async error => { throw new Error(error.message + '\n' + (await popup.locator('#popup_target').innerText()).slice(0, 700)); });
                    await pressNative(4);
                    await heading.waitFor({state: 'hidden', timeout: 4000});
                }
                await pressNative(3); // View + Menu opens Valve's own main menu.
                const menu = popup.frameLocator('iframe[name="MainMenu_uid2"]');
                await menu.getByText('Library', {exact: true}).waitFor({state: 'visible', timeout: 7000}).catch(async error => {
                    throw new Error(error.message + '\n' + (await popup.locator('#popup_target').innerText()).slice(0, 450));
                });
                await pressNative(8); // B closes the menu and returns to the library.
                await menu.getByText('Library', {exact: true}).waitFor({state: 'hidden'});
                console.log('Native View + Menu opened Steam and B returned to the library. Tour shown:', hasTour);
                return;
            }
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
        if (process.argv.includes('--native-startup')) await verifyStartup(browser, source, fixtureAccount, true);
        else {
            await verifyStartup(browser, source, null);
            await verifyStartup(browser, source, fixtureAccount);
        }
    } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
