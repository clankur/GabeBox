// Adapter for the signed, pinned Steam shared context. The packaged host owns
// authentication, ownership checks, launch routing, and the game process.
(() => {
    'use strict';
    if (window !== window.top || location.href.split('#')[0] !== 'https://steamloopback.host/index.html?IN_CLIENT=true' ||
        'SteamClient' in window) return;
    const host = window.chrome?.webview;
    if (!host) return;
    installXboxStartupMovieProbe(host, xboxStartupMovieOverrideAbsent);

    const pending = new Map();
    const listeners = new Set();
    const detailListeners = new Set();
    let requestId = crypto.getRandomValues(new Uint32Array(1))[0] & 0x3fffffff;
    let runtime, cloudCache;
    let updating;
    let refreshAgain = false;
    let activeApp = 0;
    let controlsApp = 0;
    let focusStore;
    let cloudBackupApps = new Map();
    const localCloudConfig = xboxLocalCloudCacheReads && typeof xboxNativeWindow !== 'undefined' &&
        xboxNativeWindow.deviceFamily === 'Windows.Xbox';
    // Capture the release's serializer without replacing its entry, routes or modules.
    (window.webpackChunksteamui ||= []).push([['xbox-app-overview'], {}, require => {
        runtime = require;
        if (xboxSteamReleaseVersion === 1794536281) {
            // sp.js registers the app-actions module in chunk 3714. Run
            // after registration and before the awaited import executes it.
            require.O(undefined, [3714], () => {
                installXboxCloudBackup(require, send, appId => !activeApp && cloudBackupApps.get(appId));
            }, 1);
        }
        if (localCloudConfig) {
            // The pinned library entry defers on chunk 41. Run this callback
            // before its entry so the store's first data request uses the cache.
            require.O(undefined, [41], () => {
                try {
                    const module = require(83957);
                    cloudCache?.installLocalReads(module.__esModule ? module.default : module);
                } catch (_) {}
            }, -1);
        }
    }]);
    installXboxStartupTiming(host, xboxProfileStartupServices, () => runtime?.(76725).J());

    host.addEventListener('message', ({data}) => {
        if (data?.event === 'xbox-catalog-changed') {
            refreshLibrary();
            return;
        }
        if (data?.event === 'xbox-install') {
            installer.update(data.appId, data.stage, data.progress);
            return;
        }
        if (data?.event === 'xbox-game') {
            if ((data.appId !== 0 && !validId(data.appId)) || typeof data.overlay !== 'boolean') return;
            const changed = activeApp !== data.appId;
            const exited = data.appId === 0 ? activeApp : 0;
            activeApp = data.appId;
            controlsApp = data.overlay ? activeApp : 0;
            if (controlsApp && !focusStore) attachRunningGameFocus();
            const requested = controlsApp;
            function updateControls() {
                if (controlsApp === requested) {
                    const path = runtime?.(5757).oy.WindowStore.GamepadUIMainWindowInstance?.History?.location.pathname;
                    // Valve goes Back when this game stops. A second copy of its
                    // controls would lead straight back to the stopped game's overlay.
                    const alreadyOpen = requested && path && runtime(49519).B6(path, {
                        path: runtime(72500).BV.GamepadUI.AppOverlay.Root(requested), exact: false
                    });
                    if (!alreadyOpen) showGameOverlay(requested);
                }
                if (exited && !activeApp) returnToLibrary(exited);
            }
            if (changed) refresh().then(updateControls, error => console.error('Steam library unavailable', error.message));
            else updateControls();
            return;
        }
        if (data?.event) return;
        const request = pending.get(data?.id);
        if (!request) return;
        pending.delete(data.id);
        clearTimeout(request.timeout);
        if (typeof data.error === 'string') request.reject(new Error(data.error));
        else request.resolve(data.result);
    });

    function send(method, appId, argument) {
        if (pending.size >= 16) return Promise.reject(new Error('Steam broker is busy'));
        const id = requestId = requestId % 0x7fffffff + 1;
        return new Promise((resolve, reject) => {
            const timeout = method !== 'play' && method !== 'install' && method !== 'storage-move' ? setTimeout(() => {
                pending.delete(id);
                reject(new Error('Steam broker timed out'));
            }, method === 'cloud-backup-download' || method === 'cloud-backup-restore' ? 180000 : 60000) : undefined;
            pending.set(id, {resolve, reject, timeout});
            try {
                const request = {id, method};
                if (appId !== undefined) request.appId = appId;
                if (argument !== undefined) request.argument = argument;
                host.postMessage(request);
            } catch (error) {
                pending.delete(id);
                clearTimeout(timeout);
                reject(error);
            }
        });
    }

    function validId(id) {
        return Number.isInteger(id) && id > 0 && id <= 0xffffff;
    }

    function attachRunningGameFocus() {
        const store = runtime?.(5757).oy;
        if (!store || typeof store.SetRunningApp !== 'function') return;
        const original = store.SetRunningApp;
        // The release invokes this selection command for its Resume button. It
        // also raises a desktop HWND when an injected overlay exists. Xbox's
        // single presenter needs the equivalent ApplicationView switch.
        Object.defineProperty(store, 'SetRunningApp', {configurable: true, value: function (appId) {
            const result = original.call(this, appId);
            if (controlsApp === appId && !store.WindowStore.GetOverlayInstances(appId).length)
                client.Apps.RaiseWindowForGame(String(appId)).catch(error => console.error('Steam game focus unavailable', error.message));
            return result;
        }});
        focusStore = store;
    }

    function returnToLibrary(appId) {
        const gamepad = runtime(5757).oy.WindowStore.GamepadUIMainWindowInstance;
        if (!gamepad) return;
        const details = '/library/app/' + appId, overlay = '/app/' + appId + '/overlay/';
        const end = Date.now() + 8000;
        const popup = xboxGamepadPopup?.document;
        let timer, selectedAt;
        function stop() {
            clearTimeout(timer);
            popup?.removeEventListener('pointerdown', stop, true);
            popup?.removeEventListener('keydown', stop, true);
        }
        function check() {
            if (activeApp || Date.now() >= end) return stop();
            const path = gamepad.History.location.pathname;
            // After Valve unmounts its running controls it restores the pre-game
            // details asynchronously. Wait for that route before replacing it.
            if (path === details) {
                gamepad.Navigator.LibraryTab('AllGames', {bReplace:true});
                selectedAt = Date.now();
            } else if (path === '/library/tab/AllGames') {
                if (selectedAt && Date.now() - selectedAt > 400) return stop();
            } else if (!path.startsWith(overlay)) return stop();
            timer = setTimeout(check, 120);
        }
        popup?.addEventListener('pointerdown', stop, true);
        popup?.addEventListener('keydown', stop, true);
        check();
    }

    function refresh() {
        if (updating) {
            refreshAgain = true;
            return updating;
        }
        if (!listeners.size && !detailListeners.size) return Promise.resolve();
        updating = send('library').then(games => {
            // Module 76142 in Valve release 1794536281 exports the protobuf
            // that Apps.RegisterForAppOverviewChanges supplies to UpdateAppOverview.
            const {bs: change, Ul: status} = runtime(76142);
            const current = new Set();
            const backups = new Map();
            const apps = games.map(game => {
                if (!validId(game.id) || current.has(game.id) || typeof game.name !== 'string' ||
                    typeof game.installed !== 'boolean' || typeof game.canPlay !== 'boolean') {
                    throw new Error('Invalid owned-game snapshot');
                }
                current.add(game.id);
                if (game.installed && game.cloudBackup === true) backups.set(game.id, {canRestore: game.cloudRestore === true});
                const local = {clientid: '0', installed: game.installed};
                if (game.id === activeApp && game.installed) local.display_status = status.Fi; // Running
                else if (game.installed) local.display_status = game.canPlay ? status.hr : status.Yh; // ReadyToLaunch / NotLaunchable
                else local.display_status = installer.status(game.id, status) ?? status.jw; // ReadyToInstall
                return {appid: game.id, gameid: String(game.id), app_type: 1, display_name: game.name,
                    sort_as: game.name.toLowerCase(), visible_in_game_list: true, subscribed_to: true,
                    per_client_data: [local]};
            });
            cloudBackupApps = backups;
            const revision = JSON.stringify(apps.map(app => [
                app.appid, app.display_name, app.per_client_data[0].display_status
            ]));
            for (const subscription of listeners) {
                if (subscription.revision === revision) continue;
                const removed = [...subscription.ids].filter(id => !current.has(id));
                const bytes = change.fromObject({app_overview: apps, removed_appid: removed,
                    full_update: true, update_complete: true}).serializeBinary();
                subscription.ids = current;
                subscription.revision = revision;
                subscription.callback(bytes);
            }
            // The native broker serves the catalog only to its signed-in owner.
            openStartupLibrary(apps.length === 0);
            const byId = new Map(apps.map(app => [app.appid, app]));
            for (const sub of detailListeners) {
                const app = byId.get(sub.id);
                if (!app) continue;
                const local = app.per_client_data[0];
                if (sub.last === local.display_status && sub.name === app.display_name) continue;
                sub.last = local.display_status;
                sub.name = app.display_name;
                sub.callback({unAppID: app.appid, strDisplayName: app.display_name, bIsSubscribedTo: true,
                    bHasAnyLocalContent: local.installed, eDisplayStatus: local.display_status,
                    rtLastTimePlayed: 0, // No verified play timestamp is available to the broker.
                    // Xbox enumerates base games only and has no beta selector.
                    vecDLC: [], vecChildConfigApps: [], strSelectedBeta: '', vecBetas: [], achievements: {}});
            }
        }).finally(() => {
            updating = undefined;
            if (refreshAgain) {
                refreshAgain = false;
                return refresh();
            }
        });
        return updating;
    }

    window.SteamClient = {Apps: {
        async GetLaunchOptionsForApp(id) {
            if (!validId(id)) throw new TypeError('Expected an AppID');
            const game = (await send('library')).find(app => app.id === id);
            if (!game || !game.installed || !game.canPlay) return [];
            // Xbox owns one default launch path per installed playable game.
            // Valve's pinned ELaunchOptionType_Default is 1.
            return [{nIndex: 0, eType: 1, strGameName: game.name, strDescription: ''}];
        },
        ReportLibraryAssetCacheMiss(appId, assetType) {
            if (!Number.isInteger(appId) || appId <= 0 || appId > 0xffffffff ||
                !Number.isInteger(assetType) || assetType < 0 || assetType > 31) return;
            // Valve advances its own artwork fallback after this synchronous notification.
            // Retain one document-local diagnostic without storing account or game identifiers.
            const mark = 'steam-library-asset-cache-miss';
            performance.clearMarks(mark);
            performance.mark(mark, {detail: {assetType}});
        },
        RegisterForAppDetails(id, callback) {
            if (!validId(id) || typeof callback !== 'function') throw new TypeError('Expected an AppID and a details listener');
            const sub = {id, callback};
            detailListeners.add(sub);
            refresh().catch(error => console.error('Steam app details unavailable', error.message));
            return {unregister() { detailListeners.delete(sub); }};
        },
        // This package has no Workshop event source. The pinned WorkshopStore
        // accepts an absent subscription handle and does not consume it further.
        RegisterForWorkshopChanges() { return undefined; },
        RegisterForWorkshopItemInstalled() { return undefined; },
        RegisterForAppOverviewChanges(callback) {
            if (typeof callback !== 'function') throw new TypeError('Expected an overview listener');
            const subscription = {callback, ids: new Set()};
            listeners.add(subscription);
            refresh().catch(error => console.error('Steam library unavailable', error.message));
            return {unregister() { listeners.delete(subscription); }};
        },
        async RunGame(gameId, launchOptions, launchOptionIndex, source) {
            const appId = Number(gameId);
            if (!validId(appId) || String(appId) !== String(gameId) || launchOptions !== '' || (launchOptionIndex !== -1 && launchOptionIndex !== 0)) {
                throw new Error('Unsupported Steam launch request');
            }
            const result = await runAction(appId, source, () => send('play', appId));
            if (result === 'unsupported-build') throw new Error('This Steam build has no matching Xbox compatibility profile.');
            if (result === 'source-file-check-failed') throw new Error('The Xbox compatibility file check failed.');
            if (result === 'prerequisite-restart-required') throw new Error('Reopen GabeBox before trying to play again.');
            if (result !== 'presented' && result !== 'window-ready') throw new Error(String(result || 'Launch unconfirmed'));
        }
    }};
    const client = window.SteamClient;
    const runAction = installXboxGameActions(client);
    const refreshLibrary = () => refresh().catch(error => console.error('Steam library unavailable', error.message));
    const storage = installXboxStorage(client, (operation, argument) => send(`storage-${operation}`, undefined, argument),
        appId => runtime?.(90544).tw.GetAppOverviewByAppID(appId)?.display_name, () => runtime(94601), refreshLibrary);
    const publishDownload = installXboxDownloads(client, () => runtime, () => xboxGamepadPopup);
    const installer = installXboxInstalls(client, send, refreshLibrary, () => runtime, storage.refresh, publishDownload);
    const gamepads = installXboxGamepadReader();
    client.System = {async IsSteamInTournamentMode() {
        installXboxNotifications(runtime);
        installXboxControllerService(runtime, gamepads);
        const mode = await send('system-mode');
        if (typeof mode !== 'boolean') throw new Error('Xbox presentation mode unavailable');
        return mode;
    }};
    let startupLibraryRequested = false;
    // A full ownership snapshot is delivered only after the Xbox QR broker
    // validates the session. Valve's BHasCurrentUser() additionally waits on
    // unrelated online services, including Friends, during InitAfterLogin.
    async function openStartupLibrary(emptyCatalog) {
        if (startupLibraryRequested) return;
        startupLibraryRequested = true;
        let awaitingTour;
        try {
            const owner = (await send('session'))?.steamId;
            if (typeof owner !== 'string' || !/^[1-9][0-9]{16,19}$/.test(owner)) {
                startupLibraryRequested = false;
                return;
            }
            // Valve reads this account's preference before mounting the guided tour,
            // then records it at mount. It does not indicate tour completion.
            awaitingTour = !(await runtime(14255).TT());
        } catch (_) { startupLibraryRequested = false; return; }
        const startupTimeout = 110000;
        let deadline = Date.now() + startupTimeout;
        let previous, previousPopup, selected = false, tourActive = false;
        function trace(stage) {
            if (stage === previous) return;
            previous = stage;
            host.postMessage({method: 'startup-route', stage});
        }
        function whenMounted() {
            if (activeApp) return;
            const gamepad = runtime(5757).oy.WindowStore.GamepadUIMainWindowInstance;
            const path = gamepad?.History?.location?.pathname;
            const popup = xboxGamepadPopup;
            // ActiveTourPage belongs to Valve's transition group and remains mounted
            // through all five pages. Changing its route here dismisses the tour.
            if (popup?.document.querySelector('#popup_target ._16IUnAiPD_1s1KQ0g8vJpq')) {
                tourActive = true;
                setTimeout(whenMounted, 250);
                return;
            }
            if (tourActive) {
                tourActive = false;
                awaitingTour = false;
                deadline = Date.now() + startupTimeout;
            }
            if (Date.now() > deadline) return;
            if (awaitingTour && path === '/library/home') {
                setTimeout(whenMounted, 150);
                return;
            }
            const tracked = popup && runtime(16724).Of.GetPopupForWindow(popup);
            const waiting = tracked?.m_renderWhenReady?.m_rgLoadingLinks || [];
            const styles = popup?.document.querySelectorAll('link[rel=stylesheet]') || [];
            function loaded(link) {
                return link.isConnected && !!link.sheet;
            }
            const phase = [!!popup, !!tracked, !!tracked?.m_bCreated, !!tracked?.m_onCreateRender,
                waiting.length, waiting.filter(loaded).length, styles.length, [...styles].filter(loaded).length,
                !!gamepad, !!path, !!gamepad?.Navigator].map(value => Math.min(99, Number(value))).join('-');
            if (phase !== previousPopup) {
                previousPopup = phase;
                host.postMessage({method: 'startup-popup', phase});
            }
            if (!path || !gamepad.Navigator) trace('waiting-window');
            else if (path === '/library/tab/AllGames') {
                trace('library-selected');
                // Once the original tiles mount, further routes belong to the player.
                const panel = popup?.document.querySelector('[role=tabpanel][id$="AllGames_Content"]');
                if (!awaitingTour && (panel?.querySelector('[role=gridcell]') ||
                    (emptyCatalog && panel?.querySelector('.rz1mrOnj9WN6UErES7qsq')))) return;
            } else if (!selected || /^\/(?:init|login|oobe(?:\/.*)?|library\/home)$/.test(path)) {
                selected = true;
                trace('selecting-library');
                gamepad.Navigator.LibraryTab('AllGames', {bReplace: true});
                trace(gamepad.History.location.pathname === '/library/tab/AllGames' ? 'library-selected' : 'selection-pending');
            } else return;
            setTimeout(whenMounted, 150);
        }
        whenMounted();
    }
    installXboxWindow(client, send, popup => runtime?.(16724).Of.GetPopupForWindow(popup));
    const storeBrowser = installXboxStoreBrowser(client, host, installXboxBrowserView(client),
        stage => host.postMessage({method: 'store-js-stage', stage}));
    installXboxUser(client, send, () => runtime);
    client.User.StartRestart = async force => {
        if (force !== false || await send('restart-client') !== 'restart')
            throw new Error('Steam client restart unavailable');
    };
    installXboxInput(client, gamepads, storeBrowser);
    installXboxLifecycle(client, () => runtime);
    installXboxSettings(client, () => runtime);
    installXboxNetwork(client, () => runtime, send, host);
    installXboxMessaging(client);
    installXboxCoreStartup(client, send);
    installXboxServices(client, send);
    cloudCache = installXboxCloudCache(client, send);
    if (localCloudConfig) cloudCache.observeStartup();
    const showGameOverlay = installXboxUrls(client, () => activeApp);
    document.addEventListener('visibilitychange', () => {
        if (!document.hidden) {
            refreshLibrary();
            storage.refresh();
        }
    });
})();
