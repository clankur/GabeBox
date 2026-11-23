// Project the active Xbox install into Valve's local Downloads store.
function installXboxDownloads(client, getRuntime) {
    const overviewListeners = new Set(), itemListeners = new Set();
    const completed = new Map();
    let active;
    function subscribe(listeners, callback, publish) {
        if (typeof callback !== 'function') throw new TypeError('Expected a download callback');
        listeners.add(callback);
        queueMicrotask(() => { if (listeners.has(callback)) publish(callback); });
        return {unregister() { listeners.delete(callback); }};
    }
    function progress(transfer) {
        const runtime = getRuntime();
        const {PK: overview} = runtime(12619);
        const {tgE: types} = runtime(94601);
        const values = overview.progress.map(row => ({...row}));
        if (transfer) {
            const count = transfer.progress || {};
            Object.assign(values[types.k_EAppUpdateProgress_Download], {
                bytes_in_progress: count.downloaded || 0, bytes_total: count.downloadTotal || 0
            });
            Object.assign(values[types.k_EAppUpdateProgress_Staging], {
                bytes_in_progress: count.staged || 0, bytes_total: count.stagingTotal || 0
            });
        }
        return values;
    }
    function percent(transfer) {
        const {downloaded, downloadTotal, staged, stagingTotal} = transfer?.progress || {};
        const total = (downloadTotal || 0) + (stagingTotal || 0);
        return total ? Math.min(100, Math.trunc(100 * ((downloaded || 0) + (staged || 0)) / total)) : 0;
    }
    function stageState(transfer) {
        if (transfer?.stage === 'downloading') return 'Downloading';
        if (transfer?.stage === 'staging') return 'Unpacking';
        if (transfer?.stage === 'paused') return 'Paused';
        return 'None';
    }
    function overview(callback) {
        const defaults = getRuntime()(12619).PK;
        const state = stageState(active);
        const shown = state === 'Downloading' || state === 'Unpacking';
        callback({...defaults, update_appid: shown ? active.appId : 0, update_state: shown ? state : 'None',
            update_is_install: shown, overall_percent_complete: shown ? percent(active) : 0,
            progress: progress(shown ? active : undefined), history: []});
    }
    function item(transfer) {
        const done = transfer.stage === 'installed';
        const fields = {
            appid: transfer.appId, active: ['downloading', 'staging'].includes(transfer.stage),
            paused: transfer.stage === 'paused', queue_index: done ? -1 : 0, deferred_time: 0,
            completed: done, completed_time: transfer.completedAt || 0,
            overall_percent_complete: done ? 100 : percent(transfer), buildid: 0,
            update_result: 0, update_error: ''
        };
        const types = getRuntime()(94601).$6p;
        const content = types.k_EAppUpdateContentType_Content;
        const lastType = Math.max(content, types.k_EAppUpdateContentType_Shader, types.k_EAppUpdateContentType_Workshop);
        const updateTypes = Array.from({length: lastType + 1}, (_, index) => ({
            has_update: index === content, completed: done && index === content,
            progress: progress(index === content ? transfer : undefined)
        }));
        return {...fields, update_type_info: updateTypes};
    }
    function items(callback) {
        callback(true, [{remote_client_id: '0',
            item_data: [...(active ? [item(active)] : []), ...[...completed.values()].map(item)]}]);
    }
    function refresh() {
        for (const callback of overviewListeners) overview(callback);
        for (const callback of itemListeners) items(callback);
    }
    const unsupported = () => false;
    client.Downloads = {
        RegisterForRemoteClientsConnected(callback) {
            if (typeof callback !== 'function') throw new TypeError('Expected a download callback');
            let registered = true;
            queueMicrotask(() => { if (registered) callback([]); });
            return {unregister() { registered = false; }};
        },
        RegisterForDownloadOverview: callback => subscribe(overviewListeners, callback, overview),
        RegisterForDownloadItems: callback => subscribe(itemListeners, callback, items),
        RemoveFromDownloadList(appId, clientId) {
            if (clientId !== '0' || !completed.delete(appId)) return false;
            refresh();
            return true;
        },
        EnableAllDownloads: unsupported,
        PauseAppUpdate: unsupported,
        QueueAppUpdate: unsupported,
        ResumeAppUpdate: unsupported,
        SetLaunchOnUpdateComplete: unsupported,
        SetQueueIndex: unsupported,
        SuspendDownloadThrottling: unsupported,
        SuspendLanPeerContent: unsupported
    };
    return function publish(appId, stage, counts) {
        if (stage === 'starting') {
            completed.delete(appId);
            active = {appId, stage};
        }
        if (active?.appId !== appId) return;
        if (stage === 'installed') {
            completed.set(appId, {...active, stage, completedAt: Math.floor(Date.now() / 1000)});
            active = undefined;
        } else if (stage === 'finished') {
            active = undefined;
        } else {
            active.stage = stage;
            if (counts) active.progress = counts;
        }
        refresh();
    };
}
