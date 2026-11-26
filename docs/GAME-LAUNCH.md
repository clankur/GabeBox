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

A hosted Direct3D trial can opt into the presenter keyboard queue through an empty `steam-win32-hosted-keyboard-APPID.txt` and build-matched redirects for `GetKeyState`, `GetKeyboardState`, and `TranslateMessage`. The launcher reports `steam_shadow_keyboard=hosted` when it selects the adapter. The window trace records `win32_host_keyboard=ready` once a focused virtual window can accept keys and Unicode text. [Factorio keyboard profile](../profiles/427520-26246811/hosted/keyboard) · [Hosted keyboard control](../probe/graphics-runtime-control/window-keyboard-child.c)

If the game exits during startup, a trial can bind `kernel32.dll!VirtualAlloc` to `XboxWin32LoaderTrace.dll`. The trace records failed commits with the requested bytes, Windows error, and available commit in KiB. The adapter preserves the allocation result and last error. Use the diagnostic to distinguish a CPU commit failure from a D3D heap failure when both occur during renderer startup.

Legacy CRTs can query the file type of standard-output and standard-error handles while a companion DLL attaches. For an opted-in import, `XboxWin32LoaderTrace.dll` turns the Xbox runtime's `STATUS_INVALID_HANDLE` exception into the documented `GetFileType` result of `FILE_TYPE_UNKNOWN` with `ERROR_INVALID_HANDLE`. Valid handles use the native result. [GetFileType contract](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-getfiletype)

Use `system-sha256:` in the Win32 companion list to authorize a verified staged DLL as a fallback for a system-only `LoadLibraryEx` request. The loader first attempts the original search. On `ERROR_MOD_NOT_FOUND`, it loads the approved module by its absolute path and searches the module directory and System32 for dependencies. The profile's digest check runs before the module becomes eligible. [Library search flags](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw)

When a shader fails after device creation, reproduce pipeline creation on the same adapter with the original bytecode and root signature. For fixed resource bindings, `python3 tools/dxil-legacy-bindings.py` can translate DXC disassembly from Shader Model 6.6 to 6.4. Assemble the result with `dxa`, validate and write the container with `dxv -o`, then verify the translated shader's output on the Xbox GPU. A verified PE recipe can use `python3 -m build_tools.redirect_pe_imports --recipe --overlays` to apply same-length, source-hash-matched ranges to the temporary launch copy.

### Verify owned Windows files before a trial

Add `--win32-source-profile APPID=FILE` when a game needs a build-matched runtime or asset preparation. The JSON keys are Steam build IDs. Each build lists the relative paths, byte sizes, and SHA-256 hashes needed for the trial. After verifying Steam ownership and the completed installation, Play checks these files in the selected Steam library and logs `steam_source_check`. A failed check reports the installed build and, for file errors, the relative filename. Use a small set of launch-critical files to keep checks fast.

For a `--win32-trial-profile APPID=DIRECTORY`, include `steam-win32-adapters-APPID.json` beside the recipes and launch markers. Set `build_id` to the build in the trial marker and list the required GabeBox DLL filenames in `required_adapters`. Include adapters loaded dynamically as well as import targets. When a recipe depends on particular exports, use `required_exports` to map each declared adapter to the exact case-sensitive names it must export. Assembly checks the declared DLLs and named exports against the same checksum-pinned APPX supplied through `--session-artifact` and `--session-checksum`. Missing DLLs or exports stop assembly before the layout is published. [Build-matched adapter example](../profiles/427520-26246811/hosted/steam-sdk/steam-win32-adapters-427520.json) · [PE Export Directory](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#the-edata-section-image-only)

For Marvel’s Spider-Man Remastered, use `--win32-source-profile 1817070=profiles/1817070/source.json`. This profile checks the executable, asset index, Direct3D 12, audio, and selected companion libraries for Steam builds `14752622` and `23986256` against the original and experimental UWP Spider Launcher fingerprints. To check the inputs offline, run the command below with the build ID from the local Steam manifest. [Original build fingerprints](https://github.com/BBCPrince/UWP-Spider-Launcher/blob/1865c248825f617b24ee75b258f10e634338ff0b/source/SteamFiles.json) · [Experimental build fingerprints](https://github.com/BBCPrince/UWP-Spider-Launcher/blob/1865c248825f617b24ee75b258f10e634338ff0b/source/SteamFiles40630.json)

```sh
python3 -m build_tools.verify_win32_source profiles/1817070/source.json \
  "$owned_game_directory" --build "$steam_build_id"
```

The same game builds can be tested independently in the upstream combined launcher. The attached 0.6.0.57 Xbox MSIX has SHA-256 `908bbe74275b20036d9ab533849745851573f2f3efedd6b6fdaaf48b1254d6d6` and contains the launcher’s native host and preparation support. Its preparation step generates lower-detail assets and pipeline data from the player’s own files. Our preflight can establish source compatibility for an Xbox launch trial. Gameplay in our layer still requires validating Direct3D 12 presentation, the game’s memory budget and prepared assets, Steam handoff, controller, sound, saves, and return to Big Picture. [Upstream release package](https://github.com/BBCPrince/UWP-Spider-Launcher/releases/tag/v0.6.0.56-experimental.1)

### Scope a Win32 trial’s Documents folder

When an audited image uses `SHGetFolderPathW` for Documents, bind that import to `XboxWin32Desktop.dll` and include an empty `steam-win32-documents-APPID.txt` in the matching trial profile. The native launcher creates a persistent Documents folder under the existing account and AppID profile in LocalState. The adapter returns that folder for the current user’s Documents query. It delegates unrelated queries to the system. Verify local save and reload separately from Steam Cloud. [Windows folder API](https://learn.microsoft.com/windows/win32/api/shlobj_core/nf-shlobj_core-shgetfolderpathw)

### Scope a Win32 trial’s Roaming AppData folder

For a build that queries `CSIDL_APPDATA`, bind the same Shell32 import and include an empty `steam-win32-roaming-appdata-APPID.txt`, the source fingerprint, and the declaration for `XboxWin32Desktop.dll`. The scoped Shell32 lookup returns the same persistent Roaming directory the launcher supplies in the child’s `APPDATA`. The native report records `steam_shadow_roaming_appdata=profile` after child creation. Steam resolves its own Auto-Cloud paths and synchronizes them around the tracked launch. Check the authenticated client’s resolved path and launch and exit reports separately from the game’s local file round trip. [Steam Auto-Cloud root paths](https://partner.steamgames.com/doc/features/cloud)

A matching Roaming profile may include `steam-win32-roaming-default-APPID.txt` with a relative `.ini` path on the first line and the initial settings below it. The launcher writes the complete default to a temporary file in the account’s game profile and renames it only if the destination is absent. The default's modification timestamp predates Steam Cloud so a game comparing modification times can prefer its existing cloud settings. `steam_shadow_roaming_default` reports whether the launcher created the file, found an existing file, or encountered a storage error. Play continues if the default is unavailable.

A matching Roaming profile can also list required directories in `steam-win32-roaming-folders-APPID.txt`, one relative path per line. The launcher validates the list, creates missing folders inside the account's game profile, and verifies that each is an ordinary directory. It keeps existing contents and reports `steam_shadow_roaming_folders=ready` before spawning the game.

### Verify an installed Steam SDK companion

Use `inspect-game --version PACKAGE_VERSION --app-id APPID --expected-build BUILD --fingerprint-input RELATIVE_PATH` to stream an EXE or DLL from the completed installation and report its size and SHA-256. The command checks the manifest again before reporting the result. Use that record with `--source-profile` and `--export-inputs` to inspect a verified copy locally when needed.

If the Xbox trace identifies a Steam discovery failure in a game that imports `SteamInternal_SteamAPI_Init`, bind the audited init and restart imports to `XboxWin32SteamDiscovery.dll` in the signed recipe. Declare the adapter and its two required exports. Record the verified SDK SHA-256 in `steam-win32-steam-legacy-sdk-APPID.txt` and in the `steam_api64.dll` companion row as a `sha256:` pin. The staged SDK receives the caller’s version list and error buffer. The shim verifies the expected signed-in Steam account on successful init. [Valve SDK initialization ABI](https://github.com/ValveSoftware/Proton/blob/proton_11.0/lsteamclient/steamworks_sdk_165/steam_api.h#L202-L235)

### Select a publisher’s hosted OpenGL renderer

Use `opengl3` on the third line of the Win32 shadow profile with an empty runtime line and the signed, build-matched surfaceless profile. Declare Window and LoaderTrace, and use the source fingerprint and loader trace. Place the publisher’s renderer switch in the launch arguments file. For a title with embedded SDL3, the launcher selects its Windows video driver and loads the packaged OpenGL library by name. The Window adapter uses packaged SDL2 with its offscreen EGL driver for the actual context. `steam_shadow_wgl=hosted` records the selected profile. [SDL Windows OpenGL loader](https://github.com/libsdl-org/SDL/blob/release-3.4.0/src/video/windows/SDL_windowsopengl.c#L97-L157) · [Mesa D3D12 backend](https://docs.mesa3d.org/drivers/d3d12.html)
### Route the controller through hosted XInput

Two empty profile markers select the hosted XInput adapter, `XboxWin32XInput910.dll`, which reads the controller from the host mailbox. Both set `XBOX_WIN32_HOSTED_XINPUT=1`, so the loader trace also serves `xinput1_4.dll` and `xinput1_3.dll` lookups from the adapter, and the report records `steam_shadow_xinput=hosted-lookup`.

| Marker | Requires | Behavior |
| --- | --- | --- |
| `steam-win32-hosted-xinput-APPID.txt` | A matching hosted build, the source profile, the loader trace, and a declared `XboxWin32XInput910.dll` | Slot 0 follows the host pad's connection. SDL backend selection stays with the profile, for example `steam-win32-disable-sdl-directinput-APPID.txt`. Controller notifications can be added with `steam-win32-hosted-controller-notify-APPID.txt`. |
| `steam-win32-host-xinput-APPID.txt` | A matching hosted build and a Win32 graphics adapter (the DXGI marker or OpenGL) | The hosted pad is the title's only controller (`XBOX_WIN32_HOST_XINPUT=1`). Slot 0 stays attached for the whole session. SDL's WGI, GameInput, HIDAPI and RawInput joystick backends are off, Steam's controller ignore lists are withdrawn, DirectInput reports no devices, and Steam Input reports no controllers. Slay the Spire 2 uses this marker. |

### Override Steamworks calls for one title

A title's Steamworks calls reach `steam_api64.dll` through the loader trace's `GetProcAddress`. CoreCLR resolves a Steamworks.NET `DllImport` the same way, which is how Slay the Spire 2's `SteamAPI_ISteamInput_GetConnectedControllers` already gets the hosted answer above. Four empty profile markers add opt-in overrides at the same point. Each needs a source profile, the loader trace and a Steam Core artifact that knows the option. Staging refuses an older artifact. Both are off unless the marker is present, and removing the marker restores the native call on the next launch.

| Marker | Variable | Behavior |
| --- | --- | --- |
| `steam-win32-no-steam-relay-APPID.txt` | `XBOX_WIN32_STEAM_RELAY_STUB=1` | `SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess` resolves to a no-op. The report records `steam_shadow_steam_relay=stubbed`. The trace records `asset steam-relay-override=InitRelayNetworkAccess` when the export is resolved, and `asset steam-relay=stubbed call=N utc=…` for each of the first eight calls. |
| `steam-win32-steam-callback-trace-APPID.txt` | `XBOX_WIN32_STEAM_CALLBACK_TRACE=1` | `SteamAPI_RunCallbacks` is timed. The trace records `asset steam-run-callbacks=timed threshold_us=2000` once, then `asset steam-run-callbacks us=… utc=…` for every call of 2 ms or more (up to 4096 lines). `utc` is when the call returned, as `YYYYMMDDTHHMMSS.mmmZ`. The report records `steam_shadow_steam_callback_trace=loader-trace`. |
| `steam-win32-steam-relay-worker-APPID.txt` | `XBOX_WIN32_STEAM_RELAY_WORKER=1` | The real `SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess` still runs at start-up, on a new thread, and the caller returns at once. If the thread cannot be created, the call runs on the caller's thread. The report records `steam_shadow_steam_relay=worker`. The trace records `asset steam-relay-override=InitRelayNetworkAccess mode=worker` (or `mode=worker-status`) when the export is resolved. For each of the first eight calls it records `asset steam-relay=worker call=N utc=…` when the call is queued, and `asset steam-relay=worker-done call=N us=… utc=…` when the real call returns. A failed thread start records `worker-inline` instead. |
| `steam-win32-steam-relay-status-APPID.txt` | `XBOX_WIN32_STEAM_RELAY_STATUS=1` | After `InitRelayNetworkAccess` returns, a thread polls `SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus` every 250 ms until `SteamAPI_Shutdown`. The trace records `asset steam-relay-status=polling interval_ms=250 slow_us=2000` once. Each change is recorded as `asset steam-relay-status avail=A config=C relay=R ping=0\|1 init_ms=T utc=… msg=…`, up to 256 lines. `A`, `C` and `R` are `ESteamNetworkingAvailability` values for overall, network config and any relay, where `100` is `Current`, `3` is `Attempting`, `-10` is `Retrying` and `-101` is `Failed`. `ping` is 1 while ping measurement is in progress, and `init_ms` is the time since the first `InitRelayNetworkAccess` call. `msg` holds up to 48 characters of Steam's debug text, with other characters replaced by `_`. Each poll of 2 ms or more is recorded as `asset steam-relay-status-slow us=… utc=…` (up to 1024 lines). Without the worker, the real call stays on the caller's thread and is recorded as `asset steam-relay=traced call=N us=… utc=…`. The report records `steam_shadow_steam_relay_status=loader-trace`. |

`inspect-loader` lists these lines under `events.steam`. It summarizes relay status under `diagnostics.steam_relay`: the number of changes, whether and when `avail` reached 100, the latest value, and the slow polls.

When the worker or status option is set, `SteamAPI_Shutdown` is also wrapped. It stops both threads and waits up to 5 s for a call already in progress before it runs the real shutdown. The wait is recorded as `asset steam-relay=shutdown wait_us=…`, or `shutdown-busy` if the 5 s ran out. This prevents a thread from calling into a released interface. The stub cannot be combined with the worker or status options. Staging and the launcher both refuse that combination.

**The relay stub is a diagnostic switch, not a fix.** Multiplayer is part of a title's coverage, and Slay the Spire 2's co-op runs over Steam lobbies and Steam Datagram Relay. The stub moves relay start-up to the first P2P use, which slows the first co-op connection, and its effect on lobby hosting and joining has not been measured on Xbox. Use it only in an A/B trial to find out whether relay start-up causes a stall. No checked-in profile may carry the marker; `tests/test_big_picture_packaging.py` enforces this. A fix must leave `InitRelayNetworkAccess` running at start-up, and must be checked with a co-op session that hosts and joins through the relay.

**The relay worker is a candidate fix that keeps multiplayer.** It changes only which thread makes the start-up call. Relay start-up still begins at boot, so the first co-op connection is not delayed. The SDK's `isteamnetworkingutils.h` describes initialization as asynchronous: it "typically completes in a few seconds", and completion is reported through `GetRelayNetworkStatus` or the `SteamRelayNetworkStatus_t` callback. The caller therefore never relied on the work being finished when the call returned. Steamworks supports calls from more than one thread. Valve's [`steam_api` reference](https://partner.steamgames.com/doc/api/steam_api#SteamAPI_ReleaseCurrentThreadMemory) says most functions keep per-thread parameter memory, and tells programs that call from other threads to free it with `SteamAPI_ReleaseCurrentThreadMemory`. The worker and the poll thread do this. In Valve's open-source [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_lowlevel.h), most state is protected by a global lock, and the library's own service thread does its work under that lock. No Valve source states that `ISteamNetworkingUtils` in particular is thread-safe, and steamclient's relay code is closed. Whether the worker is safe on Xbox therefore has to be shown by a console run: relay status reaching `Current` with the worker, then a co-op session. If the stall is caused by another thread holding the networking lock, the worker cannot remove it. In that case `RunCallbacks` would still wait on the lock, and the status trace records slow polls at the same time.

**Why the relay stub preserves the contract.** In the Steamworks SDK, `ISteamNetworkingUtils::InitRelayNetworkAccess()` is an inline that calls `CheckPingDataUpToDate(1e10f)`. The flat export takes the interface pointer and returns `void`. Slay the Spire 2's `steam_api64.dll` (build 26655341, 300,392 bytes, SHA-256 `1add7f151fa644870a735ae86e68d1f019f296130d8e7c0a7ed3ecc7482dccbc`) assembles it at RVA `0x2390` as `mov rax,[rcx]; movss xmm1,1e10f; jmp [rax+0x38]`, a tail call to vtable slot 7, `CheckPingDataUpToDate`. Calling it only starts Steam Datagram Relay's configuration fetch and pings early. steamclient still starts them when the title first uses the relay, for example through P2P connections or `GetRelayNetworkStatus`. So the no-op keeps the `void` contract and moves the work to the first multiplayer use. The cost is a slower first P2P connection. Steamworks.NET binds only the flat exports, and `sts2.dll` calls `SteamNetworkingUtils.InitRelayNetworkAccess` once, from `SteamInitializer.Initialize`. The game makes no direct vtable call that would need patching. Patching slot 7 would also change `CheckPingDataUpToDate` for its other callers, so the stub stays on the flat export.

### Persist Direct3D 12 pipelines across launches

A Win32 trial with the loader trace can keep the driver's compiled Direct3D 12 pipelines between launches. Add `steam-win32-d3d12-pipeline-cache-APPID.txt` to the build-matched profile. The launcher creates `LocalState\D3D12PipelineCache` and passes `XBOX_WIN32_PSO_CACHE=<folder>\APPID-BUILD` and `XBOX_WIN32_PSO_CACHE_MODE`. `XboxWin32LoaderTrace.dll` wraps the shared `ID3D12Device` pipeline methods, including the device a hosted Mesa creates.

The marker's content selects the mode. The cache never probes the title's device to choose one.

| Marker | Mode |
| --- | --- |
| empty | `passthrough`: the hooks time pipeline creation (slots 10, 11 and 47 only, as the 1.0.242.1 timing build did) and the cache makes no call on the device. |
| `mode=blobs control=0x5CCXLBFF` | `GetCachedBlob` records supplied back through `CachedPSO`. |
| `mode=library control=0x5CCXLBFF` | An `ID3D12PipelineLibrary`. |
| `mode=replay` | Record and replay: the title's pipelines are recorded by content and pre-created on later launches with the title's own creation calls. No driver persistence API is called. |
| `mode=replay-idle` | The same, with the replay starting at the title's first idle period (loading quiet for 2 s, usually the main menu) instead of at once. |

A driver cache mode (`library` or `blobs`) must name a [pipeline-cache control](../probe/README.md#direct3d-12-pipeline-cache-control) result in which that path reached outcome 1 on the console: the pipeline was stored, reloaded in a second process and hit. Staging and the launcher refuse any other marker. A runtime without `XBOX_WIN32_PSO_CACHE_MODE` is refused as well, because it chose the library by calling `CreatePipelineLibrary` on the title's device. One mode never falls back to the other. A library that cannot be created, or blobs on a device without single-PSO support, leaves the launch in passthrough.

In a cache mode, each pipeline is keyed by a SHA-256 of its full desc, with the root signature, shaders and semantic names hashed by content. The file is `APPID-BUILD-IDENTITY.bin`. Its identity covers the adapter, the user-mode driver version, the OS version and `d3d12.dll`. A changed driver, a corrupt file or a rejected library is deleted and rebuilt. Writes go through a temporary file. They happen when creation goes quiet, right after any pipeline that took 500 ms or more, and at exit.

**Device safety.** `ID3D12Device::GetDeviceRemovedReason` is checked after every call the cache makes into the runtime: `GetAdapterLuid`, `CheckFeatureSupport`, `QueryInterface`, `CreatePipelineLibrary`, `StorePipeline`, `Load*Pipeline`, `Serialize`, `GetCachedBlob` and creation with `CachedPSO`. The first call of each kind in a process writes `asset pso-cache call=NAME phase=before` and `phase=after result=…`. It also writes `APPID-BUILD.guard`, which is deleted when the call returns with the device intact. A removal writes `asset pso-cache device-removed after=NAME result=… reason=… last-cache-call=NAME`, then a `dred` summary. The summary has breadcrumb nodes, the first incomplete operation and the page-fault address. DRED data is only present when it was enabled before the device was created. The removal leaves the guard naming the call and stops the cache for the rest of the process. In a cache mode, the title's own creation reporting a removal is recorded the same way, as `after=game-create`. Passthrough makes no call on the device even then: it writes only `asset pso-cache device-removed after=game-create result=…` with the HRESULT the title received. A guard for the same identity puts later launches in passthrough (`asset pso-cache open … guard=NAME`). The guard also covers a process that the runtime ends inside a call. A driver, OS or game build change clears it; otherwise delete the guard from `LocalState\D3D12PipelineCache` to retry.

Every hooked call returns what the driver returns for the original desc; a miss passes the title's desc unchanged. Any other failure in the cache path falls back to the original call with one `asset pso-cache fallback op=...` line. Each run records `asset pso-cache hooks`, `open`, `bind`, `write` and `stats` lines. The stats lines give hits, misses, stores and load and create milliseconds; passthrough reports its creations as misses. The exit line (`asset pso-cache stats at=exit`) is written when the title calls a bound `ExitProcess` or `TerminateProcess` import. Slay the Spire 2 (Godot with .NET) ends through its runtimes' own `ExitProcess` imports, so neither wrapper runs and passthrough writes no exit line. Giving the loader-trace adapter an entry point to write the line at process detach (89e7708) made every Slay the Spire 2 launch exit with `0xC000007B` before its first frame on Xbox, so the adapters again link with `/noentry` and the exit line stays missing for such titles. Each pipeline that takes 2 ms or more also gets a `pso kind=... cache=hit|miss|...` line in the raw trace. The launch report records `steam_shadow_pipeline_cache_mode`.

#### Record and replay

The library and cached-blob paths remove the device on the Series X driver (control `0x5CC4AA63`, OS 26100.9673), and the driver reports no automatic in-process or disk shader cache (`D3D12_SHADER_CACHE_SUPPORT` `0x63`). `mode=replay` warms pipelines without them. It needs no control result because it calls only `CreateRootSignature`, `CreateGraphicsPipelineState`, `CreateComputePipelineState` and `ID3D12Device2::CreatePipelineState`, which the title already calls on the same device. It still uses the guard. The [replay control](../probe/README.md#record-and-replay-control) proves the path in two processes before a title uses it. Staging refuses the marker with a runtime that does not know the mode, because such a runtime would read it as passthrough.

- **Record.** Each pipeline the title compiles in 1 ms or more is serialized by content to the same `APPID-BUILD-IDENTITY.bin` (file mode 3), as a graph of chunks: the desc, every shader's bytecode, the input layout and its semantic names, stream-output declarations, and the root signature by its blob. Pointer slots are stored as fixups, so the file holds no process address. Each record also stores the title's creation order, the time since its first creation, its loading phase (a new phase starts after 2 s without a creation) and how long the title took. A record the title has not asked for in 8 launches is dropped. The file is limited to 2048 pipelines, 512 root signatures and 64 MiB.
- **Replay.** Once the title's device is known (its first root signature or pipeline) and the file is read, one below-normal-priority thread rebuilds each desc, keys it again and requires the recorded key before it creates the pipeline. It follows the title's recorded order (loading phase, then time since the first creation). Because it starts at the title's first root signature, it runs ahead of the title through the startup burst: the driver compiles the next pipeline on the replay thread while Mesa translates shaders on the title's thread. Later phases (the menu, the first combat) are built long before they are reached. The root signature is the title's own object for that blob when the title has created it. Otherwise the replay creates one from the recorded blob.
- **Serve.** A title creation whose key matches a pre-created pipeline on the same device receives that pipeline, but only when its desc names the same root-signature object the pipeline was created with. The cache calls `QueryInterface` to the title's `riid` and releases its own reference, so the title holds the only reference, as after a fresh creation. The object never had a name or private data set. A null `ppPipelineState`, a desc with a `CachedPSO` blob, an unknown stream subobject and a second creation of one desc all go to the driver unchanged.
  - **Pipeline in progress.** If the replay is building that pipeline, the title waits for it, and the replay thread is raised to the title thread's priority. The rest of a creation already under way takes no longer than a fresh one, unless the driver serializes creations internally (see the `concurrent` counters).
  - **Pipeline not started.** The title compiles it and the replay skips it. No desc is compiled twice at once.
- **Root-signature identity.** D3D12 requires the root signature bound at draw time to match the PSO's, and documents that match by layout. Serving only on object identity is stricter, and it is the condition that holds without relying on that. If the runtime returns the replay's object when the title later creates the same blob (`roots same=`), every pre-created pipeline can be served. If it returns a new object (`roots distinct=`), the affected pipelines are rebuilt with the title's object, and the replay waits for the title's roots from then on. [Using a root signature](https://learn.microsoft.com/windows/win32/direct3d12/using-a-root-signature) · [vkd3d-proton root-signature compatibility by content hash](https://github.com/HansKristian-Work/vkd3d-proton/blob/71cae091b06571e51446cf2a937f0b04c237c04d/libs/vkd3d/vkd3d_private.h#L2158-L2172)
- **Memory.** Pre-created pipelines the title has not taken are released 10 minutes after the replay starts, with `asset pso-replay release unused=N commit-freed-kib=N`. Records stay in memory for the next write, bounded by the file limits. All held references, including the title's root signatures and the device, are dropped at release.
- **Device safety.** The whole replay pass, from the first creation until nothing is left to build, runs under the guard with call `replay-create`. Own root signatures use `replay-root`. A removal reported after a replay creation, or by the title's own creation, stops the replay, records `device-removed after=replay-create|game-create` with a DRED summary and keeps the guard, so later launches with the same identity run in passthrough. A normal exit during the pass deletes the guard when the device is intact.

Trace lines: `asset pso-replay load pipelines=N late=N roots=N`, `start records=N start=immediate|idle wait-ms=N`, `end built=N failed=N bad=N deferred=N own-roots=N build-ms=N elapsed-ms=N hits=N early=N`, `release …`, and three `stats|ms|roots at=…` lines. The `stats` line reports hits, wait-hits, early, root-mismatch, recorded and released. The `ms` line reports build, hit, wait, saved, miss, concurrent and concurrent-ms. The `roots` line reports same, distinct, own, requeued, deferred, repeats and unrecorded. Each pipeline that takes 2 ms or more is traced as `pso kind=… cache=hit|wait-hit|miss|early|root-mismatch|expired|repeat|…`. `python3 -m tools.xbox_deploy inspect-loader` summarizes them by outcome under `pipelines.outcomes`.

Record and replay removes the driver's share of first-use stalls: the `CreateGraphicsPipelineState` and `CreatePipelineState` time. Mesa's GLSL → NIR → DXIL translation runs before that call and still happens on every launch.

On Slay the Spire 2 (Steam build 26655341, OpenGL route, `1.0.246.1`), the first build of this cache probed `CreatePipelineLibrary` on Mesa's device at the first pipeline. The call returned `DXGI_ERROR_DEVICE_REMOVED`, although the device reported library support (`0x63`), and the game exited with that code. The pipeline-cache control later confirmed that both driver paths remove the device (`0x5CC4AA63`), so the profile now selects `mode=replay`. [ID3D12PipelineLibrary](https://learn.microsoft.com/windows/win32/api/d3d12/nn-d3d12-id3d12pipelinelibrary) · [Cached PSOs](https://learn.microsoft.com/windows/win32/api/d3d12/ns-d3d12-d3d12_cached_pipeline_state)

### Cap the video memory budget Mesa trims against

Mesa's d3d12 driver keeps allocations resident until its usage passes 70% of the local plus non-local `Budget` from `IDXGIAdapter3::QueryVideoMemoryInfo`, and evicts to make room only once usage reaches it. On Xbox, the local budget is sized from the process memory limit (Factorio logged 4147 MB on a Series X). The GPU-visible memory is far smaller: Device Portal reports a 512 MiB dedicated carve-out and 832 MiB of shared system memory for the whole partition, which Big Picture also renders into. Mesa therefore never trims. During a Slay the Spire 2 session, dedicated memory reached 99.97% after about 30 minutes, and the frames flickered white. [Mesa d3d12 residency](https://github.com/XboxEmulationHub/mesa-uwp/blob/34732bbc207daea10bdf2fb969a4a0f7769ea1de/src/gallium/drivers/d3d12/d3d12_residency.cpp) · [QueryVideoMemoryInfo](https://learn.microsoft.com/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo)

Every hosted WGL title (the Mesa route) now installs a cap when its first GL context is created. `XboxWin32Dxgi.dll` replaces `QueryVideoMemoryInfo` in the native adapter's method table, which also covers the adapter Mesa obtains from the system `dxgi.dll`. The budget it reports is the lower of the native budget and half of the adapter's `DedicatedVideoMemory + SharedSystemMemory`. Usage, reservations and every other adapter method stay native. If native usage ever exceeds that capacity, the usage figure counts more than GPU memory, and the budget passes through unchanged for the rest of the process. To override the cap, put `xbox-gpu-memory-budget.txt` in the package root. It holds either `native` (no cap) or a cap in MiB from 64 to 1048576.

Trace lines: `dxgi gpu-memory-budget install result=… dedicated_mib=… shared_mib=… capacity_mib=… cap_mib=… source=default|override`, the first values Mesa receives as `dxgi gpu-memory-first local_budget_mib=… local_used_mib=… reservable_mib=… nonlocal_budget_mib=… nonlocal_used_mib=… budget_mib=… mode=capped|native`, and every 60 seconds `dxgi gpu-memory local_used_mib=… nonlocal_used_mib=… budget_mib=… native_budget_mib=… peak_mib=… queries=… t_s=… utc=…`. `inspect-loader` lists them under `events.gpu-memory`. The wrapper also records each `IDXGIAdapter3::RegisterVideoMemoryBudgetChangeNotificationEvent` call as `dxgi gpu-memory-budget-event register result=… cookie=… count=… utc=…`. Mesa at `34732bb` makes no such call.

### Count and pin Mesa's residency changes

Mesa's d3d12 residency manager calls `ID3D12Device::Evict` on least recently used allocations when usage passes 70% of the budget, or when an `ID3D12Device15` trim notification asks it to. It calls `ID3D12Device3::EnqueueMakeResident` before a batch uses an evicted allocation. On a device with `ID3D12Device8`, Mesa also creates textures and buffers with `D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT`, so that first `EnqueueMakeResident` is what makes them resident. [Mesa d3d12 residency](https://github.com/XboxEmulationHub/mesa-uwp/blob/34732bbc207daea10bdf2fb969a4a0f7769ea1de/src/gallium/drivers/d3d12/d3d12_residency.cpp) · [Not-resident creation](https://github.com/XboxEmulationHub/mesa-uwp/blob/34732bbc207daea10bdf2fb969a4a0f7769ea1de/src/gallium/drivers/d3d12/d3d12_resource.cpp#L366-L368) · [MakeResident](https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12device-makeresident) · [EnqueueMakeResident](https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12device3-enqueuemakeresident)

`XboxWin32LoaderTrace.dll` wraps the shared device method table: `MakeResident` (slot 34), `Evict` (35), `EnqueueMakeResident` (50), and `RegisterTrimNotificationCallback` (83) when the runtime implements `ID3D12Device15`. The slot numbers are counted from `d3d12.idl`. To select a mode, put `xbox-d3d12-residency.txt` in the package root:

| File | Behavior |
| --- | --- |
| absent or empty | `native`. The calls are counted only when the pipeline hooks already create a device of their own, as the Slay the Spire 2 profile does. |
| `native` | Every call goes to the driver and is counted. |
| `pin` | `Evict` returns `S_OK` without reaching the driver, so nothing is ever evicted. `MakeResident` and `EnqueueMakeResident` still go to the driver, which signals the requested fence value itself. |
| `off` | No residency hooks. |

`pin` cannot skip the make-resident calls, because a `CREATE_NOT_RESIDENT` allocation would then never become resident. On an allocation that a pinned `Evict` left resident, the driver call only raises its residency count. No hook changes a COM reference count, return value or last-error value.

Under a budget that binds, `pin` makes Mesa stall. Usage never falls, so Mesa's `evict_to_fence_or_budget` keeps evicting (now a no-op) and waits on the CPU for older batches. Under `pin`, use a budget that does not bind. To vary memory pressure, use the cap in `native` mode.

Trace lines:
- At start: `asset residency mode=native|pin|off source=default|package-file hooks=pending|none|off`.
- After the hooks install: `asset residency install result=… slots=34-35-50[-83] device3=… device15=… mode=…`.
- For each second with at least one call: `asset residency evict=… makeresident=… enqueue=… evict_obj=… evict_kib=… res_obj=… res_kib=… restored=… [trim=…] [failed=…] utc=…`. The `evict`, `makeresident` and `enqueue` fields count calls. `res_*` covers both make-resident calls. `restored` counts objects made resident again after an `Evict` of the same address, which is a paging round trip in `native` mode.
- Every 60 s: `asset residency-total mode=… t_s=… …` with the totals.
- `asset residency trim flags=… bytes_kib=…` and `asset residency trim-register result=…`.

`inspect-loader` lists all of these under `events.residency`.

### Warm a CoreCLR title's managed code before first use

An IL-only game assembly is JIT-compiled the first time each method runs, on every launch. The first card play, attack or save then compiles new code on the game thread. Add `steam-win32-managed-warmup-APPID.txt` to the trial profile to select [`runtime/managed-warmup`](../runtime/managed-warmup). The component is packaged as `XboxManagedWarmup.dll`. It prepares the listed assemblies' methods on a below-normal-priority background thread with `RuntimeHelpers.PrepareMethod`. The publisher's files are not changed. [Slay the Spire 2 plan](../profiles/2868840/26655341/steam-win32-managed-warmup-2868840.txt)

```text
# Comment lines are dropped at staging.
assembly=sts2 mvid=a5742661-f95f-4ba1-8b21-18dedf86422b
priority=MegaCrit.Sts2.Core.GameActions
remaining=yes
depth=4
```

| Line | Meaning |
| --- | --- |
| `assembly=NAME [mvid=GUID]` | One to eight game assemblies, matched by simple name in any load context. With an MVID pin, a rebuilt assembly is skipped and logged. ReadyToRun and mixed-mode images are skipped. |
| `priority=PREFIX` | Up to 32 namespace or type prefixes, warmed first in this order. |
| `remaining=yes\|no` | Whether the rest of each assembly follows the priorities. The default is `yes`. |
| `depth=0..8` | How far to follow calls from prepared code to the generic instantiations it needs, such as async state-machine boxes and collections over the game's value types, and to non-generic methods in other IL-only assemblies such as GodotSharp. The default is `4`. |
| `collect=after` | Optional, off by default. Once the warm-up completes, the warm-up thread runs one `GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true)`. On Slay the Spire 2 that is 22–34 s after the first frame, while the menu loads. A blocking collection suspends every managed thread, including the game thread, for its duration. The large object heap is not compacted. A staged plan with this line needs a component that supports it, because an older one rejects the whole plan. |

**How it is started.** CoreCLR runs `DOTNET_STARTUP_HOOKS` only from `Assembly::ExecuteMainMethod`. Godot .NET never calls `hostfxr_run_app`. It initializes hostfxr for the game assembly and loads `GodotPlugins.Game.Main` through `load_assembly_and_get_function_pointer`, so a startup hook never runs. When the plan is selected, the launcher sets `XBOX_WIN32_MANAGED_WARMUP`. The adapter that serves the engine's `GetProcAddress` then wraps `hostfxr_get_runtime_delegate`: `XboxWin32Window.dll` on the OpenGL route and `XboxWin32LoaderTrace.dll` on Direct3D 12. After the engine's first successful load, the same delegate loads `XboxManagedWarmup.Entry.Start`. Titles started through apphost or `dotnet` run the same assembly as a startup hook. Only one entry starts per process. [CoreCLR startup hooks](https://github.com/dotnet/runtime/blob/v9.0.7/src/coreclr/vm/assembly.cpp) · [Godot .NET hosting](https://github.com/godotengine/godot/blob/4.5.1-stable/modules/mono/mono_gd/gd_mono.cpp)

**Why it is safe.** `PrepareMethod` goes through the same prestub as a first call (`MethodDesc::DoPrestub`). Each method therefore gets the tier-0 code its first call would have produced, and tiered promotion is unchanged. In .NET 9, `CEEInfo::initClass` returns `CORINFO_INITCLASS_USE_HELPER` instead of running a class constructor during JIT, so preparation does not run game code. Methods are prepared directly only in IL-only images. Reference-type instantiations from ReadyToRun assemblies are skipped, because their shared code is precompiled and binding it would resolve ReadyToRun fixups. [initClass](https://github.com/dotnet/runtime/blob/v9.0.7/src/coreclr/vm/jitinterface.cpp) · [PrepareMethod](https://github.com/dotnet/runtime/blob/v9.0.7/src/coreclr/vm/reflectioninvocation.cpp)

**Diagnostics.** The launch report records `steam_shadow_managed_warmup=selected`, and the loader trace records `managed-warmup bootstrap=hostfxr load=0x… entry=0x…`. A zero `load` and `entry` mean the warm-up thread started. The component appends to `LocalState\managed-warmup-APPID.log`:

- the entry path (`via=hostfxr` or `via=startup-hook`);
- for each assembly, its MVID and image kind;
- a UTC-stamped line as each priority group finishes;
- totals for roots, instantiations, cross-assembly callees and failures by exception type;
- `managed_warmup=complete gen0=… gen1=… gen2=… utc=…`, with the runtime's collection counts so far;
- with `collect=after`, `managed_warmup_collect=after generation=2 mode=forced blocking=yes compacting=yes ms=… pause_ms=… heap_kib_before=… heap_kib_after=… gen0=… gen1=… gen2=… latency=… server=… start=… utc=…`. `ms` is the wall time of the call and `pause_ms` is the runtime's own suspension time from `GC.GetTotalPauseDuration`, which is present from .NET 7 on. `latency=Interactive` means concurrent (background) GC is enabled.

**Desktop check.** `runtime/managed-warmup/check` runs the shipped assembly against an IL-only fixture loaded into an isolated `AssemblyLoadContext`. It counts `MethodJittingStarted` events for the fixture's hot path. Expect a positive control, zero events after the warm-up through both entries, no type initializer before first use, and a refused MVID pin. A fourth child runs the plan with `collect=after` and expects exactly one `managed_warmup_collect` line after `managed_warmup=complete`. The default plan must log none. The `Build Xbox probe` workflow runs it. [Workflow step](../.github/workflows/xbox-probe.yml)

**Alternatives considered.**

| Option | Result |
| --- | --- |
| ReadyToRun (crossgen2) for the game assembly | Precompiles only the game's own version bubble. Instantiations of framework generics over the game's types still JIT, and on STS2's first turn those were about a third of main-thread JIT. It also needs the game's full reference set at build time, and it puts a rebuilt publisher assembly in the package. Running crossgen2 on the console from the player's own copy would avoid redistribution but is untested. |
| Multicore JIT (`DOTNET_MultiCoreJitProfile` or `ProfileOptimization`) | CoreCLR 9 records at most `0x4000` methods per profile, including ReadyToRun method loads. Its player stops 60 s after start (`MULTICOREJITLIFE`), and the environment variable path uses the default binder. A Godot game assembly lives in an isolated load context, and its first combat comes minutes after start. [multicorejitimpl.h](https://github.com/dotnet/runtime/blob/v9.0.7/src/coreclr/vm/multicorejitimpl.h) |
| `DOTNET_TieredCompilation=0` | Every first call becomes a fully optimized JIT, so first-use stalls get longer. |
| `DOTNET_TieredPGO=0`, `TC_CallCountThreshold`, `TC_CallCountingDelayMs`, `TC_BackgroundWorkerTimeoutMs`, `OSR_*` | These change background promotion or loop transitions, not first-call JIT. `TieredPGO=0` removes the instrumented tier's background rejits at some cost to steady-state code. Measure it separately if CPU contention shows up. |

The warm-up compiles methods a session may never call. Memory and CPU are recorded but bounded only by the plan, so use `remaining=no` to restrict a title to its priorities. While the warm-up is compiling, tiered compilation's call-counting delay stays active. Tier-1 promotion of the game's hot code therefore waits until the warm-up finishes.

## Hosted titles

Register an adapter only when a bundled runtime can execute the verified installed format. An adapter declares its AppID, backend, native authorization method, success marker, and start function. The dispatcher opens a hosted runtime only after that method verifies the selected install. A refused or failed authorization returns to the library with its diagnostic.

The SDL player fills the Xbox viewport and fits the game's complete image at its original aspect ratio. Hold Menu and View together to open the player menu. Use the D-pad and A to choose Resume or Quit to library. The game keeps its regular Menu and View actions when each button is pressed individually.

A hosted game with keyboard support receives keys through the player. To type with a controller, focus the game’s text field, hold Menu and View, and select **Keyboard**. Enter text with the Xbox keyboard, then choose **Send text**. A primary native panel with the hosted keyboard opt-in resumes first, then sends the text after the presenter clears held buttons. Use the game’s confirmation control to continue. Select **Steam** in the game controls to open Big Picture.

For DOS, the native component allowlists the exact supported AppID and directory. The fixed profile in `signin/xcom-dos.js` records the publisher's DOS mount, startup command, supported configuration and file exclusions. The host handles file copying, process lifetime, return to the library, and optional local trial snapshots. When adding a DOS title, verify its owned depot, define a separate fixed profile, and extend the native allowlist to that exact title and install path. Keep publication of the runtime separate from the user's game files. Input and save persistence also need their own validation for each supported backend.

## Verification

Run these from the repository root. GitHub Actions runs the same tests without Steam credentials or game files.

```sh
uv run --locked python -m pytest
node --test tests/*.test.js tests/big-picture-webview/*.test.js
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

The integrated Big Picture build stages the MonoGame startup hook from the checksum-pinned Steam Core APPX when that artifact includes it. Supply the verified SDL 2.30.9 build with `--shared-sdl ARTIFACT DLL_SHA256`.

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
mkdir -p "$runtime_dir"
javac --release 8 -d "$runtime_dir" runtime/lwjgl/SlayConsoleDiagnostic.java
python3 runtime/lwjgl/build-lwjgl-sdl.py \
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
python3 -m tools.measure fhd-present-times.bin --start-utc "$scene_start_plus_eight_seconds" --duration 30
```

The exchange never shows a frame the GPU has not finished writing. The game's GL renderbuffer is never shared. Mesa's `MesaGLInteropEGLFlushObjects` moves it to `COMMON`, and `glFlush` submits the game's work on Mesa's graphics queue. The producer then records a copy into one of two exchange textures on that same queue and signals that slot's shared `ready` fence after the copy ([`sdl-gpu-producer.cpp`](../runtime/sdl/sdl-gpu-producer.cpp)). Because the published sequence number can arrive before the copy completes, the player draws a slot only when `ready` has reached its sequence. It also queues a GPU `Wait` on that fence before sampling, then signals the slot's `done` fence after its draw ([`gpu-receiver.cpp`](../runtime/presenter/gpu-receiver.cpp)). The producer reuses a slot only after both fences have passed its previous sequence. While the other slot is still held, it drops the frame rather than overwrite a texture the player may be reading. The DXGI publisher follows the same protocol ([`dxgi-frame.cpp`](../runtime/win32/dxgi-frame.cpp)).

The panel status and the hosted session report include exchange counters:

- `gpu_wait_producer` counts polls that found a published frame whose copy had not completed. Each such frame was left for a later refresh.
- `gpu_held` counts polls that kept a newer completed frame for the next refresh.
- `gpu_released_unshown` counts frames acknowledged without being drawn.
- `gpu_drawn` counts frames drawn after the fence wait.
- `gpu_ready_fence` and `gpu_done_fence` give the last drawn slot's completed producer fence and the consumer value signalled for it. `gpu_ready_fence` is always at least `gpu_done_fence`.

The current GPU receiver checks the presenter's DXGI adapter flags and confirms that its adapter LUID matches the game's shared textures. The fullscreen and primary XAML presenters write version 2 frame traces. Use `python3 -m tools.measure --require-gpu-60` to require a scene of at least 30 seconds, exclusively hardware shared-texture frames, an average of at least 59.5 FPS, and at least 57 distinct frames in each rolling second. The average threshold accommodates nominal 60 Hz displays such as 59.94 Hz. Supply observed `--internal-resolution` and `--output-resolution` as `WIDTHxHEIGHT`. The trace measures cadence and transport; the game renderer and display settings establish the two dimensions. Record the observed gameplay sample as `performance_scene: "gameplay"` and include its measured summary as `performance`. The compatibility coverage tool's `--require-gpu-60` check requires both dimensions to be 1920×1080. [DXGI adapter flags](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_adapter_flag)

The summary's `startup` values report the time from trace open to the first present and the longest interval ending before the measured scene. Use the launch observer for stalls before trace open. Record any deliberate engine caps and fixed internal resolutions alongside the cadence results.

Series X build `1.0.230.54` ran Slay the Spire through the primary XAML presenter at 1920 × 1080. The first 30-second window, beginning five seconds after the first frame, averaged 57.40 FPS with a 1.25-second startup stall. The immediately following 30-second title window submitted 1,798 hardware frames at 59.93 FPS. Its lowest rolling second was 59 FPS, its 95th-percentile frame interval was 16.76 ms, and every frame used a shared texture. This establishes the updated hardware path and title pacing for the original Slay the Spire.

### Hosted Direct3D 11 pacing experiment

Build the same `XboxWin32Dxgi.dll` into each signed package with the title's hosted Direct3D 11 profile and `fhd-measure.txt` at package root. Select the optional settings by adding empty package-root markers before packing and signing.

| Package-root markers in addition to `fhd-measure.txt` | Producer timing |
| --- | --- |
| None | Publish the GPU texture before waiting on the existing 60 Hz clock. |
| `fhd-paced-publish.txt` | For an established hosted swap chain presenting at interval 1 with no DXGI flags, wait before copying and publishing the GPU texture. |
| `fhd-refresh-5994.txt` | Use the 60000/1001 Hz period when the producer honors a synchronized interval. Use this variant on a display measured near 59.94 Hz. |
| Both | Wait before copying and publishing at the 60000/1001 Hz cadence. |

Keep the game build, resolution, display setting, and gameplay scene fixed. Compare distinct GPU frames and frame intervals in the same settled 30-second scene using `python3 -m tools.measure --require-gpu-60`. With the loader trace enabled, `dxgi pacing-before-publish` confirms the reorder ran and `dxgi pacing-rate-millihz` reports 59940 as hexadecimal `0x0000EA24`. Compare the producer's `attempts`, `submitted`, `slot-busy`, `present-rejected`, and `max-gap-ms` with the panel report's visible render callbacks, source-empty callbacks, and presentation-busy callbacks. A large producer gap with few busy slots still needs timing for the game's work and the underlying DXGI call. A visibility pause can stop the panel from retiring textures and make both slots busy even while the game keeps presenting. The standard package uses the existing 60 Hz path unless measurement and an experiment marker are present.

For Hades, time a 40–45-second block of movement and combat after the room settles. Record the start and end with a clock checked against the Xbox. Use the same central 30 seconds in both traces with `python3 -m tools.measure --start-utc "$scene_start_plus_five_seconds" --duration 30 --require-gpu-60`. Let the game keep presenting for a few seconds after the block so the last buffered records are available. Collect the app's frame file and producer trace before installing the next build. The automatic first-frame sampling window can include menus and loading.

Add `--diagnose-stalls --producer-log loader-trace.txt` to correlate the longest gaps with game calls, GPU publication, software pacing and native Present. The producer log can also come from the same run's saved launch report after normal exit. Use both records from the same game process and scene. A gap without a matching producer phase remains unattributed.
