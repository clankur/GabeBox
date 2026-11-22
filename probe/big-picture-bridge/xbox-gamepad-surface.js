// Valve's pinned gamepad window manager writes its React portal into the
// returned blank document. Use a real WindowProxy in the existing WebView.
function createXboxGamepadSurface(seed, host) {
    let frame;
    const params = {createflags:'6292754', minwidth:'853', minheight:'534',
        pid:String(seed.pid), browser:'1', browserType:'3', useragent:'Valve Steam Gamepad'};
    function approved(address, name) {
        if ((name !== 'SP BPM' && name !== 'SP BPM_uid' + seed.pid) ||
            typeof address !== 'string' || address.length > 2048 || !address.startsWith('about:blank?')) return false;
        const url = new URL(address);
        return !url.hash && url.searchParams.size === Object.keys(params).length &&
            Object.entries(params).every(([key,value]) => url.searchParams.get(key) === value);
    }
    function close(current) {
        if (current !== frame) return;
        frame = undefined;
        current.remove();
        host.postMessage('xbox-window-frame-closed');
    }
    return {
        open(address, name) {
            if (!approved(address, name)) return null;
            if (frame?.isConnected) return frame.contentWindow;
            const element = document.createElement('iframe');
            element.id = 'xbox-steam-gamepad-surface';
            element.name = name;
            element.title = 'Steam Big Picture';
            // The original owner schedules controller input in this window.
            // Valve's exit dialog handles a local HTML form submit. Allow its
            // submit event while form-action and native navigation policy block destinations.
            element.setAttribute('sandbox', 'allow-same-origin allow-scripts allow-forms');
            element.setAttribute('csp', "default-src 'none'; script-src 'none'; style-src 'unsafe-inline' https://steamloopback.host; " +
                "img-src data: https://steamloopback.host https://*.steamstatic.com; font-src https://steamloopback.host; " +
                "media-src https://steamloopback.host https://*.steamstatic.com; frame-src about:; connect-src 'none'; form-action 'none'; base-uri 'none'");
            element.style.cssText = 'position:fixed;inset:0;width:100%;height:100%;display:block;border:0;z-index:2147483640';
            document.documentElement.appendChild(element);
            frame = element;
            const popup = frame.contentWindow;
            // The initial about:blank document doesn't receive a network CSP
            // header. Apply the form policy to the document before Valve writes its portal.
            const policy = popup.document.createElement('meta');
            policy.httpEquiv = 'Content-Security-Policy';
            policy.content = "form-action 'none'";
            popup.document.head.appendChild(policy);
            popup.opener = window;
            popup.close = () => close(element);
            host.postMessage('xbox-window-frame-open');
            return popup;
        },
        update(state) {
            if (!frame) return;
            const shown = state.visible && state.popupVisible;
            const layout = state.warming && state.popupOpen && state.layoutPopup;
            frame.style.display = shown || layout ? 'block' : 'none';
            if (shown && !state.warming && state.focused !== false && !frame.contentDocument.hasFocus()) frame.contentWindow.focus();
        }
    };
}
