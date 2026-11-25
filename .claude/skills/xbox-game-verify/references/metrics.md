# What each check measures, and what it does not

Run these from the repository root. `--env-file` is a **top-level** flag on
`tools.xbox_deploy`, so it goes before the subcommand:
`python3 -m tools.xbox_deploy --env-file "$xbox_env" observe …`.

Everything here except `tools.xbox_remote_input` is read-only; the remote input tool is
the one that drives the console, so coordinate before you use it.

All of them need the current launch's identity. Get it once and reuse it:

```sh
python3 -m tools.xbox_deploy report --version "$version"     # steam_launch_run_id
run_id=...
```

Contents: frame rate · visible gameplay · driving input · controller · keyboard ·
audio · save and reload · return to library · startup gating · feeding coverage ·
what the package has to enable.

## Frame rate and pacing

```sh
python3 -m tools.xbox_deploy observe --version "$version" --app-id "$app_id" \
    --run-id "$run_id" --output phase-run
python3 -m tools.measure phase-run/fhd-present-times.bin \
    --start-utc "$settled_scene_start_utc" --duration 30 --diagnose-stalls \
    --internal-resolution "$internal" --output-resolution "$output"
```

- Sample **after the scene settles** — eight seconds past the point the scene became
  playable is the convention — and keep the game being played through the whole window.
  A 30 s window inside a 40–45 s block of play is the shape that works.
- **The verdict is the average: a settled gameplay window should run at about 60 FPS**
  (`fps` at or above 59.5). Every measured gameplay window has to hold it, so the
  weakest window decides, not the best.
- **Hitches are issues, reported separately, and never silently flip that verdict.**
  Record `worst_ms`, `intervals_over_33_33ms` and `min_rolling_1s_fps` per window: a
  window can average 59.97 and still contain a 450 ms stall, and the player feels the
  stall. Name the phase each one happened in.
- `gpu_60fps` in the same result is the project's strict gate — average at or above
  59.5 **and** lowest rolling second at least 57, every frame on a shared GPU texture, a
  verified hardware adapter, and at least 30 s. Report it alongside the average. When
  the average passes and `gpu_60fps` does not, that difference *is* the hitch finding —
  and it matters, because counting the title needs the strict gate (see *Feeding
  coverage*).
- Adding `--require-gpu-60` makes the command exit nonzero when that gate fails. It
  still prints the JSON; read the flag from the JSON if you would rather not treat a
  measured shortfall as a command failure.
- Any `cpu_presented` frames are an issue in their own right: the transport fell off the
  hardware path.
- A Device Portal screenshot stalls the game for about half a second. Take captures
  outside the measurement window.

## Visible gameplay

Three ways to see what the game is drawing, in descending order of usefulness:

1. **A Device Portal screenshot during play** — `shot:FILE.png` as a step in
   `tools.xbox_remote_input`, or the endpoint directly. It returns real 1920 × 1080 TV
   content for this app, which is how the recorded library and loading-screen captures
   in `analysis/` were taken. Two known failure modes: the Big Picture **overlay**
   returns HTTP 500, and the capture costs the game roughly half a second. A black frame
   is a real result, not an endpoint artifact — the Spider-Man work confirmed black
   output by reading the game's own backbuffer in-process.
2. **The package's game capture** — drop an empty `big-picture-capture-game.request`
   into the package's `LocalState` while the game is in front and the native view writes
   `big-picture-game-video.png`.
3. **The startup BMP** — `inspect-render --output` copies whatever the package wrote.
   On `main` the only writer is the SDL path's one-shot startup capture, so this is a
   **startup** frame for SDL-hosted titles and nothing at all for a Win32/DXGI title.
   It persists for the whole launch, so it will show up in every phase you sample: it is
   not evidence of the live scene. (`inspect-render` also looks for `-late` and
   `-requested` variants, which nothing on `main` writes.)

```sh
python3 -m tools.xbox_deploy inspect-render --version "$version" --app-id "$app_id" \
    --run-id "$run_id" --output render-check
```

The mailbox side reports source texture dimensions, DXGI format, producer sequence, and
the generations published and accepted at each end. `updating` means a generation
changed mid-read — sample again. A sequence that advances proves frames are being
produced; only an image shows what is in them.

## Driving input

`tools.xbox_remote_input` sends controller, keyboard and mouse input through Device
Portal's `/ext/remoteinput` WebSocket. The presenter merges the gamepad codes into the
hosted pad, so a game reading hosted XInput sees them as controller input.

```sh
python3 -m tools.xbox_remote_input --check          # open and close, send nothing
python3 -m tools.xbox_remote_input --gap 1.0 A wait:3 shot:after-a.png DOWN hold:LS_UP:2
python3 -m tools.xbox_remote_input --script scripts/hades-wander.txt
python3 -m tools.xbox_remote_input --dry-run hold:LS_UP:2 key:ENTER   # validate a script offline
```

Steps: the face and shoulder buttons, D-pad, Menu, View, stick clicks and stick
directions; `hold:NAME:SECONDS`; `key:NAME`, `scan:HEX`, `vk:HEX`; `move:X,Y` and
`click:X,Y`; `wait:SECONDS`; `shot:FILE.png`; and `*N` to repeat a step. The client
sends a clear-all on close, so a cancelled script cannot leave a stick held.

Two limits worth knowing before planning a session: **mouse messages do not raise host
pointer events**, so a pointer-driven title cannot be driven this way, and overlay
screenshots return HTTP 500.

Drive and sample at the same time — start the input, then sample the mailbox while it is
in flight — or the sample will miss the presses entirely.

## Controller delivery

```sh
python3 -m tools.xbox_deploy inspect-input --version "$version" --app-id "$app_id" \
    --run-id "$run_id" --seconds 10 --output input-check
```

`--seconds` is capped at 30, and defaults to 0, which takes a single snapshot — a
snapshot can never show a change, so pass a duration whenever you are testing delivery.
`sampling-host-input` on stderr marks the start of the window.

Two different fields answer two different questions:

| `activity.gamepad` (whole window) | Meaning |
| --- | --- |
| `host-activity` | the host published a change during the window |
| `host-idle` | the sampled serial never changed |
| `insufficient-samples` | fewer than two stable samples; the window was too short |

| sample `gamepad.state` | Meaning |
| --- | --- |
| `acknowledged` | the game-side adapter read that serial |
| `unacknowledged` | the game has not read it yet |
| `host-idle` | nothing published and nothing acknowledged |
| `updating` | the serial changed during the read; sample again |

Together they separate the three failures that look identical on a TV: the host never
saw the pad, the host published and the game ignored it, or the game read it and did
nothing. Only the third is a game-side defect. A held button also shows
`connected: true` with its bit set in the button mask; the six axes are SDL ordering and
the triggers rest at -32768.

`--panel-menu` adds the package's presenter milestones — readiness, held chords, menu
requests — which checks the player menu independently of the game. Those counters are
package-scoped and cumulative: read the delta across your window.

**Pair every input sample with what the screen did.** Delivery is not response.

## Keyboard delivery

Same command: `keyboard_ready` records whether the game enabled the key queue. A title
that never enables it cannot receive text — a finding about the title, not a test
failure. Drive text with `key:` steps (or `scan:`/`vk:` for keys the message cannot
carry), and confirm the characters appear in the field. Where the title is reached
through the player's own on-screen keyboard, test that path too: hold Menu and View,
choose **Keyboard**, enter text, **Send text**.

## Audio

This project measures **no audio signal**. What exists:

- SDL-hosted titles: `report --version "$version" --sdl-startup` carries
  `sdl_audio_init` and `sdl_audio_device` with `begin`, `ready` or `error`. A device
  opened; nothing about sound leaving the console.
- Win32 titles: the loader trace records `asset xaudio27-activation result=0x…`, and the
  Spider-Man work used it to establish XAudio2 object and mastering-voice
  initialization. Again: initialization, not output.
- Output itself: a person confirming a specific expected sound at a named moment. Record
  what the sound was and when, not "audio works".

On an unattended run, audio output is `unobserved`. Do not infer it from the game
reaching a scene that should have music.

## The Steam menu over a running game

While a game is running, holding **View and Menu together** — or pressing **F10** — asks
the presenter to bring the library forward over it. That is the player's only way back to
Steam without quitting, and it has broken before: on the Hades trial that launched the
game before Big Picture registered a running game, the pad worked and the Steam menu did
not open at all.

Drive it with `key:F10`, which reaches the same `requestOverlay()` path as the chord. The
chord itself cannot be injected — remote input sends one button down and up before the
next step, so View and Menu never overlap — so a physical chord test still needs a person.

```sh
python3 -m tools.xbox_remote_input key:F10 wait:4 shot:overlay-open.png A wait:3
python3 -m tools.xbox_deploy inspect-input --version "$version" --app-id "$app_id" \
    --run-id "$run_id" --seconds 10 --panel-menu --output overlay-check
```

The presenter writes `menu_chord=held` and `menu_request=ready` as it handles the request,
and `--panel-menu` returns them as the counts `chord_held` and `menu_request`. Those
counts are package-scoped and cumulative, so read the `added` delta for your window: a
request recorded during the window is the machine evidence that the menu opened. Pair it
with the captures either side of the request — one with the menu over the game, one after
resuming — because the count proves the request reached the host, not that the menu drew
or that the game came back.

Closing it is the player menu's own business: D-pad and **A** choose **Resume** or **Quit
to library**, so a driven run presses A expecting Resume. If a title's menu focuses
something else, drive the phase from your own script instead.

## Save and reload

Create a save the game itself recognizes — its own save point, slot or autosave — and
name the state concretely (slot, location, resources, progress marker). Leave, reload,
and confirm the same state. A file on disk is not a save; a loaded state that matches
is. Note whether Steam Cloud restore was involved, since that is its own check.

## Return to the library

Exit as a player would. Confirm the game process ends, the app stays open, Big Picture
responds to the pad, and a second launch still works.

```sh
python3 -m tools.xbox_deploy inspect-loader --version "$version" --app-id "$app_id" \
    --run-id "$run_id" --output loader-check
```

This reads the live staging trace, or after cleanup the terminal report with the child
exit code, and counts graphics pipeline compilations, boot waits and shell requests —
useful for explaining a stutter in the first minutes of play. It only works for a
**staged Win32 trial**: a launch that does not report `steam_shadow_stage=verified-local-copy`
is refused, and the exit code simply stays unobserved for other titles.

## Gating a startup instead of watching it

```sh
python3 -m tools.xbox_deploy observe --version "$version" --app-id "$app_id" \
    --image-name "$exe" --timeout 180 --checkpoint-timeout 20 \
    --require-checkpoint 'dxgi gpu-frame-submitted' \
    --require-checkpoint "8:$game_ready_record" \
    --watch-checkpoint "$progress_record" \
    --fail-on-checkpoint 'dxgi gpu-frame-error result=0x887A0005' \
    --output startup-checkpoints
```

Required records must appear in order, each with its own budget (a `SECONDS:` prefix
overrides the default). `checkpoint-stalled` names the record that did not arrive;
`checkpoint-failed` names a selected failure record. `--fail-on-checkpoint` needs at
least one `--require-checkpoint`.

## Feeding coverage

`tools.steam_compatibility coverage` is what actually counts a title, and it reads
trials with five specific check names:

```json
[{"id": 1145360, "build_id": "10929685", "target": "series-x-26100.9673 / 1.0.232.60",
  "evidence": "https://github.com/clankur/GabeBox/issues/33",
  "checks": {"gameplay": true, "controller": true, "audio": true,
             "save-reload": true, "return-to-library": true},
  "performance_scene": "gameplay",
  "performance": {"fps": 59.97, "min_rolling_1s_fps": 58, "gpu_60fps": true,
                  "internal_resolution": "1920x1080", "output_resolution": "1920x1080"}}]
```

```sh
python3 -m tools.steam_compatibility coverage steam-library.json --target "$target" \
    --library-audit library-audit.json --evidence trial.json --require-gpu-60
```

A trial counts only when all five checks are `true`, its `build_id` matches the audited
build, and — with `--require-gpu-60` — `performance_scene` is `gameplay` and the sample
reaches `gpu_60fps` at internal and output resolution exactly `1920x1080`. So a session
can pass the average-FPS verdict and still not be countable; say which of the two you
have.

Two recorded samples to calibrate against: a failing one in
`analysis/hades-compatibility.md` (1,681 frames, 56.03 FPS, 25 in the slowest rolling
second, a 450.37 ms hitch) and a passing one in
`analysis/steam-ui-cloud-performance.md` (59.900 FPS, 58 in the lowest rolling second,
zero CPU uploads).

## What the package has to enable

| Check | Depends on |
| --- | --- |
| Frame rate | the native frame trace enabled in the signed build |
| Startup capture | an SDL-hosted title writing its one-shot startup BMP |
| Live capture | Device Portal screenshots, or the package's game-capture request |
| Input sampling | the title being on the hosted input mailbox |
| SDL audio checkpoints | an SDL-hosted title with the startup trace |
| Loader trace and exit code | a staged Win32 trial reporting `verified-local-copy` |

Ask which of these the build under test enables **before** the session. A check whose
evidence the build cannot produce is `unobserved`, and the follow-up is a build that
enables it — say so rather than running the session twice.
