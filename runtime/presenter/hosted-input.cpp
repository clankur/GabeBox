// Input for the native CoreWindow on its dispatcher thread. The wire formats
// match SteamSdlSurface and the WWA player. No captures or polling survive End.
#include "hosted-input.h"
#include "hosted-rumble.h"
#include <roapi.h>
#include <winstring.h>
#include <stdint.h>

namespace {
// The SDK ABI is used directly so this companion can be cross-built without
// the C++/WinRT runtime. Slot numbers include IInspectable's six methods.
enum WindowMethod {
    Bounds = 7,
    InputEnabled = 12,
    Visible = 17,
    Activated = 24,
    CharacterReceived = 28,
    InputEnabledChanged = 32,
    KeyDown = 34,
    KeyUp = 36,
    CaptureLost = 38,
    PointerExited = 42,
    PointerMoved = 44,
    PointerPressed = 46,
    PointerReleased = 48,
    PointerWheelChanged = 52,
    VisibilityChanged = 56
};
template <class... Args> HRESULT method(IUnknown *obj, unsigned slot, Args... args) {
    using Fn = HRESULT(STDMETHODCALLTYPE *)(IUnknown *, Args...);
    return reinterpret_cast<Fn>((*reinterpret_cast<void ***>(obj))[slot])(obj, args...);
}
constexpr GUID handlerWindowActivatedEventArgs = {
    0x26a73b35, 0xa7f9, 0x52db, {0x88, 0xd6, 0x15, 0x72, 0x6d, 0xeb, 0x25, 0x23}};
constexpr GUID handlerKeyEventArgs = {0xa3ec0774, 0x55ac, 0x5d61, {0x82, 0x32, 0xb3, 0x5c, 0x5d, 0x35, 0xc9, 0x3c}};
constexpr GUID handlerPointerEventArgs = {
    0x420e1bb6, 0xe99d, 0x5e64, {0x8e, 0x25, 0x07, 0x46, 0x7e, 0x3c, 0xae, 0x9e}};
constexpr GUID handlerCharacterReceivedEventArgs = {
    0x73f846a8, 0x0af6, 0x5872, {0x8f, 0xb8, 0xae, 0x2f, 0x56, 0xd8, 0x55, 0x3e}};
constexpr GUID handlerVisibilityChangedEventArgs = {
    0x14b7f692, 0x5583, 0x52a1, {0xaa, 0x42, 0xfc, 0x18, 0x43, 0xc0, 0xf7, 0x48}};
constexpr GUID handlerInputEnabledEventArgs = {
    0xe230a64a, 0x506a, 0x59c3, {0xbb, 0x61, 0x55, 0x59, 0xff, 0x99, 0x56, 0x63}};
constexpr GUID iidICoreWindow = {0x79b9d5f2, 0x879e, 0x4b89, {0xb7, 0x98, 0x79, 0xe4, 0x75, 0x98, 0x03, 0x0c}};
constexpr GUID iidIGamepadStatics = {0x8bbce529, 0xd49c, 0x39e9, {0x95, 0x60, 0xe4, 0x7d, 0xde, 0x96, 0xb7, 0xc8}};
struct Point {
    float x, y;
};
struct Rect {
    float x, y, w, h;
};
struct KeyStatus {
    UINT repeat, scan;
    BYTE extended, menu, wasDown, released;
};
struct Reading {
    UINT64 timestamp;
    UINT buttons;
    double lt, rt, lx, ly, rx, ry;
};
static_assert(sizeof(Reading) == 64 && sizeof(KeyStatus) == 12, "WinRT input ABI");
SdlHostMailbox *mail;
HostedRumble rumble;
IUnknown *window, *pads;
enum OverlayState { Playing, Requested, Reported };
OverlayState overlay;
BOOL nativePad = TRUE, focused, visible, inputEnabled, blockChord;
bool menuOnRelease, chordPending;
LONG blockedMenuButtons;
UINT gameW = 1920, gameH = 1080;
DWORD lastTime;
UINT highSurrogate;
LONG mouse, legacyKeys;
constexpr UINT pointerEdgeCount = 32;
struct PointerUpdate { LONG position, wheel; };
LONG wantedPointer, wantedKeys, pendingPointerSerial;
PointerUpdate pointerEdges[pointerEdgeCount];
UINT pointerEdgeRead, pointerEdgeSize;
double cursorX, cursorY;
LONG physicalButtons;
bool padClick, freshPointer, physicalPointer, routedPointer;
LONG heldVirtual[256];
SdlHostGamepadState previousPad;
bool sentPad;

bool alive() { return mail && !SdlHostLoad(&mail->quit); }
bool foreground() { return alive() && focused && visible && inputEnabled && !overlay; }
bool keyboard() { return alive() && SdlHostLoad(&mail->keyboard.available); }
void setPad(SdlHostGamepadState const &pad) {
    if (!alive())
        return;
    bool same = sentPad && pad.buttons == previousPad.buttons;
    for (unsigned i = 0; i < SDL_HOST_PAD_AXES; ++i)
        same = same && pad.axes[i] == previousPad.axes[i];
    if (same)
        return;
    SdlHostPublishGamepad(&mail->gamepad, &pad);
    previousPad = pad;
    sentPad = true;
}
LONG pointer(LONG pos, LONG keys, LONG wheel = 0) {
    if (!alive() || (!wheel && pos == mouse && keys == legacyKeys))
        return 0;
    InterlockedIncrement(&mail->input_serial);
    InterlockedExchange(&mail->input_pointer, pos);
    InterlockedExchange(&mail->input_keys, keys | wheel);
    const LONG serial = InterlockedIncrement(&mail->input_serial);
    mouse = pos;
    legacyKeys = keys;
    return serial;
}
void publishPanelPointer() {
    if (!alive() || (pendingPointerSerial && SdlHostLoad(&mail->input_ack) != pendingPointerSerial)) return;
    pendingPointerSerial = 0;
    if (pointerEdgeSize) {
        const auto update = pointerEdges[pointerEdgeRead];
        pendingPointerSerial = pointer(update.position, wantedKeys, update.wheel);
        pointerEdgeRead = (pointerEdgeRead + 1) % pointerEdgeCount;
        --pointerEdgeSize;
    } else {
        pointer(wantedPointer, wantedKeys);
    }
}
void neutral() {
    rumble.StopBeforeSuspend();
    const auto idle = SdlHostIdleGamepad(alive() ? SdlHostLoad(&mail->gamepad.buttons) : 0);
    if (nativePad)
        setPad(idle);
    else
        previousPad = idle;
    if (keyboard())
        InterlockedExchange(&mail->keyboard.reset, SdlHostLoad(&mail->keyboard.write));
    if (alive()) {
        mouse = SdlHostLoad(&mail->input_pointer);
        legacyKeys = SdlHostLoad(&mail->input_keys);
        wantedPointer = mouse & 0xffffff;
        wantedKeys = pendingPointerSerial = 0;
        pointerEdgeRead = pointerEdgeSize = 0;
        pointer(wantedPointer, 0);
    }
    // A or B used in the library must be released before the game sees it.
    blockedMenuButtons = 3;
    for (auto &key : heldVirtual)
        key = 0;
    highSurrogate = 0;
    padClick = false;
    physicalButtons = 0;
    physicalPointer = false;
    blockChord = true;
    chordPending = false;
}
void queueKey(UINT scan, bool down, const char *utf8 = nullptr) {
    if (!keyboard())
        return;
    auto &q = mail->keyboard;
    UINT write = (UINT)SdlHostLoad(&q.write), read = (UINT)SdlHostLoad(&q.read);
    if (write - read >= SDL_HOST_KEY_QUEUE_SIZE) {
        InterlockedExchange(&q.reset, (LONG)write);
        return;
    }
    auto &entry = q.entries[write % SDL_HOST_KEY_QUEUE_SIZE];
    entry.scan = (uint16_t)scan;
    entry.down = down;
    unsigned i = 0;
    if (utf8)
        while (utf8[i] && i + 1 < sizeof(entry.text)) {
            entry.text[i] = utf8[i];
            ++i;
        }
    entry.text[i] = 0;
    InterlockedExchange(&q.write, (LONG)(write + 1));
}
void character(UINT cp) {
    if (cp >= 0xd800 && cp <= 0xdbff) {
        highSurrogate = cp;
        return;
    }
    if (cp >= 0xdc00 && cp <= 0xdfff) {
        if (!highSurrogate)
            return;
        cp = 0x10000 + ((highSurrogate - 0xd800) << 10) + (cp - 0xdc00);
    }
    highSurrogate = 0;
    if (cp < 32 || cp == 127 || cp > 0x10ffff)
        return;
    char b[5] = {};
    unsigned n = 0;
    if (cp < 0x80)
        b[n++] = (char)cp;
    else {
        unsigned shift = cp < 0x800 ? 6 : cp < 0x10000 ? 12 : 18;
        b[n++] = (char)((shift == 6 ? 0xc0 : shift == 12 ? 0xe0 : 0xf0) | (cp >> shift));
        while (shift) {
            shift -= 6;
            b[n++] = (char)(0x80 | ((cp >> shift) & 63));
        }
    }
    queueKey(0, false, b);
}
UINT scanFor(UINT vk, KeyStatus s) {
    // Set-1 hardware positions map to SDL's USB HID usage IDs independent of layout.
    const BYTE mainKeys[] = {0,   41, 30,  31, 32, 33, 34,  35, 36, 37, 38, 39,  45, 46, 42,  43, 20, 26,
                             8,   21, 23,  28, 24, 12, 18,  19, 47, 48, 40, 224, 4,  22, 7,   9,  10, 11,
                             13,  14, 15,  51, 52, 53, 225, 49, 29, 27, 6,  25,  5,  17, 16,  54, 55, 56,
                             229, 85, 226, 44, 57, 58, 59,  60, 61, 62, 63, 64,  65, 66, 67,  83, 71, 95,
                             96,  97, 86,  92, 93, 94, 87,  89, 90, 91, 98, 99,  0,  0,  100, 68, 69};
    if (!s.extended && s.scan && s.scan < sizeof(mainKeys) && mainKeys[s.scan]) {
        if (vk == VK_PAUSE)
            return 72;
        return mainKeys[s.scan];
    }
    if (vk >= 'A' && vk <= 'Z')
        return 4 + vk - 'A';
    if (vk >= '1' && vk <= '9')
        return 30 + vk - '1';
    if (vk == '0')
        return 39;
    if (vk >= VK_F1 && vk <= VK_F12)
        return 58 + vk - VK_F1;
    if (vk >= VK_F13 && vk <= VK_F24)
        return 104 + vk - VK_F13;
    switch (vk) {
    case VK_RETURN:
        return s.extended ? 88 : 40;
    case VK_ESCAPE:
        return 41;
    case VK_BACK:
        return 42;
    case VK_TAB:
        return 43;
    case VK_SPACE:
        return 44;
    case VK_SNAPSHOT:
        return 70;
    case VK_PAUSE:
        return 72;
    case VK_INSERT:
        return 73;
    case VK_HOME:
        return 74;
    case VK_PRIOR:
        return 75;
    case VK_DELETE:
        return 76;
    case VK_END:
        return 77;
    case VK_NEXT:
        return 78;
    case VK_RIGHT:
        return 79;
    case VK_LEFT:
        return 80;
    case VK_DOWN:
        return 81;
    case VK_UP:
        return 82;
    case VK_DIVIDE:
        return 84;
    case VK_APPS:
        return 101;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return s.extended || vk == VK_RCONTROL ? 228 : 224;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return s.scan == 54 || vk == VK_RSHIFT ? 229 : 225;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return s.extended || vk == VK_RMENU ? 230 : 226;
    case VK_LWIN:
        return 227;
    case VK_RWIN:
        return 231;
    default:
        return 0;
    }
}
double clamp(double value, double lo, double hi) { return value < lo ? lo : value > hi ? hi : value; }
void sendPointer(LONG wheel = 0) {
    LONG pos = (LONG)(clamp(cursorX, 0, 1) * 4095 + .5) | ((LONG)(clamp(cursorY, 0, 1) * 4095 + .5) << 12);
    pos |= physicalButtons;
    if (padClick || (!keyboard() && heldVirtual[VK_RETURN])) pos |= SDL_HOST_POINTER_LEFT;
    if (nativePad && physicalPointer) pos |= SDL_HOST_POINTER_VISIBLE;
    LONG keys = 0;
    if (!keyboard()) {
        if (heldVirtual[VK_ESCAPE])
            keys |= 32;
        if (heldVirtual[VK_SPACE])
            keys |= 64;
    }
    if (!nativePad && (previousPad.buttons & 2))
        keys |= 32;
    if (!menuOnRelease) {
        pointer(pos, keys, wheel);
        return;
    }
    if (wheel || ((pos ^ wantedPointer) & SDL_HOST_POINTER_BUTTONS)) {
        if (pointerEdgeSize == pointerEdgeCount) --pointerEdgeSize;
        pointerEdges[(pointerEdgeRead + pointerEdgeSize) % pointerEdgeCount] = {pos, wheel};
        ++pointerEdgeSize;
    }
    wantedPointer = pos;
    wantedKeys = keys;
    publishPanelPointer();
}
void mouseEvent(IUnknown *args, bool scrolling) {
    IUnknown *point = nullptr, *props = nullptr;
    Point pos = {};
    Rect bounds = {};
    if (FAILED(method(args, 6, &point)) || !point)
        return;
    HRESULT hr = method(point, 7, &pos);
    if (SUCCEEDED(hr))
        hr = method(point, 13, &props);
    LONG buttons = 0, delta = 0;
    BYTE horizontal = 0;
    if (SUCCEEDED(hr) && props) {
        constexpr struct { unsigned property; LONG button; } properties[] = {
            {16, SDL_HOST_POINTER_LEFT}, {17, SDL_HOST_POINTER_RIGHT}, {18, SDL_HOST_POINTER_MIDDLE},
            {25, SDL_HOST_POINTER_X1}, {26, SDL_HOST_POINTER_X2}};
        for (const auto &property : properties) {
            BYTE pressed = 0;
            if (SUCCEEDED(method(props, property.property, &pressed)) && pressed) buttons |= property.button;
        }
        if (scrolling && FAILED(method(props, 19, &delta))) delta = 0;
        if (delta && FAILED(method(props, 20, &horizontal))) horizontal = 0;
    }
    if (props)
        props->Release();
    point->Release();
    if (FAILED(hr) || FAILED(method(window, Bounds, &bounds)) || !(bounds.w > 0 && bounds.h > 0))
        return;
    double scale = clamp(bounds.w / gameW, 0, bounds.h / gameH);
    if (scale <= 0)
        return;
    // PointerPoint.Position is relative to this view. Exclude letterbox bars.
    FhdHostedPointerUpdate((pos.x - (bounds.w - gameW * scale) * .5) / (gameW * scale),
        (pos.y - (bounds.h - gameH * scale) * .5) / (gameH * scale), buttons, delta, horizontal);
}
void requestOverlay() {
    if (!overlay) {
        neutral();
        overlay = Requested;
    }
}
void event(unsigned type, IUnknown *args) {
    if (!alive() || !args)
        return;
    if (type == Activated) {
        UINT state = 1;
        if (SUCCEEDED(method(args, 6, &state))) {
            const bool wasFocused = focused != FALSE;
            focused = (state == 0 || state == 2);
            if (!focused)
                neutral();
            else if (!menuOnRelease && !wasFocused && overlay == Reported)
                FhdResumeHostedInput();
        }
        return;
    }
    if (type == VisibilityChanged || type == InputEnabledChanged) {
        BYTE value = 0;
        if (SUCCEEDED(method(args, 6, &value))) {
            if (type == VisibilityChanged)
                visible = value;
            else
                inputEnabled = value;
            if (!value)
                neutral();
        }
        return;
    }
    if (!foreground())
        return;
    if (type == CaptureLost || type == PointerExited) {
        if (!routedPointer) FhdHostedPointerRelease(type == PointerExited);
        return;
    }
    if (type == PointerMoved || type == PointerPressed || type == PointerReleased || type == PointerWheelChanged) {
        if (!routedPointer) mouseEvent(args, type == PointerWheelChanged);
        return;
    }
    UINT key = 0;
    if (FAILED(method(args, 6, &key)))
        return;
    if (type == CharacterReceived) {
        if (!heldVirtual[VK_CONTROL] && !heldVirtual[VK_LCONTROL] && !heldVirtual[VK_RCONTROL] &&
            !heldVirtual[VK_MENU] && !heldVirtual[VK_LMENU] && !heldVirtual[VK_RMENU] && !heldVirtual[VK_LWIN] &&
            !heldVirtual[VK_RWIN])
            character(key);
        return;
    }
    if (key >= 256 || (key >= 136 && key <= 143) || (key >= 195 && key <= 218))
        return;
    KeyStatus status = {};
    if (FAILED(method(args, 7, &status)))
        return;
    bool down = type == KeyDown;
    if (key == VK_F10) {
        if (down && !status.wasDown)
            requestOverlay();
        return;
    }
    heldVirtual[key] = down;
    UINT scan = scanFor(key, status);
    if (keyboard()) {
        if (scan && (!down || !status.wasDown))
            queueKey(scan, down);
    } else
        sendPointer();
}
// TypedEventHandler<CoreWindow, TArgs> delegates have IUnknown's three slots
// followed by Invoke. IDs were instantiated from the Windows.winmd signatures.
struct Handler;
struct HandlerVtable {
    HRESULT(STDMETHODCALLTYPE *query)(Handler *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *retain)(Handler *);
    ULONG(STDMETHODCALLTYPE *release)(Handler *);
    HRESULT(STDMETHODCALLTYPE *invoke)(Handler *, IUnknown *, IUnknown *);
};
struct Handler {
    const HandlerVtable *v;
    const GUID *iid;
    unsigned type;
    INT64 token;
    bool registered;
};
bool equal(REFGUID a, REFGUID b) {
    auto *x = reinterpret_cast<const BYTE *>(&a), *y = reinterpret_cast<const BYTE *>(&b);
    for (unsigned i = 0; i < 16; ++i)
        if (x[i] != y[i])
            return false;
    return true;
}
ULONG STDMETHODCALLTYPE retainHandler(Handler *) { return 2; } // process-lifetime static instances
ULONG STDMETHODCALLTYPE releaseHandler(Handler *) { return 1; }
HRESULT STDMETHODCALLTYPE queryHandler(Handler *h, REFIID id, void **out) {
    constexpr GUID unknown = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (!equal(id, *h->iid) && !equal(id, unknown))
        return E_NOINTERFACE;
    *out = h;
    retainHandler(h);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE invokeHandler(Handler *h, IUnknown *, IUnknown *args) {
    event(h->type, args);
    return S_OK;
}
const HandlerVtable events = {queryHandler, retainHandler, releaseHandler, invokeHandler};
Handler handlers[] = {{&events, &handlerWindowActivatedEventArgs, Activated},
                      {&events, &handlerVisibilityChangedEventArgs, VisibilityChanged},
                      {&events, &handlerInputEnabledEventArgs, InputEnabledChanged},
                      {&events, &handlerKeyEventArgs, KeyDown},
                      {&events, &handlerKeyEventArgs, KeyUp},
                      {&events, &handlerCharacterReceivedEventArgs, CharacterReceived},
                      {&events, &handlerPointerEventArgs, PointerMoved},
                      {&events, &handlerPointerEventArgs, PointerPressed},
                      {&events, &handlerPointerEventArgs, PointerReleased},
                      {&events, &handlerPointerEventArgs, PointerWheelChanged},
                      {&events, &handlerPointerEventArgs, PointerExited},
                      {&events, &handlerPointerEventArgs, CaptureLost}};
LONG axis(double value) {
    value = clamp(value, -1, 1); // JS Math.round uses floor(x + .5), including ties on negatives.
    double scaled = value * (value < 0 ? 32768 : 32767) + .5;
    LONG result = (LONG)scaled;
    return scaled < result ? result - 1 : result;
}
IUnknown *readPad(Reading *out) {
    if (!pads)
        return nullptr;
    IUnknown *list = nullptr, *pad = nullptr;
    HRESULT hr = method(pads, 10, &list);
    UINT size = 0;
    if (SUCCEEDED(hr) && list) {
        hr = method(list, 7, &size);
        if (SUCCEEDED(hr) && size)
            hr = method(list, 6, 0u, &pad);
        list->Release();
    }
    if (!pad)
        return nullptr;
    hr = method(pad, 8, out);
    if (FAILED(hr)) {
        pad->Release();
        return nullptr;
    }
    return pad;
}
} // namespace
extern "C" BOOL FhdHostedInputForeground() { return foreground(); }
extern "C" void FhdSetHostedInputProfile(BOOL gamepad, UINT width, UINT height) {
    if (!!nativePad != !!gamepad) {
        if (alive()) {
            const auto disconnected = SdlHostIdleGamepad(0);
            if (!gamepad)
                setPad(disconnected);
            neutral();
        }
        nativePad = gamepad;
        sentPad = false;
    }
    // A new mailbox starts at zero. WWA resumes and existing input (even a
    // deliberate top-left position) must keep their original coordinates.
    if (!nativePad && freshPointer && alive()) {
        freshPointer = false;
        if (!SdlHostLoad(&mail->input_serial) && !SdlHostLoad(&mail->input_pointer)) {
            cursorX = cursorY = .5;
            sendPointer();
        }
    }
    if (width && height) {
        gameW = width;
        gameH = height;
    }
}
extern "C" void FhdResumeHostedInput() {
    if (!mail || overlay == Playing)
        return;
    // WWA can have moved the cursor while showing the overlay.
    mouse = SdlHostLoad(&mail->input_pointer);
    cursorX = (mouse & 4095) / 4095.;
    cursorY = ((mouse >> 12) & 4095) / 4095.;
    neutral();
    overlay = Playing;
    lastTime = GetTickCount();
}
extern "C" void FhdPauseHostedInput() {
    if (mail && overlay != Reported) {
        neutral();
        overlay = Reported;
    } else {
        rumble.Stop();
    }
}
extern "C" void FhdStopHostedRumble() { rumble.Stop(); }
extern "C" void FhdUseRoutedPointer() { routedPointer = true; }
extern "C" void FhdHostedPointerUpdate(double x, double y, LONG buttons, LONG wheelDelta, BOOL horizontalWheel) {
    if (!foreground()) return;
    cursorX = clamp(x, 0, 1);
    cursorY = clamp(y, 0, 1);
    physicalButtons = buttons & SDL_HOST_POINTER_BUTTONS;
    physicalPointer = true;
    const auto delta = static_cast<SHORT>(clamp(wheelDelta, -32768, 32767));
    LONG wheel = static_cast<LONG>(static_cast<UINT>(static_cast<USHORT>(delta)) << 16);
    if (delta && horizontalWheel) wheel |= SDL_HOST_WHEEL_HORIZONTAL;
    sendPointer(wheel);
}
extern "C" void FhdHostedPointerRelease(BOOL exited) {
    if (!foreground()) return;
    physicalButtons = 0;
    if (exited) physicalPointer = false;
    sendPointer();
}
extern "C" void FhdHostedMenuOnRelease(BOOL enabled) {
    menuOnRelease = enabled != FALSE;
}
extern "C" BOOL FhdHostedMenuHeld() { return chordPending; }
extern "C" void FhdEndHostedInput() {
    rumble.Attach(nullptr);
    if (window) {
        for (auto &h : handlers)
            if (h.registered) {
                method(window, h.type + 1, h.token);
                h.registered = false;
            }
        neutral();
        window->Release();
        window = nullptr;
    }
    if (pads) {
        pads->Release();
        pads = nullptr;
    }
    mail = nullptr;
    overlay = Playing;
    menuOnRelease = false;
    routedPointer = false;
    sentPad = false;
}
extern "C" HRESULT FhdBeginHostedInput(SdlHostMailbox *box, SdlRumbleMailbox *vibration, void *coreWindow) {
    FhdEndHostedInput();
    if (!box || !coreWindow || SdlHostLoad(&box->magic) != SDL_HOST_MAGIC)
        return E_INVALIDARG;
    HRESULT hr =
        static_cast<IUnknown *>(coreWindow)->QueryInterface(iidICoreWindow, reinterpret_cast<void **>(&window));
    if (FAILED(hr))
        return hr;
    mail = box;
    rumble.Attach(vibration);
    focused = TRUE;
    BYTE shown = 1;
    method(window, Visible, &shown);
    visible = shown;
    BYTE enabled = 1;
    method(window, InputEnabled, &enabled);
    inputEnabled = enabled;
    mouse = SdlHostLoad(&box->input_pointer);
    legacyKeys = SdlHostLoad(&box->input_keys);
    freshPointer = !mouse && !SdlHostLoad(&box->input_serial);
    cursorX = (mouse & 4095) / 4095.;
    cursorY = ((mouse >> 12) & 4095) / 4095.;
    neutral();
    const WCHAR name[] = L"Windows.Gaming.Input.Gamepad";
    HSTRING str = nullptr;
    if (SUCCEEDED(WindowsCreateString(name, ARRAYSIZE(name) - 1, &str))) {
        RoGetActivationFactory(str, iidIGamepadStatics, reinterpret_cast<void **>(&pads));
        WindowsDeleteString(str);
    }
    for (auto &h : handlers) {
        hr = method(window, h.type, &h, &h.token);
        if (FAILED(hr)) {
            FhdEndHostedInput();
            return hr;
        }
        h.registered = true;
    }
    lastTime = GetTickCount();
    return S_OK;
}
extern "C" int FhdPollHostedInput(SdlHostMailbox *box, void *coreWindow) {
    if (!mail || box != mail || !coreWindow)
        return 0;
    if (!alive()) {
        rumble.Stop();
        return 0;
    }
    if (overlay) {
        rumble.Stop();
        if (overlay == Requested) {
            overlay = Reported;
            return 1;
        }
        return 0;
    }
    DWORD now = GetTickCount(), elapsed = now - lastTime;
    lastTime = now;
    if (!foreground()) {
        rumble.Stop();
        return 0;
    }
    Reading reading = {};
    auto value = SdlHostIdleGamepad(0);
    IUnknown *pad = readPad(&reading);
    const bool connected = pad != nullptr;
    if (nativePad) rumble.Update(pad);
    else rumble.Stop();
    if (pad) pad->Release();
    if (connected) {
        const unsigned bits[] = {4, 8, 16, 32, 2, 0, 1, 4096, 8192, 1024, 2048, 64, 128, 256, 512};
        value.buttons = SDL_HOST_PAD_CONNECTED;
        for (unsigned i = 0; i < SDL_HOST_PAD_BUTTONS; ++i)
            if (reading.buttons & bits[i])
                value.buttons |= 1 << i;
        value.axes[0] = axis(reading.lx);
        value.axes[1] = axis(-reading.ly);
        value.axes[2] = axis(reading.rx);
        value.axes[3] = axis(-reading.ry);
        value.axes[4] = axis(2 * reading.lt - 1);
        value.axes[5] = axis(2 * reading.rt - 1);
    }
    blockedMenuButtons &= value.buttons;
    value.buttons &= ~blockedMenuButtons;
    const LONG chord = (1 << 4) | (1 << 6);
    if ((value.buttons & chord) == chord) {
        if (!blockChord && menuOnRelease) chordPending = true;
        else if (!blockChord) {
            requestOverlay();
            overlay = Reported;
            return 1;
        }
        value.buttons &= ~chord;
    } else {
        if (!connected) chordPending = false;
        if (!(value.buttons & chord)) {
            if (chordPending) {
                requestOverlay();
                overlay = Reported;
                return 1;
            }
            blockChord = false;
        } else if (chordPending) value.buttons &= ~chord;
    }
    double dx = 0, dy = 0;
    if (!keyboard()) {
        dx = heldVirtual[VK_RIGHT] - heldVirtual[VK_LEFT];
        dy = heldVirtual[VK_DOWN] - heldVirtual[VK_UP];
    }
    if (nativePad)
        setPad(value);
    else {
        previousPad = value;
        if (value.axes[0] < -6500 || value.axes[0] > 6500)
            dx += value.axes[0] / 32768.;
        if (value.axes[1] < -6500 || value.axes[1] > 6500)
            dy += value.axes[1] / 32768.;
        dx += !!(value.buttons & (1 << 14)) - !!(value.buttons & (1 << 13));
        dy += !!(value.buttons & (1 << 12)) - !!(value.buttons & (1 << 11));
        padClick = (value.buttons & 1) != 0;
    }
    if (dx || dy || !nativePad) {
        cursorX = clamp(cursorX + dx * clamp(elapsed, 0, 100) / 1250., 0, 1);
        cursorY = clamp(cursorY + dy * clamp(elapsed, 0, 100) / 1250., 0, 1);
        sendPointer();
    }
    if (menuOnRelease) publishPanelPointer();
    return 0;
}
