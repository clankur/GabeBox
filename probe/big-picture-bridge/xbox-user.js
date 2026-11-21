// Called only by the package's top-level, origin-checked bridge.
function installXboxUser(client, send, getRuntime) {
    "use strict";
    const subscribers = new Set();
    let pending;
    let timer;
    let projectedId;

    function publishIdentity(id) {
        const runtime = getRuntime?.();
        if (!runtime) return;
        // SharedConnection supplies these public UserConfig fields on desktop.
        // The QR broker supplies the same SteamID here. Connection state,
        // credentials, account names and private profile data remain Valve-owned.
        const config = runtime(30610).iA;
        if (config.logged_in || (config.steamid && config.steamid !== projectedId)) {
            projectedId = undefined;
            return;
        }
        if (id) {
            const account = new (runtime(55324).b)(id);
            config.steamid = id;
            config.accountid = account.GetAccountID();
            projectedId = id;
        } else if (projectedId) {
            config.steamid = '';
            config.accountid = 0;
            projectedId = undefined;
        }
    }

    function session() {
        if (!pending) {
            pending = Promise.resolve().then(() => send("session")).then(result => {
                const id = result && result.steamId;
                if (id === null && result) return null;
                if (typeof id !== "string" || !/^[1-9][0-9]{16,19}$/.test(id) ||
                    (id.length === 20 && id > "18446744073709551615")) throw Error("session-unavailable");
                return id;
            }).finally(() => { pending = undefined; });
        }
        return pending;
    }

    function refresh() {
        session().then(id => {
            publishIdentity(id);
            for (const sub of subscribers) {
                if (sub.last === id) continue;
                sub.last = id;
                // Keep Valve callbacks independent. One service's failure should
                // follow the renderer's normal error path without starving another.
                Promise.resolve().then(() => {
                    if (!subscribers.has(sub)) return;
                    if (sub.currentUser) sub.callback(id ? { strSteamID: id } : {});
                    else sub.callback("", id ? 5 : 1, id ? 1 : 0, 0, id ? 100 : 0, "");
                });
            }
        }, () => publishIdentity(null)); // A failed check revokes this projection, without emitting a login decision.
    }

    function subscribe(callback, currentUser) {
        if (typeof callback !== "function") throw TypeError("Expected a callback");
        const sub = { callback, currentUser };
        subscribers.add(sub);
        if (!timer) timer = setInterval(refresh, 5000);
        refresh();
        return { unregister() {
            subscribers.delete(sub);
            if (!subscribers.size && timer) {
                clearInterval(timer);
                timer = undefined;
            }
        } };
    }
    client.User = client.User || {};
    // The Xbox QR host selects the account before starting this renderer. No
    // Steam desktop user chooser is pending (default chooser state in this release).
    client.User.GetStartupUserChooserState = async () => 0;
    client.User.RegisterForLoginStateChange = callback => subscribe(callback, false);
    client.User.RegisterForCurrentUserChanges = callback => subscribe(callback, true);
}
