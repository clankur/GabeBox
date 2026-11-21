// WebView2 can require a browser gesture before it exposes a controller.
// The native fallback reads the foreground Xbox view's Windows.Gaming.Input
// collection and never synthesizes the platform-reserved Guide button.
function installXboxGamepadReader() {
    const native = typeof xboxNativeWindow !== 'undefined' && xboxNativeWindow.nativeGamepads === true;
    let cached = [], request;
    if (native) window.chrome.webview.addEventListener('message', ({data}) => {
        if (data?.event !== 'xbox-gamepads' || !Array.isArray(data.pads)) return;
        cached = data.pads.map(pad => {
            const bits = [4, 8, 16, 32, 1024, 2048, 0, 0, 2, 1, 4096, 8192, 64, 128, 256, 512];
            const buttons = bits.map((mask, i) => ({pressed: i === 6 ? pad.lt >= .5 : i === 7 ? pad.rt >= .5 : !!(pad.bits & mask)}));
            return {connected: true, index: pad.slot, id: 'Gamepad ' + (pad.slot + 1), mapping: 'standard', buttons,
                axes: [pad.lx, -pad.ly, pad.rx, -pad.ry]};
        });
        if (request) request.resolve(cached);
    });
    function sampleNative() {
        if (!request) {
            let finish;
            const promise = new Promise((resolve, reject) => { finish = {resolve, reject}; });
            const timer = setTimeout(() => { request = undefined; finish.reject(new Error('Xbox input unavailable')); }, 1000);
            request = {promise, resolve(pads) { clearTimeout(timer); request = undefined; finish.resolve(pads); }};
            try { window.chrome.webview.postMessage('xbox-read-gamepads'); }
            catch (error) { clearTimeout(timer); request = undefined; finish.reject(error); }
            return promise;
        }
        return request.promise;
    }
    function browser(view) {
        try { return Array.from(view.navigator.getGamepads()).filter(pad => pad?.connected); }
        catch (error) { if (!native) throw error; return []; }
    }
    return {
        read(view) {
            const pads = browser(view);
            if (pads.length || !native) return pads;
            sampleNative().catch(() => { cached = []; });
            return cached;
        },
        async enumerate(view) {
            const pads = browser(view);
            return pads.length || !native ? pads : await sampleNative();
        }
    };
}
