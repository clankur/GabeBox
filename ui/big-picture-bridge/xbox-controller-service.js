// This endpoint serves read-only Xbox input enumeration inside Valve's shared
// context. Steam CM and all account, cloud and transaction requests remain
// outside it. Each response uses the original generated Valve message class.
function installXboxControllerService(runtime, gamepads) {
    if (!runtime?.m?.[45121]) return;
    const defaults = runtime(52660).OI();
    if (defaults.GetDefaultTransport()) return;
    const methods = new Set(['SteamInputManager.GetControllerList#1', 'SteamInputManager.GetControllerAccessibilityStrings#1',
        'SteamInputManager.WaitInitialControllerStateEnumerated#1', 'SteamInputManager.ShouldTritonPairInOobe#1']);
    defaults.SetDefaultTransport({
        async SendMsg(method, request, response, options) {
            if (options?.eClientExecutionSite !== 2 || !methods.has(method))
                throw new Error('Steam client service unavailable: ' + method);
            if (method === 'SteamInputManager.ShouldTritonPairInOobe#1') {
                // Windows.Gaming.Input does not provide Steam Triton pairing.
                // Valve's OOBE handles this specific unsuccessful response.
                const reply = runtime(98237).w.InitFromObject(response, {});
                reply.Hdr().set_eresult(20); // k_EResultServiceUnavailable
                return reply;
            }
            if (method === 'SteamInputManager.GetControllerAccessibilityStrings#1') {
                // Xbox supplies logical gamepad controls but no Steam hardware
                // accessibility strings. The pinned store can load zero strings.
                const reply = runtime(98237).w.InitFromObject(response, {strings: []});
                reply.Hdr().set_eresult(1);
                return reply;
            }
            const view = getXboxGamepadWindow();
            // Browsers expose only the gamepads made available to this view.
            // A successful empty enumeration is distinct from a denied read.
            const pads = await gamepads.enumerate(view);
            const body = method === 'SteamInputManager.GetControllerList#1' ? {
                controllers: pads.map(pad => ({controller_index: pad.index, name: pad.id, initialized: true}))
            } : {};
            const reply = runtime(98237).w.InitFromObject(response, body);
            reply.Hdr().set_eresult(1);
            return reply;
        },
        SendNotification() { return false; }
    });
}
