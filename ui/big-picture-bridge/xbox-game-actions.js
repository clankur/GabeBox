// Action IDs describe work that this Xbox renderer has submitted to Steam Core.
// The foreground handoff happens in the signed host when it receives Play.
function installXboxGameActions(client) {
    let sequence = 0, active;
    const apps = client.Apps;
    const started = new Set(), ended = new Set();
    function listen(owners, callback) {
        if (typeof callback !== 'function') throw new TypeError('Expected an action callback');
        owners.add(callback);
        return {unregister() { owners.delete(callback); }};
    }
    function publish(listeners, ...args) {
        for (const callback of [...listeners]) queueMicrotask(() => {
            if (listeners.has(callback)) callback(...args);
        });
    }
    apps.RegisterForGameActionStart = callback => listen(started, callback);
    apps.RegisterForGameActionEnd = callback => listen(ended, callback);
    apps.GetActiveGameActions = async () => active ? [{...active.details}] : [];
    apps.GetGameActionForApp = (game, callback) => {
        if (active?.details.gameid === String(game)) callback(...active.startArgs);
    };
    apps.GetGameActionDetails = (id, callback) => {
        if (active?.details.nGameActionID === id) callback({...active.details});
    };
    // These signals describe native desktop dialogs or progress phases.
    // Steam Core reports one terminal result and presents the game itself.
    const unavailable = () => undefined;
    for (const event of ['TaskChange', 'UserRequest', 'ShowError', 'ShowUI'])
        apps['RegisterForGameAction' + event] = unavailable;
    return async function run(gameId, source, submit) {
        if (active) throw new Error('Steam game launch already in progress');
        const id = sequence = sequence % 0x7fffffff + 1;
        const details = {nGameActionID: id, gameid: String(gameId), strActionName: 'LaunchApp',
            strTaskName: '', strTaskDetails: '', strNumDone: '', strNumTotal: '', bWaitingForUI: false};
        const startArgs = [id, details.gameid, details.strActionName, source];
        active = {details, startArgs};
        publish(started, ...startArgs);
        try {
            return await submit();
        } finally {
            active = undefined;
            publish(ended, id);
        }
    };
}
