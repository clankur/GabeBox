# Xbox runtime diagnostics

Use the same Device Portal configuration and verified certificate pin as the deployment CLI. The observer reads an already-installed Steam package and writes one JSON result to standard output. Status checkpoints go to standard error. Open Steam or select Play on the console after the observer prints `waiting-for-startup` or `waiting-for-launch`. Those checkpoints mark the end of the baseline read. For a package update, complete installation separately before starting the observer.

## Observe Big Picture startup

```sh
python3 xbox-deploy.py observe --version "$version" --startup --output startup-run
```

The observer waits for both the native sign-in run and the WebView run to change. The result includes the UI checkpoint, game count and startup timings when the package reports them. `setup-ready` records the setup surface. `gamepad-ready` records native gamepad readiness.

## Inspect an active Steam install

```sh
python3 xbox-deploy.py report --version "$version" --download
python3 xbox-deploy.py inspect-game --version "$version" --app-id "$steam_app_id"
```

The download report identifies its counter source in `steam_download_measurement`. For `steam`, the bundled client supplies live download and staging counters for the active AppID. A rate needs consecutive live samples with comparable totals. The report keeps the on-disk manifest values under `steam_download_manifest_bytes_*` when both sources are available.

Steam can update title-specific depot records while its on-disk manifest still shows zero bytes. When live counters and meaningful manifest counters are unavailable, `steam_download_depot_activity=observed` records a recent change in those depot records. Use it as an activity signal. For a transfer rate or installed byte count, use the corresponding byte counters.

## Observe a game launch

```sh
python3 xbox-deploy.py observe --version "$version" --app-id "$steam_app_id" \
    --frame-seconds 40 --timeout 180 --output game-run
```

Use `--image-name "$game_exe_basename"` to also follow the game's Device Portal process listing. For an already-started session, pass the exact `--run-id` shown as `steam_launch_run_id` by `xbox-deploy.py report --version "$version"`. For a fresh observation with no baseline run ID, the observer uses the local clock to screen out stale reports. Keep the computer and console clocks in sync for that case.

The deadline covers observation of the selected run. The JSON `outcome` distinguishes a first hosted frame, a process exit, a launch failure, or a timeout. The CLI exits with a nonzero status for an observation timeout or an argument, connection, or portal error. Terminal game outcomes are carried in JSON. `frame-observed` records a presentation milestone, which can include a black startup frame. Use the game on the console to verify input, audio, save behavior and return to the Library.

### Require game startup checkpoints

Use a short sequence of loader records from the diagnostic build under test. Keep the initial GPU submission as one checkpoint and use a later engine event for the transition you want to verify.

```sh
python3 xbox-deploy.py observe --version "$version" --app-id "$steam_app_id" \
    --image-name "$game_exe_basename" --timeout 180 --checkpoint-timeout 20 \
    --require-checkpoint 'dxgi gpu-frame-submitted' \
    --require-checkpoint "$game_ready_checkpoint" \
    --fail-on-checkpoint 'dxgi gpu-frame-error result=0x887A0005' \
    --output startup-checkpoints
```

Each required checkpoint matches a complete filtered loader record or the first tokens of one. The checkpoints must appear in order. The per-checkpoint clock starts when the child, its window, or its first frame is observed. A new required checkpoint restarts that clock. Repeated markers and frame presents leave its deadline in effect. The overall `--timeout` still covers staging and the full observation.

The result reports the checkpoints reached and the next record it was waiting for. `checkpoint-stalled` means that record failed to appear in time. The trace source is included so a missing diagnostic can be distinguished from a trace that recorded the preceding steps. `checkpoint-failed` identifies an explicitly selected failure record. The CLI returns a nonzero status if the required sequence is incomplete and saves the available artifacts. An attached `--run-id` uses the same checks and gives the current game a fresh observation budget. The observer leaves the Xbox session running.

## Inspect the current game's render state

Use the AppID and `run_id` from the game observation while that launch is current.

```sh
python3 xbox-deploy.py inspect-render --version "$version" --app-id "$steam_app_id" \
    --run-id "$launch_run_id" --output render-check
```

The command reads the selected package's GPU mailbox and reports the source texture dimensions, DXGI format, producer sequence, and the generations published and accepted by the two ends of the exchange. A publisher changing its texture generation produces an `updating` state. Take another sample to see the stable configuration.

With `--output`, the command saves filtered metadata to `observation.json` and copies available run-scoped captures to `bootstrap.bmp`, `bootstrap-late.bmp`, and `requested.bmp`. The package must have enabled and written the captures. Missing or incomplete files have a state in the JSON result. Available window and SDL checkpoints are saved to `sdl-startup.txt`. The command checks launch identity again before saving. Compare screenshots to inspect visible content, and use the frame trace to measure presentation cadence.

## Inspect the current game's loader trace

For a running game that reports `steam_shadow_stage=verified-local-copy`, use the AppID and exact `steam_launch_run_id` from the same package version.

```sh
python3 xbox-deploy.py inspect-loader --version "$version" --app-id "$steam_app_id" \
    --run-id "$launch_run_id" --output loader-check
```

The command finds a single staging directory created between the launch timestamp and five minutes after it. It stops if that window contains zero or multiple candidates, or if the launch or selected directory changes during collection. The trace remains available while the staged copy exists.

The JSON summary gives the latest filtered records and counts for graphics pipeline compilation, boot waits, and shell requests. Counts cover recorded events, including repeats. If the runtime records graphics or compute pipeline request ordinals, `pipelines` reports the highest recorded request for each kind, the most recent sampled commit headroom, and the last observed failure HRESULT when present. The request ordinal is a lower bound and can include an in-flight request. If the runtime also samples its cumulative counters, `latest_counter_sample` gives the counter values and whether a pipeline call was active at that instant.

The inspector reads up to 1 MiB of the trace and marks `truncated` when more bytes are available. A record still being written is skipped. Pass `--output` to save the summary and the complete allowlisted records within the read limit. The inspection only reads the selected package's diagnostics through Device Portal.

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

For a 60 FPS comparison, use the same scene and settings on both builds. Record the UTC start time of a settled gameplay segment, continue playing through the sample, and feed the collected native trace to the existing analyzer.

```sh
python3 probe/fhd/measure.py game-run/fhd-present-times.bin \
    --start-utc "$scene_start_utc" --duration 30 --require-gpu-60
```

See the [native presentation measurement guide](GAME-LAUNCH.md#native-fullscreen-presentation) for the hardware and cadence checks.
