# Xbox runtime diagnostics

Use the same Device Portal configuration and verified certificate pin as the deployment CLI. The observer reads an already-installed Steam package and writes one JSON result to standard output. Status checkpoints go to standard error. Open Steam or select Play on the console after the observer prints `waiting-for-startup` or `waiting-for-launch`. Those checkpoints mark the end of the baseline read. For a package update, complete installation separately before starting the observer.

## Observe Big Picture startup

```sh
python3 xbox-deploy.py observe --version "$version" --startup --output startup-run
```

The observer waits for both the native sign-in run and the WebView run to change. The result includes the UI checkpoint, game count and startup timings when the package reports them. `setup-ready` records the setup surface. `gamepad-ready` records native gamepad readiness.

## Observe a game launch

```sh
python3 xbox-deploy.py observe --version "$version" --app-id "$steam_app_id" \
    --frame-seconds 40 --timeout 180 --output game-run
```

Use `--image-name "$game_exe_basename"` to also follow the game's Device Portal process listing. For an already-started session, pass the exact `--run-id` shown as `steam_launch_run_id` by `xbox-deploy.py report --version "$version"`. For a fresh observation with no baseline run ID, the observer uses the local clock to screen out stale reports. Keep the computer and console clocks in sync for that case.

For an unattended trial, prepare the Big Picture layout with `--play-installed-app-id APPID` and sign it. This requests one Play through Big Picture per installed package version after sign-in, subject to Steam's ownership and completed-install checks. Install the package without launching it, start the observer, and activate the app after `waiting-for-launch`. The observer saves filtered `unity-startup.txt` checkpoints when the matching launch reports a Unity player log. Those checkpoints include graphics selection, managed assembly reload, and large allocation errors.

The deadline covers observation of the selected run. The JSON `outcome` distinguishes a first hosted frame, a process exit, a launch failure, or a timeout. The CLI exits with a nonzero status for an observation timeout or an argument, connection, or portal error. Terminal game outcomes are carried in JSON. `frame-observed` records a presentation milestone, which can include a black startup frame. Use the game on the console to verify input, audio, save behavior and return to the Library.

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

For a 60 FPS comparison, use the same scene and settings on both builds. Record the UTC start time of a settled gameplay segment, continue playing through the sample, and feed the collected native trace to the existing analyzer.

```sh
python3 probe/fhd/measure.py game-run/fhd-present-times.bin \
    --start-utc "$scene_start_utc" --duration 30 --require-gpu-60
```

See the [native presentation measurement guide](GAME-LAUNCH.md#native-fullscreen-presentation) for the hardware and cadence checks.
