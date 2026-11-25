// The pinned WebUI transport owns Valve's protobuf notification dispatcher.
// Its normal desktop Init also connects to two authenticated client services.
// The Xbox host can register/remove local listeners without those connections.
let notifyXboxControllerListChanged;
function installXboxNotifications(runtime) {
    if (!runtime?.m?.[45121]) return;
    const defaults = runtime(52660).OI();
    if (defaults.GetDefaultHandlerRegistry() || defaults.GetDefaultTransport()) return;
    const registry = runtime(45121).N.messageHandlers;
    defaults.SetDefaultHandlerRegistry(registry);
    // Valve's native navigation provider invalidates its controller list on
    // this empty notification and requests a fresh typed inventory.
    const services = runtime(744).f_;
    const {w: Message, lI: Header} = runtime(98237);
    const {pV: Packet} = runtime(98890);
    function dispatch(spec, index) {
        const event = Message.Init(spec.request, runtime(98089).bSr);
        event.Hdr().set_target_job_name(spec.name);
        if (index !== undefined) event.Body().set_controller_index(index);
        // The registry reconstructs the typed body from the packet for each listener.
        registry.DispatchMsgToHandlers(Header.InitHeaderFromPacket(new Packet(event.Serialize())));
    }
    notifyXboxControllerListChanged = removed => {
        for (const index of removed) dispatch(services.NotifyControllerDisconnectedHandler, index);
        dispatch(services.NotifyControllerListChangedHandler);
    };
}
