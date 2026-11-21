// Steam's per-user UI preferences persist in the WebView's package-local data folder.
// Authenticate each operation with the account that owns this renderer session.
function installXboxServices(client, send) {
    const keys = new Set(['CurrentKeyboardTheme', 'GetKeyboardSkins', 'GetStartupMovies', 'seen_clip_saved_hint']);
    const profileKey = /^GetEquippedProfileItemsForUser([1-9][0-9]{16,19})$/;
    const maxValue = 64 * 1024;
    let queue = Promise.resolve(), pending = 0, owner;

    function operation(key, action) {
        if (typeof key !== 'string' || (!keys.has(key) && !profileKey.test(key))) throw new Error('Unsupported Steam UI preference');
        if (pending >= 32) throw new Error('Steam UI preference storage is busy');
        ++pending;
        const result = queue.then(async () => {
            const session = await send('session');
            const id = session?.steamId;
            if (typeof id !== 'string' || !/^[1-9][0-9]{16,19}$/.test(id) ||
                (id.length === 20 && id > '18446744073709551615') || (owner && owner !== id)) {
                throw new Error('Steam UI preference account is unavailable');
            }
            owner = id;
            const profile = profileKey.exec(key);
            if (profile && profile[1] !== id) throw new Error('Steam UI preference belongs to a different account');
            return action('xbox-steamui:1:' + id + ':' + key);
        });
        // Valve's StoreString/RemoveObject callers issue writes and move on.
        // A subsequent read must observe their order even when a write failed.
        queue = result.then(() => { --pending; }, () => { --pending; });
        return result;
    }

    // The stock UI registers these before it loads its localization or
    // settings. The local client has no voice or remote settings events.
    function channel() {
        const listeners = new Set();
        return callback => {
            if (typeof callback !== 'function') throw new TypeError('Expected a client listener');
            listeners.add(callback);
            return {unregister() { listeners.delete(callback); }};
        };
    }
    client.Friends = {RegisterForVoiceChatStatus: channel()};
    client.FriendSettings = {RegisterForSettingsChanges: channel()};
    client.Storage = {
        GetString(key) {
            return operation(key, name => {
                const value = window.localStorage.getItem(name);
                // Valve's sp.js migrates unprefixed localStorage after a miss.
                // Ownership of a legacy key cannot be verified in a shared UDF.
                if (!value && window.localStorage.getItem(key) !== null)
                    throw new Error('Legacy Steam UI preference has no account owner');
                return value;
            });
        },
        SetString(key, value) {
            if (typeof value !== 'string' || value.length > maxValue) throw new Error('Invalid Steam UI preference value');
            return operation(key, name => window.localStorage.setItem(name, value));
        },
        DeleteKey(key) { return operation(key, name => window.localStorage.removeItem(name)); }
    };
}
