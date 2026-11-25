# mouse2xac cursor firmware

Firmware that lets a USB mouse drive a cursor in the Dev Mode package through an Xbox
Adaptive Controller (XAC). A Dev Mode package never receives a physical mouse, but it
does receive authenticated controllers. This board turns the mouse into a USB HID
joystick, the XAC turns that into a thumbstick, and the package turns the thumbstick back
into cursor motion. See the mouse-as-a-controller section of
`analysis/xbox-mouse-input.md` for why this route exists.

**Status: untested on hardware.** It compiles in CI. Nothing here has run on a Feather,
an XAC, or the console.

Forked from [touchgadget/mouse2xac](https://github.com/touchgadget/mouse2xac) at
`081f1ca`, with the joystick layout from
[touchgadget/flight_stick_tinyusb](https://github.com/touchgadget/flight_stick_tinyusb) at
`ff67cad`. Both are MIT; see [LICENSE](LICENSE).

## Hardware

- Adafruit Feather RP2040 with USB Type-A Host
- Xbox Adaptive Controller, powered from a 5 V / 2 A supply
- A USB mouse that supports the boot protocol (nearly all do)

Plug the mouse into the Feather's USB-A port and the Feather's USB-C port into one of the
XAC's USB ports. Upstream recommends connecting the mouse before connecting the Feather
to the XAC.

## Build and flash

CI builds `mouse2xac.uf2` in the
[mouse2xac firmware workflow](../../.github/workflows/mouse2xac-firmware.yml). To flash it,
hold BOOT, tap RESET, and copy the UF2 onto the `RPI-RP2` drive.

To build locally with `arduino-cli`, install the arduino-pico core and the Pico PIO USB
library at the versions the workflow pins, then compile with the same FQBN. The CPU
clock must be 120 or 240 MHz, and the USB stack must be Adafruit TinyUSB.

## What changed from upstream

Upstream mapped each mouse report directly onto the stick and recentered after 32 ms
without a report. Used as a cursor, that loses motion three ways:

| Upstream behavior | Effect on a cursor | Here |
| --- | --- | --- |
| Keeps only the latest report. A report arriving before the previous one is sent is dropped. | On a 500 or 1000 Hz mouse, most motion never leaves the board. | Every report's motion is added to a shared accumulator. |
| Maps a single report's ±127 counts to full deflection. | Scale depends on the mouse's report rate, and a fast flick saturates the stick and loses the excess. | Deflection is velocity on a fixed scale, updated once per output period. Motion beyond full deflection carries into later periods, up to a backlog limit. |
| Ignores the wheel. | No scrolling. | Wheel detents pulse two spare buttons. |

Also:

- A click shorter than one output period is still held for one period.
- The hat switch rests at null instead of 0 ("up").

## Decoding contract

The package must decode with these values; they are the constants at the top of
[mouse2xac.ino](mouse2xac.ino).

| Item | Value |
| --- | --- |
| Output period | 8 ms. Each stick value is held for one period. |
| Full deflection | 8000 mouse counts per second (5 in/s on a 1600 CPI mouse) |
| Joystick axis | 10-bit, center 512, reach ±511. X right is positive. Y down is positive. |
| Counts per stick unit per period | 8000 × 0.008 / 511 ≈ 0.125 |
| Backlog limit | 6 periods of full deflection (about 383 counts). Motion above it is dropped. |
| Buttons 1-5 | Left, right, middle, back, forward |
| Button 6 | Wheel up, one press per detent |
| Button 7 | Wheel down, one press per detent |

The package recovers displacement by integrating `deflection × 8000 counts/s × elapsed`,
where deflection is the normalized stick value from -1 to 1. It then applies its own
cursor gain. Integrating over the time it actually observed each value keeps the result
correct whatever rate it samples at, provided it samples at least once per period.

The XAC sits between the board and the package. How it maps the joystick onto a gamepad
stick, including any deadzone, curve, inversion, or calibration, is not known yet. The
controller pointer probe measures that. Expect the package-side constants to change once
it has.

## Limits

- Only the boot-protocol report layout is read: buttons, 8-bit X, 8-bit Y, and an
  optional 8-bit wheel. TinyUSB requests the boot protocol from boot-capable mice. A
  mouse without boot support may send a different layout.
- The XAC reads only the first 8 joystick buttons, so the wheel uses buttons 6 and 7 and
  button 8 is unused.
- The output period must be at least the console's controller reading interval, or values
  are skipped and motion lost. Set `OUTPUT_PERIOD_US` from the probe's measured
  interval.
- Fidelity is below a real mouse: a 10-bit axis, the XAC's own processing, and the
  console's controller update rate all sit in between.

## Verification still needed

1. Flash the board and confirm the XAC accepts it on current XAC firmware. Upstream was
   tested on 5.17.3202.0.
2. Run the controller pointer probe with the board attached. Record the reading interval,
   the smallest axis step, the resting values, and any deadzone between the raw and
   typed readings.
3. Set the output period and full-scale constants from those results and update the
   decoding contract to match.
4. Add a pointer-stick profile to the hosted presenter that decodes this contract, then
   check cursor feel on the console.
