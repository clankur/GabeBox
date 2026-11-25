# Slay the Spire 2 on Xbox Series X

Slay the Spire 2 is Steam AppID `2868840`. The audited Windows build is Steam build `26655341`, publisher release `v0.111.1` (commit `cc1f9462`, dated 2026-11-13). The game runs on MegaDot v4.5.1.m.14, a fork of Godot 4.5, with a bundled .NET 9 runtime in `data_sts2_windows_x86_64`. The executable is `SlayTheSpire2.exe` (894,748,672 bytes, SHA-256 `8602c26bffd2937e3841835fd8360ef8e974624a543e05977229fd3d062be231`). It carries full DWARF debug info, so `x86_64-w64-mingw32-addr2line -f -C -e SlayTheSpire2.exe 0x140000000+RVA` symbolizes fault addresses offline. The game data is `SlayTheSpire2.pck`.

## Current result

Tested on Xbox Series X, OS `26100.9673`, with SteamCore `1.0.232.200` and the build-pinned OpenGL profile in [`profiles/2868840/26655341`](../profiles/2868840/26655341). Big Picture **Play** started the game in run `1795588…` (launched 02:17:46 local). These results were observed:

- The game initialized Steamworks with the signed-in account and reported `Steam is running: True`.
- It loaded its localization, model cache, atlases and save system, and synced Steam Cloud saves into the trial profile.
- It displayed its first-run **What's Early Access?** screen at 1920 × 1080 for the full four-minute observation.
- The presenter received 2,701 GPU frames and no CPU uploads after the first startup frame.

A settled 30-second window on that screen passed `python3 -m tools.measure --require-gpu-60 --internal-resolution 1920x1080 --output-resolution 1920x1080`. It held 1,798 distinct shared-texture presents at **59.933 FPS**, with **59 frames in the lowest rolling second**, a worst interval of 17.04 ms, zero intervals over 33.3 ms and zero CPU uploads. The game's own settings cap it at 60 FPS (`fps_limit: 60`, `vsync: adaptive`). This is a menu sample, not gameplay; see the pending checks below.

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

### WER and the "suspended host"

The earlier OpenGL trials recorded SteamPrimary "suspending" seconds after the game started. Process sampling at one-second intervals on this build showed the cause. A second `SlayTheSpire2.exe` with `IsRunning: false` and a 36 KB working set appeared together with two `WerFault.exe` processes. That process is Windows Error Reporting's reflection clone of a crashing process. The Xbox then sends the app Home, and its lifecycle log records `suspending`. With the publisher's Sentry crash handler staged, Sentry's native init raised a breakpoint in `libsentry.windows.release.x86_64.dll+0xC6557` while starting `crashpad_handler.exe`. The profile no longer stages `crashpad_handler.exe` or `crashpad_wer.dll`. Sentry then logs `backend disabled` and the game continues. Publisher crash reporting is unavailable in this configuration.

### Steam detection

Steamworks initialized through the private discovery hive, but `SteamAPI_IsSteamRunning` reads `HKCU\Software\Valve\Steam\ActiveProcess` on every call. The adapter restored HKCU right after initialization, so the game later showed its "The Steam application has crashed" dialog. Legacy titles have each SDK call wrapped, but STS2 reaches `steam_api64.dll` through .NET P/Invoke. For a retained handoff that matches the signed-in account, the hive now stays mapped for the rest of the game process. The report sets `STEAM_HANDOFF_SDK_MAPPED`. [Discovery adapter](../runtime/win32/steam-discovery.c)

### Frame pacing

On the old presenter every 33 ms gap advanced the producer by two frames: two frames completed within one display refresh, and the presenter drew the newest and released both. The presenter now draws the oldest completed frame newer than the last one shown and holds the newer frame for the next refresh. The producer already skips a slot the presenter has not released, so at most one frame waits and the game thread never blocks. This change applies to every hosted GPU title. [GPU receiver](../runtime/presenter/gpu-receiver.cpp)

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

## Pending console checks

The game has not yet been driven past its first-run screen, so these remain open on build `26655341`:

1. Controller input: proceed past the Early Access screen and start a run.
2. A settled 30-second combat sample with `tools.measure --require-gpu-60`.
3. Audio. FMOD initialized; audible output has not been checked.
4. Save and reload. Cloud sync ran and local saves were written; a reload has not been checked.
5. Return to the library through the Steam overlay.

STS2 is not counted as covered until these pass.
