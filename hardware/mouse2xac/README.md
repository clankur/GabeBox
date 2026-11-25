# mouse2xac cursor firmware

Firmware that lets a USB mouse drive a cursor in the Dev Mode package through an Xbox
Adaptive Controller (XAC). A Dev Mode package never receives a physical mouse, but it
does receive authenticated controllers. This board turns the mouse into a USB HID
joystick, the XAC turns that into a thumbstick, and the package turns the thumbstick back
into cursor motion. See the mouse-as-a-controller section of
`analysis/xbox-mouse-input.md` for why this route exists.

**Status: untested on hardware.** The firmware compiles in CI, and the tuner's encoder,
protocol and simulator have Node tests. Nothing here has run on a Feather, an XAC, or the
console, including the settings storage and the config channel.

Forked from [touchgadget/mouse2xac](https://github.com/touchgadget/mouse2xac) at
`081f1ca`, with the joystick layout from
[touchgadget/flight_stick_tinyusb](https://github.com/touchgadget/flight_stick_tinyusb) at
`ff67cad`. Both are MIT; see [LICENSE](LICENSE).

## Hardware

| Item | Approximate price |
| --- | --- |
| Adafruit Feather RP2040 with USB Type-A Host | $12–15 |
| Xbox Adaptive Controller (XAC) | $100 |
| A USB mouse that supports the boot protocol (nearly all do) | — |
| Optional: 5 V / 2 A barrel-jack supply for the XAC | — |

Plug the mouse into the Feather's USB-A port and the Feather's USB-C port into one of the
XAC's USB ports. Upstream recommends connecting the mouse before connecting the Feather
to the XAC.

### Power

The Feather and mouse are powered from the XAC's USB port, and the XAC is powered by
its internal battery or its USB-C cable. The XAC's 5 V / 2 A barrel jack is meant for
USB accessories that need extra power. A Feather and an ordinary mouse should draw only
about 100 mA, so try first without the barrel supply:

1. **One cable.** Connect the XAC to the console with a USB-C cable. The console
   charges the XAC while it runs.
2. **No cables.** Run the XAC wirelessly on its battery. Expect shorter battery life,
   because the battery also powers the Feather and mouse.

Upstream mouse2xac recommends the barrel supply, without giving a reason. Whether the
XAC's USB ports power the Feather without it, and whether the XAC works wired to a
Series X, still have to be checked on hardware. Fall back to the barrel supply if the
Feather browns out or the mouse fails to enumerate.

## Build and flash

CI builds `mouse2xac.uf2` in the
[mouse2xac firmware workflow](../../.github/workflows/mouse2xac-firmware.yml). To flash it,
hold BOOT, tap RESET, and copy the UF2 onto the `RPI-RP2` drive.

To build locally with `arduino-cli`, install the arduino-pico core and the Pico PIO USB
library at the versions the workflow pins, then compile with the same FQBN. The CPU
clock must be 120 or 240 MHz, and the USB stack must be Adafruit TinyUSB.

## Tuner

[`tuner/index.html`](tuner/index.html) is a static page with no build step. Open it
directly in desktop Chrome or Edge, or serve the folder:

```sh
cd hardware/mouse2xac/tuner && python3 -m http.server 8000
# then open http://localhost:8000/
```

The scripts are classic scripts rather than ES modules, because Chrome does not load ES
modules from `file://` URLs.

- **Simulator.** Click the view to capture this computer's mouse with Pointer Lock. The
  page asks for `unadjustedMovement` so counts skip OS pointer acceleration, and says when
  the browser refuses. It feeds the counts through a port of the firmware encoder
  (`encoder.js`), then samples the stick at a console interval with jitter and decodes it
  with the formula in the [decoding contract](#decoding-contract). It draws the mouse path
  against the decoded path and reports motion in flight, motion dropped by the backlog cap
  and the boot report limit, saturated periods, skipped stick values, a best-fit delay,
  and how far each gesture lands off once the hand stops. CPI presets and a hand speed
  for full deflection set the full scale. The input scale multiplies counts to stand in
  for a higher-CPI mouse than the one at hand.
- **Device.** With the Feather plugged into the computer, Connect asks WebHID for
  Adafruit's vendor ID (`0x239A`) and the joystick collection. The panel reads, writes,
  saves and restores the settings, shows the counters, and draws the board's live
  joystick reports. It can copy settings between the simulator and the board.

`encoder.js` uses single-precision arithmetic in the sketch's order of operations. Run its
tests with `node --test tests/mouse2xac-tuner.test.js`. The parity cases' expected values
came from the sketch's encoder functions compiled on a host. A test also checks the
protocol numbers and setting ranges against the sketch. The tuner has run only in a
headless browser. It has not run against a board.

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
- The encoding is set at runtime over a vendor feature report and kept in flash. See
  [Settings](#settings).

## Settings

The board starts with the settings saved in flash, or with the defaults when flash holds
no valid record. Settings take effect at the next output period after they are written.

| ID | Setting | Default | Range |
| --- | --- | --- | --- |
| `0x01` | Output period, µs | 8000 | 2000–100000. The joystick endpoint is polled every 2 ms. |
| `0x02` | Full scale, mouse counts per second at full deflection | 8000 | 1–2000000 |
| `0x03` | Backlog, in periods of full deflection, including the current one | 6 | 1–64 |
| `0x04` | Invert X | 0 | 0 or 1 |
| `0x05` | Invert Y | 0 | 0 or 1 |
| `0x06` | Wheel buttons | 1 | 0 or 1. With 0, wheel motion is discarded. |

The record in flash, written with arduino-pico's `EEPROM` library, holds a magic number
(`M2XC`), a record version, its size, the six values, and an FNV-1a checksum. The board
falls back to the defaults if any of those is wrong or a value is out of range. Saving
erases a 4 KB flash sector with both cores paused, which stops the mouse host for tens
of milliseconds.

## Config protocol

Tune the board with it plugged into a computer. The console never sends feature reports,
so the channel is idle while the board is on the XAC.

The channel is the 4-byte vendor feature report (usage page `0xFF00`, usage `0x01`, no
report ID) that the upstream report descriptor already declares. The descriptor is
unchanged.

`SET_FEATURE` sends `{command, value bits 0–7, bits 8–15, bits 16–23}`. Each command also
selects the register that the next `GET_FEATURE` returns as `{register, value bits 0–7,
bits 8–15, bits 16–23}`. To read a register, select it and then get it.

| Command | Value | Effect | Next GET returns |
| --- | --- | --- | --- |
| `0x00` select | Register | — | That register |
| `0x01` save | Ignored | Writes the settings to flash from the output loop | Status |
| `0x02` restore defaults | Ignored | Sets the defaults in RAM. Save to keep them. | Status |
| `0x03` reload | Ignored | Discards unsaved changes | Status |
| `0x04` clear counters | Ignored | Zeroes the report and clip counters | Mouse reports |
| `0x80` + setting ID | New value | Writes one setting if it is in range | That setting |

| Register | Contents |
| --- | --- |
| `0x01`–`0x06` | Settings, as above |
| `0x10` | Firmware version: major, minor, patch from the high byte down. Currently 0.1.0. |
| `0x11` | Protocol version, currently 1. Selected at boot. |
| `0x12` | Mouse reports received since boot or the last clear, modulo 2^24 |
| `0x13` | Mice connected |
| `0x14` | Counts dropped by the backlog cap since boot or the last clear, saturating at 2^24 − 1 |
| `0x15` | Status: bit 0 flash holds valid settings, bit 1 save pending, bit 2 unsaved changes, bit 3 last save failed |
| `0x16` | Flash saves since boot |
| `0xFF` | Last error: byte 0 code, byte 1 command, byte 2 low byte of the value |

A rejected command selects `0xFF`. Error codes are 1 for an unknown command, 2 for an
unknown register or setting, and 3 for a value out of range. A rejected write leaves the
setting unchanged.

After a save, poll the status register until bit 1 clears.

TinyUSB can run the feature-report callbacks from a core 0 interrupt that preempts the
output loop. The callbacks therefore only change the settings in RAM and set flags. The
output loop copies the settings with interrupts disabled and writes flash itself. The
callbacks never take the mutex that guards motion shared with core 1.

## Decoding contract

The output period, full scale, backlog and inversion are now settings (see
[Settings](#settings)). The package must decode with the values the board holds. The
values below are the defaults.

| Item | Default |
| --- | --- |
| Output period | 8 ms. Each stick value is held for one period. |
| Full deflection | 8000 mouse counts per second (5 in/s on a 1600 CPI mouse) |
| Joystick axis | 10-bit, center 512, reach ±511. X right is positive. Y down is positive, unless inverted. |
| Counts per stick unit per period | full scale × period / 511; 8000 × 0.008 / 511 ≈ 0.125 |
| Backlog limit | 6 periods of full deflection (384 counts). Motion above it is dropped. |
| Buttons 1-5 | Left, right, middle, back, forward |
| Button 6 | Wheel up, one press per detent |
| Button 7 | Wheel down, one press per detent |

The package recovers displacement by integrating `deflection × full scale × elapsed`
(8000 counts/s by default), where deflection is the normalized stick value from -1 to 1. It then applies its own
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
  are skipped and motion lost. Set the output period from the probe's measured interval.
- A boot report carries at most 127 counts per axis, so a 1000 Hz mouse tops out at
  127,000 counts/s. What a mouse does with motion above that varies.
- Fidelity is below a real mouse: a 10-bit axis, the XAC's own processing, and the
  console's controller update rate all sit in between.

## Verification still needed

1. Flash the board and confirm the XAC accepts it on current XAC firmware. Upstream was
   tested on 5.17.3202.0.
2. Run the controller pointer probe with the board attached. Record the reading interval,
   the smallest axis step, the resting values, and any deadzone between the raw and
   typed readings.
3. Write and save settings over the config channel from a computer, then confirm they
   survive a power cycle, that a corrupt or empty record falls back to the defaults, and
   that the XAC still accepts the board.
4. Set the output period and full scale from the probe results and update the decoding
   contract to match.
5. Add a pointer-stick profile to the hosted presenter that decodes this contract, then
   check cursor feel on the console.
