// Bind the platform facts WebView2 actually exposes to the signed Steam context.
function installXboxLifecycle(client, getRuntime) {
    if (typeof xboxNativeWindow === 'undefined') return;
    const {deviceFamily, deviceFamilyVersion, devToolsEnabled} = xboxNativeWindow;
    if (typeof devToolsEnabled === 'boolean') {
        (client._internal ||= {}).IsDebuggingEnabled = () => devToolsEnabled;
    }

    const zoneObservers = new Set();
    let timeZone, zoneTimer;
    const zoneKey = 'xbox-steamui:machine:timezone', languageKey = 'xbox-steamui:machine:language';
    function currentZone() { return localStorage.getItem(zoneKey) || new Intl.DateTimeFormat().resolvedOptions().timeZone; }
    function checkZone() {
        const next = currentZone();
        if (next === timeZone) return;
        timeZone = next;
        for (const fn of zoneObservers) fn(next);
    }
    (client.Settings ||= {}).RegisterForTimeZoneChange = callback => {
        if (typeof callback !== 'function') throw new TypeError('Expected a time zone listener');
        zoneObservers.add(callback);
        if (!zoneTimer) {
            timeZone = currentZone();
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
    client.Settings.GetWindowed = async () => {
        const {width, height, scale, screenWidth, screenHeight} = xboxNativeWindow;
        if (![width, height, scale, screenWidth, screenHeight].every(n => Number.isFinite(n) && n > 0))
            throw new Error('Xbox window dimensions are unavailable');
        return Math.round(width * scale) < screenWidth || Math.round(height * scale) < screenHeight;
    };
    client.Settings.GetTimeZone = async () => currentZone();
    client.Settings.GetAvailableTimeZones = async () => {
        const zones = new Set([currentZone(), 'UTC', ...Intl.supportedValuesOf('timeZone')]);
        return [...zones].map(timezoneID => {
            const title = new Intl.DateTimeFormat(undefined, {timeZone: timezoneID, timeZoneName: 'longGeneric'})
                .formatToParts().find(p => p.type === 'timeZoneName')?.value;
            return {timezoneID, timezoneLocalizationToken: title || timezoneID, regionsLocalizationToken: timezoneID};
        });
    };
    client.Settings.SetTimeZone = zone => {
        // The signed renderer owns its display preference. Other Xbox apps use the OS time zone.
        const canonical = new Intl.DateTimeFormat(undefined, {timeZone: zone}).resolvedOptions().timeZone;
        localStorage.setItem(zoneKey, canonical);
        checkZone();
    };
    client.Settings.GetCurrentLanguage = async () => localStorage.getItem(languageKey) || steamXboxLanguage() || 'english';
    client.Settings.GetAvailableLanguages = async () => {
        const runtime = getRuntime();
        await runtime.e(2561);
        // The actual setup wizard gets its localized language names from 82561.
        const catalog = runtime(82561).default;
        return Object.keys(catalog).filter(runtime(4726).VD).map(strShortName => ({
            language: runtime(94601).sfN(strShortName), strShortName
        }));
    };
    client.Settings.SetCurrentLanguage = language => {
        if (!getRuntime()(4726).VD(language)) throw new Error('Unsupported Steam language');
        localStorage.setItem(languageKey, language);
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
    // The native compositor owns only SharedJSContext and SP BPM. Local
    // menus are frames in SP BPM. None of these is a WebChat overlay browser.
    client.WebChat = {
        RegisterOverlayChatBrowserInfoChanged: dialogRegistration(),
        GetOverlayChatBrowserInfo: async () => []
    };
    if (deviceFamily === 'Windows.Xbox') {
        // Xbox Settings owns OS updates. Valve's first-run wizard consumes this
        // capability and skips its SteamOS updater on systems it cannot update.
        client.Updates = {
            RegisterForUpdateStateChanges(callback) {
                if (typeof callback !== 'function') throw new TypeError('Expected an update-state listener');
                let active = true;
                queueMicrotask(() => {
                    if (!active) return;
                    const runtime = getRuntime();
                    const msg = runtime(78544).q.fromObject({state: runtime(25589)._U, supports_os_updates: false});
                    callback(msg.serializeBinary());
                });
                return {unregister() { active = false; }};
            }
        };
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
