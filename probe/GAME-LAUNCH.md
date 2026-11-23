# Steam game launch contract

The Steam library and ordinary Play workflow work across titles. Whether a particular game can run depends on its installed build and the Xbox runtime available to it. X-COM is the verified DOS example. Factorio uses ordinary Steam Play with its publisher's Windows x64 launch option and currently exits in the Windows loader.

## Common game checks

1. Restore or establish the Steam session and use Steam's library metadata to identify the selected AppID.
2. Install through Steam into the app's own writable library. Steam must report the install complete before launch. For a hosted game also verify the completed local manifest and exact title directory.
3. Recheck the live session, subscription, installed library path, and Steam prerequisites in the native component before starting either backend.
4. Record the AppID and chosen backend in the launch report. Distinguish an accepted request, a captured process, and visible game rendering. The installed state only establishes that the content is available.

## Windows titles

The default route calls Steam Play for the selected AppID. Use the publisher's Windows x64 launch option when the default does not select the right executable. Pass `--steam-launch-option APPID:INDEX` once per title to the local package builder. Options are available through ordinary library Play and each signed marker applies only to its AppID. The older `--launch-app-id APPID --launch-option INDEX` form still configures the automatic trial title.

Check a failed launch against the installed executable's architecture, supported Windows API imports, runtime dependencies, and graphics backend. `inspect-game --executable` reads the installed PE header. The loader and process checkpoints show where the request stopped. Any runtime dependency added to the package should come from the original signed redistributable. Keep changes scoped to the verified title and depot.

### Check desktop namespace assumptions before adapting a title

Audit the executable and its companions together. Include delayed imports and the paths a library passes to dynamic loads, named IPC, and helper processes. Use the same AppContainer and package dependencies as the real launch when testing the following boundaries.

| Desktop assumption | Xbox compatibility check |
| --- | --- |
| A local IPC name reaches the same object in both processes | Keep the endpoints in the same app. For named pipes, resolve both creation and open/wait calls in `\\.\pipe\LOCAL\`. Exercise the helper launch and handle inheritance when the protocol uses them. |
| A mutex, event, semaphore, timer, or file mapping uses a shared desktop name | Check that both processes open the same object under the app's identity. Trace the actual name and access flags. When the protocol shares a handle, verify that both endpoints can use it and close it. |
| DLLs resolve beside the original executable or from a desktop runtime install | Stage verified game companions at the expected relative paths. Check system exports and signed package-framework exports with the corresponding loader search modes. |
| Documents and app data use ordinary desktop profile paths | Supply writable paths scoped to the signed-in account and AppID. Exercise the game's log and save locations through its actual Win32 folder APIs. |
| The Steam SDK discovers a global desktop client registration | Resolve the real authenticated Steam session with the app's private discovery configuration and verify the SDK reports the expected signed-in account. |

Check device creation and reported graphics capabilities separately. A successful DLL load or a usable window does not establish the feature level, shader model, presentation path, or frame rate required by the game. [AppContainer named pipes](https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-createnamedpipew) · [Packaged library loading](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-loadpackagedlibrary)

The compatibility scan flags named-pipe, synchronization, and file-mapping imports across the executable and its companion DLLs, including delay imports where available. Use these leads to decide what to trace. Synchronization and mapping calls can create unnamed objects, so check their runtime arguments before selecting an adapter. [Sharing named objects](https://learn.microsoft.com/en-us/windows/apps/develop/communication/sharing-named-objects)

If the game exits during startup, a trial can bind `kernel32.dll!VirtualAlloc` to `XboxWin32LoaderTrace.dll`. The trace records failed commits with the requested bytes, Windows error, and available commit in KiB. The adapter preserves the allocation result and last error. Use the diagnostic to distinguish a CPU commit failure from a D3D heap failure when both occur during renderer startup.

Legacy CRTs can query the file type of standard-output and standard-error handles while a companion DLL attaches. For an opted-in import, `XboxWin32LoaderTrace.dll` turns the Xbox runtime's `STATUS_INVALID_HANDLE` exception into the documented `GetFileType` result of `FILE_TYPE_UNKNOWN` with `ERROR_INVALID_HANDLE`. Valid handles use the native result. [GetFileType contract](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-getfiletype)

Use `system-sha256:` in the Win32 companion list to authorize a verified staged DLL as a fallback for a system-only `LoadLibraryEx` request. The loader first attempts the original search. On `ERROR_MOD_NOT_FOUND`, it loads the approved module by its absolute path and searches the module directory and System32 for dependencies. The profile's digest check runs before the module becomes eligible. [Library search flags](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw)

When a shader fails after device creation, reproduce pipeline creation on the same adapter with the original bytecode and root signature. For fixed resource bindings, `dxil-legacy-bindings.py` can translate DXC disassembly from Shader Model 6.6 to 6.4. Assemble the result with `dxa`, validate and write the container with `dxv -o`, then verify the translated shader's output on the Xbox GPU. A verified PE recipe can use `redirect-pe-imports.py --recipe --overlays` to apply same-length, source-hash-matched ranges to the temporary launch copy.

### Verify owned Windows files before a trial

Add `--win32-source-profile APPID=FILE` with a current `--win32-runtime-template` when a game needs a build-matched runtime or asset preparation. The JSON keys are Steam build IDs. Each build lists the relative paths, byte sizes, and SHA-256 hashes needed for the trial. After verifying Steam ownership and the completed installation, Play checks these files in the selected Steam library and logs `steam_source_check`. A failed check reports the installed build and, for file errors, the relative filename. Use a small set of launch-critical files to keep checks fast.

For Marvel’s Spider-Man Remastered, use `--win32-source-profile 1817070=probe/win32-runtime/profiles/1817070/source.json`. This profile checks the executable, asset index, Direct3D 12, audio, and selected companion libraries for Steam builds `14752622` and `23986256` against the original and experimental UWP Spider Launcher fingerprints. To check the inputs offline, run the command below with the build ID from the local Steam manifest. [Original build fingerprints](https://github.com/BBCPrince/UWP-Spider-Launcher/blob/1865c248825f617b24ee75b258f10e634338ff0b/source/SteamFiles.json) · [Experimental build fingerprints](https://github.com/BBCPrince/UWP-Spider-Launcher/blob/1865c248825f617b24ee75b258f10e634338ff0b/source/SteamFiles40630.json)

```sh
python3 probe/verify-win32-source.py probe/win32-runtime/profiles/1817070/source.json \
  "$owned_game_directory" --build "$steam_build_id"
```

The same game builds can be tested independently in the upstream combined launcher. The attached 0.6.0.57 Xbox MSIX has SHA-256 `908bbe74275b20036d9ab533849745851573f2f3efedd6b6fdaaf48b1254d6d6` and contains the launcher’s native host and preparation support. Its preparation step generates lower-detail assets and pipeline data from the player’s own files. Our preflight can establish source compatibility for an Xbox launch trial. Gameplay in our layer still requires validating Direct3D 12 presentation, the game’s memory budget and prepared assets, Steam handoff, controller, sound, saves, and return to Big Picture. [Upstream release package](https://github.com/BBCPrince/UWP-Spider-Launcher/releases/tag/v0.6.0.56-experimental.1)

### Scope a Win32 trial’s Documents folder

When an audited image uses `SHGetFolderPathW` for Documents, bind that import to `XboxWin32Desktop.dll` and include an empty `steam-win32-documents-APPID.txt` in the matching trial profile. The native launcher creates a persistent Documents folder under the existing account and AppID profile in LocalState. The adapter returns that folder for the current user’s Documents query. It delegates unrelated queries to the system. Verify local save and reload separately from Steam Cloud. [Windows folder API](https://learn.microsoft.com/windows/win32/api/shlobj_core/nf-shlobj_core-shgetfolderpathw)

## Hosted titles

Register an adapter only when a bundled runtime can execute the verified installed format. An adapter declares its AppID, backend, native authorization method, success marker, and start function. The dispatcher opens a hosted runtime only after that method verifies the selected install. A refused or failed authorization returns to the library with its diagnostic.

The SDL player fills the Xbox viewport and fits the game's complete image at its original aspect ratio. Hold Menu and View together to open the player menu. Use the D-pad and A to choose Resume or Quit to library. The game keeps its regular Menu and View actions when each button is pressed individually.

A hosted SDL game with the keyboard-capable SDL build receives physical keyboard keys through the same player. To type with a controller, focus the game's text field, hold Menu and View, and select **Keyboard**. Enter text with the Xbox keyboard, then choose **Send text**. Use the game's confirmation control to continue. The Xbox player closes the text overlay after sending and returns focus to the game.

For DOS, the native component allowlists the exact supported AppID and directory. The fixed profile in `signin/xcom-dos.js` records the publisher's DOS mount, startup command, supported configuration and file exclusions. The host handles file copying, process lifetime, return to the library, and optional local trial snapshots. When adding a DOS title, verify its owned depot, define a separate fixed profile, and extend the native allowlist to that exact title and install path. Keep publication of the runtime separate from the user's game files. Input and save persistence also need their own validation for each supported backend.

## Verification

Run these from the repository root. GitHub Actions runs the same tests without Steam credentials or game files.

```sh
node --test tests/*.test.js
python3 -m unittest discover -s tests
```

The tests cover native Play and two synthetic hosted profiles, authorization refusal, launch-option layout, DOS files and empty save slots, and stale callbacks after a game is closed. On Xbox, verify each backend with a real installed title. For hosted games, capture a recognizable frame and sample input across the host and child with `inspect-input`. Use a physical controller to confirm a visible game response, then open Steam with View + Menu, resume and exit to the library. Run overlay checks through Big Picture Play or a signed `steam-launch-via-big-picture.txt` trial so the running game has a Steam session to return to. For Windows titles, keep the actual Steam result and process exit in the compatibility record. [Input observer](XBOX-DIAGNOSTICS.md#inspect-the-current-games-input)

### Native process startup snapshot

For a local native Steam Play trial, add `--launch-app-id 975370 --startup-snapshot` when preparing the package. The option binds the diagnostic to that AppID. Before submitting Play, the probe waits 50 ms on its own process and records the control outcome and the probe PID. A healthy control reports `steam_launch_snapshot_control=running`.

Once Steam identifies a newly created process for the selected AppID, the probe writes its PID, creation time, available executable basename, and immediate Windows wait result at `captured-process`. It then waits up to one second on the same handle on a background thread and writes `sampled-process` before returning to Steam's callback and window polling. `running` describes an unsignaled handle at the sample time. `exited` includes the actual exit code. Neither outcome establishes a game window.

For Dwarf Fortress, compare the stock and rebuilt SDL2 on the same installed package. Use the captured PID and creation time to correlate Device Portal observations. If only the first snapshot survives, record its exact fields and whether the probe PID still exists before restarting the app.

The Series X control with the stock SDL2 and no Visual C++ framework dependency exited immediately with `0xC0000135`. Adding the official signed desktop Visual C++ framework as a package dependency changed the startup exit to `0xC0000139`. The loader control identified `imm32!ImmGetIMEFileNameA` as unavailable. Keep the framework dependency in both arms of an SDL comparison.

With the version-matched SDL 2.26.2 rebuild using `SDL_DISABLE_WINDOWS_IME`, the same package captured `Dwarf Fortress.exe` and found its process handle still running at the one-second sample. The trial did not reach a confirmed game window. Restore the publisher's SDL2 after the experiment and verify its original hash. The next comparison should record whether the process reaches SDL window creation or renderer initialization. [Dwarf Fortress compatibility results](https://github.com/clankur/steam-xbox-devmode/issues/35)

The shared SDL trial artifacts can write initialization, window, OpenGL and renderer checkpoints when started under `--startup-snapshot`. Read a run with `report --version "$version" --sdl-startup`. Use `--startup-sdl-renderer software` or `--startup-sdl-renderer direct3d11` to request a renderer in a separate signed test. `--startup-sdl-dummy-video` requests the SDL offscreen test driver. `--startup-sdl-working-directory` asks SDL's main entry point to set the child process's current directory to the verified app-local game installation. The native report records requested settings and the SDL trace records which startup calls returned.

Use `--startup-wait-only` with `--startup-snapshot` and `--repeat-launch` to classify a fast exit on the captured Windows process handle. The probe writes up to twelve 500 ms samples and records its app memory usage and budget when available. It skips Steam's window query during that observation. The final status `process-wait-complete` describes the diagnostic, and the sampled Windows exit code describes the process.

On `1.0.188.0`, Dwarf Fortress resolved the executable base path and initialized SDL's timer, then `SDL_GetPrefPath` returned an error before it initialized video. Add `--startup-sdl-local-preferences` to create `LocalState/SDLPreferences/<AppID>` for a repeatable comparison with the shared SDL fallback build. The child first calls the original SDL lookup. On failure it attempts the provided directory and logs `sdl_pref_path_local=ready` if it obtains a writable preference path. This stores the trial's preferences inside the sideloaded package's local data.

Build `1.0.189.0` reached SDL video, window creation and the software renderer after applying that fallback. The first captured frame showed the game's loading screen. For a later frame, prepare with `--startup-sdl-late-frame --startup-sdl-dummy-video --startup-sdl-renderer software` and run the shared `sdl-trial` with `--frame "$frame_bmp" --frame-timeout 38`. Build `1.0.191.0` captured the full 1200 × 800 Dwarf Fortress main menu twelve seconds after the first presentation, with all twelve half-second process samples reporting `running`. [Xbox SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44909296379) · [Shared SDL build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44909296473)

### Play Dwarf Fortress in the Steam library

Prepare the regular sign-in package with `--startup-sdl-host` and the official desktop VC++ framework. Deploy it and install the checksum-verified shared SDL 2.26.2 artifact with `sdl-trial --install-only --app-id 975370`. The CLI confirms Steam's local manifest and the publisher DLL before replacement, verifies the installed replacement, and stores the original in the specified backup directory. Use the same artifact and backup directory with `sdl-trial --restore-only` to restore it.

The library's Play action uses Steam to check the license and launch the installed executable. The SDL player receives software frames and uses controller-driven pointer input for Dwarf Fortress. Move with the left stick or D-pad and press A to click. The Series X trial `1.0.218.0` confirmed fullscreen presentation and pointer event delivery with the existing version-matched SDL adapter. [Live SDL workflow](https://github.com/clankur/steam-xbox-devmode/actions/runs/44910058092) · [Fullscreen Xbox host SDK](https://github.com/clankur/steam-xbox-devmode/actions/runs/44920639328)

### Play Stardew Valley in the Steam library

Supply the Windows CI artifact `xbox-hosted-monogame/StardewStartupDiagnostic.dll` to `--stardew-sdl-host` and the extracted `xbox-sdl2-2.30.9-x64` artifact directory to `--stardew-sdl-runtime`. The package builder verifies the SDL checksum, architecture and version. Stage Mesa with `--sdl-surfaceless --dxil-archive "$dxc_zip"` and deploy both official VC++ frameworks. The library starts the installed game through Steam. [Mesa and DXIL staging](https://github.com/clankur/steam-xbox-devmode/pull/53)

For a console trial, add `--launch-app-id 413150 --repeat-launch` to the hosted Stardew layout and run `probe --steam-core` through the Xbox CLI. `1.0.211.0` presented the full title menu, acknowledged the host's test pointer event and remained on the title menu at the 40-second screenshot. The trial verifies startup, display and pointer delivery. Gameplay, audio and saves still need console validation. [Shared SDL2 build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44916965047) · [Xbox host SDK](https://github.com/clankur/steam-xbox-devmode/actions/runs/44916965212)

For controller play, use the shared SDL2 build with the virtual gamepad transport. The player forwards Xbox buttons, both sticks and both triggers as SDL game controller state. `steam_sdl_host_gamepad=delivered` verifies test input was readable through that SDL device on the console. [Controller SDL2 build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44921504825)

### Play Slay the Spire in the Steam library

Supply `--slay-sdl-host "$runtime_dir"` with the built `SlayConsoleDiagnostic` classes, signed-template Java bridge DLLs, adapted LWJGL JAR, verified shared SDL 2.30.9 DLL and license, and `slay-font-atlas-limit.txt` containing `512`. Stage the same Mesa and DXIL release files alongside the Java and SDL adapters. The current library launch authorizes the installed Steam AppID `646570` before starting the original launcher in its packaged Java runtime. The Xbox SDL player owns the frame and pointer mailbox throughout the session. [Slay Java and LWJGL adapter](https://github.com/clankur/steam-xbox-devmode/pull/52) · [Shared SDL2 build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44916965047)

With `--launch-app-id 646570 --repeat-launch`, `1.0.213.0` reached Slay the Spire's save-slot selection in the live Xbox player and was still animating at the 40-second capture. `1.0.214.0` confirmed pointer delivery using a repeated test motion after input initialization. Keep the automated trial at the menu and validate save creation during a separate gameplay session. [Xbox host SDK](https://github.com/clankur/steam-xbox-devmode/actions/runs/44918568603) · [Slay bridge SDK](https://github.com/clankur/steam-xbox-devmode/actions/runs/44918265865)

Build the LWJGL adapter with the pinned libGDX 1.9.5 API inputs using `--gdx-api` and `--gdx-controllers-api` to supply its desktop gamepad provider. In `1.0.217.0`, the game recognized the bridged controller through libGDX and displayed its own Xbox button prompt in fullscreen. [Controller bridge SDK](https://github.com/clankur/steam-xbox-devmode/actions/runs/44920534966)

Slay's text entry requires the keyboard-capable LWJGL adapter, native display bridge, and shared SDL2 build together. For the save-slot name prompt, test typing and Backspace with a physical keyboard, then use the player **Keyboard** action to send text using the controller. The game is responsible for accepting the name and creating the save slot.

The Series X run of `1.0.221.0` reached save-slot selection and reported controller delivery. The live mailbox advertised keyboard support. Local tests checked LWJGL key transitions, editing and Unicode, and the player's controller text entry. Entering a name with the Xbox system keyboard still needs a console check. The regular library build `1.0.222.0` uses the same keyboard components. [Xbox host SDK](https://github.com/clankur/steam-xbox-devmode/actions/runs/44923292933) · [Shared SDL2 build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44922918078) · [Slay bridge SDK](https://github.com/clankur/steam-xbox-devmode/actions/runs/44922917181)

Build the hosted adapter with JDK 11 or later and the pinned upstream JARs. Use a fresh output path. The selected virtual window size is embedded in the adapter. The player scales the window to fit the TV and maps input into the same dimensions.

```sh
python3 probe/build-lwjgl-sdl.py \
  "$deps/lwjgl-2.9.2-sources.jar" "$deps/lwjgl-2.9.2.jar" "$deps/asm-9.7.1.jar" \
  "$runtime_dir/slay-lwjgl-display.jar" \
  --gdx-api "$deps/gdx-1.9.5.jar" --gdx-controllers-api "$deps/gdx-controllers-1.9.5.jar" \
  --render-size 854x480
```

Pass the resulting runtime directory to the existing `--slay-sdl-host` packaging option. Selecting **Play** starts the signed-in library launch. The offscreen game uses the TV player's presentation clock. A configured 60 FPS permits the renderer to produce up to 120 frames per second so fresh frames can be ready between TV refreshes. Lower frame limits retain their configured values. The adapter reads the player's existing FPS setting from `info.displayconfig` and stores subsequent Xbox FPS changes in app-local `slay-native-temp/xbox-slay-fps.txt`.

On Series X, the original libGDX pixmap decoder failed on both `title/title.jpg` and `bottomScene/scene.jpg`. The hosted adapter decodes the title and act atlas pages at half size on each axis and retains the original atlas coordinates. The Series X loaded the title and entered an Act 1 encounter with those assets. Other game textures use the original loader.

For a presentation trace, add `--launch-app-id 646570 --repeat-launch --measure-sdl-frames` to a hosted Slay test package. In the HTML fallback, the player counts submissions of new SDL sequences to the fullscreen canvas. Native measurements use the swapchain trace described below. A slow load can lower that first sample. For steady-state gameplay comparisons, start each sample after the scene has finished loading. Capture Device Portal screenshots outside the sample period because a capture can stall the game for about half a second.

The earlier HTML player used an 854×480 render target, scaled to fill the 16:9 display. In a Series X test with a fresh, isolated Ironclad profile, each scene warmed up for eight seconds before a 30-second sample. Fullscreen presentation used lossless RGB frame differences. A player with an older native bridge uses the existing full-frame transfer.

| Scene | New frames presented per second | Lowest rolling one-second count | Longest frame interval |
| --- | ---: | ---: | ---: |
| Title menu | 61.98 | 58 | 34.0 ms |
| Act 1 map | 62.42 | 60 | 34.3 ms |
| Act 1 combat | 61.52 | 56 | 48.4 ms |

The combat sample included two attacks, three blocks, enemy turns, and hand redraws. These counts measure the application submitting distinct frames to the canvas. At 960×540 the same transfer averaged 55.82 FPS during combat, including a Device Portal screenshot. The baseline title test with the original frame timers averaged 15.25 FPS at 1280×720.

A hosted launcher can request a shared virtual desktop with the environment variables `XBOX_SDL_RENDER_WIDTH` and `XBOX_SDL_RENDER_HEIGHT`. Supply both as decimal pixel counts between 640×360 and 1920×1080. The SDL dummy and offscreen backends advertise the virtual desktop at 60 Hz and use it for resizable windows. Games retain control of fixed-size windows, exclusive display modes and logical rendering sizes. Slay uses the same dimensions for its libGDX viewport, offscreen display and input. With no valid request, the game uses its packaged display profile.

### Native fullscreen presentation

The Steam SDK template includes a native fullscreen player. The library requests a 1920×1080 render target for hosted games. Mesa games publish their completed color buffer through two shared Direct3D textures. GPU fences let the player present the latest finished frame on the next display refresh. Software-rendered games use the same player through a pixel upload. Hold Menu and View together, or press F10, to open the player menu.

On Xbox Series X, both games below used a native 1920×1080 game backbuffer, viewport and renderbuffer. Each 30-second window counted distinct frames submitted to the visible swapchain. Slay’s combat sample included two attacks, three blocks, three turns and damage to both sides. Stardew’s farmer walked 9,503 pixels during the outdoor sample.

| Scene | Distinct GPU frames presented | Average FPS | Lowest rolling second | Longest frame interval |
| --- | ---: | ---: | ---: | ---: |
| Slay the Spire title | 1,799 | 59.97 | 58 | 32.3 ms |
| Slay the Spire map | 1,798 | 59.93 | 59 | 29.6 ms |
| Slay the Spire combat | 1,799 | 59.97 | 58 | 48.4 ms |
| Stardew Valley title | 1,798 | 59.93 | 58 | 34.0 ms |
| Stardew Valley outdoor farm | 1,800 | 59.97 | 58 | 34.9 ms |

The same native player presented Dwarf Fortress’s 1920×1080 software-rendered title screen at 49.97 FPS with the default graphics cap of 50. Setting the game’s graphical FPS cap to 60 produced 1,797 distinct frames in 30 seconds (59.90 FPS), with a lowest rolling second of 58 and a longest interval of 39.0 ms. Its simulation cap remained 100.

Stardew kept its original fixed simulation step and requested a swap interval of one. Carrying the fractional 60 Hz deadline between SDL swaps raised outdoor presentation from 58.85 FPS to 59.97 FPS.

Add `--measure-sdl-frames` to a repeat launch of an enabled SDL profile to record native presentation timestamps to `LocalState/fhd-present-times.bin`. Copy the trace after the scene finishes. Analyze a 30-second interval starting eight seconds after the scene became playable, using the console’s UTC clock. The output includes the source transport, distinct frames per second, lowest rolling one-second rate and frame intervals.

```sh
python3 probe/fhd/measure.py fhd-present-times.bin --start-utc "$scene_start_plus_eight_seconds" --duration 30
```

The current GPU receiver checks the presenter's DXGI adapter flags and confirms that its adapter LUID matches the game's shared textures. The fullscreen and primary XAML presenters write version 2 frame traces. Use `--require-gpu-60` with `measure.py` to require a scene of at least 30 seconds, exclusively hardware shared-texture frames, an average of at least 59.5 FPS, and at least 57 distinct frames in each rolling second. The average threshold accommodates nominal 60 Hz displays such as 59.94 Hz. Use the same flag with the compatibility coverage tool. Record the observed gameplay sample as `performance_scene: "gameplay"` and include its measured summary as `performance`. [DXGI adapter flags](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_adapter_flag)

Series X build `1.0.230.54` ran Slay the Spire through the primary XAML presenter at 1920 × 1080. The first 30-second window, beginning five seconds after the first frame, averaged 57.40 FPS with a 1.25-second startup stall. The immediately following 30-second title window submitted 1,798 hardware frames at 59.93 FPS. Its lowest rolling second was 59 FPS, its 95th-percentile frame interval was 16.76 ms, and every frame used a shared texture. This establishes the updated hardware path and title pacing for the original Slay the Spire.

### Hosted Direct3D 11 pacing experiment

Build the same `XboxWin32Dxgi.dll` into each signed package with the title's hosted Direct3D 11 profile and `fhd-measure.txt` at package root. Select the optional settings by adding empty package-root markers before packing and signing.

| Package-root markers in addition to `fhd-measure.txt` | Producer timing |
| --- | --- |
| None | Publish the GPU texture before waiting on the existing 60 Hz clock. |
| `fhd-paced-publish.txt` | For an established hosted swap chain presenting at interval 1 with no DXGI flags, wait before copying and publishing the GPU texture. |
| `fhd-refresh-5994.txt` | Use the 60000/1001 Hz period when the producer honors a synchronized interval. Use this variant on a display measured near 59.94 Hz. |
| Both | Wait before copying and publishing at the 60000/1001 Hz cadence. |

Keep the game build, resolution, display setting, and gameplay scene fixed. Compare distinct GPU frames and frame intervals in the same settled 30-second scene using `measure.py --require-gpu-60`. With the loader trace enabled, `dxgi pacing-before-publish` confirms the reorder ran and `dxgi pacing-rate-millihz` reports 59940 as hexadecimal `0x0000EA24`. Compare the producer's `attempts`, `submitted`, `slot-busy` and `max-gap-ms` with the panel report's visible render callbacks, source-empty callbacks, and presentation-busy callbacks. A large producer gap with few busy slots still needs timing for the game's work and the underlying DXGI call. A visibility pause can stop the panel from retiring textures and make both slots busy even while the game keeps presenting. The standard package uses the existing 60 Hz path unless measurement and an experiment marker are present.

For Hades, time a 40–45-second block of movement and combat after the room settles. Record the start and end with a clock checked against the Xbox. Use the same central 30 seconds in both traces with `measure.py --start-utc "$scene_start_plus_five_seconds" --duration 30 --require-gpu-60`. Let the game keep presenting for a few seconds after the block so the last buffered records are available. Collect the app's frame file and producer trace before installing the next build. The automatic first-frame sampling window can include menus and loading.

Add `--diagnose-stalls --producer-log loader-trace.txt` to correlate the longest gaps with game calls, GPU publication, software pacing and native Present. The producer log can also come from the same run's saved launch report after normal exit. Use both records from the same game process and scene. A gap without a matching producer phase remains unattributed.
