// Steam's 1794536281 navigation provider consumes logical button edges. It
// implements hold/repeat itself. These identifiers are from sp.js module 43251.
function installXboxInput(client, gamepads) {
    const digital = new Set(), analog = new Set(), system = new Set(), unbound = new Set(), keyboards = new Set();
    const states = new Map();
    // W3C standard buttons A, B, X, Y, bumpers, triggers, View, Menu and stick clicks.
    const buttonCodes = [0, 1, 2, 3, 30, 31, 28, 29, 35, 36, 25, 41];
    let analogEnabled = false, unboundSignature, deviceSignature, timer, timerWindow, stopped = false;
    const denied = new WeakSet();

    function dispatch(listeners, ...args) {
        for (const listener of [...listeners]) listener(...args);
    }
    function padDirections(pad, last) {
        const x = pad.axes[0] || 0, y = pad.axes[1] || 0;
        // Hysteresis prevents duplicate navigation around the stick threshold.
        return [-y, y, -x, x].map((value, i) =>
            !!pad.buttons[12 + i]?.pressed || value >= (last.has(20 + i) ? 0.45 : 0.6));
    }
    function release(index, state) {
        for (const code of state.down) dispatch(digital, index, code, false, 0, 0);
        if (analogEnabled && (state.x || state.y)) dispatch(analog, index, 24, false, 0, 0);
    }
    function samplePad(pad) {
        let state = states.get(pad.index);
        if (state && state.name !== pad.id) {
            release(pad.index, state);
            states.delete(pad.index);
            state = undefined;
        }
        if (!state) {
            state = {name: pad.id, down: new Set(), x: 0, y: 0, guide: false};
            states.set(pad.index, state);
        }
        const next = new Set();
        buttonCodes.forEach((code, index) => { if (pad.buttons[index]?.pressed) next.add(code); });
        // Valve's navigation map uses LEFTSTICK_* for UI directions. The
        // D-pad and left stick share those actions, including overlapping holds.
        padDirections(pad, state.down).forEach((pressed, index) => { if (pressed) next.add(20 + index); });
        for (const code of state.down) if (!next.has(code)) dispatch(digital, pad.index, code, false, 0, 0);
        for (const code of next) if (!state.down.has(code)) dispatch(digital, pad.index, code, true, 0, 0);
        state.down = next;
        // Xbox reserves its Guide button. Use the same View + Menu chord as
        // the game presenter to open Valve's menu inside the Steam window.
        const guide = !!pad.buttons[16]?.pressed || (!!pad.buttons[8]?.pressed && !!pad.buttons[9]?.pressed);
        if (guide && !state.guide) dispatch(system, {eKey: 0, nControllerIndex: pad.index});
        state.guide = guide;
        const x = pad.axes[0] || 0, y = pad.axes[1] || 0;
        if (analogEnabled && (state.x !== x || state.y !== y)) dispatch(analog, pad.index, 24, false, x, y);
        state.x = x;
        state.y = y;
    }
    function read(view) {
        const pads = gamepads.read(view);
        const inventory = JSON.stringify(pads.map(pad => [pad.index, pad.id, pad.mapping]));
        if (inventory !== deviceSignature) {
            if (deviceSignature !== undefined) notifyXboxControllerListChanged?.();
            deviceSignature = inventory;
        }
        const mapped = new Set();
        const unmapped = [];
        for (const pad of pads) {
            if (pad.mapping === 'standard') {
                mapped.add(pad.index);
                samplePad(pad);
            } else {
                // The gamepad configurator consumes strName for unbound devices.
                unmapped.push({strName: pad.id});
            }
        }
        for (const [index, state] of states) {
            if (mapped.has(index)) continue;
            release(index, state);
            states.delete(index);
        }
        const signature = JSON.stringify(unmapped);
        if (signature !== unboundSignature) {
            unboundSignature = signature;
            dispatch(unbound, unmapped);
        }
    }
    function releaseAll() {
        for (const [index, state] of states) release(index, state);
        states.clear();
    }
    function popupClosing() {
        timerWindow.clearTimeout(timer);
        timer = timerWindow = undefined;
        releaseAll();
        window.setTimeout(schedule, 0);
    }
    function schedule() {
        if (stopped || timer || !(digital.size || analog.size || system.size || unbound.size)) return;
        const view = getXboxGamepadWindow();
        if (timerWindow !== view) {
            if (timerWindow && timerWindow !== window) timerWindow.removeEventListener('pagehide', popupClosing);
            timerWindow = view;
            if (view !== window) view.addEventListener('pagehide', popupClosing, {once: true});
        }
        timer = view.setTimeout(() => {
            timer = undefined;
            try {
                if (view.document.hidden) releaseAll();
                else read(view);
            } catch (error) {
                if (error.name !== 'SecurityError') throw error;
                releaseAll();
                if (!denied.has(view)) {
                    console.warn('Gamepad input is unavailable in this window', error.name);
                    denied.add(view);
                }
            } finally { schedule(); }
        }, digital.size || analog.size || system.size ? 16 : 250);
    }
    function listen(listeners, callback) {
        if (typeof callback !== 'function') throw new TypeError('Expected a controller listener');
        listeners.add(callback);
        if (listeners === unbound) unboundSignature = undefined;
        schedule();
        return {unregister() { listeners.delete(callback); }};
    }

    const seed = typeof xboxNativeWindow === 'undefined' ? undefined : xboxNativeWindow;
    let keyboardPresent = seed?.keyboardPresent;
    window.chrome?.webview?.addEventListener('message', ({data}) => {
        const state = data?.state;
        if (!seed || data?.event !== 'xbox-window' || state?.viewId !== seed.viewId || state.pid !== seed.pid ||
            (state.keyboardPresent !== 0 && state.keyboardPresent !== 1) || state.keyboardPresent === keyboardPresent) return;
        keyboardPresent = state.keyboardPresent;
        dispatch(keyboards, keyboardPresent);
    });
    client.Input = {
        RegisterForUnboundControllerListChanges: callback => listen(unbound, callback),
        RegisterForControllerInputMessages: callback => listen(digital, callback),
        RegisterForControllerAnalogInputMessages: callback => listen(analog, callback),
        EnableControllerAnalogInputMessages(enabled) { analogEnabled = !!enabled; },
        RegisterForKeyboardDeviceChanges(callback) {
            if (typeof callback !== 'function') throw new TypeError('Expected a keyboard listener');
            keyboards.add(callback);
            queueMicrotask(() => {
                if (keyboards.has(callback) && (keyboardPresent === 0 || keyboardPresent === 1)) callback(keyboardPresent);
            });
            return {unregister() { keyboards.delete(callback); }};
        },
        RegisterForDualSenseUpdateNotification() {
            // The constructor requests this unconditionally. Web Gamepad cannot
            // query controller firmware. Leave the subscription unavailable.
            return undefined;
        }
    };
    (client.System.UI ||= {}).RegisterForSystemKeyEvents = callback => listen(system, callback);
    window.addEventListener('pagehide', () => { stopped = true; }, {once: true});
}
