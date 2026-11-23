// Valve owns the Install button and error dialog. Steam Core installs to the
// Xbox library currently selected in Storage.
function installXboxInstalls(client, send, refresh, getRuntime, installed, publishDownload) {
    const stageKeys = {
        starting: '$g', queued: 'Bp', resuming: 'Bp', paused: 'Cy', downloading: 'cM', staging: '$g'
    };
    const wizardListeners = new Set();
    let active;
    let failedApp;

    function showWizard(state, appId) {
        const {H24, VIA} = getRuntime()(94601);
        for (const listener of wizardListeners)
            listener({eInstallState: H24[state], currentAppID: appId, eAppError: VIA});
    }

    function dismiss() {
        if (failedApp === undefined) return;
        showWizard('k_EInstallMgrStateNone', failedApp);
        failedApp = undefined;
    }

    function update(appId, stage, progress) {
        if (!active || active.appId !== appId) return;
        if (stage === 'installed') {
            publishDownload?.(appId, active.stage, progress);
            return;
        }
        if (!Object.hasOwn(stageKeys, stage)) return;
        const changed = stageKeys[stage] !== stageKeys[active.stage];
        active.stage = stage;
        publishDownload?.(appId, stage, progress);
        if (changed) refresh();
    }

    async function open(appIds, clientId) {
        if (clientId !== '0' || !Array.isArray(appIds) || appIds.length !== 1 ||
            !Number.isInteger(appIds[0]) || appIds[0] < 1 || appIds[0] > 0xffffff || active) return;
        dismiss();
        const appId = appIds[0];
        active = {appId, stage: 'starting'};
        publishDownload?.(appId, 'starting');
        refresh();
        let outcome;
        try {
            outcome = await send('install', appId);
        } catch (_) {
            outcome = 'failed';
        } finally {
            active = undefined;
            publishDownload?.(appId, outcome === 'installed' ? 'installed' : 'finished');
            refresh();
        }
        if (outcome === 'installed' || outcome === 'already-installed') installed?.();
        else if (outcome !== 'signed-out') {
            failedApp = appId;
            showWizard('k_EInstallMgrStateFailed', appId);
        }
    }

    const unavailable = () => undefined;
    client.Installs = {
        OpenInstallWizard: open,
        CancelInstall: dismiss,
        RegisterForShowInstallWizard(callback) {
            if (typeof callback !== 'function') throw new TypeError('Expected an installer callback');
            wizardListeners.add(callback);
            return {unregister() { wizardListeners.delete(callback); }};
        },
        RegisterForShowConfirmUninstall: unavailable,
        RegisterForShowFailedUninstall: unavailable
    };
    return {
        update,
        status(appId, statuses) { if (active?.appId === appId) return statuses[stageKeys[active.stage]]; }
    };
}
