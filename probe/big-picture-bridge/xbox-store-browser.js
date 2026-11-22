// Only Valve's MainBrowser in its owned gamepad popup may create the isolated public Store.
function installXboxStoreBrowser(client, host, ownerWindow) {
    const api = client.BrowserView, destroyPopup = api.Destroy;
    const initialPage = 'data:text/html,%3Cbody%3E%3C%2Fbody%3E';
    const options = ['strName', 'parentPopupBrowserID', 'strInitialURL', 'strUserAgentIdentifier', 'bPreventCloseFromJavascript', 'strVROverlayKey'];
    const keys = new Map([[0, 'enter'], [20, 'up'], [21, 'down'], [22, 'left'], [23, 'right']]);
    let serial = 0, current;

    function publicURL(value) {
        if (typeof value !== 'string' || value.length > 2048) return;
        try {
            const url = new URL(value);
            if (url.origin === 'https://store.steampowered.com' && !url.username && !url.password) return url.href;
        } catch {}
    }
    function post(record, action, fields = {}) {
        if (current === record) host.postMessage({method: 'store-browser', action, id: record.id, ...fields});
    }
    function emit(record, event, ...args) {
        for (const callback of [...(record.events.get(event) || [])]) callback(...args);
    }
    function release(record) {
        for (const [key, holders] of record.held) if (holders.size) post(record, 'input', {key, down: false});
        record.held.clear();
    }
    function close(record, requested) {
        release(record);
        if (requested) post(record, 'close');
        current = undefined;
        record.owner.removeEventListener('pagehide', record.unload);
        emit(record, 'before-close');
        record.events.clear();
    }
    function committed(record, url) {
        const {entries} = record;
        if (record.step && entries[record.index + record.step] === url) record.index += record.step;
        else if (entries[record.index] !== url) {
            entries.splice(record.index + 1, Infinity, url);
            if (entries.length > 128) entries.shift();
            record.index = entries.length - 1;
        }
        record.step = 0;
        // The stock router compares the internal blank entry with its own constant.
        emit(record, 'history-changed', {index: record.index, entries: entries.map(value =>
            ({url: value === 'about:blank' ? initialPage : value}))});
    }
    host.addEventListener('message', ({data}) => {
        const record = current;
        if (!record || data?.event !== 'store-browser' || data.id !== record.id) return;
        const url = data.url === 'about:blank' ? data.url : publicURL(data.url);
        switch (data.kind) {
        case 'start':
            if (!url) return;
            record.requested = url;
            record.title = '';
            emit(record, 'start-request', url);
            emit(record, 'start-loading', url, true);
            break;
        case 'complete':
            if (!url || typeof data.success !== 'boolean') return;
            if (data.success) {
                emit(record, 'finished-request', url, record.title);
                committed(record, url);
            } else {
                record.step = 0;
                emit(record, 'load-error', Number.isInteger(data.code) ? data.code : -1, url, 'Store navigation failed');
            }
            break;
        case 'history':
            if (typeof data.canGoBack !== 'boolean' || typeof data.canGoForward !== 'boolean') return;
            record.back = data.canGoBack; record.forward = data.canGoForward;
            emit(record, 'can-go-back-forward-changed', record.back, record.forward);
            break;
        case 'title':
            if (typeof data.title === 'string') {
                record.title = data.title.slice(0, 512);
                emit(record, 'set-title', record.title);
            }
            break;
        case 'focus':
            if (typeof data.value === 'boolean') emit(record, 'focus-changed', data.value);
            break;
        case 'error':
            emit(record, 'load-error', Number.isInteger(data.code) ? data.code : -1,
                record.requested || 'about:blank', 'Store browser unavailable');
            break;
        case 'closed': close(record, false); break;
        }
    });
    api.Create = function (spec) {
        const owner = ownerWindow();
        if (!owner || spec?.strName !== 'MainBrowser' || spec.parentPopupBrowserID !== 2 ||
            spec.strInitialURL !== initialPage || spec.strUserAgentIdentifier !== 'Valve Steam Gamepad' ||
            spec.bPreventCloseFromJavascript !== true || spec.strVROverlayKey != null || Object.keys(spec).some(key => !options.includes(key)))
            throw new Error('Unsupported Xbox Store browser');
        if (current) throw new Error('Xbox Store browser already open');
        const record = {id: ++serial, owner, events: new Map(), entries: [], index: -1, step: 0,
            requested: '', title: '', visible: false, focus: false, held: new Map()};
        const view = {
            on(event, callback) {
                if (current !== record || typeof callback !== 'function') return;
                if (!record.events.has(event)) record.events.set(event, new Set());
                record.events.get(event).add(callback);
            },
            off(event, callback) { record.events.get(event)?.delete(callback); },
            LoadURL(value) {
                const url = publicURL(value);
                if (!url) {
                    emit(record, 'load-error', -1, '', 'Only the public Steam Store is available');
                    return;
                }
                post(record, 'navigate', {url});
            },
            SetBounds(x, y, width, height) {
                const w = owner.innerWidth, h = owner.innerHeight;
                if (!(w > 0 && h > 0) || ![x, y, width, height].every(Number.isFinite)) return;
                const clamp = value => Math.max(0, Math.min(1, value));
                const left = clamp(x / w), top = clamp(y / h);
                post(record, 'bounds', {x: left, y: top, width: Math.max(0, clamp((x + width) / w) - left),
                    height: Math.max(0, clamp((y + height) / h) - top)});
            },
            SetFocus(value) {
                if (typeof value !== 'boolean') return;
                if (!value) release(record);
                record.focus = value;
                post(record, 'focus', {value});
            },
            SetVisible(value) {
                if (typeof value !== 'boolean') return;
                if (!value) release(record);
                record.visible = value;
                post(record, 'visible', {value: value && !record.glass});
            },
            AddGlass(enabled, style) {
                if (typeof enabled !== 'boolean' || style !== 'darkblur') return;
                // Steam drew a local modal over this web panel. Leave its own
                // modal visible above the independent Xbox composition surface.
                if (enabled) release(record);
                record.glass = enabled;
                post(record, 'visible', {value: record.visible && !enabled});
            },
            // Composition has one Store child. Valve explicitly hides it for modal UI.
            SetWindowStackingOrder() {},
            GoBack() { if (record.back) { record.step = -1; post(record, 'back'); } },
            GoForward() { if (record.forward) { record.step = 1; post(record, 'forward'); } },
            Reload() { post(record, 'reload'); }
        };
        current = record;
        record.view = view;
        record.unload = () => { if (current === record) close(record, true); };
        owner.addEventListener('pagehide', record.unload, {once: true});
        try { post(record, 'create'); } catch (error) { close(record, false); throw error; }
        return view;
    };
    api.Destroy = view => {
        if (current && current.view === view) close(current, true);
        else destroyPopup(view);
    };
    return {
        setActionset(enabled) {
            if (!current || typeof enabled !== 'boolean') return;
            if (!enabled) release(current);
            current.actionset = enabled;
        },
        button(index, code, down) {
            const record = current, key = keys.get(code);
            // B remains with Valve's browser/router backstack, as do Steam/Menu and overlay buttons.
            if (!key || !record?.actionset || !record.visible || record.glass || !record.focus) return false;
            if (!record.held.has(key)) record.held.set(key, new Set());
            const holders = record.held.get(key), wasDown = holders.size > 0;
            if (down) holders.add(index); else holders.delete(index);
            if (wasDown !== (holders.size > 0)) post(record, 'input', {key, down});
            return true;
        }
    };
}
