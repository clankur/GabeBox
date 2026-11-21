// Called only by the package's top-level, origin-checked bridge.
function installXboxUser(client, send) {
    "use strict";
    const subscribers = new Set();
    let pending;
    let timer;

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
        }, () => {}); // A transport failure does not confirm a login or logout.
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
    client.User.RegisterForLoginStateChange = callback => subscribe(callback, false);
    client.User.RegisterForCurrentUserChanges = callback => subscribe(callback, true);
}
