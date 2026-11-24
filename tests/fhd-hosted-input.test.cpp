// Build with a C++17 compiler and the Windows SDK. This fixture supplies its
// own clock, activation factory, and event source, so it needs no controller.
#include <roapi.h>
#include <winstring.h>
static DWORD testTick();
static HRESULT testFactory(HSTRING, REFIID, void **);
static HRESULT testCreateString(const WCHAR *, UINT, HSTRING *);
static HRESULT testDeleteString(HSTRING);
#define GetTickCount testTick
#define RoGetActivationFactory testFactory
#define WindowsCreateString testCreateString
#define WindowsDeleteString testDeleteString
#include "../runtime/presenter/hosted-input.cpp"
#undef GetTickCount
#undef RoGetActivationFactory
#undef WindowsCreateString
#undef WindowsDeleteString
#include <cassert>
#include <cstring>
#include <iostream>
#include "../runtime/presenter/pointer-cursor.h"
static DWORD clockMs;
static DWORD testTick() { return clockMs; }
struct Obj {
    void **vtable;
};
static Obj fakeWindow, fakeStatics, fakeList, fakePad, replacementPad;
static Obj *selectedPad = &fakePad;
static void *winVt[58], *listVt[12], *padVt[10], *staticsVt[12];
static Reading physical;
static UINT state;
static Rect gameBounds = {0, 0, 1920, 1080};
static Point where = {0, 0};
static BYTE left;
static Obj fakePoint, fakeProps;
static HRESULT getBounds(void *, Rect *r) {
    *r = gameBounds;
    return S_OK;
}
static HRESULT getPosition(void *, Point *r) {
    *r = where;
    return S_OK;
}
static HRESULT getLeft(void *, BYTE *r) {
    *r = left;
    return S_OK;
}
static HRESULT getPoint(void *, IUnknown **r) {
    *r = (IUnknown *)&fakePoint;
    return S_OK;
}
static HRESULT getProps(void *, IUnknown **r) {
    *r = (IUnknown *)&fakeProps;
    return S_OK;
}
static HRESULT getState(void *, UINT *result) {
    *result = state;
    return S_OK;
}
static HRESULT getKeyStatus(void *, KeyStatus *result) {
    *result = {1, 0, 0, 0, 0, 0};
    return S_OK;
}
static HRESULT query(void *o, REFIID, void **result) {
    *result = o;
    return S_OK;
}
static ULONG ref(void *) { return 1; }
static HRESULT getVisible(void *, BYTE *v) {
    *v = 1;
    return S_OK;
}
static HRESULT reg(void *, Handler *h, INT64 *tok) {
    *tok = h->type;
    return S_OK;
}
static HRESULT unreg(void *, INT64) { return S_OK; }
static HRESULT getList(void *, IUnknown **r) {
    *r = (IUnknown *)&fakeList;
    return 0;
}
static HRESULT getSize(void *, UINT *r) {
    *r = selectedPad ? 1u : 0u;
    return 0;
}
static HRESULT getPad(void *, UINT, IUnknown **r) {
    *r = (IUnknown *)selectedPad;
    return 0;
}
static HRESULT getReading(void *, Reading *r) {
    *r = physical;
    return 0;
}
struct MotorWrite {
    double left, right, leftTrigger, rightTrigger;
};
static MotorWrite lastMotors;
static void *lastMotorPad;
static unsigned motorWrites, failStops;
static HRESULT setVibration(void *pad, MotorWrite motors) {
    ++motorWrites;
    lastMotorPad = pad;
    lastMotors = motors;
    if (!motors.left && !motors.right && failStops) {
        --failStops;
        return E_FAIL;
    }
    return S_OK;
}
static HRESULT testFactory(HSTRING, REFIID id, void **p) {
    assert(equal(id, iidIGamepadStatics));
    *p = &fakeStatics;
    return 0;
}
static HRESULT testCreateString(const WCHAR *, UINT, HSTRING *r) {
    *r = nullptr;
    return 0;
}
static HRESULT testDeleteString(HSTRING) { return 0; }
static SdlHostMailbox box;
int main() {
    SdlHostGamepad wire = {};
    const auto neutralPad = SdlHostIdleGamepad(SDL_HOST_PAD_CONNECTED | 7);
    SdlHostGamepadState sample = {};
    assert(!SdlHostReadGamepad(&wire, 0, &sample));
    const LONG firstPacket = SdlHostPublishGamepad(&wire, &neutralPad);
    assert(firstPacket == 2 && SdlHostReadGamepad(&wire, 0, &sample) == firstPacket);
    assert(sample.buttons == SDL_HOST_PAD_CONNECTED && sample.axes[4] == -32768 && !wire.ack);
    assert(!SdlHostReadGamepad(&wire, firstPacket, &sample));
    wire.serial = 3;
    assert(!SdlHostReadGamepad(&wire, firstPacket, &sample));
    wire.serial = -2;
    const SdlHostGamepadState held = {SDL_HOST_PAD_CONNECTED | 1 | 1 << 20, {-40000, 40000}};
    assert(SdlHostPublishGamepad(&wire, &held) == 2);
    assert(SdlHostReadGamepad(&wire, 0, &sample) == 2);
    assert(sample.buttons == (SDL_HOST_PAD_CONNECTED | 1) && sample.axes[0] == -32768 && sample.axes[1] == 32767);
    for (void **vt : {winVt, listVt, padVt, staticsVt}) {
        vt[0] = (void *)query;
        vt[1] = (void *)ref;
        vt[2] = (void *)ref;
    }
    for (auto &h : handlers) {
        winVt[h.type] = (void *)reg;
        winVt[h.type + 1] = (void *)unreg;
    }
    winVt[InputEnabled] = (void *)getVisible;
    winVt[Bounds] = (void *)getBounds;
    winVt[Visible] = (void *)getVisible;
    staticsVt[10] = (void *)getList;
    listVt[6] = (void *)getPad;
    listVt[7] = (void *)getSize;
    padVt[7] = (void *)setVibration;
    padVt[8] = (void *)getReading;
    fakeWindow = {winVt};
    fakeStatics = {staticsVt};
    fakeList = {listVt};
    fakePad = {padVt};
    replacementPad = {padVt};
    box.magic = SDL_HOST_MAGIC;
    box.keyboard.available = 1;
    box.input_pointer = 0x0369089;
    assert(FhdBeginHostedInput(&box, nullptr, &fakeWindow) == S_OK);
    assert(box.input_pointer == 0x0369089);
    assert(!FhdPollHostedInput(&box, &fakeWindow));
    physical.buttons = 4 | 2048 | 512;
    physical.lx = -1;
    physical.ly = 1;
    physical.rx = .5;
    physical.lt = .5;
    physical.rt = 1;
    assert(!FhdPollHostedInput(&box, &fakeWindow));
    assert(box.gamepad.buttons == (SDL_HOST_PAD_CONNECTED | 1 | 1 << 10 | 1 << 14));
    assert(box.gamepad.axes[0] == -32768 && box.gamepad.axes[1] == -32768 && box.gamepad.axes[2] == 16384);
    assert(box.gamepad.axes[4] == 0 && box.gamepad.axes[5] == 32767);
    LONG serial = box.gamepad.serial;
    FhdPollHostedInput(&box, &fakeWindow);
    assert(box.gamepad.serial == serial);
    void *argVt[8] = {};
    argVt[6] = (void *)getState;
    argVt[7] = (void *)getKeyStatus;
    Obj arg = {argVt};
    state = 'Q';
    event(KeyDown, (IUnknown *)&arg);
    assert(box.keyboard.write == 1 && box.keyboard.entries[0].scan == 20 && box.keyboard.entries[0].down == 1);
    event(KeyUp, (IUnknown *)&arg);
    assert(box.keyboard.entries[1].down == 0);
    character(0xd83d);
    character(0xde00);
    assert(!std::strcmp(box.keyboard.entries[2].text, "\xF0\x9F\x98\x80"));
    state = 1;
    event(Activated, (IUnknown *)&arg);
    assert(!focused && box.gamepad.buttons == SDL_HOST_PAD_CONNECTED && box.keyboard.reset == 3);
    state = 2;
    event(Activated, (IUnknown *)&arg);
    assert(focused);
    physical.buttons = 0;
    FhdPollHostedInput(&box, &fakeWindow);
    physical.buttons = 2; // View alone is game input.
    clockMs = 100;
    assert(!FhdPollHostedInput(&box, &fakeWindow));
    assert(box.gamepad.buttons & (1 << 4));
    physical.buttons = 3; // Menu joins View on the next controller sample.
    clockMs += 16;
    assert(FhdPollHostedInput(&box, &fakeWindow) == 1);
    assert(!FhdPollHostedInput(&box, &fakeWindow));
    assert(box.gamepad.buttons == SDL_HOST_PAD_CONNECTED && box.gamepad.axes[4] == -32768);
    FhdEndHostedInput();
    assert(!mail && !window);
    assert(FhdBeginHostedInput(&box, nullptr, &fakeWindow) == S_OK);
    clockMs = 2000;
    assert(!FhdPollHostedInput(&box, &fakeWindow));
    clockMs = 3200;
    assert(!FhdPollHostedInput(&box, &fakeWindow));
    state = VK_ESCAPE;
    event(KeyDown, (IUnknown *)&arg);
    assert(!FhdPollHostedInput(&box, &fakeWindow));
    assert(box.keyboard.entries[3].scan == 41);
    void *pointMethods[14] = {}, *propsMethods[17] = {}, *pointerMethods[8] = {};
    pointMethods[2] = (void *)ref;
    propsMethods[2] = (void *)ref;
    pointerMethods[6] = (void *)getPoint;
    pointMethods[7] = (void *)getPosition;
    pointMethods[13] = (void *)getProps;
    propsMethods[16] = (void *)getLeft;
    fakePoint = {pointMethods};
    fakeProps = {propsMethods};
    Obj pointerArgs = {pointerMethods};
    FhdSetHostedInputProfile(FALSE, 1024, 768);
    assert(!box.gamepad.buttons);
    where = {240, 540};
    left = 1;
    event(PointerPressed, (IUnknown *)&pointerArgs);
    assert((box.input_pointer & 4095) == 0 && ((box.input_pointer >> 12) & 4095) == 2048 &&
           (box.input_pointer >> 24) == 1);
    event(CaptureLost, (IUnknown *)&pointerArgs);
    assert((box.input_pointer >> 24) == 0);
    physical.buttons = 0;
    FhdPollHostedInput(&box, &fakeWindow);
    physical.buttons = 4;
    physical.lx = 1;
    clockMs += 100;
    FhdPollHostedInput(&box, &fakeWindow);
    assert((box.input_pointer >> 24) == 1 && (box.input_pointer & 4095) > 0);
    FhdSetHostedInputProfile(TRUE, 1920, 1080);
    state = VK_F10;
    event(KeyDown, (IUnknown *)&arg);
    assert(FhdPollHostedInput(&box, &fakeWindow) == 1);
    state = 1;
    event(Activated, (IUnknown *)&arg);
    state = 0;
    event(Activated, (IUnknown *)&arg);
    assert(overlay == Playing);
    physical.buttons = 4;
    assert(!FhdPollHostedInput(&box, &fakeWindow));
    assert(!(box.gamepad.buttons & 1));
    physical.buttons = 0;
    FhdPollHostedInput(&box, &fakeWindow);
    physical.buttons = 4;
    FhdPollHostedInput(&box, &fakeWindow);
    assert(box.gamepad.buttons & 1);
    // InputEnabledChanged has a WinRT boolean rather than the activation enum.
    state = 0;
    argVt[6] = (void *)+[](void *, BYTE *v) -> HRESULT {
        *v = (BYTE)state;
        return S_OK;
    };
    event(InputEnabledChanged, (IUnknown *)&arg);
    assert(!foreground() && !(box.gamepad.buttons & 1));
    state = 1;
    event(InputEnabledChanged, (IUnknown *)&arg);
    assert(foreground());
    FhdEndHostedInput();
    box.input_serial = box.input_pointer = 0;
    assert(FhdBeginHostedInput(&box, nullptr, &fakeWindow) == S_OK);
    FhdSetHostedInputProfile(FALSE, 1024, 768);
    assert(box.input_pointer == (2048 | 2048 << 12) && box.input_serial > 0);
    const auto firstSerial = box.input_serial;
    FhdSetHostedInputProfile(FALSE, 1024, 768);
    assert(box.input_serial == firstSerial);
    FhdEndHostedInput();
    // After genuine input the top-left is a valid position, including on resume.
    box.input_pointer = 0;
    assert(FhdBeginHostedInput(&box, nullptr, &fakeWindow) == S_OK);
    FhdSetHostedInputProfile(FALSE, 1024, 768);
    assert(box.input_pointer == 0 && box.input_serial == firstSerial);
    FhdEndHostedInput();

    SdlRumbleMailbox vibration{};
    vibration.magic = SDL_RUMBLE_MAGIC;
    assert(FhdBeginHostedInput(&box, &vibration, &fakeWindow) == S_OK);
    FhdSetHostedInputProfile(TRUE, 1920, 1080);
    FhdPollHostedInput(&box, &fakeWindow);
    const LONG firstRumble = SdlRumbleGeneration(&vibration);
    assert(firstRumble && motorWrites == 1 && lastMotorPad == &fakePad &&
           !lastMotors.left && !lastMotors.right);
    assert(SdlRumblePublish(&vibration, firstRumble, 65535, 32768));
    FhdPollHostedInput(&box, &fakeWindow);
    assert(motorWrites == 2 && lastMotors.left == 1 && lastMotors.right == 32768 / 65535.0 &&
           !lastMotors.leftTrigger && !lastMotors.rightTrigger);
    FhdPollHostedInput(&box, &fakeWindow);
    assert(motorWrites == 2);

    failStops = 1;
    selectedPad = &replacementPad;
    FhdPollHostedInput(&box, &fakeWindow);
    assert(!SdlRumbleGeneration(&vibration) && motorWrites == 3 && lastMotorPad == &fakePad);
    FhdPollHostedInput(&box, &fakeWindow);
    assert(!SdlRumbleGeneration(&vibration) && motorWrites == 4 && lastMotorPad == &fakePad);
    FhdPollHostedInput(&box, &fakeWindow);
    const LONG replacementRumble = SdlRumbleGeneration(&vibration);
    assert(replacementRumble && replacementRumble != firstRumble &&
           motorWrites == 5 && lastMotorPad == &replacementPad);
    assert(!SdlRumblePublish(&vibration, firstRumble, 555, 888));
    assert(SdlRumblePublish(&vibration, replacementRumble, 0, 65535));
    FhdPollHostedInput(&box, &fakeWindow);
    assert(lastMotorPad == &replacementPad && !lastMotors.left && lastMotors.right == 1);

    failStops = 2;
    const unsigned writesBeforePause = motorWrites;
    FhdPauseHostedInput();
    assert(!SdlRumbleGeneration(&vibration) && !failStops && motorWrites == writesBeforePause + 3);
    FhdResumeHostedInput();
    FhdPollHostedInput(&box, &fakeWindow);
    const LONG resumed = SdlRumbleGeneration(&vibration);
    assert(resumed && resumed != replacementRumble && !SdlRumbleRead(&vibration));
    assert(SdlRumblePublish(&vibration, resumed, 65535, 0));
    FhdPollHostedInput(&box, &fakeWindow);
    assert(lastMotors.left == 1 && !lastMotors.right);
    failStops = 1;
    FhdSetHostedInputProfile(FALSE, 1920, 1080);
    assert(!SdlRumbleGeneration(&vibration) && !failStops);
    FhdPollHostedInput(&box, &fakeWindow);
    assert(!lastMotors.left && !lastMotors.right);
    FhdSetHostedInputProfile(TRUE, 1920, 1080);
    FhdPollHostedInput(&box, &fakeWindow);
    assert(SdlRumblePublish(&vibration, SdlRumbleGeneration(&vibration), 65535, 0));
    FhdPollHostedInput(&box, &fakeWindow);
    InterlockedExchange(&box.quit, 1);
    FhdPollHostedInput(&box, &fakeWindow);
    assert(!SdlRumbleGeneration(&vibration) && !lastMotors.left && !lastMotors.right);
    FhdEndHostedInput();

    static_assert(FhdPointer::pixel(0, 1024) == 0 && FhdPointer::pixel(4095, 1024) == 1023);
    static_assert(FhdPointer::pixel(2048, 768) == 383);
    constexpr auto white = FhdPointer::image(false), amber = FhdPointer::image(true);
    constexpr int c = FhdPointer::center, n = FhdPointer::size;
    static_assert(white[c*n+c] == 0xffffffffU && amber[c*n+c] == 0xff00bfffU);
    // Even if the cursor is clipped at a corner, its inward-facing arms have
    // a bright fill and dark outline on both axes.
    for (int sign : {-1, 1}) {
        assert(white[c*n+c+5*sign] == 0xffffffffU && white[(c+5*sign)*n+c] == 0xffffffffU);
        assert(white[(c+2*sign)*n+c+5*sign] == 0xff101010U);
        assert(white[(c+5*sign)*n+c+2*sign] == 0xff101010U);
    }
    std::cout << "Native input bridge checks passed.\n";
}
