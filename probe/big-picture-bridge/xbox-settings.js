// These are device setup choices made by Valve's own UI. Only persist a
// completion flag after that UI actually writes it through its native API.
function installXboxSettings(client, getRuntime) {
    const prefix = 'xbox-steamui:machine:1:';
    const names = ['oobe_completed', 'oobe_stage_2_completed'];
    const listeners = new Set();
    function codec() { return getRuntime()(29303).Ne; }
    function values() {
        const result = {};
        for (const key of names) {
            const value = localStorage.getItem(prefix + key);
            if (value === 'true' || value === 'false') result[key] = value === 'true';
        }
        return result;
    }
    function publish(only) {
        const snapshot = values();
        if (!Object.keys(snapshot).length) return;
        const bytes = codec().fromObject(snapshot).serializeBinary();
        for (const callback of only || listeners) {
            if (listeners.has(callback)) callback(bytes);
        }
    }
    const settings = client.Settings ||= {};
    settings.RegisterForSettingsArrayChanges = callback => {
        if (typeof callback !== 'function') throw new TypeError('Expected a settings listener');
        listeners.add(callback);
        queueMicrotask(() => { if (listeners.has(callback)) publish([callback]); });
        return {unregister() { listeners.delete(callback); }};
    };
    settings.SetSetting = async base64 => {
        if (typeof base64 !== 'string' || !base64.length || base64.length > 32) throw new Error('Unsupported client setting');
        const type = codec();
        // Compare the exact native wire message to a single known field. A
        // message containing other settings is never partially applied.
        for (const key of names) for (const value of [true, false]) {
            const bytes = type.fromObject({[key]: value}).serializeBinary();
            if (!bytes.length || btoa(String.fromCharCode(...bytes)) !== base64) continue;
            localStorage.setItem(prefix + key, String(value));
            publish();
            return;
        }
        throw new Error('Unsupported client setting');
    };
    const keys = new Set(['keyboard_layout', 'continuously_render_store']);
    function key(name) {
        if (!keys.has(name)) throw new Error('Unsupported Steam device preference');
        return prefix + name;
    }
    client.MachineStorage = {
        async GetString(name) { return localStorage.getItem(key(name)); },
        async SetString(name, value) {
            if (typeof value !== 'string' || value.length > 4096) throw new Error('Invalid Steam device preference');
            localStorage.setItem(key(name), value);
        },
        async DeleteKey(name) { localStorage.removeItem(key(name)); }
    };
}
