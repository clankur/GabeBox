# Physical mouse input on an Xbox Dev Mode package

A physical mouse attached to a Series X works in the console's own browser and does not
reach a sideloaded Dev Mode package. Ten console builds tested every documented route;
all of them are either unimplemented on Xbox or implemented and silent. The cause is a
partition and licensing boundary, not a defect in this compatibility layer, so an
Edge-like cursor has to be driven from a source this layer can read.

Contents: the answer · why · the platform statement · console evidence · the route
matrix · what this rules out · GDK · what remains · mouse as a controller · where the
probes live.

## The answer

| Question | Result |
| --- | --- |
| Can a Dev Mode UWP package read a physical Xbox mouse? | No, on any documented route |
| Does the console know the mouse is attached? | Yes: `mouse_present=1`, one mouse pointer device, GameInput enumerates two HID mouse streams with status `0x7` |
| Does foreground input reach the package at all? | Yes: keyboard delivers CoreWindow key events and GameInput key-count changes |
| Do restricted input capabilities change it? | No, and Dev Mode installs them without complaint |
| Would the Microsoft GDK fix it? | Yes for a GDK title, but its console toolchain is license-gated and runs in another OS partition |

## Why

Xbox splits its OS. Dev Mode packages run in the system partition: the console's own
Device Portal reports `"OsEdition" : "SystemOS"` on `"Platform" : "Xbox Series X"`,
OS `26100.9673.amd64fre.xb_flt_2610ge.261116-2200`. GDK console titles run in the game
partition, and Microsoft documents that the console's GameInput runtime "is included in
the Game OS file (`GameOs.xvd`) from the GDK that's bundled with your title."

That one sentence explains every measurement below. The mouse-capable input runtime
ships inside a Game OS that a sideloaded package neither carries nor enters. The system
`gameinput.dll` a Dev Mode package can load offers only the v0 ABI, exports no
`GameInputInitialize`, returns `E_NOINTERFACE` for v1 through v4, enumerates the
player's mouse, and never produces a reading for it. [GameInput on Xbox consoles](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/input/overviews/input-xbox) · [GameInput versioning](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/input/overviews/input-versioning)

## The platform statement, and two independent confirmations

Microsoft's answer on hardware mouse support for UWP titles: "For UWPs, developers are
not able to enable hardware mouse support themselves. There are a handful of UWP games
which were previously published and worked with their appropriate account managers to
coordinate with the platform team to test and enable hardware mouse support for their
title, but it is not broadly available for UWPs. We currently have no intention on
expanding the list of hardware mouse supported UWPs beyond the current list as the
benefit was for games, which have all moved on to a newer development model not using
UWP technology." [Hardware mouse support for UWP](https://learn.microsoft.com/en-us/answers/questions/505382/xbox-uwp-web-app-development-and-hardware-mouse-su)

Two other Xbox UWP projects report the same split this console shows. RetroArch's Dev
Mode request records that only keyboard input works and that mouse support needs
Microsoft; moonlight-xbox closed its request as not planned. The console's browser is a
system app, so its working cursor is not evidence that a sideloaded package can reach
the same input. [RetroArch request](https://github.com/libretro/RetroArch/issues/16934) · [moonlight-xbox request](https://github.com/TheElixZammuto/moonlight-xbox/issues/94)

Mouse mode is a separate feature and it does work: it synthesizes a pointer from the
gamepad, and `RequiresPointerMode` "is supported only on the Xbox device family, and
only when using a game pad or remote control." The player observed stick-driven pointer
movement in run `1795549354714`. [Mouse mode](https://learn.microsoft.com/en-us/windows/uwp/ui-input/gamepad-and-remote-interactions#mouse-mode)

## Console evidence

Every row is a signed build on the same Series X with a physical mouse attached.

| Build | Route tested | Result |
| --- | --- | --- |
| `1.0.232.42` run `1795548159661` | Window-level XAML pointer handlers, native 1920 × 1080 panel | Controller acknowledged; pointer serial and acknowledgment stayed zero. Device Manager showed Xbox virtual mouse entries |
| `1.0.232.43` run `1795548763929` | Pointer handlers on the native game surface's child | Zero for a 26-second sample |
| child `1795549221046`, foreground `1795549354714` | System `gameinput.dll`, v0 ABI | Mouse reading returned with no change in position, wheel, or buttons; gamepad stick did move the pointer |
| `1.0.232.46` run `1795550538225` | HTML-hosted relative `MouseDevice.MouseMoved` | Subscription succeeded; zero relative callbacks, pointer moves, presses, wheel. `CoreWindow` unavailable to the HTML host (`0x800A138F`) |
| `1.0.232.47` run `1795550910488` | Synthetic drag through the hosted window adapter | Passed, exit `0x51A7`, including press, held motion, release with modifiers. The hosted path works when something feeds it |
| `1.0.232.50` run `1795551565471` | Native game panel, six pointer subscriptions, during Factorio gameplay | All six registered; zero events at the gameplay checkpoint |
| `1.0.0.1` run `134400255418672079` | Standalone package, own identity | Foreground view, six subscriptions, `mouse_present=1`, all counters zero |
| `1.0.0.3` run `134400259011162086` | GameInput v0 per-device | Two `GameInputDeviceFamilyHid` mouse interfaces; 3,987 successful polls each; zero changes |
| `1.0.0.4` run `134400260751212089` | GameInput v1–v4 against the system runtime | No `GameInputInitialize` export; `E_NOINTERFACE` (`0x80004002`) for v1–v4; both slots status `0x7`; 1,163 polls returned one timestamp |
| `1.0.0.6` run `134400262219532090` | Microsoft's packaged Xbox `GameInputRedist.dll` 4.1.315, typed v4 | Load and `GameInputInitialize` succeeded; first mouse read returned `GAMEINPUT_E_READING_NOT_FOUND` (`0x838A0003`) |
| `1.0.0.7` run `134400265007362090` | Connection callbacks across a physical replug | Disconnect and reconnect delivered, generation 2, status `0x7`; 1,260 polls of one timestamp afterwards |
| `1.0.0.8` run `134400267213642097` | Keyboard control beside the mouse streams | **12 CoreWindow key-down, 12 key-up, 24 GameInput key-count changes**; both mouse streams `supportedInput=0x20`, zero raw reports, zero motion |
| `1.0.1.1` run `134400394910842110` | The full route matrix below, with restricted capabilities | Installed and ran; every mouse counter zero |

The `1.0.0.8` row is the control that matters: foreground input does reach this package.
Only mouse input is withheld.

## The route matrix

Build `1.0.1.1` (signed APPX SHA-256
`92c28292e656be26b1bf141b34772907d8e8f45aa90f97e80ee389fb120456b5`) sampled every
remaining route in one foreground run, with Xbox mouse mode selected so an inactive
pointer pipeline could not explain a silent result. It reported `pointer_mode=1`,
`visible=1`, `registered=0x3f`, and `window=1` for the CoreWindow handle.

| Route | Observed |
| --- | --- |
| `RegisterRawInputDevices`, `RIDEV_INPUTSINK` and foreground | Export resolves; both fail with Win32 error **120**, `ERROR_CALL_NOT_IMPLEMENTED` |
| `GetRawInputDeviceList` | Error 120; no device list to read |
| `GetRawInputBuffer`, `GetMouseMovePointsEx` | **Not exported**, `user32=0x7d` |
| `GetCursorPos` | Error 120; no system cursor position exists to poll |
| `GetAsyncKeyState` for all five buttons | Resolves; zero transitions |
| HID mouse `0x01/0x02` and keyboard `0x01/0x06` | Selector built; **zero devices enumerated** |
| HID consumer control `0x0c/0x01` and vendor `0xff00/0x01` | Same, and these pages are not system-reserved |
| `MouseCapabilities`, `PointerDevice` | `mouse_present=1`, one mouse pointer device |
| CoreWindow, XAML, relative `MouseDevice` counters | Zero through the player's move, click, and scroll |

The unimplemented routes are recorded separately from the silent ones on purpose:
error 120 from a resolved export is a stub, which is a different finding from a route
that works and delivers nothing. Two of these were already visible elsewhere in this
repository — an ULTRAKILL import control found `user32.dll!GetRawInputBuffer`
unresolved, and a Spider-Man publisher log recorded `RegisterRawInputDevices` failing
with error 120 — but neither came from a process that registered for mouse input and
then read its own queue. [Raw input registration](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerrawinputdevices) · [Reserved HID collections](https://learn.microsoft.com/en-us/windows/uwp/devices-sensors/hid)

### Restricted capabilities

Store approval "isn't required to sideload an app that declares restricted
capabilities," and `inputObservation` is documented as letting an app "observe various
forms of raw input such as HID, touch, pen, keyboard, or mouse being received by the
system regardless of its final destination." That made the capability gate testable
from Dev Mode. The capability layout declares `inputObservation`,
`inputForegroundObservation`, `inputSuppression`, and `inputInjectionBrokered` with the
four HID collections. Dev Mode **installed it without complaint**, which is itself a
result, and it changed nothing: every counter above stayed at zero.
[Restricted capabilities](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/app-capability-declarations#restricted-capabilities)

## What this rules out

The console withholds mouse input above every API a sideloaded package can call. The
layer is not reachable by a capability declaration, an input-source choice, a pointer
mode, a HID open, or a raw input registration, because the input never enters this
partition. No further UWP-side experiment can change that result, so this line of
probing is closed rather than open.

## The GDK, and why it is not a route here

Microsoft's statement above says those UWP games "moved on to a newer development
model." That model is the Microsoft GDK, and it does support mouse: GameInput "exposes
keyboards, mice, gamepads, and other controllers through one consistent interface on
XBOX and PC," for GDK Xbox titles "GameInput is the only input API available,"
`GameInputMouseButtons` lists all five buttons and wheel tilt for "Xbox One family
consoles and Xbox Series consoles," and there is an official GDK mouse sample.
[GameInput introduction](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/input/overviews/input-overview) · [GameInputMouseButtons](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/input/gameinput/enums/gameinputmousebuttons) · [Mouse Input sample](https://learn.microsoft.com/en-us/samples/microsoft/xbox-gdk-samples/mouseinput/)

It is not available to this project. The public GDK is for PC: "A version of the GDK
with Xbox Extensions (GDKX) to target Xbox consoles is only available to licensed
partners in a managed program," "The GDKX is currently only available under confidential
license within an NDA Xbox program," and what the public kit lacks is precisely "The
Xbox developer environment APIs and Tools required to target and build games that run
on Xbox consoles." Console GDK documentation returns "Access to this topic requires
membership in a non-disclosure agreement (NDA) Xbox developer program," and secure
downloads require an organization "onboarded as an Xbox development partner" with "a
valid Microsoft Game Development Kit (GDK) Agreement," with personal Microsoft accounts
unsupported. [Public GDK](https://github.com/microsoft/GDK) · [Access resources](https://learn.microsoft.com/en-us/gaming/gdk/docs/gdk-dev/development-downloads/access-resources)

Registration as an Xbox partner is free, the hard requirements are age, an NDA, and an
eligible country, and there is a documented path to "start exploring the Game
Development Kit (GDK)" with "limited XBOX services for development in secure sandboxes"
after business information, the NDA, and Partner Center enrolment. What no public page
states is whether that early path includes the console extensions; every page that
would is NDA-gated. [Partner onboarding](https://learn.microsoft.com/en-us/gaming/game-publishing/onboarding/overview)

Two things remain true regardless: such access covers developing one's own title, not a
client that installs and runs another store's games, and a working mouse would exist
inside a Game OS title rather than inside this package. Treat the GDK as a different
project, not as a fix for this one.

## What remains possible

The hosted mouse mailbox already works. The `1.0.232.47` synthetic drag control passed
with press, held motion, release, and modifiers. That control and the five-button,
two-axis-wheel version of the shared Windows adapter live on the companion
`codex/factorio-compatibility` branch; on `main` the hosted adapter delivers motion and
the left button, and [hosted-input.cpp](../runtime/presenter/hosted-input.cpp) already
carries the stick-driven cursor. The missing piece is only a source of motion, and
three exist on this console:

1. **Gamepad mouse mode.** Already demonstrated: the stick moves a XAML pointer in an
   app that selects mouse mode. This is what the console's browser gives most players.
2. **A host-supplied delta source** over the local network, for true mouse feel from a
   device the console is not asked to read.
3. **Keyboard**, which the `1.0.0.8` control proves is delivered.
4. **A mouse presented as a game controller** by external hardware, described in the
   next section.

All four terminate in the same mailbox, so building one does not foreclose another.

## Mouse as a controller

The console withholds the mouse but delivers authenticated game controllers. If the
mouse becomes a controller *before* the console sees it, the package reads it through
`Windows.Gaming.Input` like any gamepad. The conversion has to happen in external
hardware, because no layer inside this package ever receives the mouse.

### Candidate adapters

| Route | How it reaches the package | Status |
| --- | --- | --- |
| **Xbox Adaptive Controller (XAC) + RP2040 USB host board** | A mouse feeds a board such as the Adafruit Feather RP2040 USB Host running mouse2xac, or hid-remapper with its XAC device type. The board presents a USB HID joystick, which the XAC maps to a thumbstick. The XAC is a licensed controller and handles console authentication, so the package sees a normal `Gamepad`. | Off-the-shelf firmware, not yet tried on this console. mouse2xac was tested against XAC firmware 5.17.3202.0. The XAC ignores USB HID devices other than joysticks, which is why the board is needed. One teardown reports only the first 8 buttons of a USB joystick are used. |
| **Generic HID joystick read directly** | An RP2040 presents a HID joystick and the package reads it as a `RawGameController`, or as a HID joystick or gamepad collection. No XAC and no authentication. | **Untested.** The route matrix enumerated the mouse, keyboard, consumer-control and vendor pages through `HidDevice`, never the joystick or gamepad pages and never `RawGameController`. Xbox normally requires authenticated controllers, so this probably fails, but it costs one probe run. The [controller pointer probe](../probe/README.md#xbox-controller-pointer) answers it. |
| GP2040-CE Xbox One mode | RP2040 gamepad firmware that authenticates through a passthrough dongle such as the Mayflash Magic-X. | It reads no mouse, so mouse input would have to be added in a fork. |
| Commercial converters (XIM, Cronus, Titan Two) | Closed firmware presenting an authenticated controller. | Their aim curves are built for games and are hard to invert into cursor motion. Not pursued. |

[mouse2xac](https://github.com/touchgadget/mouse2xac) · [hid-remapper XAC device type](https://github.com/jfedor2/hid-remapper/releases/tag/r2024-06-27) · [XAC mouse-to-joystick](https://github.com/gdsports/xac-mouse2joy) · [XAC USB joystick teardown](https://hackaday.io/project/170365-blueretro/log/179869-xbox-one-adaptive-controller) · [GP2040-CE console compatibility](https://gp2040-ce.info/faq/faq-console-compatibility/) · [Xbox Adaptive Controller](https://www.xbox.com/en-US/accessories/controllers/xbox-adaptive-controller)

### Decoding the stick back into motion

Both ends are ours, so the package does not have to treat the stick the way a game does.

- **Velocity encoding.** mouse2xac maps each mouse report's delta directly onto the
  stick (`map(report[1], SCHAR_MIN, SCHAR_MAX, 0, 1023)`) and recenters after 32 ms
  without a report. Deflection is therefore proportional to mouse speed. Integrating
  `deflection × gain × elapsed` in the package recovers displacement independently of
  how often the package or the XAC samples. Upstream loses motion in three ways when
  used as a cursor; see [upstream mouse2xac as a cursor](#upstream-mouse2xac-as-a-cursor).
- **A separate pointer stick profile.** The hosted presenter's existing stick cursor
  ([hosted-input.cpp](../runtime/presenter/hosted-input.cpp)) uses a 20% deadzone
  (`6500/32768`) and a fixed 1.25-second full-screen traverse. An adapter needs a
  linear response, a deadzone sized from the measured resting noise, and a gain
  calibrated against the adapter's scale. mouse2xac also asks for the Xbox joystick
  calibration.
- **Routing by identity.** `RawGameController.FromGameController` exposes
  `HardwareVendorId` and `HardwareProductId`. Routing the adapter to the cursor by
  identity lets the player's own controller stay a gamepad for titles that take it
  natively. Today the presenter reads a single gamepad.
- **Buttons and wheel.** Mouse buttons arrive as joystick buttons and leave the XAC as
  gamepad buttons. Left click can use the existing `padClick` path. Right and middle
  need their own `SDL_HOST_POINTER_*` bits. The wheel likely has to become D-pad presses,
  which hid-remapper can produce.
- **Absolute encoding as an experiment.** The board could integrate position and send it
  as the stick position. That removes drift and sampling sensitivity, but 10-bit axes
  give about two pixels per step at 1920 wide, and a disconnect recenters the cursor.

Expected fidelity is below a real mouse: a 10-bit axis, the console's controller update
rate, and whatever deadzone the XAC or the typed `Gamepad` reading applies. The
controller pointer probe measures all three before any presenter work starts.

### Upstream mouse2xac as a cursor

Read from [mouse2xac.ino](https://github.com/touchgadget/mouse2xac/blob/081f1cafb8c13df16a23d0ed2052255fcf8d7fac/mouse2xac.ino)
at `081f1ca`. Nothing here has run on hardware.

| Behavior | Effect on a cursor |
| --- | --- |
| A single-slot buffer holds the latest mouse report. Reports arriving before the joystick sends it are counted as drops and discarded. | On a 500 or 1000 Hz mouse, most motion never leaves the board. |
| One report's ±127 counts map to the full 0–1023 axis. | The scale depends on the mouse's report rate, so the package cannot use a fixed gain. A fast move on a high-CPI mouse saturates the stick and the excess is lost. |
| The wheel byte is not read. | No scrolling. |
| The report is zero-initialized, so the hat switch sends 0, which means "up", rather than null. | Harmless only if the XAC ignores the hat. |

The fix happens in the firmware, not the package:

- Accumulate motion from every report.
- Write the stick once per fixed output period, encoding velocity on a fixed scale.
- Carry motion beyond full deflection into later periods, up to a bounded backlog.
- Pulse two spare buttons per wheel detent. The XAC reads only the first eight.

The package then needs only one documented constant to recover counts. A fork that does
this, with its decoding contract, is staged separately until hardware is available. Its
output period must be at least the console's controller reading interval, which the
controller pointer probe measures.

### Sequence

1. Run the controller pointer probe with a generic HID joystick board. If it enumerates,
   the XAC is unnecessary and the board can expose more axes and buttons.
2. Run it with the XAC and a board running the mouse2xac fork. Record identity, `step`,
   update interval, resting values, and the raw-versus-typed axis comparison, then set
   the fork's output period and full-scale constants from them.
3. Add a declarative pointer stick profile and identity routing to the hosted presenter,
   verify cursor feel and latency on the console, and have a separate verifier review the
   raw reports.

## Where the probes live

None of the probes in this record ship here, and that is deliberate. Each existed to
answer a question that is now answered, and four dead input paths in the shared probe
layer would have invited a fifth. They remain reachable:

| Probe | Where |
| --- | --- |
| Hosted UWP mouse source control, standalone XAML mouse capture app, GameInput v0–v4 builds, independent `SwapChainPanel` variant | Branch `codex/factorio-compatibility`, under `probe/mouse-source/` |
| Route matrix (`MOUSE_PROBE_ROUTES`, `native-routes.h`), its two-layout build workflow, and `build_tools/prepare_mouse_capture.py` | Branch `claude/xbox-mouse-cursor-tracking-3ab22b`, commit `20e5fc6` |
| The physical mouse diagnostic inside the panel presenter (`NativePanelMouseProbe`) | Same, as `probe/mouse-source/native-panel.h` |

What ships here is this record and the one probe aimed at a route that is still open:
the [controller pointer](../probe/README.md#xbox-controller-pointer), which tests
whether a mouse presented as a game controller can be read.

To retest the closed routes on a future console build, restore `probe/mouse-source`,
`build_tools/prepare_mouse_capture.py`, and `.github/workflows/xbox-mouse-routes.yml`
from `20e5fc6`, run that workflow, and deploy the `capability` layout. The route matrix
is the one worth restoring first: it covers every route in a single foreground run, and
the field meanings are in that revision's `probe/README.md`.

## Open items

- A `baseline` layout at `1.0.1.0` (no restricted capabilities, same binary) was built
  and never run. It would separate an inert capability declaration from a merely
  insufficient one. Answering it now means restoring the probe, and nothing depends on
  the answer: the capability build already recorded zero on every counter.
- Whether the free Partner Center path reaches the console extensions is unresolved and
  only answerable from inside that program.
- Whether hid-remapper's XAC device type converts mouse motion to stick motion itself, or
  only labels the joystick inputs the XAC understands, is not stated in its release
  notes. mouse2xac does convert. Neither has run against current XAC firmware on this
  console.
