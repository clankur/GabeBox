// Maps the Xbox broker's allowed libraries onto Valve's pinned Storage APIs.
function installXboxStorage(client, request, getAppName, getMoveErrors, onMove) {
    const folders = ['D', 'E', 'local'];
    const names = {D: 'Xbox Development Storage', E: 'External Storage', local: 'App Storage'};
    const listeners = new Set();
    let moveListener;
    let inventory = [];
    let reading;
    let moving;

    function subscribe(callback) {
        if (typeof callback !== 'function') throw new TypeError('Expected a storage listener');
        listeners.add(callback);
        // The pinned store only fetches its initial list on this notification.
        notify(callback);
        return {unregister() { listeners.delete(callback); }};
    }

    function notify(callback) {
        Promise.resolve().then(() => {
            if (listeners.has(callback)) return callback(0);
        }).catch(() => console.error('Steam storage listener failed'));
    }

    function refresh() {
        if (reading) {
            reading.then(refresh);
            return;
        }
        for (const callback of listeners) notify(callback);
    }

    function bytes(value) {
        return Number.isSafeInteger(value) && value >= 0 ? value : 0;
    }

    function validAppId(id) {
        return Number.isInteger(id) && id > 0 && id <= 0xffffff;
    }

    function makeFolder(folder, id, index, defaultId) {
        const apps = Array.isArray(folder?.apps) ? folder.apps.filter(app => validAppId(app?.appId)) : [];
        const vecApps = apps.map(({appId, name, sizeBytes}) => {
            const label = getAppName(appId) || (typeof name === 'string' && name) || `App ${appId}`;
            const size = bytes(sizeBytes);
            return {nAppID: appId, strAppName: label, strSortAs: label, rtLastPlayed: 0,
                nUsedSize: size, nAppSize: size, nDLCSize: 0, nStagedSize: 0, nWorkshopSize: 0, nShaderSize: 0};
        });
        const totalApps = vecApps.reduce((total, app) => total + app.nAppSize, 0);
        const capacity = bytes(folder?.totalBytes);
        const mounted = folder?.mounted === true;
        return {nFolderIndex: index, bIsMounted: mounted, bIsDefaultFolder: mounted && id === defaultId,
            bIsFixed: id !== 'E', strUserLabel: folder?.name || names[id], strDriveName: id === 'local' ? '' : `${id}:`,
            strFolderPath: folder?.path || '', nCapacity: capacity, nFreeSpace: Math.min(bytes(folder?.freeBytes), capacity),
            nUsedSize: totalApps, nAppSize: totalApps, nDLCSize: 0, nStagedSize: 0, nWorkshopSize: 0, nShaderSize: 0,
            vecApps};
    }

    function buildList(snapshot) {
        const byId = new Map();
        if (Array.isArray(snapshot?.folders)) {
            for (const folder of snapshot.folders) {
                if (folders.includes(folder?.id) && !byId.has(folder.id)) byId.set(folder.id, folder);
            }
        }
        // The pinned install wizard expects the array position to equal the
        // folder ID. Keep unavailable earlier positions hidden and reserved.
        const last = folders.reduce((index, id, current) => byId.has(id) ? current : index, -1);
        const rows = folders.slice(0, last + 1).map((id, index) => makeFolder(byId.get(id), id, index, snapshot?.defaultId));
        if (rows.some(row => row.bIsMounted)) return rows;
        // The stock view only leaves its spinner after a folder is visible.
        // This presentation-only row keeps the panel usable during an outage.
        return [makeFolder({name: 'Storage unavailable', mounted: true}, 'local', 0)];
    }

    async function getInstallFolders() {
        if (!reading) {
            reading = Promise.resolve().then(() => request('list', '')).then(snapshot => {
                inventory = buildList(snapshot);
            }).catch(() => {
                inventory = buildList();
            }).finally(() => { reading = undefined; });
        }
        await reading;
        // Valve rewrites vecApps while consuming each snapshot.
        return inventory.map(row => ({...row, vecApps: row.vecApps.map(app => ({...app}))}));
    }

    function destination(index) {
        if (index !== 0 && index !== 1) return undefined;
        const folder = inventory[index];
        if (folder?.bIsMounted && folder.strDriveName === `${folders[index]}:`) return folders[index];
    }

    async function select(index) {
        const target = destination(index);
        if (!target) return false;
        try {
            return (await request('select', target))?.ok === true;
        } catch (_) {
            return false;
        } finally {
            refresh();
        }
    }

    async function move(appId, index) {
        const {VIA: success, vyo: failed} = getMoveErrors();
        const target = destination(index);
        if (!target || moving || !validAppId(appId)) throw failed;
        const operation = {callback: moveListener};
        moving = operation;
        // The dialog transitions to its progress state after this command's
        // promise resolves, then waits for the separate progress notification.
        setTimeout(async () => {
            let result;
            try {
                result = await request('move', {appIds: [appId], destination: target});
            } catch (_) { /* The pinned dialog localizes the move failure. */ }
            moving = undefined;
            refresh();
            const done = result?.ok === true;
            if (done) onMove();
            try {
                await operation.callback?.({appid: appId, eError: done ? success : failed,
                    flProgress: done ? 100 : 0, strBytesMoved: '', strTotalBytesToMove: '', nFilesMoved: 0});
            } catch (_) { /* The dialog may have closed while the move was running. */ }
        }, 0);
    }

    client.InstallFolder = {
        RegisterForInstallFolderChanges: subscribe,
        GetInstallFolders: getInstallFolders,
        RefreshFolders: refresh,
        SetDefaultInstallFolder: select,
        RegisterForMoveContentProgress(callback) {
            if (typeof callback !== 'function') throw new TypeError('Expected a move listener');
            // The pinned move dialog does not unregister its callback on close.
            moveListener = callback;
            return {unregister() { if (moveListener === callback) moveListener = undefined; }};
        },
        MoveInstallFolderForApp: move,
        CancelMove() {
            // Native completes the admitted game move. Detach the closed dialog
            // so that it will not start additional selected games afterward.
            if (moving) moving.callback = undefined;
        }
    };
    // The Xbox package does not maintain a desktop screenshot library.
    client.Screenshots = {...client.Screenshots,
        async GetTotalDiskSpaceUsage() { return 0; },
        async GetAllAppsLocalScreenshotsCount() { return 0; }
    };
    return {refresh};
}
