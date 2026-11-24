// Release 1794536281, SystemNetworkStore (37176) consumes CMsgNetworkDevicesData
// and a separate connectivity test result. Its CM connection remains Valve's.
function installXboxNetwork(client, getRuntime, send, host) {
    const devices = new Set(), connectivity = new Set();
    let revision = 0, reading = false, active = true;

    function publish(snapshot) {
        const runtime = getRuntime();
        const network = runtime(20887), enums = runtime(94601);
        const result = enums.ckP;
        const list = [];
        const connected = snapshot?.level === 'internet' || snapshot?.level === 'local';
        const wifi = snapshot?.kind === 'wifi';
        if (connected && Number.isInteger(snapshot.device) && snapshot.device > 0) {
            const device = {id: snapshot.device, estate: network.$J.Connected,
                etype: wifi ? network.WO.Wireless : snapshot.kind === 'ethernet' ? network.WO.Wired : network.WO.Unknown,
                // This release reads the preferred route from the ip4 submessage for both IP families.
                ip4: {is_default_route: true}, ip6: {}};
            if (wifi) {
                const ap = {id: snapshot.accessPoint, is_active: true};
                if (typeof snapshot.ssid === 'string') ap.ssid = snapshot.ssid;
                if (Number.isInteger(snapshot.bars)) ap.estrength = Math.round(snapshot.bars * 4 / 5);
                device.wireless = {aps: [ap]};
            } else if (snapshot.kind === 'ethernet') device.wired = {is_cable_present: true};
            list.push(device);
        }
        const body = {devices: list};
        if (connected && wifi) body.is_wifi_enabled = true;
        const bytes = runtime(78544).co.fromObject(body).serializeBinary();
        const state = {eFakeState: enums.jrQ.k_ENetFakeLocalSystemState_Normal, bChecking: false,
            eConnectivityTestResult: snapshot?.level === 'offline' ? result.k_EConnectivityTestResult_NoLAN :
                !list.length ? result.k_EConnectivityTestResult_Unknown :
                snapshot.level === 'internet' ? result.k_EConnectivityTestResult_Connected : result.k_EConnectivityTestResult_Failed};
        for (const cb of devices) cb(bytes);
        for (const cb of connectivity) cb(state);
    }

    async function refresh() {
        const requested = ++revision;
        if (reading || !active || (!devices.size && !connectivity.size)) return;
        reading = true;
        let snapshot;
        try { snapshot = await send('network-state'); }
        catch (_) { snapshot = {level: 'unknown'}; }
        finally { reading = false; }
        if (!active) return;
        if (requested !== revision) { refresh(); return; }
        publish(snapshot);
    }
    function register(set, callback) {
        if (typeof callback !== 'function') throw new TypeError('Expected a network listener');
        set.add(callback);
        refresh();
        return {unregister() { set.delete(callback); }};
    }
    function onMessage({data}) { if (data?.event === 'xbox-network-changed') refresh(); }
    host.addEventListener('message', onMessage);
    window.addEventListener('pagehide', () => {
        active = false;
        devices.clear(); connectivity.clear();
        host.removeEventListener('message', onMessage);
    }, {once: true});
    client.System.Network = {
        RegisterForDeviceChanges: callback => register(devices, callback),
        RegisterForConnectivityTestChanges: callback => register(connectivity, callback),
        ForceTestConnectivity: refresh
    };
}
