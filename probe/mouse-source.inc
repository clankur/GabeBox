/* Native CoreWindow control for Windows.Devices.Input.MouseDevice. The UUIDs
 * and method order match Windows SDK 10.0.26100.0. */
typedef long long EventToken;
typedef struct MouseDevice MouseDevice;
typedef struct MouseHandler MouseHandler;
typedef struct MouseArgs MouseArgs;
typedef struct { LONG x, y; } MouseDelta;

typedef struct {
    void *QueryInterface, *AddRef, *Release, *GetIids, *GetRuntimeClassName, *GetTrustLevel;
    HRESULT (STDMETHODCALLTYPE *get_MouseDelta)(MouseArgs *, MouseDelta *);
} MouseArgsVtbl;
struct MouseArgs { const MouseArgsVtbl *lpVtbl; };

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(MouseHandler *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(MouseHandler *);
    ULONG (STDMETHODCALLTYPE *Release)(MouseHandler *);
    HRESULT (STDMETHODCALLTYPE *Invoke)(MouseHandler *, MouseDevice *, MouseArgs *);
} MouseHandlerVtbl;
struct MouseHandler {
    const MouseHandlerVtbl *lpVtbl;
    volatile LONG refs, phase, callbacks[2], motion[2], deltaError;
};

typedef struct {
    void *QueryInterface, *AddRef;
    ULONG (STDMETHODCALLTYPE *Release)(MouseDevice *);
    void *GetIids, *GetRuntimeClassName, *GetTrustLevel;
    HRESULT (STDMETHODCALLTYPE *add_MouseMoved)(MouseDevice *, MouseHandler *, EventToken *);
    HRESULT (STDMETHODCALLTYPE *remove_MouseMoved)(MouseDevice *, EventToken);
} MouseDeviceVtbl;
struct MouseDevice { const MouseDeviceVtbl *lpVtbl; };

typedef struct MouseDeviceStatics MouseDeviceStatics;
typedef struct {
    void *QueryInterface, *AddRef;
    ULONG (STDMETHODCALLTYPE *Release)(MouseDeviceStatics *);
    void *GetIids, *GetRuntimeClassName, *GetTrustLevel;
    HRESULT (STDMETHODCALLTYPE *GetForCurrentView)(MouseDeviceStatics *, MouseDevice **);
} MouseDeviceStaticsVtbl;
struct MouseDeviceStatics { const MouseDeviceStaticsVtbl *lpVtbl; };

static const IID IID_MOUSE_DEVICE_STATICS =
    {0x484a9045, 0x6d70, 0x49db, {0x8e, 0x68, 0x46, 0xff, 0xbd, 0x17, 0xd3, 0x8d}};
/* TypedEventHandler<MouseDevice, MouseEventArgs> */
static const IID IID_MOUSE_MOVED =
    {0x5d72e594, 0x28e4, 0x5895, {0xa3, 0x4b, 0xea, 0x91, 0x0f, 0x70, 0xfd, 0xbb}};
static const IID IID_CORE_WINDOW5 =
    {0x4b4ae1e1, 0x2e6d, 0x4eaa, {0xbd, 0xa1, 0x1c, 0x5c, 0xc1, 0xbe, 0xe1, 0x41}};

static ULONG STDMETHODCALLTYPE MouseHandlerAddRef(MouseHandler *self) { return (ULONG)InterlockedIncrement(&self->refs); }
static ULONG STDMETHODCALLTYPE MouseHandlerRelease(MouseHandler *self) { return (ULONG)InterlockedDecrement(&self->refs); }

static HRESULT STDMETHODCALLTYPE MouseHandlerQuery(MouseHandler *self, REFIID iid, void **out) {
    if (!out) return E_POINTER;
    *out = (void *)0;
    if (!GuidEqual(iid, &IID_IUNKNOWN) && !GuidEqual(iid, &IID_IAGILEOBJECT) &&
            !GuidEqual(iid, &IID_MOUSE_MOVED)) return E_NOINTERFACE;
    *out = self;
    MouseHandlerAddRef(self);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE OnMouseMoved(MouseHandler *self, MouseDevice *sender, MouseArgs *args) {
    (void)sender;
    const LONG phase = __atomic_load_n(&self->phase, __ATOMIC_RELAXED);
    InterlockedIncrement(&self->callbacks[phase]);
    MouseDelta delta = {0, 0};
    const HRESULT hr = args ? args->lpVtbl->get_MouseDelta(args, &delta) : E_POINTER;
    if (FAILED(hr)) {
        LONG unset = S_OK;
        __atomic_compare_exchange_n(&self->deltaError, &unset, hr, FALSE, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    } else if (delta.x || delta.y) {
        InterlockedIncrement(&self->motion[phase]);
    }
    return S_OK;
}

static const MouseHandlerVtbl mouseHandlerVtbl = {
    MouseHandlerQuery, MouseHandlerAddRef, MouseHandlerRelease, OnMouseMoved
};
static MouseHandler mouseHandler = {&mouseHandlerVtbl, 1, 0, {0, 0}, {0, 0}, S_OK};

static void MouseReportField(WCHAR *report, DWORD capacity, DWORD *used, PCWSTR name, DWORD value) {
    AppendText(report, capacity, used, name);
    AppendHex(report, capacity, used, value);
    AppendText(report, capacity, used, L"\r\n");
}

static DWORD MouseActivationMode(ICoreWindow *window) {
    IInspectableLike *activation = (IInspectableLike *)0;
    if (FAILED(window->lpVtbl->QueryInterface(window, &IID_CORE_WINDOW5, (void **)&activation)) || !activation)
        return (DWORD)-1;
    typedef HRESULT (STDMETHODCALLTYPE *GetActivationMode)(IInspectableLike *, int *);
    const GetActivationMode getMode = (GetActivationMode)((const void *const *)activation->lpVtbl)[7];
    int mode = -1;
    const HRESULT hr = getMode(activation, &mode);
    activation->lpVtbl->Release(activation);
    return SUCCEEDED(hr) ? (DWORD)mode : (DWORD)-1;
}

static HRESULT RunMouseSourceControl(ICoreWindow *window, ICoreDispatcher *dispatcher) {
    static const WCHAR className[] = L"Windows.Devices.Input.MouseDevice";
    HSTRING name = (HSTRING)0;
    MouseDeviceStatics *statics = (MouseDeviceStatics *)0;
    MouseDevice *mouse = (MouseDevice *)0;
    EventToken token = 0;
    HRESULT hr = WindowsCreateString(className, (DWORD)(sizeof(className) / sizeof(className[0]) - 1), &name);
    if (FAILED(hr)) return ReportStartupError(L"mouse-class-name-error", hr);
    hr = RoGetActivationFactory(name, &IID_MOUSE_DEVICE_STATICS, (void **)&statics);
    WindowsDeleteString(name);
    if (FAILED(hr) || !statics) return ReportStartupError(L"mouse-factory-error", FAILED(hr) ? hr : E_POINTER);
    hr = statics->lpVtbl->GetForCurrentView(statics, &mouse);
    statics->lpVtbl->Release(statics);
    if (FAILED(hr) || !mouse) return ReportStartupError(L"mouse-current-view-error", FAILED(hr) ? hr : E_POINTER);
    hr = mouse->lpVtbl->add_MouseMoved(mouse, &mouseHandler, &token);
    if (FAILED(hr)) {
        mouse->lpVtbl->Release(mouse);
        return ReportStartupError(L"mouse-subscription-error", hr);
    }

    const DWORD activationStart = MouseActivationMode(window);
    hr = WriteReport(L"probe=" REPORT_KIND L"\r\nstatus=waiting-for-mouse\r\n");
    if (SUCCEEDED(hr)) hr = PumpWindow(dispatcher, 200);
    const DWORD activationAfterUncaptured = MouseActivationMode(window);
    DWORD activationAfterCapture = (DWORD)-1;
    const HRESULT skipped = (HRESULT)0x8000000AL;
    HRESULT capture = skipped, hide = skipped, release = skipped, restore = skipped;
    if (SUCCEEDED(hr) && !__atomic_load_n(&mouseHandler.motion[0], __ATOMIC_RELAXED)) {
        IInspectableLike *cursor = (IInspectableLike *)0;
        const HRESULT savedCursor = window->lpVtbl->get_PointerCursor(window, &cursor);
        __atomic_store_n(&mouseHandler.phase, 1, __ATOMIC_RELAXED);
        capture = window->lpVtbl->SetPointerCapture(window);
        hide = FAILED(savedCursor) ? savedCursor : window->lpVtbl->put_PointerCursor(window, (IInspectableLike *)0);
        hr = WriteReport(L"probe=" REPORT_KIND L"\r\nstatus=waiting-for-captured-mouse\r\n");
        if (SUCCEEDED(hr)) hr = PumpWindow(dispatcher, 300);
        activationAfterCapture = MouseActivationMode(window);
        if (SUCCEEDED(capture)) release = window->lpVtbl->ReleasePointerCapture(window);
        if (SUCCEEDED(hide)) restore = window->lpVtbl->put_PointerCursor(window, cursor);
        if (cursor) cursor->lpVtbl->Release(cursor);
    }
    const HRESULT removed = mouse->lpVtbl->remove_MouseMoved(mouse, token);
    mouse->lpVtbl->Release(mouse);
    if (FAILED(hr)) return ReportStartupError(L"mouse-dispatch-error", hr);

    const LONG uncapturedMotion = __atomic_load_n(&mouseHandler.motion[0], __ATOMIC_RELAXED);
    const LONG capturePhaseMotion = __atomic_load_n(&mouseHandler.motion[1], __ATOMIC_RELAXED);
    WCHAR report[700];
    DWORD used = 0;
    AppendText(report, 700, &used, L"probe=" REPORT_KIND L"\r\nstatus=mouse-sample-complete\r\nrelative_motion=");
    AppendText(report, 700, &used, uncapturedMotion || capturePhaseMotion ? L"observed\r\n" : L"absent\r\n");
    MouseReportField(report, 700, &used, L"activation_start=", activationStart);
    MouseReportField(report, 700, &used, L"activation_after_uncaptured=", activationAfterUncaptured);
    MouseReportField(report, 700, &used, L"activation_after_capture=", activationAfterCapture);
    MouseReportField(report, 700, &used, L"uncaptured_callbacks=", __atomic_load_n(&mouseHandler.callbacks[0], __ATOMIC_RELAXED));
    MouseReportField(report, 700, &used, L"uncaptured_motion=", uncapturedMotion);
    MouseReportField(report, 700, &used, L"captured_callbacks=", __atomic_load_n(&mouseHandler.callbacks[1], __ATOMIC_RELAXED));
    MouseReportField(report, 700, &used, L"captured_motion=", capturePhaseMotion);
    MouseReportField(report, 700, &used, L"mouse_delta_hresult=", __atomic_load_n(&mouseHandler.deltaError, __ATOMIC_RELAXED));
    MouseReportField(report, 700, &used, L"capture_hresult=", capture);
    MouseReportField(report, 700, &used, L"hide_cursor_hresult=", hide);
    MouseReportField(report, 700, &used, L"release_capture_hresult=", release);
    MouseReportField(report, 700, &used, L"restore_cursor_hresult=", restore);
    MouseReportField(report, 700, &used, L"remove_mouse_moved_hresult=", removed);
    return WriteReport(report);
}
