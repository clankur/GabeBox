// Steam's 1794536281 navigation provider consumes logical button edges. It
// implements hold/repeat itself. These identifiers are from sp.js module 43251.
function installXboxInput(client, gamepads, storeBrowser) {
    const digital = new Set(), analog = new Set(), system = new Set(), unbound = new Set(), keyboards = new Set();
    const states = new Map();
    let keyboardActive = false;
    // W3C standard buttons A, B, X, Y, bumpers, triggers, View, Menu and stick clicks.
    const buttonCodes = [0, 1, 2, 3, 30, 31, 28, 29, 35, 36, 25, 41];
    let analogEnabled = false, unboundSignature, deviceSignature, devices, timer, timerWindow, stopped = false;
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
    function button(index, state, code, down) {
        if (down && !keyboardActive && storeBrowser?.button(index, code, true)) {
            state.storeButtons.add(code);
            return;
        }
        // A press can change routes before release. Its original recipient
        // still owns the release, including when Valve has hidden the Store.
        if (!down && state.storeButtons.delete(code)) {
            storeBrowser.button(index, code, false);
            return;
        }
        dispatch(digital, index, code, down, 0, 0);
    }
    function release(index, state) {
        for (const code of state.down) button(index, state, code, false);
        if (analogEnabled && (state.x || state.y)) dispatch(analog, index, 24, false, 0, 0);
    }
    function keyboardActionset(enabled) {
        if (typeof enabled !== 'boolean' || keyboardActive === enabled) return;
        keyboardActive = enabled;
        // Steam's on-screen keyboard consumes Valve's normal logical buttons.
        // Stop any Store navigation repeat immediately while leaving the real
        // up edge owned by the original press until the controller releases it.
        if (enabled) for (const [index, state] of states)
            for (const code of state.storeButtons) storeBrowser.button(index, code, false);
    }
    function insertKeyboardText(text) {
        if (!keyboardActive || typeof text !== 'string' || !text) return;
        const view = getXboxGamepadWindow();
        if (view === window) return;
        const doc = view.document, field = doc.activeElement;
        if (!field || !field.matches('input[type="search"],input[type="text"],input:not([type]),textarea') ||
            field.disabled || field.readOnly) return;
        // Module 8844 encodes Enter, Backspace and arrow keys as 1 through 6.
        // Use Blink's editing transaction for text/deletion, so Valve receives
        // its normal input events and undo history. The active field must be
        // in the actual local gamepad document; never traverse another frame.
        if (text === '\x02') doc.execCommand('delete');
        else if (text === '\x01' && field.tagName === 'TEXTAREA') doc.execCommand('insertLineBreak');
        else if (text === '\x03' || text === '\x04') {
            const selection = view.getSelection();
            selection?.modify('move', text === '\x03' ? 'backward' : 'forward', 'character');
        } else if (text === '\x05' || text === '\x06') {
            const selection = view.getSelection();
            selection?.modify('move', text === '\x05' ? 'backward' : 'forward', 'line');
        } else if (!/[\x00-\x1f\x7f]/.test(text)) doc.execCommand('insertText', false, text);
    }
    function samplePad(pad) {
        let state = states.get(pad.index);
        if (state && state.name !== pad.id) {
            release(pad.index, state);
            states.delete(pad.index);
            state = undefined;
        }
        if (!state) {
            state = {name: pad.id, down: new Set(), storeButtons: new Set(), x: 0, y: 0, guide: false};
            states.set(pad.index, state);
        }
        const view = !!pad.buttons[8]?.pressed, menu = !!pad.buttons[9]?.pressed;
        const menuChord = view && menu;
        state.menuChord = menuChord || (state.menuChord && (view || menu));
        const next = new Set();
        buttonCodes.forEach((code, index) => {
            if (state.menuChord && (index === 8 || index === 9)) return;
            if (pad.buttons[index]?.pressed) next.add(code);
        });
        // Valve's navigation map uses LEFTSTICK_* for UI directions. The
        // D-pad and left stick share those actions, including overlapping holds.
        padDirections(pad, state.down).forEach((pressed, index) => { if (pressed) next.add(20 + index); });
        for (const code of state.down) if (!next.has(code)) button(pad.index, state, code, false);
        for (const code of next) if (!state.down.has(code)) button(pad.index, state, code, true);
        state.down = next;
        // Xbox reserves its Guide button. Use the same View + Menu chord as
        // the game presenter to open Valve's menu inside the Steam window.
        const guide = !!pad.buttons[16]?.pressed || menuChord;
        if (guide && !state.guide) dispatch(system, {eKey: 0, nControllerIndex: pad.index});
        state.guide = guide;
        const x = pad.axes[0] || 0, y = pad.axes[1] || 0;
        if (analogEnabled && (state.x !== x || state.y !== y)) dispatch(analog, pad.index, 24, false, x, y);
        state.x = x;
        state.y = y;
    }
    function read(view) {
        const pads = gamepads.read(view);
        const inventory = pads.map(pad => [pad.index, pad.id, pad.mapping]);
        const signature = JSON.stringify(inventory);
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
        const unboundDevices = JSON.stringify(unmapped);
        if (unboundDevices !== unboundSignature) {
            unboundSignature = unboundDevices;
            dispatch(unbound, unmapped);
        }
        if (signature !== deviceSignature) {
            const previous = devices;
            devices = inventory;
            deviceSignature = signature;
            if (previous) {
                const removed = previous.filter(([index, name, mapping]) =>
                    !inventory.some(([slot, id, mode]) => index === slot && name === id && mapping === mode));
                notifyXboxControllerListChanged?.(removed.map(([index]) => index));
            }
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
    function registerConfigListener(callback) {
        if (typeof callback !== 'function') throw new TypeError('Expected a Steam Input configuration listener');
        // Valve registers these events before its controller inventory query.
        // The Xbox host has no Steam configuration event producer yet.
        return {unregister() {}};
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
        ControllerKeyboardSendText: insertKeyboardText,
        SetKeyboardActionset: keyboardActionset,
        ModalKeyboardDismissed() { keyboardActionset(false); },
        SetWebBrowserActionset(value) { storeBrowser?.setActionset(value); },
        RegisterForUnboundControllerListChanges: callback => listen(unbound, callback),
        RegisterForControllerInputMessages: callback => listen(digital, callback),
        RegisterForControllerAnalogInputMessages: callback => listen(analog, callback),
        RegisterForControllerConfigInfoMessages: registerConfigListener,
        RegisterForControllerConfigCloudStateChanges: registerConfigListener,
        RegisterForControllerCommandMessages: registerConfigListener,
        RegisterForRemotePlayConfigChanges: registerConfigListener,
        RegisterForShowControllerLayoutPreviewMessages: registerConfigListener,
        RegisterForConfigSelectionChanges: registerConfigListener,
        async GetConfigForAppAndController() {
            // The configurator catches rejection and marks the selected layout
            // unavailable. A physical pad alone does not establish its layout.
            throw new Error('Steam Input configuration is unavailable on this Xbox runtime');
        },
        async StartEditingControllerConfigurationForAppIDAndControllerIndex() {
            // The running game's controller page loads the layout to edit while it renders.
            // Valve handles a rejection (the layout is marked as failed to load). A missing
            // method threw inside React's render instead (minified React error #520).
            throw new Error('Steam Input configuration is unavailable on this Xbox runtime');
        },
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
