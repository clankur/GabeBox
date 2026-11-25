# Slay the Spire 2 on Xbox Series X

Slay the Spire 2 is Steam AppID `2868840`. The audited Windows build is Steam build `26655341`, publisher release `v0.111.1` (commit `cc1f9462`, dated 2026-11-13). The game runs on MegaDot v4.5.1.m.14, a fork of Godot 4.5, with a bundled .NET 9 runtime in `data_sts2_windows_x86_64`. The executable is `SlayTheSpire2.exe` (894,748,672 bytes, SHA-256 `8602c26bffd2937e3841835fd8360ef8e974624a543e05977229fd3d062be231`). It carries full DWARF debug info, so `x86_64-w64-mingw32-addr2line -f -C -e SlayTheSpire2.exe 0x140000000+RVA` symbolizes fault addresses offline. The game data is `SlayTheSpire2.pck`.

## Current result

Tested on Xbox Series X, OS `26100.9673`, with SteamCore `1.0.232.680` and the build-pinned OpenGL profile in [`profiles/2868840/26655341`](../profiles/2868840/26655341). Big Picture **Play** launched the game at 03:48:00 local on 2026-11-25. All input came from Device Portal remote input through [`tools/xbox_remote_input.py`](../tools/xbox_remote_input.py), with no physical controller. These results were observed:

- The game initialized Steamworks with the signed-in account and reported `Steam is running: True`.
- It logged `CONTROLLER DETECTED: XInput Controller` and switched its prompts to Xbox glyphs.
- **Y** passed the Early Access screen and **A** confirmed The Ironclad. **B** declined tutorials, and the game logged `Embarking on a singleplayer IRONCLAD run. Seed: ZLDW87W2UKCQ`.
- **A** entered the first map node. The log then recorded `Creating NCombatRoom with mode=ActiveCombat encounter=NIBBITS_WEAK`.
- Ten turns were played. The log recorded cards played (`STRIKE_IRONCLAD`, `DEFEND_IRONCLAD`) and monster moves (`BUTT_MOVE`, `SLICE_MOVE`, `HISS_MOVE`). The enemy fell to 20/45 HP and the player to 24/80.
- The run saved locally (`current_run.save`, 39,829 bytes). Steam Cloud writes failed (see open items).
- Screenshot SHA-256: character select `9bee081a…`, map `aad8385b…`, combat `4476f604…`, settled combat `412097e8…`.

A settled 30-second combat window starting `2026-11-25T08:56:02Z` passed `python3 -m tools.measure --require-gpu-60 --internal-resolution 1920x1080 --output-resolution 1920x1080` (`gpu_60fps: true`, trace SHA-256 `df87b242…`). The scene showed the combat idling between turns with ambient animation:

| Frames | Average | Lowest rolling second | p99 | Worst | > 33 ms | Transport |
| --- | --- | --- | --- | --- | --- | --- |
| 1,799 | **59.967 FPS** | **59** | 16.888 ms | 17.246 ms | 0 | 1,799 shared GPU textures, 0 CPU uploads, hardware adapter verified |

The active-play windows recorded before that sample were:

| Window start (UTC) | Average | Lowest rolling second | Largest gap | Cause |
| --- | --- | --- | --- | --- |
| `08:51:25` | 57.53 FPS | 29 | 517 ms | first card play and first enemy attack |
| `08:52:58` | 59.63 FPS | 50 | 134 ms | new enemy move |
| `08:54:23` | 59.53 FPS | 51 | 150 ms | first `strength_power` icon load |

The log recorded each new asset as `Asset not cached`. Every gap advanced the producer by one frame, so the game itself paused while loading. Steady-state p99 was 16.9 ms in all windows. The game caps itself at 60 FPS (`fps_limit: 60`).

### Remaining open items on this build

- **Card art and character portraits render white.** Both appear in the screenshots as plain white panels. The card frames, text, UI and combat scene render correctly. The compressed atlas or the texture format path through Mesa's OpenGL driver is the first suspect.
- **Steam Cloud writes fail.** `k_EResultIOFailure` comes back from `ISteamRemoteStorage`. The game keeps the local file. Cloud upload needs the Steam client's remote storage path on Xbox.
- **Return to library through the overlay.** A View + Menu chord followed by Down + A closed the Big Picture webview and left the game running. Device Portal screenshots of the overlay return HTTP 500, so the overlay state was not observed. The Play → overlay → exit cycle is open for all titles, as the README records.
- **Save and reload.** Local writes succeeded. Reloading the run after a restart has not been observed.
- **Audio.** FMOD initialized and no audio errors were logged. Audible output has not been checked.
- **Godot shader cache.** On this route the cache persists poorly, so the first use of each effect stalls for up to 0.5 s on every launch.

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
| `1.0.232.680` | OpenGL via Mesa | Controller detected; run started; combat played; 60 FPS gate passed in combat | — |

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

[`opengl3-bindings.json`](../profiles/2868840/26655341/opengl3-bindings.json) redirects 57 imports to shared adapters:
- Window, focus, cursor, message-pump and display queries go to the window and hosted-display adapters.
- WGL goes to the window adapter.
- `CreateDIBSection` and `DeleteObject` go to the window adapter.
- Storage tracing stays as before.
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

STS2 now runs gameplay at a sustained 60 FPS at 1920 × 1080 on shared GPU textures, driven by controller input. It is not yet counted as covered. Per AGENTS.md, coverage also needs audio, save and reload, and return to the library observed on build `26655341`, plus the card-art rendering fix.
