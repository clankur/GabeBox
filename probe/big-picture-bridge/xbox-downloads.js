// Project the active Xbox install into Valve's local Downloads store.
function installXboxDownloads(client, getRuntime, getPopup) {
    const overviewListeners = new Set(), itemListeners = new Set();
    const completed = new Map();
    let active;
    let observedDocument, noticeObserver, notice, hiddenStats;
    function subscribe(listeners, callback, publish) {
        if (typeof callback !== 'function') throw new TypeError('Expected a download callback');
        listeners.add(callback);
        queueMicrotask(() => { if (listeners.has(callback)) publish(callback); });
        return {unregister() { listeners.delete(callback); }};
    }
    function value(number) {
        return Number.isSafeInteger(number) && number >= 0 ? number : 0;
    }
    function byteCounts(transfer) {
        const count = transfer?.progress;
        if (!count || count.measurement === 'unavailable') return [0, 0, 0, 0];
        const downloadTotal = value(count.downloadTotal), stagingTotal = value(count.stagingTotal);
        return [Math.min(value(count.downloaded), downloadTotal), downloadTotal,
            Math.min(value(count.staged), stagingTotal), stagingTotal];
    }
    function progress(transfer) {
        const runtime = getRuntime();
        const {PK: overview} = runtime(12619);
        const {tgE: types} = runtime(94601);
        const values = overview.progress.map(row => ({...row}));
        const [downloaded, downloadTotal, staged, stagingTotal] = byteCounts(transfer);
        Object.assign(values[types.k_EAppUpdateProgress_Download], {bytes_in_progress: downloaded, bytes_total: downloadTotal});
        Object.assign(values[types.k_EAppUpdateProgress_Staging], {bytes_in_progress: staged, bytes_total: stagingTotal});
        return values;
    }
    function percent(transfer) {
        const [downloaded, downloadTotal, staged, stagingTotal] = byteCounts(transfer);
        const total = downloadTotal + stagingTotal;
        return total ? Math.trunc(100 * (downloaded + staged) / total) : 0;
    }
    function stageState(transfer) {
        if (transfer?.stage === 'downloading') return 'Downloading';
        if (transfer?.stage === 'staging') return 'Unpacking';
        if (transfer?.stage === 'paused') return 'Paused';
        return 'None';
    }
    function clearNotice() {
        notice?.remove();
        notice = undefined;
        hiddenStats?.style.removeProperty('display');
        hiddenStats = undefined;
    }
    function renderNotice() {
        const counts = active?.progress;
        const working = ['downloading', 'staging'].includes(active?.stage);
        const bytesUnavailable = !counts || counts.measurement === 'unavailable';
        const ratesUnavailable = counts?.networkBytesPerSecond === undefined && counts?.diskBytesPerSecond === undefined;
        const depotWorkingWithoutBytes = counts?.depotActivity && !counts.downloaded && !counts.staged &&
            !counts.networkBytesPerSecond && !counts.diskBytesPerSecond;
        const popup = working && (bytesUnavailable || ratesUnavailable || depotWorkingWithoutBytes) ? getPopup?.() : undefined;
        const doc = popup && !popup.closed ? popup.document : undefined;
        if (doc !== observedDocument) {
            noticeObserver?.disconnect();
            clearNotice();
            observedDocument = doc;
            if (doc) {
                noticeObserver = new MutationObserver(renderNotice);
                noticeObserver.observe(doc.documentElement, {childList: true, subtree: true});
            }
        }
        if (!doc) return;
        // The pinned Downloads page assumes its client always supplies a rate.
        // Keep its queue and progress controls, and explain missing telemetry above the stats.
        const styles = getRuntime()(25527);
        const wrapper = doc.querySelector(`.${styles.DownloadGraphStatsWrapper}`);
        if (!wrapper) {
            clearNotice();
            return;
        }
        const stats = wrapper.querySelector(`.${styles.DownloadGraphStats}`);
        if (ratesUnavailable && stats !== hiddenStats) {
            hiddenStats?.style.removeProperty('display');
            hiddenStats = stats;
            hiddenStats?.style.setProperty('display', 'none');
        } else if (!ratesUnavailable && hiddenStats) {
            hiddenStats.style.removeProperty('display');
            hiddenStats = undefined;
        }
        if (notice?.parentNode !== wrapper) {
            notice = doc.createElement('div');
            notice.setAttribute('role', 'status');
            wrapper.prepend(notice);
        }
        let message = 'Waiting for Steam to report a transfer rate.';
        if (bytesUnavailable) {
            message = counts?.depotActivity ? 'Steam is processing depot files. Byte progress is temporarily unavailable.' :
                'Waiting for Steam to report byte progress.';
        } else if (depotWorkingWithoutBytes) {
            message = 'Steam is processing depot files. Byte counters have not advanced.';
        }
        if (notice.textContent !== message) notice.textContent = message;
    }
    function overview(callback) {
        const defaults = getRuntime()(12619).PK;
        const state = stageState(active);
        const shown = state === 'Downloading' || state === 'Unpacking';
        const counts = shown ? active.progress : undefined;
        callback({...defaults, update_appid: shown ? active.appId : 0, update_state: shown ? state : 'None',
            update_is_install: shown, overall_percent_complete: shown ? percent(active) : 0,
            update_network_bytes_per_second: value(counts?.networkBytesPerSecond),
            update_peak_network_bytes_per_second: shown ? active.peakNetworkRate : 0,
            update_disc_bytes_per_second: value(counts?.diskBytesPerSecond),
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
        renderNotice();
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
            active = {appId, stage, peakNetworkRate: 0};
        }
        if (active?.appId !== appId) return;
        if (stage === 'installed') {
            completed.set(appId, {...active, stage, completedAt: Math.floor(Date.now() / 1000)});
            active = undefined;
        } else if (stage === 'finished') {
            active = undefined;
        } else {
            active.stage = stage;
            if (counts) {
                active.progress = counts;
                active.peakNetworkRate = Math.max(active.peakNetworkRate, value(counts.networkBytesPerSecond));
            }
        }
        refresh();
    };
}
