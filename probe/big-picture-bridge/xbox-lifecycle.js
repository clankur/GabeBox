// Bind the platform facts WebView2 actually exposes to the signed Steam context.
function installXboxLifecycle(client) {
    if (typeof xboxNativeWindow === 'undefined') return;
    const {deviceFamily, deviceFamilyVersion, devToolsEnabled} = xboxNativeWindow;
    if (typeof devToolsEnabled === 'boolean') {
        (client._internal ||= {}).IsDebuggingEnabled = () => devToolsEnabled;
    }

    const zoneObservers = new Set();
    let timeZone, zoneTimer;
    function checkZone() {
        const next = new Intl.DateTimeFormat().resolvedOptions().timeZone;
        if (next === timeZone) return;
        timeZone = next;
        for (const fn of zoneObservers) fn(next);
    }
    (client.Settings ||= {}).RegisterForTimeZoneChange = callback => {
        if (typeof callback !== 'function') throw new TypeError('Expected a time zone listener');
        zoneObservers.add(callback);
        if (!zoneTimer) {
            timeZone = new Intl.DateTimeFormat().resolvedOptions().timeZone;
            zoneTimer = setInterval(checkZone, 60000);
            window.addEventListener('focus', checkZone);
        }
        queueMicrotask(() => { if (zoneObservers.has(callback)) callback(timeZone); });
        return {unregister() {
            zoneObservers.delete(callback);
            if (!zoneObservers.size) {
                clearInterval(zoneTimer);
                zoneTimer = undefined;
                window.removeEventListener('focus', checkZone);
            }
        }};
    };
    // The two Steam-owned settings subscriptions are optional when Settings is
    // absent. The host does not have a desktop Steam settings store.
    client.Settings.RegisterForSettingsChanges = unavailable;
    client.Settings.RegisterForSettingsArrayChanges = unavailable;
    client.Settings.GetWindowed = async () => {
        const {width, height, scale, screenWidth, screenHeight} = xboxNativeWindow;
        if (![width, height, scale, screenWidth, screenHeight].every(n => Number.isFinite(n) && n > 0))
            throw new Error('Xbox window dimensions are unavailable');
        return Math.round(width * scale) < screenWidth || Math.round(height * scale) < screenHeight;
    };

    // Valve mounts these dialogs with the gamepad root and requires a removable
    // listener even when the host has no events. There is no event producer for
    // SteamVR installation, desktop DRM notifications or hardware-save prompts.
    function dialogRegistration() {
        const observers = new Set();
        return callback => {
            if (typeof callback !== 'function') throw new TypeError('Expected a dialog listener');
            const subscription = {callback};
            observers.add(subscription);
            return {unregister() { observers.delete(subscription); }};
        };
    }
    (client.OpenVR ||= {}).RegisterForInstallDialog = dialogRegistration();
    client.Apps.RegisterForDRMFailureResponse = dialogRegistration();
    client.User.RegisterShowSaveHardwareDialog = dialogRegistration();
    client.User.OnCloseSaveHardwareDialog = () => {}; // No native save dialog was presented.
    if (deviceFamily === 'Windows.Xbox') {
        client.System.IsDeckFactoryImage = async () => false;
        if (typeof deviceFamilyVersion === 'string' && /^\d{1,20}$/.test(deviceFamilyVersion)) {
            const version = BigInt(deviceFamilyVersion);
            if (version > 0n && version < (1n << 64n)) {
                const parts = [48n, 32n, 16n, 0n].map(offset => Number((version >> offset) & 0xffffn));
                client.System.GetSystemInfo = async () => ({
                    nSteamVersion: xboxSteamReleaseVersion,
                    sOSVariantId: deviceFamily,
                    sOSVersionId: parts.join('.'),
                    sOSBuildId: String(parts[2])
                });
            }
        }
    }
    client.System.RegisterForBatteryStateChanges = callback => {
        if (typeof callback !== 'function') throw new TypeError('Expected a battery listener');
        if (typeof navigator.getBattery !== 'function') return undefined;
        const events = ['chargingchange', 'levelchange', 'chargingtimechange', 'dischargingtimechange'];
        let battery, closed = false, observed = false;
        function update() {
            const {charging, chargingTime, dischargingTime, level} = battery;
            if (level < 0 || level > 1 || !Number.isFinite(level)) return;
            // The Battery Status API also reports "charging, 100%, 0s" when
            // no battery exists. Only publish after a real battery observation.
            observed ||= !charging || level < 1 || (chargingTime > 0 && Number.isFinite(chargingTime));
            if (!observed) return;
            const remaining = charging ? chargingTime : dischargingTime;
            callback({bHasBattery: true, flLevel: level, eACState: charging ? 2 : 1,
                eBatteryState: level === 1 ? 3 : charging ? 2 : 1,
                nSecondsRemaining: Number.isFinite(remaining) ? remaining : undefined,
                bShutdownRequested: false, bNotificationToastEnabled: false});
        }
        navigator.getBattery().then(manager => {
            if (closed) return;
            battery = manager;
            for (const event of events) manager.addEventListener(event, update);
            update();
        }, () => {}); // Unavailable power state does not imply a charge level.
        return {unregister() {
            closed = true;
            if (battery) for (const event of events) battery.removeEventListener(event, update);
        }};
    };

    // These subscriptions in BasicUIStore.InitWithoutUser are fire-and-forget.
    // The Xbox host supplies no desktop launcher, Steam shutdown, remote stream,
    // overlay, microtransaction, console output or Steam virtual keyboard events.
    // Returning an absent handle prevents claiming a live event source.
    function unavailable() { return undefined; }
    Object.assign(client.UI, {
        RegisterForDifferentLauncherRequested: unavailable,
        RegisterForErrorCondition: unavailable,
        RegisterForKioskModeResetSignal: unavailable
    });
    Object.assign(client.User, {
        RegisterForShutdownStart: unavailable,
        RegisterForShutdownDone: unavailable,
        RegisterForShutdownState: unavailable,
        RegisterForShutdownFailed: unavailable
    });
    Object.assign(client.Input, {
        RegisterForGameKeyboardMessages: unavailable,
        RegisterForUserKeyboardMessages: unavailable,
        RegisterForUserDismissKeyboardMessages: unavailable
    });
    client.RemotePlay = {
        RegisterForRemoteClientStarted: unavailable,
        RegisterForRemoteClientStopped: unavailable
    };
    client.Overlay = {
        RegisterForMicroTxnAuth: unavailable,
        RegisterForMicroTxnAuthDismiss: unavailable,
        RegisterForOverlayActivated: unavailable
    };
    client.Console = {RegisterForSpewOutput: unavailable};
}
