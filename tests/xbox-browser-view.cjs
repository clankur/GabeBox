// Real DOM integration. Use the same NODE_PATH and CHROMIUM_PATH as steam-big-picture-shell.cjs.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const test = require('node:test');
const {chromium} = require('playwright');

test('the local Valve main menu owns a bounded, revocable DOM surface inside its gamepad popup', async t => {
    const browser = await chromium.launch({executablePath: process.env.CHROMIUM_PATH, chromiumSandbox: true,
        args: ['--disable-background-networking', '--host-resolver-rules=MAP * ~NOTFOUND']});
    t.after(() => browser.close());
    const context = await browser.newContext({offline: true});
    await context.route('https://steamloopback.host/index.html?IN_CLIENT=true', route => route.fulfill({
        contentType: 'text/html', body: '<!doctype html><title>Steam shared context</title>'
    }));
    const page = await context.newPage();
    await page.goto('https://steamloopback.host/index.html?IN_CLIENT=true');
    await page.evaluate(() => {
        window.xboxGamepadPopup = window.open('about:blank', 'SP BPM');
        xboxGamepadPopup.SteamClient = {Browser: {GetBrowserID: () => 2}};
        xboxGamepadPopup.document.write('<!doctype html><body><button id="home">Home</button></body>');
        xboxGamepadPopup.document.close();
        window.steamFixture = {};
        window.open = () => 'delegated';
    });
    await page.addScriptTag({content: fs.readFileSync(require.resolve('../probe/big-picture-bridge/xbox-browser-view.js'), 'utf8')});
    const result = await page.evaluate(() => {
        installXboxBrowserView(steamFixture);
        const api = steamFixture.BrowserView;
        const owner = xboxGamepadPopup;
        const options = {parentPopupBrowserID: 2, strName: 'MainMenu_uid2', strVROverlayKey: undefined};
        const rejected = obj => { try { api.CreatePopup({...options, ...obj}); return false; } catch { return true; } };
        const blocked = [
            rejected({parentPopupBrowserID: 1}), rejected({strName: 'Store'}),
            rejected({strInitialURL: 'https://example.org'}), rejected({strVROverlayKey: 'overlay'})
        ];
        const {strCreateURL, browserView: view} = api.CreatePopup(options);
        const quickName = 'QuickAccess_uid2';
        const quick = api.CreatePopup({...options, strName: quickName});
        const toastName = 'notificationtoasts_uid2';
        const toast = api.CreatePopup({...options, strName: toastName});
        const droppedFlags = window.open(toast.strCreateURL, toastName);
        const toastFrame = window.open(toast.strCreateURL + '&createflags=512', toastName).frameElement;
        toast.browserView.SetBounds(0, 0, 320, 90);
        toast.browserView.SetVisible(true);
        owner.document.getElementById('home').focus();
        toast.browserView.SetFocus(true);
        const toastPassive = toastFrame.tabIndex === -1 && owner.document.activeElement.id === 'home';
        const crossed = window.open(quick.strCreateURL, options.strName);
        const quickFrame = window.open(quick.strCreateURL, quickName).frameElement;
        quick.browserView.SetBounds(900, 0, 380, 720);
        blocked.push(rejected({}));
        const forbidden = window.open('https://example.org/', options.strName);
        const wrong = window.open(strCreateURL, 'Wrong');
        const child = window.open(strCreateURL, options.strName);
        // These are the synchronous operations in the pinned Valve module 5689.
        child.document.write('<!doctype html><html><head></head><body><div id="browserview_target"></div><script>window.menuScriptExecuted=true</script></body></html>');
        child.document.close();
        const target = child.document.getElementById('browserview_target'), button = child.document.createElement('button');
        target.append(button);
        const frame = child.frameElement;
        view.SetBounds(10.5,20,420,320);
        view.SetWindowStackingOrder(1);
        view.SetVisible(true);
        view.SetTouchGesturesToCancel([9,10]);
        owner.document.getElementById('home').focus();
        view.SetFocus(true);
        const focus = owner.document.activeElement === frame;
        view.SetVisible(false);
        const restored = owner.document.activeElement.id === 'home';
        const listeners = [];
        const full = value => listeners.push(value);
        view.on('full-screen',full);
        child.document.dispatchEvent(new Event('fullscreenchange'));
        view.off('full-screen',full);
        child.document.dispatchEvent(new Event('fullscreenchange'));
        const duplicate = window.open(strCreateURL, options.strName);
        const geometry = [frame.style.left,frame.style.top,frame.style.width,frame.style.height];
        const binding = [child.parent === owner, target.ownerDocument.defaultView === child,
            child.location.href, frame.getAttribute('sandbox'), child.SteamClient === undefined, child.menuScriptExecuted === undefined, frame.style.display];
        api.Destroy(view);
        const otherStillLive = quickFrame.isConnected && quickFrame.style.left === '900px';
        const second = api.CreatePopup(options);
        const stale = window.open(strCreateURL, options.strName);
        const successor = window.open(second.strCreateURL, options.strName);
        api.Destroy(view);
        const distinct = successor.frameElement.isConnected;
        let closed = 0;
        second.browserView.on('before-close', () => { closed++; });
        owner.dispatchEvent(new Event('pagehide'));
        return {blocked, forbidden, wrong, duplicate, stale, crossed, droppedFlags, toastPassive, otherStillLive, geometry, binding, focus, restored, distinct,
            listeners, closed, frameCount: owner.document.querySelectorAll('iframe').length,
            delegated: window.open('about:blank','Other')};
    });
    assert.deepEqual(result.blocked, [true,true,true,true,true]);
    assert.deepEqual([result.forbidden,result.wrong,result.duplicate,result.stale], [null,null,null,null]);
    assert.equal(result.crossed, null);
    assert.equal(result.droppedFlags, null);
    assert.equal(result.toastPassive, true);
    assert.equal(result.otherStillLive, true);
    assert.deepEqual(result.geometry, ['10.5px','20px','420px','320px']);
    assert.deepEqual(result.binding, [true,true,'https://steamloopback.host/index.html?IN_CLIENT=true','allow-same-origin',true,true,'none']);
    assert.equal(result.focus,true);
    assert.equal(result.restored,true);
    assert.equal(result.distinct,true);
    assert.deepEqual(result.listeners,[false]);
    assert.equal(result.closed,1);
    assert.equal(result.frameCount,0);
    assert.equal(result.delegated,'delegated');
});
