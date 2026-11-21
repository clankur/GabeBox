// Scoped to the signed shared-context bridge by the packager. The native host
// injects xboxNativeWindow before the Valve scripts and owns both WebViews.
let xboxGamepadPopup;
let xboxGamepadVisible = false;
function getXboxGamepadWindow() {
    if (xboxGamepadVisible && xboxGamepadPopup && !xboxGamepadPopup.closed && !xboxGamepadPopup.document.hidden) return xboxGamepadPopup;
    return window;
}

function installXboxWindow(client, send, lookupPopup) {
    if (typeof xboxNativeWindow === 'undefined') return; // Only the WebView host can supply these bindings.
    const host = window.chrome.webview;
    const desired = new Set(), display = new Set();
    const focus = new Set(), overlayFocus = new Set(), mode = new Set();
    let state = xboxNativeWindow;
    let notifiedPopup;

    function subscribe(callbacks, fn, initial) {
        if (typeof fn !== 'function') throw new TypeError('Expected a window callback');
        callbacks.add(fn);
        if (initial) queueMicrotask(() => { if (callbacks.has(fn)) fn(); });
        return {unregister() { callbacks.delete(fn); }};
    }

    function focused() {
        return state.visible && (typeof state.focused === 'boolean' ? state.focused : window.document.hasFocus());
    }

    function focusInfo() {
        // This WebView owns Steam's shell. Game processes run in a separate
        // presenter and are not enumerable as game windows from this CoreWindow.
        return {focusedApp: focused() ? {appid: 0, pid: state.pid, windowid: state.viewId, strExeName: 'msedgewebview2.exe'} :
            {appid: 0, pid: 0, windowid: 0, strExeName: ''}, rgFocusable: []};
    }

    function notifyPopup() {
        const popup = xboxGamepadPopup;
        if (!state.popupOpen || !popup || popup.closed || popup === notifiedPopup ||
            !popup.document.getElementById('popup_target')) return;
        // The pinned window manager defers its first React render until it
        // receives this CEF lifecycle message. WebView2 has now attached the
        // approved composition controller and Valve has written its portal.
        const registered = lookupPopup?.(popup);
        if (lookupPopup && !registered) return;
        if (!registered?.m_bCreated) popup.postMessage('popup-created', location.origin);
        if (!lookupPopup || registered?.m_bCreated) notifiedPopup = popup;
    }

    host.addEventListener('message', ({data}) => {
        if (data?.event !== 'xbox-window' || data.state?.viewId !== state.viewId || data.state.pid !== state.pid) return;
        const previous = state;
        state = data.state;
        xboxGamepadVisible = state.visible && state.popupVisible;
        if (previous.focused !== state.focused || previous.visible !== state.visible || previous.popupVisible !== state.popupVisible) {
            for (const fn of focus) fn();
            // Returning to the actual Steam surface clears overlay-game focus.
            if (focused()) for (const fn of overlayFocus) fn();
        }
        if (!state.popupOpen) notifiedPopup = undefined;
        if (previous.screenWidth !== state.screenWidth || previous.screenHeight !== state.screenHeight || previous.scale !== state.scale) {
            for (const fn of display) fn();
            window.postMessage('display_changed', location.origin);
        }
        notifyPopup();
    });

    const system = (client.System ||= {});
    const systemUI = (system.UI ||= {});
    (system.Display ||= {}).EnableUnderscan = enabled => {
        if (typeof enabled !== 'boolean') throw new TypeError('Expected a display mode');
        host.postMessage(enabled ? 'xbox-display-safe-bounds' : 'xbox-display-full-bounds');
    };
    function subscribeValue(callbacks, callback, deliver, initial) {
        if (typeof callback !== 'function') throw new TypeError('Expected a window callback');
        return subscribe(callbacks, () => deliver(callback), initial);
    }
    systemUI.RegisterForFocusChangeEvents = callback => subscribeValue(focus, callback, fn => fn(focusInfo()), true);
    // Hosted games render into one Xbox application view. The focus event above
    // advertises no individually selectable desktop game windows.
    systemUI.GetGameWindowsInfo = async (appId, ids) => {
        if (!Number.isInteger(appId) || !Array.isArray(ids)) throw new TypeError('Expected an AppID and window IDs');
        return [];
    };
    systemUI.RegisterForOverlayGameWindowFocusChanged = callback =>
        subscribeValue(overlayFocus, callback, fn => { if (focused()) fn(0, 0); }, true);

    async function gameWindowAction(gameId, method, expected) {
        const id = Number(gameId);
        if (!Number.isInteger(id) || id <= 0 || id > 0xffffff || String(id) !== String(gameId))
            throw new Error('Invalid game ID');
        if (await send(method, id) !== expected) throw new Error('The selected game window is unavailable');
    }
    const apps = client.Apps ||= {};
    apps.RaiseWindowForGame = gameId => gameWindowAction(gameId, 'resume-game', 'presented');
    apps.TerminateApp = (gameId, force) => {
        if (force !== false) throw new Error('Only a graceful game exit is supported');
        return gameWindowAction(gameId, 'quit-game', 'quit-requested');
    };

    const ui = client.UI ||= {};
    // The single Xbox window requests the pinned client's gamepad UI mode (4)
    // for its entire lifetime, including while a game owns the foreground.
    ui.GetUIMode = async () => 4;
    ui.NotifyAppInitialized = () => host.postMessage('xbox-app-initialized');
    ui.RegisterForUIModeChanged = callback => subscribeValue(mode, callback, fn => fn(4), true);
    ui.GetDesiredSteamUIWindows = async () => [{
        unID: state.viewId, unPID: state.pid, nBrowserID: state.ownerId, appid: 0, windowType: 0 // MainGamepadUI
    }];
    ui.RegisterDesiredSteamUIWindowsChanged = fn => subscribe(desired, fn, true);
    ui.RegisterMoveGamepadUIMainWindowToPrimaryDisplay = fn => subscribe(display, fn, false);
    ui.EnsureMainWindowCreated = () => {
        if (state.popupOpen) host.postMessage('xbox-window-show');
        else for (const fn of desired) fn();
    };

    function bindings(target, popup) {
        let lastFocused;
        function command(name) {
            if (target.closed || (popup && target !== xboxGamepadPopup)) return;
            host.postMessage(name);
        }
        const Window = {
            GetWindowDimensions: async () => ({x: state.x, y: state.y, width: state.width, height: state.height}),
            GetDefaultMonitorDimensions: async () => ({
                flHorizontalScale: state.scale, flVerticalScale: state.scale,
                nFullLeft: 0, nFullTop: 0, nFullWidth: state.screenWidth, nFullHeight: state.screenHeight,
                nUsableLeft: Math.round(state.usableX * state.scale), nUsableTop: Math.round(state.usableY * state.scale),
                nUsableWidth: Math.round(state.usableWidth * state.scale), nUsableHeight: Math.round(state.usableHeight * state.scale)
            }),
            BringToFront() { command(popup ? 'xbox-window-show' : 'xbox-window-focus'); },
            SetKeyFocus(focus) {
                if (focus) {
                    command('xbox-window-focus');
                    if (lastFocused?.isConnected) lastFocused.focus({preventScroll: true});
                }
                else if (target.document.hasFocus()) target.document.activeElement?.blur();
            },
            MarkLastFocused() {
                if (target.document.hasFocus()) lastFocused = target.document.activeElement;
            },
            GetWindowDetails: async () => ({bUnderlaySupported: false}),
            IsWindowMinimized: async () => !state.visible || (popup && !state.popupVisible)
        };
        if (popup) Object.assign(Window, {
            SetComposition(mode, appQueue, targetWindow) {
                // Valve's opaque and hidden modes map to the actual Xbox child
                // controller. Other modes require blending with a game window.
                if (!Array.isArray(appQueue) || !Number.isSafeInteger(targetWindow) || targetWindow !== 0 ||
                    (mode !== 0 && mode !== 3)) throw new Error('Xbox cannot composite another game window');
                command(mode === 3 ? 'xbox-window-show' : 'xbox-window-hide');
            },
            ShowWindow() { command('xbox-window-show'); },
            HideWindow() { command('xbox-window-hide'); },
            Close() { target.close(); },
            MoveTo(x, y) {
                if (x !== 0 || y !== 0) throw new Error('Xbox gamepad view occupies the primary display');
                command('xbox-window-primary-display');
            }
        });
        return {Window, Browser: {
            GetBrowserID: () => popup ? state.popupId : state.ownerId,
            // Move key focus within the active CoreWindow. Chromium retains
            // control of the actual transient user-activation permission.
            NotifyUserActivation() { command('xbox-window-focus'); }
        }};
    }
    const owner = bindings(window, false);
    Object.assign(client.Window ||= {}, owner.Window);
    Object.assign(client.Browser ||= {}, owner.Browser);

    const open = window.open.bind(window);
    window.open = function (url, name, ...rest) {
        const popup = open(url, name, ...rest);
        if (!popup || typeof url !== 'string' || !url.startsWith('about:blank?createflags=') || url.length > 2048 ||
            (name !== 'SP BPM' && name !== 'SP BPM_uid' + state.pid) || popup === window) return popup;
        try {
            if (popup.opener !== window || popup.location.href.split('?')[0] !== 'about:blank') return popup;
            if (popup === xboxGamepadPopup && popup.SteamClient) return popup;
            xboxGamepadPopup = popup;
            popup.SteamClient = bindings(popup, true);
            // Valve writes the target and attaches its listener immediately
            // after window.open returns. Native publication can race that write.
            const deadline = Date.now() + 110000;
            function awaitCreation() {
                if (popup !== xboxGamepadPopup || popup.closed || popup === notifiedPopup) return;
                notifyPopup();
                if (popup !== notifiedPopup && Date.now() < deadline) setTimeout(awaitCreation, 100);
            }
            setTimeout(awaitCreation, 0);
        } catch (_) {
            // Browser-enforced origin boundaries also apply to named targets.
        }
        return popup;
    };
}
