// Valve release 1794536281, sp.js modules 5689 and 91780. The shared context
// writes browserview_target and mounts its own React portal into this document.
function installXboxBrowserView(client) {
    const open = window.open.bind(window);
    let serial = 0;
    const records = new Map();
    const prefix = 'about:blank?xbox-local-menu=';
    const menus = new Map([['MainMenu_uid2', 'Steam Main Menu'], ['QuickAccess_uid2', 'Steam Quick Access']]);
    function typeOf(name) {
        if (menus.has(name)) return {title: menus.get(name), flags: ''};
        // The native manager appends an integer while a previous toast name
        // awaits release. This specific Steam overlay requests NotFocusable (512).
        if (/^notificationtoasts_uid2(?:_\d{1,3})?$/.test(name))
            return {title: 'Steam Notifications', flags: '&createflags=512'};
    }

    function ownerWindow() {
        const popup = xboxGamepadPopup;
        if (!popup || popup.closed || popup.opener !== window) return;
        // Valve's document.write() can assign the shared context's URL to the
        // approved native WindowProxy without issuing an actual navigation.
        const url = popup.location.href;
        const entry = 'https://steamloopback.host/index.html';
        if ((url.split('?')[0] === 'about:blank' || url === entry || url === entry + '?IN_CLIENT=true') &&
            popup.SteamClient?.Browser.GetBrowserID() === 2) return popup;
    }

    function destroy(record) {
        if (records.get(record.name) !== record) return;
        records.delete(record.name);
        record.owner.removeEventListener('pagehide', record.exit);
        record.restoreFocus();
        record.frame?.contentDocument?.removeEventListener('fullscreenchange', record.fullscreen);
        record.frame?.remove();
        record.listeners.clear();
    }

    function reserve(options) {
        const owner = ownerWindow();
        const type = typeOf(options?.strName);
        if (!owner || options?.parentPopupBrowserID !== 2 || !type ||
            options.strVROverlayKey != null || Object.keys(options).some(key =>
                !['parentPopupBrowserID', 'strName', 'strVROverlayKey'].includes(key))) {
            throw new Error('Unsupported Xbox browser view');
        }
        for (const stale of records.values()) if (stale.owner !== owner) destroy(stale);
        if (records.has(options.strName) || records.size >= 6) throw new Error('Xbox local browser view unavailable');
        const record = {owner, name: options.strName, type, listeners: new Map(), url: prefix + (++serial)};
        record.restoreFocus = () => {
            if (!record.frame || owner.document.activeElement !== record.frame) return;
            if (record.previousFocus?.isConnected) record.previousFocus.focus();
            else record.frame.blur();
        };
        record.fullscreen = () => {
            const fullscreen = !!record.frame?.contentDocument?.fullscreenElement;
            for (const callback of record.listeners.get('full-screen') || []) callback(fullscreen);
        };
        record.exit = () => {
            try {
                for (const callback of record.listeners.get('before-close') || []) callback();
            } finally { destroy(record); }
        };
        owner.addEventListener('pagehide', record.exit, {once: true});
        function element() {
            if (records.get(record.name) !== record || !record.frame?.isConnected) throw new Error('Xbox menu view is closed');
            return record.frame;
        }
        const view = {
            SetBounds(x, y, width, height) {
                if (![x,y,width,height].every(Number.isFinite) || width < 0 || height < 0)
                    throw new TypeError('Invalid Xbox menu bounds');
                Object.assign(element().style, {left: x + 'px', top: y + 'px', width: width + 'px', height: height + 'px'});
            },
            SetVisible(visible) {
                const frame = element();
                if (typeof visible !== 'boolean') throw new TypeError('Invalid menu visibility');
                if (!visible) record.restoreFocus();
                frame.style.display = visible ? 'block' : 'none';
            },
            SetFocus(focus) {
                const frame = element();
                if (typeof focus !== 'boolean') throw new TypeError('Invalid menu focus');
                if (focus && !type.flags && frame.style.display !== 'none') {
                    if (owner.document.activeElement !== frame) record.previousFocus = owner.document.activeElement;
                    frame.contentWindow.focus();
                } else if (!focus) record.restoreFocus();
            },
            SetWindowStackingOrder(order) {
                if (order !== 1 && order !== 2) throw new TypeError('Invalid menu stacking order');
                element().style.zIndex = order === 1 ? '2147483646' : '-1';
            },
            SetTouchGesturesToCancel(gestures) {
                // The gamepad menu asks to suppress browser swipe navigation.
                if (!Array.isArray(gestures) || gestures.length !== 2 || gestures[0] !== 9 || gestures[1] !== 10)
                    throw new Error('Unsupported menu gestures');
                element().contentDocument.documentElement.style.overscrollBehavior = 'none';
            },
            on(event, callback) {
                if (!['full-screen', 'before-close', 'message'].includes(event) || typeof callback !== 'function')
                    throw new TypeError('Unsupported menu listener');
                if (!record.listeners.has(event)) record.listeners.set(event, new Set());
                record.listeners.get(event).add(callback);
                if (event === 'full-screen') element().contentDocument.addEventListener('fullscreenchange', record.fullscreen);
                // The menu has no native message sender. Never relay arbitrary
                // window postMessage events into Valve's BrowserView message API.
            },
            off(event, callback) {
                const listeners = record.listeners.get(event);
                listeners?.delete(callback);
                if (event === 'full-screen' && !listeners?.size)
                    record.frame?.contentDocument?.removeEventListener('fullscreenchange', record.fullscreen);
            }
        };
        record.view = view;
        records.set(record.name, record);
        return {strCreateURL: record.url, browserView: view};
    }

    window.open = function (url, name, ...args) {
        if (!typeOf(name) && !(typeof url === 'string' && url.startsWith(prefix))) return open(url, name, ...args);
        const record = records.get(name);
        if (!record || url !== record.url + record.type.flags || record.frame || record.owner !== ownerWindow()) return null;
        const frame = record.owner.document.createElement('iframe');
        frame.setAttribute('sandbox', 'allow-same-origin');
        frame.setAttribute('csp', "default-src 'none'; style-src 'unsafe-inline' https://steamloopback.host; " +
            "img-src data: https://steamloopback.host; font-src https://steamloopback.host; form-action 'none'; base-uri 'none'");
        frame.setAttribute('title', record.type.title);
        frame.name = name;
        if (record.type.flags) frame.tabIndex = -1;
        Object.assign(frame.style, {position: 'fixed', left: '0', top: '0', width: '0', height: '0', border: '0',
            display: 'none', background: 'transparent', zIndex: '2147483646'});
        record.frame = frame;
        // No src or srcdoc. The implicit about:blank document inherits the
        // gamepad origin. Native FrameNavigationStarting blocks navigation.
        record.owner.document.body.appendChild(frame);
        return frame.contentWindow;
    };
    client.BrowserView = {
        CreatePopup: reserve,
        Destroy(view) {
            for (const record of records.values()) if (record.view === view) destroy(record);
        }
    };
}
