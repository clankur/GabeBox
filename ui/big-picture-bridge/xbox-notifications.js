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
    const spec = runtime(744).f_.NotifyControllerListChangedHandler;
    notifyXboxControllerListChanged = () => {
        const event = runtime(98237).w.Init(spec.request, runtime(98089).bSr);
        event.Hdr().set_target_job_name(spec.name);
        registry.DispatchMsgToHandlers(event);
    };
}
