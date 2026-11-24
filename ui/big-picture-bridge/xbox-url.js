const xboxSteamLanguages = {en:'english',fr:'french',de:'german',es:'spanish',it:'italian',da:'danish',
    nl:'dutch',fi:'finnish',ja:'japanese',ko:'koreana',no:'norwegian',nb:'norwegian',pl:'polish',
    pt:'portuguese',ro:'romanian',ru:'russian',sv:'swedish',th:'thai',tr:'turkish',cs:'czech',
    hu:'hungarian',uk:'ukrainian',vi:'vietnamese',bg:'bulgarian',el:'greek',id:'indonesian'};
function steamXboxLanguage() {
    const locale = navigator.language.toLowerCase();
    if (/^zh-(hant|tw|hk|mo)/.test(locale)) return 'tchinese';
    if (/^zh/.test(locale)) return 'schinese';
    if (locale === 'pt-br') return 'brazilian';
    if (/^es-(419|mx|ar|cl|co|pe)/.test(locale)) return 'latam';
    return xboxSteamLanguages[locale.split('-')[0]];
}

// Valve's CURLStore requests a dictionary keyed by its original case-sensitive
// route names. Its ResolveURL supplies %p1% etc. at the point of use.
function installXboxUrls(client, activeApp = () => 0) {
    const feature = {store: 1, community: 2, profile: 3, friends: 4, news: 5, trading: 6};
    const hosts = {store: 'https://store.steampowered.com', storehttps: 'https://store.steampowered.com',
        community: 'https://steamcommunity.com', communityhttps: 'https://steamcommunity.com',
        help: 'https://help.steampowered.com', chat: 'https://steamcommunity.com/chat/',
        checkout: 'https://checkout.steampowered.com', mystuff: 'my'};
    function resolve(name) {
        const entry = Object.hasOwn(xboxUrlTemplates, name) && xboxUrlTemplates[name];
        const root = typeof xboxUrlRoots !== 'undefined' && Object.hasOwn(xboxUrlRoots, name) && xboxUrlRoots[name];
        if (!entry && !root) return;
        const [template, filter] = entry || [root, ''];
        if (filter && !Object.hasOwn(feature, filter)) return;
        let missing = false;
        const url = template.replace(/%([a-z][a-z0-9]*)%/gi, (token, key) => {
            if (/^p[1-4]$/.test(key)) return token;
            const value = key === 'language' ? steamXboxLanguage() : Object.hasOwn(hosts, key) && hosts[key];
            if (!value) missing = true;
            return value || '';
        });
        if (missing) return;
        try {
            const route = new URL(url);
            if (route.protocol !== 'https:' || route.username || route.password || route.port ||
                !/(^|\.)(steampowered\.com|steamcommunity\.com|steamstatic\.com)$/.test(route.hostname)) return;
            return {url: url === route.origin ? url + '/' : url, feature: feature[filter] || 9};
        } catch (_) {}
    }
    const subscriptions = new Map();
    // These local navigation commands select Valve-owned screens. Commands
    // involving remote peers, checkout, account changes or programs have no
    // Xbox protocol handler here.
    const localNavigation = /^(?:open\/(?:library|games|downloads|friends|screenshots|storefront|bigpicture(?:\/(?:games|librarydetails|storefront))?)|nav\/(?:games|downloads)|(?:open\/)?settings(?:\/[a-z0-9_-]+)?)$/;
    const overlayRoute = 'open/apprunningcontrols';
    let pendingOverlay = 0;
    function showGameOverlay(appId) {
        pendingOverlay = appId;
        if (appId && appId === activeApp() && subscriptions.get(overlayRoute)?.size) {
            pendingOverlay = 0;
            client.URL.ExecuteSteamURL('steam://' + overlayRoute + '/' + appId);
        }
    }
    client.URL = {
        RegisterForRunSteamURL(key, callback) {
            if (typeof key !== 'string' || typeof callback !== 'function') throw new TypeError('Expected a Steam route and callback');
            const listeners = subscriptions.get(key) || new Set();
            subscriptions.set(key, listeners);
            listeners.add(callback);
            if (key === overlayRoute && pendingOverlay) queueMicrotask(() => showGameOverlay(pendingOverlay));
            return {unregister() {
                listeners.delete(callback);
                if (!listeners.size && subscriptions.get(key) === listeners) subscriptions.delete(key);
            }};
        },
        ExecuteSteamURL(url) {
            if (typeof url !== 'string' || !url.startsWith('steam://') || url.length > 256)
                throw new Error('Unsupported Steam navigation');
            const target = url.slice('steam://'.length);
            const overlay = /^open\/apprunningcontrols\/([1-9][0-9]{0,7})$/.exec(target);
            if (!(overlay && String(activeApp()) === overlay[1]) && !localNavigation.test(target))
                throw new Error('Unsupported Steam navigation');
            // Valve registers the settings route once and parses its optional section.
            const key = overlay ? overlayRoute : target.replace(/^(open\/settings|settings)\/.+$/, '$1');
            const handlers = subscriptions.get(key);
            if (!handlers?.size) throw new Error('Steam navigation is unavailable');
            for (const callback of [...handlers]) callback(0, url);
        },
        async GetSteamURLList(names) {
            const result = Object.create(null);
            for (const name of names) {
                const value = resolve(name);
                if (value) result[name] = value;
            }
            return result;
        },
        RegisterForSteamURLChanges(callback) {
            window.addEventListener('languagechange', callback);
            return {unregister() { window.removeEventListener('languagechange', callback); }};
        }
    };
    return showGameOverlay;
}
