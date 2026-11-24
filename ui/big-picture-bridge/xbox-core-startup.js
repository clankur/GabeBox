// FriendsUI waits for the client's backend startup before constructing its own
// services. The Xbox backend is ready once the same signed-in user has a
// restored catalog. The separate gamepad window can still be loading.
function installXboxCoreStartup(client, send) {
    let checking;
    function confirmation() {
        if (!checking) {
            checking = Promise.resolve().then(async () => {
                const owner = (await send('session'))?.steamId;
                if (typeof owner !== 'string' || !/^[1-9][0-9]{16,19}$/.test(owner) ||
                    (owner.length === 20 && owner > '18446744073709551615')) return false;
                const library = await send('library');
                if (!Array.isArray(library)) return false;
                return (await send('session'))?.steamId === owner;
            }).finally(() => { checking = undefined; });
        }
        return checking;
    }

    (client.UI ||= {}).RegisterForStartupFinished = callback => {
        if (typeof callback !== 'function') throw new TypeError('Expected a startup listener');
        let listening = true;
        confirmation().then(ready => {
            if (listening && ready) callback();
        }, () => {}); // A session or library error leaves Steam's native wait outstanding.
        return {unregister() { listening = false; }};
    };
}
