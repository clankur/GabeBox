# Xbox runtime diagnostics

Use the same Device Portal configuration and verified certificate pin as the deployment CLI. The observer reads an already-installed Steam package and writes one JSON result to standard output. Status checkpoints go to standard error. Open Steam or select Play on the console after the observer prints `waiting-for-startup` or `waiting-for-launch`. Those checkpoints mark the end of the baseline read. For a package update, complete installation separately before starting the observer.

Use the [probe catalog](../probe/README.md) for isolated checks of process creation, IPC, graphics helpers, and Steam DLL loading. A probe launches its selected test package and checks a fresh report. The observer below follows the installed Steam app during normal use.

## Observe Big Picture startup

```sh
python3 -m tools.xbox_deploy observe --version "$version" --startup --output startup-run
```

The observer waits for both the native sign-in run and the WebView run to change. The result includes the UI checkpoint, game count and startup timings when the package reports them. `setup-ready` records the setup surface. `gamepad-ready` records native gamepad readiness.

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

The deadline covers observation of the selected run. The JSON `outcome` distinguishes a first hosted frame, a process exit, a launch failure, or a timeout. The CLI exits with a nonzero status for an observation timeout or an argument, connection, or portal error. Terminal game outcomes are carried in JSON. `frame-observed` records a presentation milestone, which can include a black startup frame. Use the game on the console to verify input, audio, save behavior and return to the Library.

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

Add `--rumble` to read the selected launch's optional vibration mailbox after a controller effect. The connection state can be `armed`, `disarmed`, or `updating`. On an instrumented build, `bridge_attached` and `presenter_attached` identify the endpoints. The flags show whether a connected XInput slot requested a nonzero effect, the bridge accepted it, and the presenter received a successful WinRT result. `stop_applied` records a successful zero command after a nonzero attempt. `device_error` records any failed WinRT vibration call. These flags accumulate for the launch, including while the controller is disconnected or the presenter is covered. They can capture an effect that starts and stops between two diagnostic reads. Older sidecars leave the flags clear. Confirm physical vibration and cancellation on the controller.

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

A focused Win32 profile can redirect `ExitProcess` and `TerminateProcess` to `XboxWin32LoaderTrace.dll` to record the exit API, code, and caller. For a self-exit with `0xFFFFFFFF`, the adapter also attempts to record up to eight module-relative frames from the terminating thread. A trace can show a direct exit from an engine worker. When the engine returns through `main` first, the stack only shows the remaining runtime frames. The trace reports when the stack capture API is unavailable.

## Collect and measure

`--output` creates a new local directory. Available files depend on which diagnostics the signed package enables.

| File | Contents |
| --- | --- |
| `observation.json` | Filtered startup or launch checkpoints, run ID, outcome, and captured file sizes |
| `primary-lifecycle.txt` | Filtered primary host lifecycle checkpoints when the trace changes during the observation |
| `sdl-startup.txt` | Filtered SDL and Win32 startup checkpoints for the launch's trace ID |
| `loader-trace.txt` | Filtered loader records from the launch report, the child exit report, or a uniquely matched new staging folder |
| `fhd-present-times.bin` | Native frame timestamps whose header belongs to the observed game launch window |
| `bootstrap.bmp` | Optional startup image requested with `--bootstrap-frame` |

The native launch and exit reports retain up to 24 KiB of complete allowlisted records from the last 28 KiB of the loader trace. The observer can also read up to 1 MiB from the matching live staging directory and retains up to 128 recent distinct records. After a named child exit, the observer allows up to three seconds within its existing deadline for the terminal trace to appear.

For a 60 FPS comparison, use the same scene and settings on both builds. Record the UTC start time of a settled gameplay segment, continue playing through the sample, and feed the native trace to the analyzer. Supply the dimensions observed for the game renderer and the Xbox display output. Measure each independently for the same scene.

```sh
python3 -m tools.measure game-run/fhd-present-times.bin \
    --start-utc "$scene_start_utc" --duration 30 --require-gpu-60 \
    --internal-resolution "$internal_resolution" --output-resolution "$output_resolution"
```

See the [native presentation measurement guide](GAME-LAUNCH.md#native-fullscreen-presentation) for the hardware and cadence checks.
