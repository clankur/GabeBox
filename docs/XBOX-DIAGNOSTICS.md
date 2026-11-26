# Xbox runtime diagnostics

Use the same Device Portal configuration and verified certificate pin as the deployment CLI. The observer reads an already-installed Steam package and writes one JSON result to standard output. Status checkpoints go to standard error. Open Steam or select Play on the console after the observer prints `waiting-for-startup` or `waiting-for-launch`. Those checkpoints mark the end of the baseline read. For a package update, complete installation separately before starting the observer.

Use the [probe catalog](../probe/README.md) for isolated checks of process creation, IPC, graphics helpers, and Steam DLL loading. A probe launches its selected test package and checks a fresh report. The observer below follows the installed Steam app during normal use.

## Observe Big Picture startup

```sh
python3 -m tools.xbox_deploy observe --version "$version" --startup --output startup-run
```

The observer waits for both the native sign-in run and the WebView run to change. The result includes the UI checkpoint, game count and startup timings when the package reports them. `setup-ready` records the setup surface. `gamepad-ready` records native gamepad readiness.

### Big Picture renderer failure during a game

Big Picture closes its WebView when the renderer reports `process-error:...`, including an unresponsive render process (`process-error:owner:2:unresponsive:259:none`). It does the same when it covers a game for the Menu + View chord and Valve's controls do not draw within 10 seconds (`overlay-unresponsive`). The bridge confirms the controls with an `overlay-ready` message two animation frames after it requests them. On Slay the Spire 2 (`1.0.246.2`, `1.0.250.3`), the owner renderer instead used about one CPU core and grew by roughly 6 MB/s from the chord onward. The overlay stayed white and A did nothing. Chromium reported the renderer unresponsive about 75 seconds later. If a game is running when the WebView closes, Big Picture keeps the game going:

- If the game was covered by the Steam view, Big Picture shows it again. The presenter gives keyboard focus back to the account host, and hosted input leaves its overlay pause.
- The Menu + View chord leaves the game shown, without covering it, and resumes its input until the game exits.
- When the game exits, Big Picture reopens through the same path a released renderer uses.
- If the game cannot be shown again after one retry, Big Picture restarts at once and takes over the running game, so Valve's Resume and Exit Game still reach it.

`game-restored` means that the panel presented new frames and that hosted input was live within 3 seconds, read from panel statuses published after the show. Live means unpaused, with the CoreWindow focused, visible and accepting input, and with the view's window holding Win32 keyboard focus (`native_window_focus=1`). With the native controller profile, the game must also have acknowledged the first controller sample published after the show (`native_pad_ack` at or past `native_pad_resume_serial`), which shows that the shown game polls hosted input again. Otherwise Big Picture retries once. It then records `game-input-resume-failed` with a reason and leaves the game shown. The reasons are `input-paused`, `input-background`, `window-unfocused`, `pad-unacknowledged`, `input-unreported`, `no-frames`, `panel-hidden`, `panel-closed` and `game-exited`. Both records end with the live fields that decided them, for example `game-restored appid=2868840 input=visible=1,paused=0,active=1,window_focus=1,keys=8,pad_serial=22,pad_ack=22`. A key event cannot be required, because nobody may press a button during the 3 seconds; a press afterwards shows up as `keys_after_show` in `inspect-input --panel-menu`. The `game_frame_media` line is not updated after the WebView is lost, so read the panel's state after a recovery from this suffix. A normal Resume records the same check as `game-input-resume-failed:REASON` in the status history.

`big-picture-webview-report.txt` records each transition as a numbered `webview_recovery_N=` line, and the restarted session keeps those lines. The transitions are:

- `game-webview-lost`, with the AppID, whether the game was covered, and the native cause
- `game-restore-started`, `game-restore-retry`, `game-restored`, `game-input-resume-failed` or `game-restore-failed`
- `game-kept-foreground`
- `game-overlay-unavailable`, when the Menu + View chord arrives while Big Picture is down. The game stays shown and its input resumes. (`menu_switch=declined` is written only while a Big Picture session is open, for example when it was released for the game.) On `1.0.250.3` neither appeared after the watchdog restore: `chord_held` stayed at 1 because no key event, and so no remote chord, reached the presenter (see the Win32 focus note below).
- `webview-restart-after-game` or `webview-restart-for-game`
- `game-adopted`, or `game-exited-before-adopt` when the game exited during the restart or its process is gone when Big Picture is ready. Big Picture then adopts nothing and leaves the library free for another title.
- `game-exited-unreported`, when a configured Play finds that the current game's process is gone but its exit was never reported, and releases the game

The white overlay was a render loop in Valve's running-game controls, which the offline shell test reproduces with Valve's pinned client UI (`node tests/steam-big-picture-shell.cjs ValveClient/steamui` with Playwright's Chromium). Their **Switch windows** section reads the game's window IDs from Valve's `WindowStore`. Steam fills that store from focus events, and the Xbox focus events list no game windows because a hosted game draws in the shared view. For an app without an entry, `GetAppWindowIDs` returns a new empty array on every read. The section's effect then sees a changed dependency on each render, calls `SteamClient.System.UI.GetGameWindowsInfo` again and stores the answer, which renders again. Offline that was 86,089 calls in 3 seconds, with React busy on the main thread and Playwright unable to click **Resume game**. The bridge now gives the running game a stable, empty window list (`trackXboxGameWindows` in [the window adapter](../ui/big-picture-bridge/xbox-window.js)) and removes it when the game stops. The same check then makes one call, and the main thread is idle. The shell test fails when the controls request window titles more than twice or raise a script error.

Without the window-list fix, the shell test against the `1.0.250.3` client UI fails with `The running-game controls settle: 44809 window-title requests`; with it, the controls make one request and **Resume game** responds.

The `Minified React error #520` in `renderer_error_latest` is React 19.1's recoverable-error report: a component threw during a concurrent render, and React recovered by rendering the whole root again synchronously. It is not itself a loop. Its cause was `SteamClient.Input.StartEditingControllerConfigurationForAppIDAndControllerIndex is not a function`, thrown while the controller page loaded the layout to edit. The input adapter now rejects that call, which Valve handles by marking the layout as failed to load. The renderer report now names the cause of such an error before the error itself, as in `CAUSE <- Uncaught Error: Minified React error #520`.

The report keeps the first renderer script failure as `renderer_error`. When more follow, it also writes `renderer_error_count` and `renderer_error_latest`, which records the latest failure with the status it arrived in. Valve's audio store sends `Audio.GetSupportedFeatures` once at startup and has no rejection handler. The bridge now answers that request as an unavailable service, so it no longer fills `renderer_error`.

### WebView2 child processes

The Big Picture renderer writes `LocalState\big-picture-webview-processes.txt`. Each host process starts the file again, and it stops growing at 256 KiB. The file has one line when each WebView2 environment is created, and one line for every `ProcessInfosChanged` event:

```text
webview2 environment env=1 utc=2026-11-26T04:00:01.250Z tick_ms=900 arguments=default
webview2 processes env=1 utc=2026-11-26T04:00:02.000Z tick_ms=1650 count=6 added=browser:40,renderer:41,gpu:44,utility:45,utility:46,utility:47 removed=-
webview2 processes env=1 utc=2026-11-26T04:01:44.461Z tick_ms=104111 count=7 added=utility:52 removed=-
```

`added` and `removed` list `kind:pid` pairs, where the kind is a `COREWEBVIEW2_PROCESS_KIND` name. To find how long a child lived, match its pid in the `added` and `removed` lines, then look the pid up in the `sample-processes` output. `utc` is when the handler ran on the host's UI thread, so it trails the actual process change by the dispatcher's delay. `observe --output` saves the filtered lines as `webview2-processes.txt`. Outside an observation, use `get-file '\LocalState' big-picture-webview-processes.txt --output …`.

The browser arguments stay at their defaults unless `xbox-webview2-arguments.txt` is in the package root before signing. The file may hold `--disable-component-update`, `--disable-background-networking` or both, separated by spaces or newlines, at most once each and in at most 256 characters. Anything else rejects the whole file, and the browser keeps its default arguments. The environment line then reads `arguments=package-file switches=…`, `arguments=invalid` or `arguments=unreadable`. Both switches disable Chromium background services (component updates, and background network requests such as variations and safe-browsing list updates) for this host's browser process only. A change to the arguments applies only once the existing browser process for the profile has exited, so restart Steam Core after you install the file.

## Inspect an active Steam install

```sh
python3 -m tools.xbox_deploy report --version "$version" --download
python3 -m tools.xbox_deploy inspect-game --version "$version" --app-id "$steam_app_id"
```

The download report identifies its counter source in `steam_download_measurement`. For `steam`, the bundled client supplies live download and staging counters for the active AppID. A rate needs consecutive live samples with comparable totals. The report keeps the on-disk manifest values under `steam_download_manifest_bytes_*` when both sources are available.

Steam can update title-specific depot records while its on-disk manifest still shows zero bytes. When live counters and meaningful manifest counters are unavailable, `steam_download_depot_activity=observed` records a recent change in those depot records. Use it as an activity signal. For a transfer rate or installed byte count, use the corresponding byte counters.

## Observe a game launch

```sh
python3 -m tools.xbox_deploy observe --version "$version" --app-id "$steam_app_id" \
    --frame-seconds 40 --timeout 180 --output game-run
```

Use `--image-name "$game_exe_basename"` to also follow the game's Device Portal process listing. For an already-started session, pass the exact `--run-id` shown as `steam_launch_run_id` by `python3 -m tools.xbox_deploy report --version "$version"`. For a fresh observation with no baseline run ID, the observer uses the local clock to screen out stale reports. Keep the computer and console clocks in sync for that case.

For an unattended trial, prepare the Big Picture layout with `--play-installed-app-id APPID` and sign it. This requests one Play through Big Picture per installed package version after sign-in, subject to Steam's ownership and completed-install checks. Install the package without launching it, start the observer, and activate the app after `waiting-for-launch`. The observer saves filtered `unity-startup.txt` checkpoints when the matching launch reports a Unity player log. Those checkpoints include graphics selection, managed assembly reload, and large allocation errors.

The deadline covers observation of the selected run. `--timeout` and `--frame-seconds` each accept up to 3600 seconds, so one run can follow an hour of gameplay. The bound only catches typing errors. The observer polls about once a second and keeps a bounded amount of trace data, whatever the duration. The JSON `outcome` distinguishes a first hosted frame, a process exit, a launch failure, or a timeout. The CLI exits with a nonzero status for an observation timeout or an argument, connection, or portal error. Terminal game outcomes are carried in JSON. `frame-observed` records a presentation milestone, which can include a black startup frame. Use the game on the console to verify input, audio, save behavior and return to the Library.

### Connection drops and certificate changes

The observer keeps collecting through transient Device Portal failures: timeouts such as `[Errno 60] Operation timed out`, connection resets and refusals, and dropped TLS or HTTP connections. It retries a failed read after 0.5, 1, 2 and 4 seconds, within the observation deadline. If the portal still does not answer, that read is skipped for the current poll and the next poll tries again. While the portal stays unreachable, each read is tried once per poll. A skipped process listing leaves the previous `--image-name` state unchanged, so an outage does not end the run as `game-process-gone`. Status lines `portal-unreachable …; retrying` and `portal-recovered after Ns` go to standard error.

When any read was retried, the result has a `portal` object with the number of `retries`, the number of `skipped_reads`, and each `gaps` entry's `start_utc`, `end_utc`, `seconds`, error type and errno, `failed_attempts` and `skipped_reads`. `unrecovered_gap` describes an outage still open when the observation ended. Compare gap times with frame traces and loader records: data written during a gap is collected on the next successful read, but a staging folder deleted during a gap cannot be read again. HTTP errors other than the existing 404 and 409 handling still stop the command.

A certificate that differs from `XBOX_PORTAL_CERT_SHA256` is never retried or trusted. Every command stops with the console's new SHA-256 fingerprint. The console can create a new portal certificate after a reboot or a Dev Home reset. Confirm that the fingerprint belongs to your console, for example with `python3 -m tools.xbox_deploy fingerprint` on a trusted network, before updating the pin. An observer with `--output` first saves what it already copied, with outcome `portal-certificate-changed`. Interrupting the observer with Ctrl-C saves the same way with outcome `interrupted`.

### Require game startup checkpoints

Use a short sequence of loader records from the diagnostic build under test. Keep the initial GPU submission as one checkpoint and use a later engine event for the transition you want to verify.

```sh
python3 -m tools.xbox_deploy observe --version "$version" --app-id "$steam_app_id" \
    --image-name "$game_exe_basename" --timeout 180 --checkpoint-timeout 20 \
    --require-checkpoint 'dxgi gpu-frame-submitted' \
    --require-checkpoint "8:$game_ready_checkpoint" \
    --watch-checkpoint "$game_progress_record" \
    --fail-on-checkpoint 'dxgi gpu-frame-error result=0x887A0005' \
    --output startup-checkpoints
```

Each checkpoint matches a complete filtered loader record or its opening tokens. Use `key=*` to accept any nonempty value for that field while still matching the following tokens. The required records must appear in order. Prefix a required record with `SECONDS:` to override `--checkpoint-timeout` for that stage. The first budget begins when the child, its window, or its first frame is observed. Each new required record starts the next budget. Repeated markers and frame presents leave its deadline in effect. The overall `--timeout` still covers staging and the full observation.

The result reports the checkpoints reached, the trace source, and the next expected record with its time budget. `--watch-checkpoint` includes the last matching record and its source, or `null` if the run produced no match. Watched records leave the deadline in effect. `checkpoint-stalled` means the expected record failed to appear in time. `checkpoint-failed` identifies an explicitly selected failure record. An in-flight Device Portal read or final artifact collection can delay the command's return. The CLI returns a nonzero status if the required sequence is incomplete and saves the available artifacts. An attached `--run-id` reads the current launch's existing trace and gives its unresolved checkpoint a fresh budget. The observer leaves the Xbox session running.

## Inspect the current game's input

Use the AppID and `steam_launch_run_id` from the current hosted launch. Take one snapshot to inspect the connection bit, button mask, six SDL gamepad axes, pointer and gamepad serials and acknowledgments, and keyboard readiness.

```sh
python3 -m tools.xbox_deploy inspect-input --version "$version" --app-id "$steam_app_id" \
    --run-id "$launch_run_id"
```

When `sampling-host-input` appears, hold a controller button or move a stick, then release before the sample ends. Hold each input long enough to span a poll.

```sh
python3 -m tools.xbox_deploy inspect-input --version "$version" --app-id "$steam_app_id" \
    --run-id "$launch_run_id" --seconds 10 --panel-menu --output input-check
```

`host-idle` in `activity` means the sampled serial did not change. `host-activity` means the host published a change. In an individual sample, `unacknowledged` means the game has yet to acknowledge that serial, and `acknowledged` means the game-side adapter read it. Compare several samples to distinguish a transient delay from a repeated gap. `updating` means the serial changed during the read or publication was in progress. Keyboard readiness records whether the game enabled the queue.

`--panel-menu` reads the package's optional native presenter milestones independently of game input. `added` counts readiness, held chords, and menu requests recorded during a sampling window when the checkpoint sequence still contains the baseline. The file is package-scoped and its cumulative counts may include earlier activity. The command reads mailbox metadata only and rechecks the launch before emitting JSON or saving it.

Add `--keyboard` to read the queue's read, write, and reset counters when the game enables it. The sample contains no key events or text.

Add `--rumble` to read the selected launch's optional vibration mailbox after a controller effect. The connection state can be `armed`, `disarmed`, or `updating`. On an instrumented build, `bridge_attached` and `presenter_attached` identify the endpoints. The flags show whether a connected XInput slot requested a nonzero effect, the bridge accepted it, and the presenter received a successful WinRT result. `stop_applied` records a successful zero command after a nonzero attempt. `device_error` records any failed WinRT vibration call. These flags accumulate for the launch, including while the controller is disconnected or the presenter is covered. They can capture an effect that starts and stops between two diagnostic reads. Older sidecars leave the flags clear. Confirm physical vibration and cancellation on the controller.
The presenter accepts Menu + View only when one source holds both buttons: the live Windows.Gaming.Input reading, or the VK_GAMEPAD key events that Device Portal remote input and a physical pad raise. The two presses must land within 400 ms of each other, and the pair must stay held for 50 ms. Big Picture opens when both buttons are released. If one button stays down for more than 1 s after the other is released, the chord is dropped, and that button stays hidden from the game until it is released. `chord_from_keys` counts held chords that came from key events; the rest of `chord_held` came from the pad. `chord_ignored` counts times both buttons were down without a paired press, such as a View held for a game action or a key whose release was lost. `chord_expired` counts dropped chords. The panel status reports the same totals as `native_menu_chords_ignored` and `native_menu_chords_expired`.

On `1.0.250.3`, after `game-restored`, the panel had `panel_focus=restored` and `panel_input=resumed`, but `keys_after_show` stayed at zero, the remote pad serial stopped, and a second chord never reached the presenter. Big Picture's WebView2 takes Win32 keyboard focus for its browser input window when it covers the game. XAML does not track that window, so focusing the account host afterwards reported success without returning Win32 focus to the CoreWindow, which then raised no key events. Each time the game is shown, the presenter now first gives Win32 focus back to the view's window when another window, or none, holds it, and records `panel_window_focus=taken from=other|none` (or `=declined`). While the game is shown, it also takes focus back, at most once a second, when no window holds it (`panel_window_focus=retaken from=none`). It resolves `GetFocus` and `SetFocus` at run time, from the rtcore API set or user32, and reports nothing when neither has them. The panel status reports `native_window_focus` (1 the view's window, 0 another window, 2 none) and `native_window_focus_taken`. `inspect-input --panel-menu` counts `window_focus_taken`, `window_focus_retaken` and `window_focus_declined`.

Each time the game is shown again, the presenter gives keyboard focus to the account host. Normal gameplay keeps focus there, and from there the CoreWindow raises the key events that carry remote controller input, the keyboard and the remote chord. `focus_restored` counts successful hand-offs, and `focus_declined` counts hand-offs the XAML control refused. `input_resumed` counts exits from the overlay pause. `keys_after_show` counts shows that were followed by a key event. After a recovery, a remote button press that leaves `keys_after_show` at zero means that key events still do not reach the window. The panel status reports `native_input_active`, `native_input_paused`, `native_key_events` and `native_focus_restores`. After Resume, the first controller sample is published under a new serial even if it has not changed, so `inspect-input` shows the game acknowledge it.

### Panel composition counters

A white flash over a game ([clankur/GabeBox#89](https://github.com/clankur/GabeBox/issues/89)) can only come from whatever is behind the game panel. Everything behind it is now black: the XAML page (`Grid` background), the account host's WebView default background, the Steam composition host, and both Big Picture WebView2 controllers (`DefaultBackgroundColor`). A gap in the panel's composition therefore shows the letterbox colour.

The presenter counts what the panel went through, and reports the totals in the panel status and, when the package enables the frame trace, in `fhd-panel-stats.txt`. It rewrites that file in place at most once a second, so it stays small during a long session. `inspect-input --panel-menu` reads it into `panel_menu.composition`, with `totals` and, over the sampling window, `added`:

| Field | Counts |
| --- | --- |
| `panel_hidden` | changes from shown to hidden |
| `panel_hidden_shell` | hides because the Steam shell was shown over the game |
| `panel_hidden_panel` | hides because the panel was collapsed or had no size |
| `panel_hidden_window` | hides because the view was hidden or not active in the foreground |
| `panel_uncovered` | changes from hidden to shown, including the first show |
| `panel_reordered` | changes in the panel's place among the page's layers (account host, panel, Steam host) |
| `panel_repeat_presents` | presents of a producer frame that was already presented (`repeat_presents` in the panel status) |
| `panel_presented` | all presents |

During steady gameplay, `added` should show no hides, shows or reorders. A flicker with rising `panel_hidden_window` points at the view's activation. Rising `panel_hidden_shell` points at Big Picture's foreground state. Rising `panel_reordered` points at a layer moving over the game. If all of them stay at zero while the flicker is visible, the cause is inside the frames themselves.

## Drive a hosted game with remote input

`python3 -m tools.xbox_remote_input` sends controller, keyboard and mouse input through Device Portal's `/ext/remoteinput` WebSocket. The presenter merges the controller presses into the hosted pad, so games reading hosted XInput receive them as controller input. Each step can be followed by a screenshot:

```sh
python3 -m tools.xbox_remote_input --gap 1.0 A wait:3 shot:after-a.png DOWN Y hold:A:0.5
```

`chord:MENU+VIEW` presses both buttons together, holds them for 0.25 s, then releases them. It is the deliberate Big Picture chord. Separate `MENU` and `VIEW` taps never form one.

To overlap holds, join `NAME[:SECONDS][@START]` items with `+` in one `hold:` step. Each input is pressed `START` seconds after the step begins (default 0) and released `SECONDS` later (default a 0.12 s tap). For example, `hold:VIEW:2+MENU:0.3@1.0` holds View, presses Menu one second later for 0.3 s, and releases View at two seconds. That pair lands more than 400 ms apart, so the presenter counts it in `chord_ignored` and does not open Big Picture. Releases and presses due at the same moment are sent release first. `--dry-run` prints the offset of each message.

Run `--help` for the step list. Each step is logged to standard output with the UTC time, to the millisecond, at which it started. For `wait:` that is when the pause began, like every other step. Use those times to find a step in a frame trace or loader record. `--check` opens and closes the WebSocket without sending input. Confirm delivery with `inspect-input`: a held button shows `connected: true` and its bit set in the host pad, and the game's `ack` follows once it reads the pad. Device Portal mouse messages do not raise host pointer events.

Device Portal screenshots of the Big Picture overlay can come back as a valid PNG that is entirely white, not as an HTTP error. A `shot:` step whose image is a single colour adds the colour to its log line, for example `uniform-color=#ffffffff`. `screenshots` records the same thing as `uniform_color`. To see the overlay, use the package's own surface capture under [Capture the Big Picture surface](#capture-the-big-picture-surface). [Remote Input API](https://learn.microsoft.com/en-us/windows/uwp/xbox-apps/uwp-remoteinput-api) · [Slay the Spire 2 example](../analysis/slay-the-spire-2-compatibility.md#controller-input-chain)

## Inspect the current game's render state

Use the AppID and `run_id` from the game observation while that launch is current.

```sh
python3 -m tools.xbox_deploy inspect-render --version "$version" --app-id "$steam_app_id" \
    --run-id "$launch_run_id" --output render-check
```

The command reads the selected package's GPU mailbox and reports the source texture dimensions, DXGI format, producer sequence, and the generations published and accepted by the two ends of the exchange. A publisher changing its texture generation produces an `updating` state. Take another sample to see the stable configuration.

With `--output`, the command saves filtered metadata to `observation.json` and copies available run-scoped captures to `bootstrap.bmp`, `bootstrap-late.bmp`, and `requested.bmp`. The package must have enabled and written the captures. Missing or incomplete files have a state in the JSON result. Available window and SDL checkpoints are saved to `sdl-startup.txt`. The command checks launch identity again before saving. Compare screenshots to inspect visible content, and use the frame trace to measure presentation cadence.

## Inspect the current game's loader trace

For a game that reports `steam_shadow_stage=verified-local-copy`, use the AppID and exact `steam_launch_run_id` from the same package version.

```sh
python3 -m tools.xbox_deploy inspect-loader --version "$version" --app-id "$steam_app_id" \
    --run-id "$launch_run_id" --output loader-check
```

The command reads the live trace from a single staging directory created within five minutes of launch. After stage cleanup, it can read the terminal report named by the same launch and include the child exit code. The JSON identifies the source as `live-stage` or `exit-report`. The command stops if the launch or its diagnostic source changes during collection, or if multiple staging directories match.

The JSON summary gives the latest filtered records and counts for graphics pipeline compilation, boot waits, and shell requests. Counts cover recorded events, including repeats. If the runtime records graphics or compute pipeline request ordinals, `pipelines` reports the highest recorded request for each kind, the most recent sampled commit headroom, and the last observed failure HRESULT when present. The request ordinal is a lower bound and can include an in-flight request. If the runtime also samples its cumulative counters, `latest_counter_sample` gives the counter values and whether a pipeline call was active at that instant.

The inspector reads up to 1 MiB of the live trace and marks `truncated` when more bytes are available. A record still being written is skipped. A terminal report must include its completion marker. Pass `--output` to save the summary and the complete allowlisted records within the read limit. The inspection only reads the selected package's diagnostics through Device Portal.

A focused Win32 profile can redirect `ExitProcess` and `TerminateProcess` to `XboxWin32LoaderTrace.dll` to record the exit API, code, and caller. For a self-exit with `0xFFFFFFFF`, the adapter also attempts to record up to eight module-relative frames from the terminating thread. A trace can show a direct exit from an engine worker. When the engine returns through `main` first, the stack only shows the remaining runtime frames. The trace distinguishes a missing capture export, a missing module lookup, an empty stack, and a stack with no mapped frames. The adapter's x64 build includes unwind metadata for its own frames.

A profile that redirects `RaiseException` can also capture up to eight module-relative frames for each of the first four MSVC C++ throws as `fault cxx-stack`. This records the throw site before the game's exception handler runs. The adapter forwards the exception code, flags and arguments unchanged.

A signed working-directory profile records `asset cwd-before`, `asset cwd-set` and `asset cwd-after`. These checkpoints identify whether the current directory matches the verified install directory and whether the native set operation succeeded. For profiles that also redirect the executable path, the adapter attempts the change on the first executable path query or DLL load, whichever occurs first.

For path canonicalization, a focused recipe can route `CreateFileW` and `GetFinalPathNameByHandleW` through the storage adapter. It records metadata-only opens and final-path query results as `asset storage=` records. Open records classify the input by scope and path form. Final-path records identify the requested form. After a denied normalized DOS query, the adapter tries to obtain the volume-relative name from `FileNameInfo` on the same handle. It accepts a path within the launcher's scoped roaming or verified install directory after checking for reparse points along the scoped path and verifying the volume and file ID against the original handle. The trace omits the paths.
### Measurement records

The loader trace also carries measurement records with their own exact formats. The inspector and observer keep them in trace order and summarize them under `diagnostics` (`inspect-loader`) or `loader_diagnostics` (`observe`). `recent` lists only the other records.

| Records | Summary |
| --- | --- |
| `audio meter=…` and `audio window=N peak-milli=P audible=A/S` | `audio`: meter state, window count and range, windows with at least half of the read samples audible, silent and unread windows, highest peak in thousandths of full scale, and the first and latest three windows. Each window covers 10 seconds of the default render endpoint's mix, sampled every 100 ms. The meter stops after 90 windows. It does not show which process produced the sound, so a person still needs to listen. |
| `pso timing result=…` and `pso kind=G\|C\|S utc=… us=… result=…[ cache=OUTCOME]` (with `XBOX_WIN32_PSO_TRACE=1` or a pipeline-cache marker) | `pipelines`: whether timing was installed, the number of pipeline creations that took at least 2 ms by kind, their total and longest durations, failures, and the five slowest records. Under a cache or replay mode, `outcomes` counts them by outcome (`hit`, `wait-hit`, `miss`, `early`, `root-mismatch`, …). The runtime stops after 2000 records. |
| `dxgi dred-…` | `device_removed`: incomplete command lists, whether a GPU page fault was reported, and up to 16 records |
| `asset steam-run-callbacks us=… utc=…` (with `steam-win32-steam-callback-trace-APPID.txt`) | `steam_callbacks`: the number of `SteamAPI_RunCallbacks` calls that took at least 2 ms, their total and longest durations, and the five slowest records. The runtime stops after 4096 records. [Steam title overrides](GAME-LAUNCH.md#override-steamworks-calls-for-one-title) |

The runtime deletes the staging folder, and its raw `loader-trace.txt`, when the game exits normally. The child exit report carries only the general load, export, fault, asset, DXGI and input records, so measurement records cannot be recovered from it afterwards. To keep them, run `observe` with `--output` while the game runs, or copy the raw trace with `get-file` before the game exits.

## Collect and measure

`--output` creates a new local directory. Available files depend on which diagnostics the signed package enables.

| File | Contents |
| --- | --- |
| `observation.json` | Filtered startup or launch checkpoints, run ID, outcome, and captured file sizes |
| `primary-lifecycle.txt` | Filtered primary host lifecycle checkpoints when the trace changes during the observation |
| `sdl-startup.txt` | Filtered SDL and Win32 startup checkpoints for the launch's trace ID |
| `loader-trace.txt` | Filtered loader records from the launch report, the child exit report, or a uniquely matched new staging folder |
| `loader-diagnostics.txt` | Every audio meter, pipeline timing and device-removed record copied from the live staging folder, in order |
| `fhd-present-times.bin` | Native frame timestamps whose header belongs to the observed game launch window |
| `bootstrap.bmp` | Optional startup image requested with `--bootstrap-frame` |

The native launch and exit reports retain up to 24 KiB of complete allowlisted records from the last 28 KiB of the loader trace. With `--output`, for a launch that reports `steam_shadow_stage=verified-local-copy`, the observer copies the live trace on every poll while the game runs, up to 1 MiB, so the copy survives the staging folder's removal at exit. A new observation selects the one staging folder that was not present at its baseline. With `--run-id`, it selects the one folder created within five minutes of that launch. `live_loader` in the result names the folder, the number of successful reads, and the time and size of the last read. Records written after the last read, normally less than a few seconds before exit, are not copied. `loader-trace.txt` keeps up to 128 recent distinct general records. `loader-diagnostics.txt` keeps up to 4096 measurement records without merging repeats. After a named child exit, the observer allows up to three seconds within its existing deadline for the terminal trace to appear.

For a 60 FPS comparison, use the same scene and settings on both builds. Record the UTC start time of a settled gameplay segment, continue playing through the sample, and feed the native trace to the analyzer. Supply the dimensions observed for the game renderer and the Xbox display output. Measure each independently for the same scene.

```sh
python3 -m tools.measure game-run/fhd-present-times.bin \
    --start-utc "$scene_start_utc" --duration 30 --require-gpu-60 \
    --internal-resolution "$internal_resolution" --output-resolution "$output_resolution"
```

Add `--list-gaps 100` to list every presenter gap longer than 100 ms in the window, with its UTC time and the change in GPU source sequence. Match the times against remote input steps, screenshots, process samples and `pso` records.

See the [native presentation measurement guide](GAME-LAUNCH.md#native-fullscreen-presentation) for the hardware and cadence checks.

## Sample processes, take screenshots and copy files

These commands read from the console without controlling it. Each selects the installed Steam Core package, or the version passed with `--version`, and works for any title.

```sh
python3 -m tools.xbox_deploy sample-processes --seconds 900 --image-name "$game_exe_basename" \
    --output game-run-processes.jsonl
python3 -m tools.xbox_deploy screenshots --delay 45 --interval 45 --count 10 --output game-run-shots
python3 -m tools.xbox_deploy list-files '\TempState' --recursive --since-utc "$launch_utc"
python3 -m tools.xbox_deploy get-file '\TempState\SteamWin32Trial (163)' loader-trace.txt --output loader-raw.txt
```

`sample-processes` writes one JSON line per sample with the UTC time and, for each process of the package, each `--image-name` and `WerFault.exe`, its CPU percentage, private working set and total commit in MiB. `package_commit_mib` sums the package's processes. A process that starts or exits between samples adds an `event` line, which shows crash reporting or a game exit without watching the console. A sample Device Portal cannot answer after retries is written as `unavailable` and sampling continues. Standard output gets a summary with each process's first and last sample and its peak commit, peak working set and highest CPU percentage.

`screenshots` saves PNG files on the schedule given and writes `screenshots.jsonl` with each capture's UTC start and end, duration, dimensions and SHA-256. HTTP errors are recorded and the schedule continues. An image in which every pixel has the same colour gets `uniform_color`, for example `#ffffffff`, and the summary counts these images under `uniform`. Device Portal can return a white image, not an error, while the Big Picture overlay is shown. Treat a uniform image as showing no game or Steam content. Each capture stalls presentation on the console for about half a second, so exclude capture times from frame-rate windows.

### Capture the Big Picture surface

The Big Picture WebView can save its own surface when it finds a request file in the package's `LocalState`. Upload an empty `big-picture-surface.request` with Device Portal's File explorer to request the whole gamepad surface. Upload `big-picture-capture-library.request` to request the Library only. The WebView checks for these files every four seconds while the Steam view is settled and visible. It deletes the request and writes `big-picture-surface.png` or `big-picture-library.png`. It records `surface_capture=` or `library_capture=` in `big-picture-webview-report.txt` as `saved width=… height=…`, `not-visible` or `error`. Copy the PNG with `get-file '\LocalState' big-picture-surface.png`.

A request can remain in `LocalState` as a 0-byte file. This happens when it was placed while a game owned the screen, while Big Picture was not running, or while an earlier capture had no result. A stale request is harmless. It takes one capture the next time the Steam view is settled and visible, and then the WebView deletes it. Nothing else reads it, and no other cleanup is needed. Delete it in File explorer to cancel that capture. The hosted game view treats `big-picture-capture-game.request` the same way. It consumes the request only while the game's 1920 × 1080 video is shown, and then writes `big-picture-game-video.png`.

`list-files` and `get-file` accept folders under `\LocalState`, `\TempState`, `\LocalCache`, `\RoamingState` and `\AC` of the package. `get-file` streams to a new local file, prints its size and SHA-256, and removes the local copy if the transfer fails.

## Restart Steam Core

```sh
python3 -m tools.xbox_deploy relaunch --version "$version"
```

`relaunch` stops the installed Steam Core package, including any game it started, waits up to `--stop-timeout` seconds (default 30) until Device Portal lists none of the package's processes, then activates the package again. It does not activate a package whose processes did not stop. It restarts Steam only. A configured `--play-installed-app-id` Play runs once per installed package version. The package records the version it played in its local settings, which Device Portal cannot clear, so a relaunch does not request that Play again. To launch the game again, select Play on the console, or install a new package version and activate it. This command controls the console: coordinate with anyone else using it, and start the observer first if you want the next launch recorded.
