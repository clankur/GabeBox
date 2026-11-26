# Slay the Spire 2 on Xbox Series X

Slay the Spire 2 is Steam AppID `2868840`. The audited Windows build is Steam build `26655341`, publisher release `v0.111.1` (commit `cc1f9462`, dated 2026-11-13). The game runs on MegaDot v4.5.1.m.14, a fork of Godot 4.5, with a bundled .NET 9 runtime in `data_sts2_windows_x86_64`. The executable is `SlayTheSpire2.exe` (894,748,672 bytes, SHA-256 `8602c26bffd2937e3841835fd8360ef8e974624a543e05977229fd3d062be231`). It carries full DWARF debug info, so `x86_64-w64-mingw32-addr2line -f -C -e SlayTheSpire2.exe 0x140000000+RVA` symbolizes fault addresses offline. The game data is `SlayTheSpire2.pck`.

## Current result

Tested on Xbox Series X, OS `26100.9673`, with SteamCore `1.0.234.10` and then `1.0.234.11`. Both were built from commit `7094809` with the Steam Core template from [CI run 45585728786](https://github.com/clankur/steam-xbox-devmode/actions/runs/45585728786) and the build-pinned OpenGL profile. `.11` is the same build with a new version number, so that Big Picture's one-time configured **Play** runs again. All input came from Device Portal remote input through [`tools/xbox_remote_input.py`](../tools/xbox_remote_input.py).

| Check | Result on `1.0.234.10` / `.11` | Evidence |
| --- | --- | --- |
| Rendering | Main menu, character portrait and card art render. No `shader_gles3.cpp:800` errors (13 before). | Screenshots `45828fb1…` (menu), `e67a375a…` (Ironclad), `c9830e19…` (combat with Strike, Defend and Bash art) |
| Controller input | Remote pad drove menus, character select, map, combat, pause menu and quit. | Every step's screenshot |
| Steam Cloud writes | Every write logs `Wrote … in steam remote store` (prefs, progress, the 41,362-byte `current_run.save`, `profile.save`). No `Cloud write failed`. | `LocalState\SteamUserData\118829389\2868840\remote\profile1\saves`, `remotecache.vdf` |
| Save and reload | **Save and Quit** showed **Continue** (Overgrowth floor 1, 80/80, 99 gold). After quitting, relaunching and upgrading to `.11`, the game synced without deleting anything and **Continue** resumed the same combat, seed `8F0N0CD7JU9B`. Preferences persisted too: the Early Access screen did not return. | `0967238d…` (menu after save), `c81d2cc1…` (menu after relaunch), `e5cde100…` (resumed combat) |
| Return to library | Main menu **Quit** → **Yes** logged `NGame.Quit called` and `Steamworks shutdown succeeded`. Device Portal process listings after each quit showed no `SlayTheSpire2.exe` and no `WerFault.exe`. The Big Picture library screenshot was taken after the first quit; screenshots after the second returned HTTP 500. | `0101f360…`, process listings |
| 60 FPS | Active combat from `2026-11-25T09:36:55Z`: 1,799 frames, **59.967 FPS**, lowest rolling second **59**, worst 17.11 ms, all shared GPU textures, `gpu_60fps: true`. | trace `c8afeab2…` |
| Audio | On `1.0.236.1` the loader trace sampled the default render endpoint's peak meter every 100 ms. In every 10 s window from the menu onward, 100 of 100 samples were above silence. Peaks were 0.026–0.036 of full scale in the menu and 0.043–0.059 once combat started. FMOD logged `Successfully initialized` and `System released`. On 2026-11-25 a person at the console heard the menu music and card hits on SteamCore `1.0.244.2`. | loader trace `47fab8d2…`, `audio window=` lines |

The run's opening window (from `09:36:19Z`) averaged 59.47 FPS with a lowest second of 46. It contained two game-side stalls (167 ms and 83 ms) at the first card plays of the run, each advancing the producer by one frame. This matches the first-use loading recorded on `1.0.232.680` below. Windows from `09:36:40Z` and `09:37:00Z` also passed.

### Steam Cloud saves

`steamclient64.dll` sets its install root in a static initializer: it calls `GetModuleHandleA("steamclient64.dll")` and then tier0's `GetModuleFileNameUTF8`, and strips the file name. That code is at `steamclient64.dll` RVA `0x9AD325`–`0x9AD35A`, SHA-256 `5237b350…`. Steam keeps each account's state below `<root>\userdata`, including `<account>\<app>\remote` and `remotecache.vdf`. A packaged client's root is the read-only package directory, so every `ISteamRemoteStorage` write ended in `k_EResultIOFailure`. STS2 treats Steam Cloud as authoritative, so it deleted its local saves at the next launch. Only `AC\Steam\local.vdf` existed among the app's writable folders.

After the sign-in client loads, the host now rebinds the path-taking kernel32 imports of `steamclient64`, `tier0_s64` and `vstdlib_s64`. Only `<root>\userdata` resolves to `LocalState\SteamUserData`, and other paths pass through unchanged. The rebound imports are `CreateFileW`, directory create and remove, delete, attributes, find, move, copy, replace and temp-name. The host's sign-in report records `steam_userdata=redirected` and the number of rebound imports. A failed redirected write is recorded once in `LocalState\steam-userdata-report.txt`. "Not found" probes are not recorded. [Path mapping](../runtime/steam/steam-userdata-path.h) · [Import rebinding](../runtime/steam/SteamUserDataRedirect.h)

This makes Steam's local remote cache persistent, which is what save and reload needs. `remotecache.vdf` still shows `remotetime 0`, so this run did not confirm that the files upload to Valve's servers.

### Shader cache and the current-directory limit

Godot's `DirAccessWindows::change_dir` sets the process current directory, reads it back and restores it. On Xbox, `SetCurrentDirectoryW` fails with error 206 once the path passes `MAX_PATH`. That happens at `…\SlayTheSpire2\shader_cache\<Shader>\<sha256>`, which `1.0.230.82` recorded first. On every launch after the first, `make_dir` then returned `ERR_ALREADY_EXISTS`. `ShaderGLES3::initialize` returned at [`shader_gles3.cpp:800`](https://github.com/godotengine/godot/blob/4.5.1-stable/drivers/gles3/shader_gles3.cpp#L795-L809) before it recorded `max_image_units`. Every sampler that Godot assigns a unit counted back from that limit then got an invalid unit. The result was white card art, portraits and menu art.

For error 206 only, the storage adapter now accepts an existing directory as a virtual current directory. `GetCurrentDirectoryW` reports it until the next successful change. The process directory itself does not move. Paths with `.` or `..` components keep the original failure. The profile binds `GetCurrentDirectoryW` beside `SetCurrentDirectoryW`. [Storage adapter](../runtime/win32/storage-trace.c)

### Remaining open items

- **Physical controller and audio (verified by hand).** On 2026-11-25, with SteamCore `1.0.244.2`, a person at the console heard the game's audio and played with a physical controller, including navigating the Big Picture library after **Quit**.
- **Steam Cloud upload.** Saves persist in Steam's local remote cache. Upload to Valve's servers has not been checked.
- **First-use stalls.** They recur on every launch, not only the first. On `1.0.236.1` (third launch of the same save), the first card played at `10:01:56.47Z` produced four long intervals, the worst 83 ms. The first enemy turn then dropped single frames (33.4 ms) at `10:02:05`–`:09`. The opening 30 s averaged 59.33 FPS with a lowest second of 48. The pinned Mesa build ([mesa-uwp `34732bb`](https://github.com/XboxEmulationHub/mesa-uwp/tree/34732bbc207daea10bdf2fb969a4a0f7769ea1de)) has no shader disk cache. So every Godot shader variant is compiled to DXIL at first use on each launch, and Godot's program-binary cache gets nothing to store. On that launch the game logged no `Asset not cached` load at the first Strike or the first enemy move, so these stalls are not the game's on-demand asset loads. They match first-use shader work: Godot's GLES3 specialization, then Mesa's NIR → DXIL variant compile at the first draw. That cost comes from the compatibility layer. A Mesa program-binary cache alone would store NIR, not DXIL, so the variant compile would remain. The fix is either a persistent DXIL variant cache in the Mesa UWP build or the Direct3D 12 route, whose pipeline cache persists. The D3D12 route still loses the device after its first frames. A second source is the game's own: `sts2.dll` (9,757,184 bytes) is IL-only, with CLI flags `0x1` and no ReadyToRun header. CoreCLR therefore JIT-compiles every C# path the first time it runs, on every launch and on any platform, and the first card play goes through a large amount of new game code. These traces cannot separate JIT time from shader compilation. On `1.0.238.2` an empty `steam-win32-managed-jit-trace-2868840.txt` marker made the launcher enable CoreCLR EventPipe. It recorded the JIT keyword at verbose level to `LocalState\\managed-jit-2868840.nettrace` (SHA-256 `cd6a2519…`, frame trace `da8c8f4f…`, cut off at exit without its trailer). The session compiled 28,955 methods, totalling 7.66 s of JIT time. In the first turn, starting `10:20:33Z`, each frame gap over 25 ms was compared with the JIT time that overlapped it:

| Frame gap (UTC) | Length | Overlapping JIT |
| --- | --- | --- |
| `10:20:34.303` | 50 ms | 16 ms |
| `10:20:36.805` | 100 ms | 40 ms |
| `10:20:36.922` | 67 ms | 4 ms |
| `10:20:36.989` | 133 ms | 28 ms |
| `10:20:42.444` | 117 ms | none |
| `10:20:47.099` / `.149` | 50 + 50 ms | 15 + 12 ms |

Everything that overlapped came from one thread compiling the game's card-play path: `PlayCardAction`, `AttackCommand`, `DamageResult`, `NCardPlayQueue` and async state machines. JIT therefore accounts for roughly a quarter of the first-card stall time. The thread running `NCard` Godot property accessors compiled for 33.7 ms at `10:20:45.010` without any frame gap. The remaining stall time, including the 117 ms gap with no JIT, is other first-use work, most likely Godot and Mesa shader compilation for the first attack's VFX and damage text. A crossgen2 ReadyToRun build of `sts2.dll` in the staged copy would remove only the JIT share. It would also mean shipping a rebuilt publisher assembly, which this repository has avoided so far. Parse the trace with Microsoft.Diagnostics.Tracing.TraceEvent: match `MethodJittingStarted` to `MethodLoadVerbose` on the same thread.

**Why a shader-cache Mesa build would not help.** At the pinned revision, Mesa's d3d12 driver (`src/gallium/drivers/d3d12/d3d12_screen.cpp`, `d3d12_compiler.cpp` at mesa-uwp `34732bb`) never implements `pipe_screen::get_disk_shader_cache` and has no disk-cache code. Mesa's GL front end caches compiled programs only through that hook, and program-binary formats depend on it too. Rebuilding with `-Dshader-cache=enabled` would therefore change nothing. mesa-uwp also has no Windows CI: it builds with a UWP-patched Meson and the Visual Studio backend. Options checked:

| Option | Status |
| --- | --- |
| Rebuild pinned Mesa with `-Dshader-cache=enabled` | Would not help: the d3d12 driver has no cache hook |
| Implement a d3d12 disk cache (NIR plus variant key → DXIL) in a Mesa fork | Driver change in Mesa; not started |
| Direct3D 12 route (ubershaders) | `1.0.240.2` enabled DRED on Godot's `ID3D12DeviceFactory`. The device was removed 2.6 s in with `DXGI_ERROR_DRIVER_INTERNAL_ERROR` after one present, with no incomplete breadcrumbs and no page fault: a driver-side failure (`SraKmd_arden`) during startup pipeline creation, not a GPU hang. Godot 4.5's D3D12 driver also has no PSO cache (`pipeline_cache_create`: "PSO caching is not implemented yet"), so this route would compile pipelines on every launch too |
| ReadyToRun `sts2.dll` via crossgen2 | Removes only the JIT share, about a quarter; would ship a rebuilt publisher assembly |

On `1.0.242.1` the same marker also wrapped the shared `ID3D12Device` pipeline-creation methods, timing the device Mesa creates. During the first turn, starting `10:48:53Z`, frame gaps added 650 ms over a steady 60 FPS. Driver pipeline creation overlapped 82 ms of it: 34 ms and 44 ms at the first card play. The combat load itself created pipelines of 119–189 ms each. Taken with the JIT measurement, about 13% of the first-turn stall is the Xbox driver compiling pipelines and about a quarter is the game's JIT. The rest, including a 217 ms gap at `10:48:59.018` with neither, is Mesa's GLSL → NIR → DXIL translation or the game's own scene setup. An `ID3D12PipelineLibrary` cache in the layer would therefore remove only the driver share.

Until one of these lands, the first combat of each launch has stalls up to 217 ms on this route. Gameplay after first use holds 60 FPS.

**Mesa shader cache, first console runs.** Packages `1.0.270.4` and `1.0.270.5` ran the patched Mesa (`runtime/mesa`, CI artifact `3a4e806c…`) with the cache marker. The stats file reported `d3d12_shader_cache=enabled`. Both the cold and the warm run had 0 hits (77 and 75 misses), and `MesaShaderCache` stayed empty. The disk cache's directory setup had failed, which leaves the cache object in place but makes every lookup and store a no-op. The 77 lookups took 26 µs in total, so none reached the file system. Separately, a temporary entry path below this package's LocalState is about 263 characters, past `MAX_PATH`. The backend now uses extended-length wide-character Win32 paths. It also writes the cache state and the first failed file operation, with its path and Win32 error, into the stats file. That build has not run on the console yet. [Cause, fix and acceptance](../runtime/mesa/README.md#console-result-nothing-stored-on-packages-102704-and-102705)
- **White flicker and texture corruption ([#89](https://github.com/clankur/GabeBox/issues/89)).** One hypothesis was that the player could show a shared texture before Mesa finished rendering it. Code review on 2026-11-26 ruled this out for the exchange as written. The game's renderbuffer is never shared, and the producer copies it into one of two exchange textures on Mesa's own queue, after Mesa's `COMMON` transition and `glFlush`. A shared `ready` fence is signalled after the copy. The player draws a slot only once that fence has completed, and it also waits on the fence on the GPU. The producer reuses a slot only after both its `ready` and `done` fences have passed. A race in this exchange would show a neighbouring game frame, which is a tear. It could not produce white blocks, wrong textures or a frame nested inside itself. Those point inside the game's own rendering: Mesa's d3d12 residency (`Evict` and `EnqueueMakeResident` against the capped budget) or a replayed pipeline (`mode=replay`). The panel status now reports `gpu_wait_producer`, `gpu_held`, `gpu_released_unshown`, `gpu_drawn`, `gpu_ready_fence` and `gpu_done_fence` so a live run can confirm the gate ([exchange contract](../docs/GAME-LAUNCH.md#native-fullscreen-presentation)). Still to do on the console: read those counters during a flicker, and compare runs with the pipeline cache in passthrough and with residency calls traced. The loader trace now counts Mesa's `Evict`, `MakeResident` and `EnqueueMakeResident` calls each second. This profile already runs the pipeline hooks, so the counters are on by default. `xbox-d3d12-residency.txt` holding `pin` makes `Evict` a no-op ([residency switch](../docs/GAME-LAUNCH.md#count-and-pin-mesas-residency-changes)). Under the default 2304 MiB cap, Mesa's per-batch eviction should not run: it starts above 70% of the budget (1613 MiB), and the recorded peak was 727 MiB. If the native counters read `evict=0` and `restored=0` during a flicker on that budget, Mesa's eviction is ruled out. That reading has not been taken on the console. The A/B then compares `pin` with `native` on the same build under a 400 MiB cap, which makes Mesa evict. Expect the `pin` arm to run slower, because Mesa stalls when its budget binds and usage never falls.
- **Overlay exit.** Leaving through the Steam overlay chord is still open for every title; the game's own **Quit** works.
- **Virtual current directory trace.** The shader-cache path no longer fails, but the adapter's one-time `virtual-long` trace record has not been read from the console.
- **Other titles.** Recheck ULTRAKILL, Spider-Man and Hades after the shared input, storage and Steam-client changes.
- **Multiplayer.** Co-op is in scope and has not been tested on the console. It needs a second Steam account that owns the game on another PC or console. Check that a lobby can be hosted from the Xbox and joined from the other side, and the reverse. Play a combat turn together, with the relay status read from `GetRelayNetworkStatus`. A fix elsewhere must not disable or defer these paths.
- **T97, a one-time stall about 97 s after the first frame.** Opt-in switches for the three leading candidates are in place. None has run on the console. See [T97](#t97-one-time-stall-about-97-s-after-the-first-frame).

## T97: one-time stall about 97 s after the first frame

On packages `1.0.244`–`1.0.272` (build 26655341), every session has a disturbance window of about 3 s. It starts **97.6 ± 0.4 s after the game's first present** (SD over 8 of 9 runs) and contains several frame gaps, one of them 0.05–5.1 s long. It happens whatever the game is doing: on the idle menu, in menu navigation and during combat. It is locked to the game process start, not to the main menu: replay builds reach the menu about 6 s earlier and the onset does not move. CPU stays low in every process. Run-3's loader trace shows one native `glDraw*` blocked for 119.7 ms inside the window. No `sts2.dll` timer, delay, telemetry, save, preload or `GC.Collect` fires 60–120 s after start. The cause is not identified. This is offline analysis of the `console-246` to `console-262` artifacts, not a console result.

| Candidate | Evidence | Switch |
| --- | --- | --- |
| Steam Datagram Relay start-up on the game thread | `SteamInitializer.Initialize` calls `SteamNetworkingUtils.InitRelayNetworkAccess()` at boot, and `SteamAPI.RunCallbacks()` runs on the main thread every frame. A steamclient lock or IPC wait there would be a low-CPU stall that does not depend on game state. | `steam-win32-no-steam-relay-2868840.txt` makes the flat export a no-op. `steam-win32-steam-callback-trace-2868840.txt` times `SteamAPI_RunCallbacks` and records calls of 2 ms or more. [Steam title overrides](../docs/GAME-LAUNCH.md#override-steamworks-calls-for-one-title) |
| First background gen2 GC (CoreCLR 9.0.7) | A window of several pauses fits a background GC. Private working set rose 8–17 MiB at onset, which fits a mark-array commit. Against: the timing does not move with activity. | `collect=after` in the warm-up plan forces one blocking, compacting gen2 collection once the warm-up completes, 22–34 s after the first frame. [Warm-up plan](../docs/GAME-LAUNCH.md#warm-a-coreclr-titles-managed-code-before-first-use) |
| One-shot `msedgewebview2.exe` child of the Big Picture host | It lives 1–4 s and appears 120–125 s after the host's WebView2 starts, which is about 93–99 s after the game's first frame in the normal launch flow. Against: `console-246` run2 had the spawn and no stall, and the `1.0.232` runs had a small T97 with no spawn. | The host now logs every WebView2 `ProcessInfosChanged` change with kind, pid and UTC time. `xbox-webview2-arguments.txt` in the package root can add `--disable-component-update` or `--disable-background-networking`. [WebView2 child processes](../docs/XBOX-DIAGNOSTICS.md#webview2-child-processes) |

Every switch is off by default. The pinned profile carries none of the markers, and its warm-up plan is unchanged.

Multiplayer is in scope for this title, so the relay stub can only locate the stall; it cannot ship. It defers relay start-up to the first co-op connection ([relay stub](../docs/GAME-LAUNCH.md#override-steamworks-calls-for-one-title)). If arm B removes T97, the fix must keep `InitRelayNetworkAccess` at start-up and take the wait off the game thread or out of the first two minutes. It must then pass a co-op session that hosts and joins through Steam lobbies and the relay.

### Console test matrix

Take the console through the `xbox-queue` skill. Build every arm from one Steam Core artifact (ProbeNative, LoaderTrace, `XboxManagedWarmup.dll` and the WebView2 host built from this branch). Add `steam-win32-steam-callback-trace-2868840.txt` to every arm, so that only the switch under test differs between arms. Run each arm at least three times. A single clean run proves nothing, because `console-246` run2 had no stall at all.

| Arm | Profile change (beyond the callback trace) | Launch |
| --- | --- | --- |
| A, baseline | none | Restart Steam Core. Select **Play** as soon as Big Picture is ready, as in earlier runs. |
| B, relay stub (diagnostic only) | `steam-win32-no-steam-relay-2868840.txt` | as A |
| C, early GC | append `collect=after` to `steam-win32-managed-warmup-2868840.txt` | as A |
| D, long-lived host | none (the A package) | Restart Steam Core, then wait until at least 180 s after the `webview2 environment` line before **Play**. Confirm that the host's short-lived child (`added=` then `removed=` within a few seconds) is logged before the game starts. |

To stage an arm, copy the profile and add its markers:

```sh
profile=$(mktemp -d)
cp profiles/2868840/26655341/steam-win32-* "$profile/"
touch "$profile/steam-win32-steam-callback-trace-2868840.txt"
touch "$profile/steam-win32-no-steam-relay-2868840.txt"                  # arm B only
printf 'collect=after\n' >> "$profile/steam-win32-managed-warmup-2868840.txt"  # arm C only
```

Rebuild the recipe and package with `--win32-trial-profile "2868840=$profile"` as described in [Build-matched profile](#build-matched-profile). Give each arm its own package version.

For each run:

1. Start the observers before **Play**:
   ```sh
   python3 -m tools.xbox_deploy observe --version "$version" --app-id 2868840 --image-name SlayTheSpire2.exe \
       --frame-seconds 130 --timeout 300 --output "t97-$arm-$n"
   python3 -m tools.xbox_deploy sample-processes --version "$version" --seconds 300 \
       --image-name SlayTheSpire2.exe --image-name msedgewebview2.exe --output "t97-$arm-$n-processes.jsonl"
   ```
2. Leave the game on the idle main menu, with no input and no screenshots, for at least 110 s after the first frame.
3. Before the game exits, copy the raw trace. The observer's `loader-trace.txt` keeps only recent distinct general records. Find the trial folder with `list-files '\TempState' --since-utc "$launch_utc"`, then run `get-file '\TempState\SteamWin32Trial (N)' loader-trace.txt --output "t97-$arm-$n/loader-trace-raw.txt"`. Also copy `managed-warmup-2868840.log` from `\LocalState`.
4. List the frame gaps around the window, and a control window from the same idle menu:
   ```sh
   python3 -m tools.measure "t97-$arm-$n/fhd-present-times.bin" --start-offset-s 95 --duration 7 --list-gaps 45
   python3 -m tools.measure "t97-$arm-$n/fhd-present-times.bin" --start-offset-s 60 --duration 30 --list-gaps 45
   ```

Record for each run:
- the package version and arm;
- the UTC of the first present;
- every gap of 45 ms or more from 95 s to 102 s, with its offset, length and total excess over 16.7 ms;
- the same for the 60–90 s control window;
- `steam_callbacks` from `loader_diagnostics` and any `asset steam-run-callbacks` line within ±3 s of the onset;
- for B, `asset steam-relay=stubbed call=1`;
- for C, the `managed_warmup_collect` line, including its `ms` and `pause_ms` (this is a new stall during menu loading), and `gen2` at `managed_warmup=complete`;
- the `webview2 processes` lines from 60 s to 130 s after the first frame;
- the STS2 private working set around the onset.

Interpret the runs as follows:
- An arm **removes T97** if none of its three runs has a gap of 45 ms or more in 95–102 s, while at least two baseline runs do.
- If A records a `steam-run-callbacks` call of 100 ms or more that covers a T97 gap, the stall is in Steam's callback dispatch even if B does not remove it.
- If D keeps T97 at +97 s after the game's first frame, the event is inside the game process. If T97 disappears in D, or moves to about 120–125 s after the host's WebView2 start, it comes from the host. Then try `xbox-webview2-arguments.txt` with `--disable-component-update` as arm E.
- If C removes T97, keep `collect=after` only if its own pause during menu loading is shorter than the T97 stall it replaces. `System.GC.Concurrent=false` in a staged runtimeconfig is the alternative to test.

Whichever switch wins stays in the build-matched profile and is documented there. A switch that does not help is left off.

## Gameplay run on `1.0.232.680`


Tested on Xbox Series X, OS `26100.9673`, with SteamCore `1.0.232.680` and the build-pinned OpenGL profile in [`profiles/2868840/26655341`](../profiles/2868840/26655341). Big Picture **Play** launched the game at 03:48:00 local on 2026-11-25. All input came from Device Portal remote input through [`tools/xbox_remote_input.py`](../tools/xbox_remote_input.py), with no physical controller. These results were observed:

- The game initialized Steamworks with the signed-in account and reported `Steam is running: True`.
- It logged `CONTROLLER DETECTED: XInput Controller` and switched its prompts to Xbox glyphs.
- **Y** passed the Early Access screen and **A** confirmed The Ironclad. **B** declined tutorials, and the game logged `Embarking on a singleplayer IRONCLAD run. Seed: ZLDW87W2UKCQ`.
- **A** entered the first map node. The log then recorded `Creating NCombatRoom with mode=ActiveCombat encounter=NIBBITS_WEAK`.
- Ten turns were played. The log recorded cards played (`STRIKE_IRONCLAD`, `DEFEND_IRONCLAD`) and monster moves (`BUTT_MOVE`, `SLICE_MOVE`, `HISS_MOVE`). The enemy fell to 20/45 HP and the player to 24/80.
- The run saved locally (`current_run.save`, 39,829 bytes). Steam Cloud writes failed (see open items).
- Screenshot SHA-256: character select `9bee081a…`, map `aad8385b…`, combat `4476f604…`, settled combat `412097e8…`.

An active 30-second combat window from `2026-11-25T08:51:55Z` to `08:52:25Z` passed `python3 -m tools.measure --require-gpu-60 --internal-resolution 1920x1080 --output-resolution 1920x1080` (`gpu_60fps: true`). During that window cards were played, the turn ended and HP changed. No screenshot was taken inside it. The trace's SHA-256 is `df87b242…`. An independent verifier reproduced these numbers from the raw trace:

| Window | Frames | Average | Lowest rolling second | Worst | > 33 ms | Transport |
| --- | --- | --- | --- | --- | --- | --- |
| Active combat, `08:51:55Z` | 1,799 | **59.967 FPS** | **59** | 17.15 ms | 0 | 1,799 shared GPU textures, 0 CPU uploads, hardware adapter verified |
| Idle combat, `08:56:02Z` | 1,799 | 59.967 FPS | 59 | 17.25 ms | 0 | same |

Windows that overlap first-use content miss the lowest-second rule:

| Window start (UTC) | Average | Lowest rolling second | Game-side stalls |
| --- | --- | --- | --- |
| `08:51:25` | 57.53 FPS | 29 | 50 + 117 ms at the first card play |
| `08:52:58` | 59.63 FPS | 50 | 134 + 50 ms at a new enemy move |
| `08:54:23` | 59.53 FPS | 51 | 150 ms at the first `strength_power` icon load |

The log reported each new asset as `Asset not cached`. Every one of these gaps advanced the producer by a single frame, so the game was loading, not the presenter dropping frames. Thirteen further stalls of 500–567 ms all fall within 0.5 s of a Device Portal `/ext/screenshot` capture, so do not take screenshots inside a measured window. Internal resolution is the game's 1920 × 1080 fullscreen window as logged. Godot does not log a render scale. The game caps itself at 60 FPS (`fps_limit: 60`).

## First failing operations, in order

Each row is a console run of the same Steam build. Run numbers are this note's; package versions identify them on the console.

| Package | Route | First failing operation | Resolution |
| --- | --- | --- | --- |
| `1.0.232.80` | D3D12 Forward+ | App and game vanished during Godot shader setup; a `WerFault.exe` reflection clone of the game appeared | Diagnosed below |
| `1.0.232.110` | D3D12 Forward+ | Near-null read in the system pipeline compiler `newbe_xs.DLL+0x00B95960` while Godot built its Forward+ compute shaders | `--rendering-method mobile` |
| `1.0.232.120` | D3D12 Mobile | First GPU frame presented through the hosted composition swap chain; then a near-null write at `SlayTheSpire2.exe+0x00499C91`, `DisplayServerWindows::cursor_set_custom_image` line 3036: `CreateDIBSection` returned no buffer for the adapter's desktop DC | Shared `window-gdi.c` |
| `1.0.232.130`–`.170` | D3D12 Mobile | `DXGI_ERROR_DRIVER_INTERNAL_ERROR` device removal 2.24 s after the first present, with or without the hosted frame copy (`fhd-skip-publish.txt`); Godot then `CRASH_NOW`s (`0xC000001D` at `RenderingDeviceDriverD3D12::_check_device_removed`), WER captures it and the app is sent Home | Open; route set aside |
| `1.0.232.180` | OpenGL via Mesa | Sustained GPU frames; the game's own **Steam Error!** dialog after `SteamAPI_IsSteamRunning` returned false | Discovery hive stays mapped |
| `1.0.232.190` | OpenGL via Mesa | **What's Early Access?** screen, 58.97 FPS with 17 doubled intervals | Presenter draws frames in order |
| `1.0.232.200` | OpenGL via Mesa | Passes the 60 FPS gate on the menu | — |
| `1.0.232.600` | OpenGL via Mesa | Remote A reached the host pad (`0x00010001`) but the game never acknowledged a read; SDL had abandoned its Windows joystick driver after `CoCreateInstance(CLSID_DirectInput8)` failed | Empty DirectInput |
| `1.0.232.620`–`.650` | OpenGL via Mesa | SDL found hosted XInput user 0 but never opened it: `SDL_GAMECONTROLLER_IGNORE_DEVICES` was set inside the game process | Ignore lists withdrawn after the handoff |
| `1.0.232.660` | OpenGL via Mesa | Pad polled and active, no reaction: STS2 used Steam Input, which reported a controller but delivered no actions | Steam Input reports no controllers |
| `1.0.232.670` | OpenGL via Mesa | Still no reaction to keys or pad: Godot's first `WM_ACTIVATE` waits on a `SetTimer` that never fired on the virtual window | `SetTimer`/`KillTimer` for virtual windows |
| `1.0.232.680` | OpenGL via Mesa | Controller detected; run started; combat played; 60 FPS gate passed in combat. Saves deleted at each launch: Steam Cloud writes failed with `k_EResultIOFailure`. Card art white | Writable Steam `userdata`; virtual long current directory |
| `1.0.234.10`–`.11` | OpenGL via Mesa | Art renders; saves reload; quit returns to the library; 60 FPS in combat | — |

### WER and the "suspended host"

The earlier OpenGL trials recorded SteamPrimary "suspending" seconds after the game started. Process sampling at one-second intervals on this build showed the cause. A second `SlayTheSpire2.exe` with `IsRunning: false` and a 36 KB working set appeared together with two `WerFault.exe` processes. That process is Windows Error Reporting's reflection clone of a crashing process. The Xbox then sends the app Home, and its lifecycle log records `suspending`. With the publisher's Sentry crash handler staged, Sentry's native init raised a breakpoint in `libsentry.windows.release.x86_64.dll+0xC6557` while starting `crashpad_handler.exe`. The profile no longer stages `crashpad_handler.exe` or `crashpad_wer.dll`. Sentry then logs `backend disabled` and the game continues. Publisher crash reporting is unavailable in this configuration.

### Steam detection

Steamworks initialized through the private discovery hive, but `SteamAPI_IsSteamRunning` reads `HKCU\Software\Valve\Steam\ActiveProcess` on every call. The adapter restored HKCU right after initialization, so the game later showed its "The Steam application has crashed" dialog. Legacy titles have each SDK call wrapped, but STS2 reaches `steam_api64.dll` through .NET P/Invoke. For a retained handoff that matches the signed-in account, the hive now stays mapped for the rest of the game process. The report sets `STEAM_HANDOFF_SDK_MAPPED`. [Discovery adapter](../runtime/win32/steam-discovery.c)

### Frame pacing

On the old presenter every 33 ms gap advanced the producer by two frames: two frames completed within one display refresh, and the presenter drew the newest and released both. The presenter now draws the oldest completed frame newer than the last one shown and holds the newer frame for the next refresh. The producer already skips a slot the presenter has not released, so at most one frame waits and the game thread never blocks. This change applies to every hosted GPU title. [GPU receiver](../runtime/presenter/gpu-receiver.cpp)

### Controller input chain

Device Portal's `/ext/remoteinput` WebSocket delivers controller presses as `VK_GAMEPAD_*` key events on the host window. [Remote Input API](https://learn.microsoft.com/en-us/windows/uwp/xbox-apps/uwp-remoteinput-api) Each step below is a shared fix, verified on the console by the trace line in brackets:

1. The presenter merges those keys into the hosted pad alongside the Windows.Gaming.Input reading. [Hosted input](../runtime/presenter/hosted-input.cpp)
2. Loader trace returns an `IDirectInput8` with no devices when the console has no DirectInput (`input directinput8=empty`). SDL needs it to keep its Windows joystick driver, which also owns XInput. [Loader trace](../runtime/win32/loader-trace.c)
3. Hosted XInput user 0 stays attached for the session (`xinput-capabilities slot=0 result=0x0`, then `xinput-polled=hosted-slot`). SDL scans once at startup and re-scans only on device notifications. [XInput adapter](../runtime/win32/xinput.c)
4. For hosted-XInput profiles, the launcher turns off SDL's WGI, GameInput, HIDAPI and RAWINPUT backends (`sdl-backends=hosted-only`). After the SDK handoff, the Steam discovery adapter withdraws the controller ignore lists Steamworks publishes. [Launcher](../runtime/steam/ProbeNative.cpp) · [Discovery](../runtime/win32/steam-discovery.c)
5. `steam_api64!SteamAPI_ISteamInput_GetConnectedControllers` reports no controllers for hosted-XInput titles (`steam-input-controllers=hosted-none`). The profile routes the publisher's `coreclr.dll` P/Invoke resolution through loader trace with a hash-pinned companion recipe. STS2 then uses its `GodotControllerInputStrategy` fallback, disassembled from `SteamControllerInputStrategy::ProcessInput`.
6. The window adapter runs `SetTimer` timers on virtual windows as `WM_TIMER` messages. Godot completes its first activation, `first_activation_done` and window focus from that timer. [Window adapter](../runtime/win32/window.c) · [Godot activation](https://github.com/godotengine/godot/blob/4.5.1-stable/platform/windows/display_server_windows.cpp#L4759-L4773)

## Direct3D 12 route

The publisher ships `launch_d3d12.bat`, `launch_vulkan.bat` and `launch_opengl.bat`. Godot's Direct3D 12 context loads `D3D12.dll`, `DXGI.dll` and `Dcomp.dll` dynamically. This build was compiled with `DCOMP_ENABLED`, so it creates its swap chain with `CreateSwapChainForComposition` and binds it with DirectComposition. [Godot D3D12 context](https://github.com/godotengine/godot/blob/4.5.1-stable/drivers/d3d12/rendering_context_driver_d3d12.cpp#L102-L135) · [Godot swap chain setup](https://github.com/godotengine/godot/blob/4.5.1-stable/drivers/d3d12/rendering_device_driver_d3d12.cpp#L2808-L2884)

The shared adapters now cover that contract, verified on the console by trace lines `asset dcomp-device=hosted`, `dxgi hosted-composition-chain`, `dxgi dcomp-target`, `dxgi dcomp-commit-bound result=0x1`, `dxgi gpu-frame-submitted` and `dxgi steam-handoff-ready result=0x707F0000`:

- **Hosted composition swap chains.** With virtual windows active, `CreateSwapChainForComposition` returns the hosted swap chain that publishes to the GPU mailbox. [DXGI adapter](../runtime/win32/dxgi.cpp)
- **Hosted DirectComposition device.** `XboxWin32Dxgi.dll` exports `DCompositionCreateDevice` for virtual-window targets, visuals, `SetContent`, `SetRoot` and `Commit`. `Commit` binds each hosted chain to its window. Unsupported calls return `E_NOTIMPL`. Real HWNDs and foreign visuals return `E_INVALIDARG`.
- **Dynamic routing.** In hosted DXGI mode loader trace redirects `GetProcAddress(dcomp, "DCompositionCreateDevice")`, and substitutes the adapter if `LoadLibraryW(L"dcomp.dll")` fails. [Loader trace](../runtime/win32/loader-trace.c)
- **Steamworks handoff.** The hosted DXGI factory performs the SDK handoff for `steam-win32-steam-sdk` profiles, as the WGL adapter does for OpenGL.

Godot's Forward+ startup crashes the system pipeline compiler, and the Mobile renderer's first frame removes the device about 2.2 s later. The variant profile is kept in [`profiles/2868840/26655341/d3d12`](../profiles/2868840/26655341/d3d12). Measured packages time device removal with the `device-removed-*` trace lines. The next step is to bisect which Mobile-renderer pipeline hangs the GPU.

## Build-matched profile

[`opengl3-bindings.json`](../profiles/2868840/26655341/opengl3-bindings.json) redirects 61 imports to shared adapters:
- Window, focus, cursor, message-pump and display queries go to the window and hosted-display adapters.
- WGL goes to the window adapter.
- `CreateDIBSection` and `DeleteObject` go to the window adapter.
- `SetCurrentDirectoryW` and `GetCurrentDirectoryW` go to the storage adapter, which keeps a virtual current directory for deep existing paths and traces directory results.
- `LoadLibrary*` goes to loader trace, which provides hosted `XInput1_4.dll` and the fault observer.

The launch marker selects `--rendering-driver opengl3`, the Godot launcher, loader trace and `--accessibility disabled`. The companion list stages the 212 files beside the executable except the `.pck`, the launch scripts, the Steam Input configuration and Sentry's crash handler.

```sh
profile_source="$PWD/profiles/2868840/26655341"
profile=$(mktemp -d)
(cd "$steam_install" && shasum -a 256 -c "$profile_source/SOURCE-SHA256SUMS") || exit 1
cp "$profile_source"/steam-win32-* "$profile/"
python3 -m build_tools.redirect_pe_imports --recipe --trim-dwarf "$steam_install/SlayTheSpire2.exe" \
  "$profile_source/opengl3-bindings.json" "$profile/steam-win32-shadow-2868840.xwp"
```

If no local Windows install exists, export the executable from the console with `inspect-game --source-profile` (a one-file profile with the size and SHA-256 above) and `--export-inputs`. Package with `--surfaceless-profile 2868840=26655341 --win32-trial-profile "2868840=$profile"`, using a Steam Core template and presenter from this branch.

## Coverage status

On build `26655341` with SteamCore `1.0.234.10`/`.11`, the following were observed on the console: gameplay, controller input through the hosted pad, save and reload, return to the library, and a sustained 60 FPS at 1920 × 1080 on shared GPU textures. Audio is initialized but has not been heard. A person at the console has since heard the audio and played with a physical controller. Co-op multiplayer is in scope and has not been tested, so STS2 is not yet counted as covered. [Multiplayer](#remaining-open-items)
