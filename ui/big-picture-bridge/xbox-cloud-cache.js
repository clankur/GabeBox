// SteamUI 1794536281, module 40992. WriteKey is the local backing for
// cloudstorage/<key>.json; the actual Steam CM sync remains owned by Valve.
function installXboxCloudCache(client, send) {
    const host = window.chrome.webview;
    const index = 'cloud-storage-namespaces';
    const keyPattern = /^cloud-storage-namespace-[134](?:\.modified)?$/;
    const maxBytes = 3 * 1024 * 1024;
    let database, owner, revoked = false, pending = 0, tail = Promise.resolve();

    function validKey(key) { return typeof key === 'string' && (key === index || keyPattern.test(key)); }
    function validData(key, text) {
        if (typeof text !== 'string' || text.length > maxBytes || new TextEncoder().encode(text).length > maxBytes)
            throw new Error('Invalid Steam UI cache size');
        const rows = JSON.parse(text);
        if (!Array.isArray(rows) || rows.length > 10000) throw new Error('Invalid Steam UI cache data');
        const pairs = row => Array.isArray(row) && row.length === 2;
        const ok = key === index ? rows.length <= 3 && rows.every(row => pairs(row) && [1,3,4].includes(row[0]) &&
            typeof row[1] === 'string' && /^(no-version|\d{1,20})$/.test(row[1])) :
            key.endsWith('.modified') ? rows.every(item => typeof item === 'string') :
            rows.every(row => pairs(row) && typeof row[0] === 'string' && row[1] && typeof row[1] === 'object' && !Array.isArray(row[1]));
        if (!ok) throw new Error('Invalid Steam UI cache records');
    }
    async function account() {
        if (revoked) throw new Error('Steam UI cache session ended');
        const id = (await send('session'))?.steamId;
        if (owner && id !== owner) revoked = true;
        if (revoked || typeof id !== 'string' || !/^[1-9]\d{16,19}$/.test(id) ||
            (id.length === 20 && id > '18446744073709551615')) throw new Error('Steam UI cache session ended');
        owner = id;
        return id;
    }
    function open() {
        if (!database) database = new Promise((resolve, reject) => {
            const request = indexedDB.open('xbox-steamui-local-cloud-cache', 1);
            request.onupgradeneeded = () => request.result.createObjectStore('documents');
            request.onsuccess = () => {
                const db = request.result;
                db.onversionchange = () => { db.close(); database = undefined; };
                resolve(db);
            };
            request.onerror = () => { database = undefined; reject(request.error); };
        });
        return database;
    }
    function access(key, value) {
        if (!validKey(key) || pending >= 16) return Promise.reject(new Error('Steam UI cache unavailable'));
        if (value !== undefined) validData(key, value);
        ++pending;
        const work = tail.then(async () => {
            const db = await open();
            const id = await account();
            const data = await new Promise((resolve, reject) => {
                const tx = db.transaction('documents', value === undefined ? 'readonly' : 'readwrite');
                const store = tx.objectStore('documents');
                const req = value === undefined ? store.get([id,key]) : store.put(value, [id,key]);
                tx.oncomplete = () => resolve(value === undefined ? req.result : undefined);
                tx.onabort = () => reject(tx.error || new Error('Steam UI cache transaction aborted'));
            });
            await account(); // Never return a previous user's record after a session change.
            return data;
        });
        tail = work.then(() => { --pending; }, () => { --pending; });
        return work;
    }
    const reportedReads = new Set();
    let readAdapterInstalled = false;
    function reportRead(key, status) {
        if (reportedReads.has(key)) return;
        reportedReads.add(key);
        const label = key === index ? 'index' : key.slice('cloud-storage-'.length);
        try { host.postMessage({method: 'startup-cloud-cache-read', label, status}); } catch (_) {}
    }
    function installLocalReads(axios) {
        if (readAdapterInstalled || typeof axios?.interceptors?.request?.use !== 'function' ||
            typeof axios.AxiosError !== 'function') return;
        readAdapterInstalled = true;
        axios.interceptors.request.use(config => {
            if (config.method !== 'get' || config.withCredentials !== false || config.params || config.baseURL ||
                config.auth || config.headers?.has?.('Authorization') || typeof config.url !== 'string') return config;
            const path = /^\/cloudstorage\/([a-z0-9.-]+)\.json$/.exec(config.url);
            if (!path || !validKey(path[1])) return config;
            const key = path[1];
            config.adapter = async request => {
                let timer, data, status;
                try {
                    const body = await Promise.race([access(key), new Promise((_, reject) => {
                        timer = setTimeout(() => reject(new Error('Steam UI cache read timed out')), 5000);
                    })]);
                    if (body === undefined) status = 404;
                    else {
                        data = JSON.parse(body);
                        status = 200;
                    }
                } catch (error) {
                    status = error?.message === 'Steam UI cache session ended' ? 403 : 503;
                } finally {
                    clearTimeout(timer);
                }
                reportRead(key, status);
                const response = {data, status, statusText: {200: 'OK', 403: 'Forbidden', 404: 'Not Found', 503: 'Service Unavailable'}[status],
                    headers: {'content-type': 'application/json'}, config: request};
                if (status !== 200)
                    throw new axios.AxiosError('Steam UI cache read returned HTTP ' + status,
                        status < 500 ? axios.AxiosError.ERR_BAD_REQUEST : axios.AxiosError.ERR_BAD_RESPONSE,
                        request, undefined, response);
                return response;
            };
            return config;
        });
    }
    client.CloudStorage = {async WriteKey(key, json) {
        if (typeof json !== 'string') throw new TypeError('Expected Steam UI cache data');
        await access(key, json);
    }};
    host.addEventListener('message', ({data}) => {
        if (data?.event !== 'xbox-cloud-cache-get' || !Number.isSafeInteger(data.token) || data.token <= 0 ||
            !validKey(data.key)) return;
        const reply = (status, body) => host.postMessage({method: 'cloud-cache-result', token: data.token, status, body});
        access(data.key).then(body => reply(body === undefined ? 404 : 200, body),
            error => reply(error.message === 'Steam UI cache session ended' ? 403 : 503));
    });
    return installLocalReads;
}
