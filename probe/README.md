# Xbox capability probes

Each probe exercises a piece of the compatibility layer on an Xbox. The named-pipe case starts with the same helper-exit check as the process case and then requires a successful request and reply. The Steam loader case checks that the diagnostic completed, the DLL loaded, and its factory export resolved.

```sh
python3 -m tools.xbox_deploy probe --list
python3 -m tools.xbox_deploy probe --case named-pipe --version "$version" --json
```

Install the corresponding signed package before running a case. The JSON includes the installed package identity, expected checks, observed fields, and the outcome. A failed check or timeout gives a nonzero exit status. For a graphics variant, record the build option alongside the package version so the child exit code can be interpreted for that control.

| Capability | Console control |
| --- | --- |
| Startup, process creation, and named pipes | The packaged process test and startup control below. |
| Process lifetime across a foreground switch | The [multi-app lifetime control](DEVELOPMENT.md#test-a-foreground-switch), run with `xbox_deploy lifetime`. |
| Loader, SDL, XInput, desktop windowing, and DXGI | The [graphics runtime control](graphics-runtime-control/build.sh) and its build options. |
| Steam DLL loading and native Big Picture launcher | The [Steam Core Loader Probe](../docs/STEAM-CORE.md#run-the-on-console-sign-in) and [native launcher](../analysis/big-picture.md). |
| WebView and window activation | The [primary XAML](big-picture-primary/), [WebView smoke](big-picture-webview/), [renderer integration](big-picture-integration-control/), and [windowed](big-picture-windowed/) controls. |

When adding a single-package control, reuse the relevant runtime adapter and report the operation's outcome and a per-launch `run_id`. Register the package and its terminal checks in [`catalog.py`](catalog.py). The [runner](runner.py) handles fresh reports, deadlines, and verdicts. Use [normal Big Picture Play](../docs/GAME-LAUNCH.md#verification) to verify a title after the control passes.

## Physical mouse input

A sideloaded Dev Mode package cannot read an attached Xbox mouse on any documented
route. Dev Mode runs in the console's `SystemOS` partition while the mouse-capable
GameInput runtime ships inside a GDK title's `GameOs.xvd`, the remaining Win32 raw input
entry points are either unexported or return `ERROR_CALL_NOT_IMPLEMENTED`, no HID
collection enumerates a device, and the restricted input capabilities install without
effect. Ten console builds established this; the evidence, the citation trail, and the
branches holding the retired probes are in [physical mouse input on an Xbox Dev Mode
package](../analysis/xbox-mouse-input.md).

A cursor therefore has to be driven from a source this layer can read: Xbox mouse mode,
a keyboard, a host-supplied delta source, or the controller route below.

## Xbox controller pointer

A physical mouse cannot reach a Dev Mode package, but an adapter that presents the
mouse as a game controller can. [native-app.cpp](controller-pointer/native-app.cpp) is a
standalone XAML app with its own package identity and no Steam dependencies. It lists every
controller `Windows.Gaming.Input` exposes and measures how each one's readings behave.
The [build workflow](../.github/workflows/xbox-controller-pointer.yml) produces two
layouts with `prepare_controller_pointer`: `baseline` declares no capabilities, and
`hid` also declares the HID joystick (`0001 0004`) and gamepad (`0001 0005`)
collections and enumerates them. [Mouse as a controller](../analysis/xbox-mouse-input.md#mouse-as-a-controller)

```sh
gh run download "$run" -n xbox-controller-pointer-x64 -D pointer
python3 -m tools.xbox_deploy deploy pointer/XboxControllerPointer-hid-1.0.2.1.appx \
    --certificate pointer/XboxControllerPointer.cer --timeout 180
python3 -m tools.xbox_deploy probe --case controller-pointer --version 1.0.2.1 --restart --timeout 60
python3 -m tools.xbox_deploy report --version 1.0.2.1 --controller-pointer
```

The case passes once the view has stayed foreground for three consecutive one-second
snapshots, both HID watchers have finished their first enumeration, and every device
they found has finished its open attempt. That is when the app first
writes `status=controller-sampling`. The report is rewritten every second for the life
of the app, and the HID watchers keep running, so a device plugged in later appears
without a relaunch. Read the report again after each step:

1. **Positive control.** With only an Xbox controller connected, `raw_controllers`,
   `gamepads` and `controllers_seen` are at least 1, and that controller's record shows
   `gamepad=1`.
2. **Generic HID joystick.** Plug in a board that presents as a plain USB HID joystick or
   gamepad, such as an RP2040 running hid-remapper. A new `controllerN` record with
   `gamepad=0` and advancing `distinct`, or `opened` above zero on the `hid_joystick` or
   `hid_gamepad` line of the `hid` layout, means the package can read it without
   accessory authentication. A nonzero `hid_*_found` with `open_failed` above zero means
   the console lists the collection but refuses access; `open_hr` records why. The open
   runs on the UI thread so a consent prompt can appear; accept it if the console shows
   one, since an unanswered prompt holds the case at `controller-starting`. If nothing changes
   and `raw_added` stays the same, that route is closed for this console build.
3. **Adapter characterization.** With a mouse-to-joystick adapter connected (directly,
   or through an Xbox Adaptive Controller), leave it still for five seconds, then move
   the mouse in slow and fast strokes on each axis and press each button. Record
   `vid` and `pid`. Each `controllerN_axisM` line gives `first` (the resting value),
   `min` and `max` (the travel reached), and `step`, the smallest change between two
   readings, which estimates the resolution that reaches the package. In
   `controllerN_readings`, `distinct` counts timestamp changes out of `polls`, and
   `read_failures` with `last_read_hr` records rejected reads. The sampler paces itself
   with a high-resolution waitable timer and reports its measured loop period as
   `sampler_period_us`. While that period is shorter than the device's,
   `interval_min` and `interval_avg` describe the device's own update interval in the
   reading's timestamp units. `above_32` counts presses on buttons past the 32-bit
   `seen` mask. The
   `controllerN_gamepad_*` lines record the same device through the typed `Gamepad`
   reading that the hosted presenter uses. A gap between the raw and typed axes there
   points to a deadzone or curve applied above the raw reading.

The app selects `RequiresPointerMode::WhenRequested`, so the console does not turn the
controller into the system pointer. [RawGameController](https://learn.microsoft.com/en-us/uwp/api/windows.gaming.input.rawgamecontroller) · [Raw game controller guide](https://learn.microsoft.com/en-us/windows/uwp/gaming/raw-game-controller)

## Xbox packaged process test

This x64 UWP diagnostic calls `CreateProcessW` for one fixed executable bundled with the app. The helper returns `0x000051A7` when its entry point runs. The app displays the result and writes it to `LocalState/probe-report.txt`. It requires no Steam account, Valve files, or network access on the Xbox.

Version `1.0.9.0` was installed and launched by CLI on a Series X in Developer Mode. The report recorded `create_process=success`, `child_exit_code=0x000051A7`, and `exit_code_matches=yes`. [Signed Windows SDK build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44857081600) · [Developer workflow and hardware observations](DEVELOPMENT.md)

Version `1.0.10.0` also verifies a request and reply over one same-package named pipe. A successful report includes `ipc=success` and `ipc_child_exit=0x000051A8`. Add `--require-ipc` to make that result part of the CLI exit status. [Signed IPC build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44858026281)

### Test in Developer Mode

Configure the Device Portal HTTPS address and certificate fingerprint using the [setup guide](../README.md#setup). Supply a Portal username and password if your console has Portal authentication enabled.

Download the `xbox-probe-windows-sdk` artifact for the commit you want to test and verify `SHA256SUMS`. Use the signed APPX from that artifact with the CLI.

```sh
python3 -m tools.xbox_deploy deploy /path/to/PackagedProcessProbe-x64.appx --test-probe
python3 -m tools.xbox_deploy probe --version 1.0.10.0 --require-ipc
```

Use `--certificate path/to/publisher.cer` if the portal requests the package-signing certificate. The CLI verifies the installed version, launches the app, and waits for a fresh report. If needed, read `probe-report.txt` manually in Device Portal under **File explorer → LocalAppData → Packaged process test → LocalState**. [Device Portal app deployment API](https://learn.microsoft.com/en-us/windows/uwp/debug-test-perf/device-portal-api-core#app-deployment)

### Reading the result

- `exit_code_matches=yes` confirms that the bundled helper ran and returned its expected code on the tested console.
- `create_process=failure` reports the `CreateProcessW` error in `win32_error`.
- A fresh startup checkpoint without a final result identifies the last step the app reached. An unexpected helper exit code is a separate test result even when `CreateProcessW` succeeds.

The console result applies to the fixed helper in this package. Steam client startup and local gameplay remain unverified. The [original prerelease package](downloads/PackagedProcessProbe-x64.appx) is preserved for comparison with the earlier native startup path.
