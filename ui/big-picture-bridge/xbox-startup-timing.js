function installXboxStartupTiming(host, enabled, startupLocales) {
    if (!enabled || typeof xboxNativeWindow === 'undefined' ||
        xboxNativeWindow.deviceFamily !== 'Windows.Xbox') return;
    const totalTitle = 'SteamApp Init - After Login total time';
    const groups = new Map([
        ['Key dependencies', ['key', new Map([['CloudStorage', 'key_cloud'],
            ['FriendsChatStore - Startup', 'key_friends'], ['SettingsStore', 'key_settings'],
            ['GuidedTourStore', 'key_tour']])]],
        ['AppStore', ['apps', new Map([['AppStore', 'apps_store']])]],
        ['FriendStore', ['friends', new Map([['FriendStore (steamui)', 'friends_store']])]],
        ['CollectionStore', ['collections', new Map([['CollectionStore', 'collections_store']])]],
        ['Remaining services', ['remaining', new Map([['AppInfoStore', 'remaining_appinfo'],
            ['BasicUIStore - with user', 'remaining_ui'], ['LibraryUIStore', 'remaining_library']])]]
    ]);
    const reported = new Set();
    let activeGroup, started;

    function publish(label, duration) {
        const elapsedMs = Math.round(duration);
        if (!Number.isInteger(elapsedMs) || elapsedMs < 0 || elapsedMs > 120000 || reported.has(label)) return;
        reported.add(label);
        host.postMessage({method: 'startup-service-timing', label, elapsedMs});
    }

    function duration(label, title) {
        const prefix = title + ' - ';
        if (typeof label !== 'string' || label.length > 128 || !label.startsWith(prefix) || !label.endsWith('ms')) return;
        const locales = startupLocales();
        const format = new Intl.NumberFormat(locales);
        const parts = format.formatToParts(12345.6);
        const group = parts.find(part => part.type === 'group')?.value;
        const decimal = parts.find(part => part.type === 'decimal')?.value;
        const digits = new Intl.NumberFormat(locales, {useGrouping: false});
        let value = label.slice(prefix.length, -2);
        if (group) value = value.split(group).join('');
        if (decimal) value = value.replace(decimal, '.');
        for (let i = 0; i < 10; ++i) value = value.split(digits.format(i)).join(String(i));
        if (/^\d{1,6}(?:\.\d{1,3})?$/.test(value)) return Number(value);
    }

    function observe(method, profile) {
        const original = console[method];
        console[method] = function (...args) {
            const result = Reflect.apply(original, this, args);
            try { if (args.length === 1 || !args.length) profile(args[0]); } catch (_) {}
            return result;
        };
    }

    observe('time', label => {
        if (label === totalTitle) started = performance.now();
    });
    observe('timeEnd', label => {
        if (label === totalTitle && started !== undefined) publish('total', performance.now() - started);
    });
    observe('groupCollapsed', label => {
        activeGroup = undefined;
        for (const [title, [id, members]] of groups) {
            const elapsed = duration(label, 'SteamApp Init - After Login - ' + title);
            if (elapsed === undefined) continue;
            publish(id, elapsed);
            activeGroup = members;
            break;
        }
    });
    observe('log', label => {
        if (!activeGroup) return;
        for (const [title, id] of activeGroup) {
            const elapsed = duration(label, 'async ' + title) ?? duration(label, title);
            if (elapsed !== undefined) publish(id, elapsed);
        }
    });
    observe('groupEnd', () => { activeGroup = undefined; });
}
