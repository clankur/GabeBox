// Adapter for the signed, pinned Steam shared context. The packaged host owns
// authentication, ownership checks, launch routing, and the game process.
(() => {
    'use strict';
    if (window !== window.top || location.href.split('#')[0] !== 'https://steamloopback.host/index.html?IN_CLIENT=true' ||
        'SteamClient' in window) return;
    const host = window.chrome?.webview;
    if (!host) return;

    const pending = new Map();
    const listeners = new Set();
    const detailListeners = new Set();
    let requestId = crypto.getRandomValues(new Uint32Array(1))[0] & 0x3fffffff;
    let runtime;
    let updating;
    let refreshAgain = false;
    let activeApp = 0;
    // Capture the release's serializer without replacing its entry, routes or modules.
    (window.webpackChunksteamui ||= []).push([['xbox-app-overview'], {}, require => { runtime = require; }]);

    host.addEventListener('message', ({data}) => {
        if (data?.event === 'xbox-game') {
            if ((data.appId !== 0 && !validId(data.appId)) || typeof data.overlay !== 'boolean') return;
            const changed = activeApp !== data.appId;
            activeApp = data.appId;
            if (changed) refresh().catch(error => console.error('Steam library unavailable', error.message));
            showGameOverlay(data.overlay ? activeApp : 0);
            return;
        }
        const request = pending.get(data?.id);
        if (!request) return;
        pending.delete(data.id);
        clearTimeout(request.timeout);
        if (typeof data.error === 'string') request.reject(new Error(data.error));
        else request.resolve(data.result);
    });

    function send(method, appId) {
        if (pending.size >= 16) return Promise.reject(new Error('Steam broker is busy'));
        const id = requestId = requestId % 0x7fffffff + 1;
        return new Promise((resolve, reject) => {
            const timeout = method !== 'play' && method !== 'install' ? setTimeout(() => {
                pending.delete(id);
                reject(new Error('Steam broker timed out'));
            }, 60000) : undefined;
            pending.set(id, {resolve, reject, timeout});
            try {
                host.postMessage(appId === undefined ? {id, method} : {id, method, appId});
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
            const apps = games.map(game => {
                if (!validId(game.id) || current.has(game.id) || typeof game.name !== 'string' ||
                    typeof game.installed !== 'boolean' || typeof game.canPlay !== 'boolean') {
                    throw new Error('Invalid owned-game snapshot');
                }
                current.add(game.id);
                const local = {clientid: '0', installed: game.installed};
                if (game.id === activeApp && game.installed) local.display_status = status.Fi; // Running
                else if (game.installed) local.display_status = game.canPlay ? status.hr : status.Yh; // ReadyToLaunch / NotLaunchable
                else local.display_status = status.jw; // ReadyToInstall
                return {appid: game.id, gameid: String(game.id), app_type: 1, display_name: game.name,
                    sort_as: game.name.toLowerCase(), visible_in_game_list: true, subscribed_to: true,
                    per_client_data: [local]};
            });
            for (const subscription of listeners) {
                const removed = [...subscription.ids].filter(id => !current.has(id));
                const bytes = change.fromObject({app_overview: apps, removed_appid: removed,
                    full_update: true, update_complete: true}).serializeBinary();
                subscription.callback(bytes);
                subscription.ids = current;
            }
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
                    vecChildConfigApps: [], achievements: {}});
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
            if (!validId(appId) || String(appId) !== String(gameId) || launchOptions !== '' || launchOptionIndex !== -1) {
                throw new Error('Unsupported Steam launch request');
            }
            const result = await runAction(appId, source, () => send('play', appId));
            if (result !== 'presented' && result !== 'window-ready') throw new Error(String(result || 'Launch unconfirmed'));
        }
    }};
    const client = window.SteamClient;
    const runAction = installXboxGameActions(client);
    const gamepads = installXboxGamepadReader();
    client.System = {async IsSteamInTournamentMode() {
        installXboxNotifications(runtime);
        installXboxControllerService(runtime, gamepads);
        const mode = await send('system-mode');
        if (typeof mode !== 'boolean') throw new Error('Xbox presentation mode unavailable');
        return mode;
    }};
    installXboxWindow(client, send);
    installXboxBrowserView(client);
    installXboxUser(client, send, () => runtime);
    client.User.StartRestart = async force => {
        if (force !== false || await send('restart-client') !== 'restart')
            throw new Error('Steam client restart unavailable');
    };
    installXboxInput(client, gamepads);
    installXboxLifecycle(client, () => runtime);
    installXboxSettings(client, () => runtime);
    installXboxMessaging(client);
    installXboxCoreStartup(client, send);
    installXboxServices(client, send);
    installXboxCloudCache(client, send);
    const showGameOverlay = installXboxUrls(client, () => activeApp);
    document.addEventListener('visibilitychange', () => {
        if (!document.hidden) refresh().catch(error => console.error('Steam library unavailable', error.message));
    });
})();
