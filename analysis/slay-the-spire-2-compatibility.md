# Slay the Spire 2 on Xbox Series X

Slay the Spire 2 is Steam AppID `2868840`. The audited Windows build is Steam build `26655341`, publisher release `v0.111.1` (commit `cc1f9462`, dated 2026-11-13). The game runs on MegaDot, a fork of Godot 4.5, with a bundled .NET 9 runtime in `data_sts2_windows_x86_64`. The executable is `SlayTheSpire2.exe` (894,748,672 bytes, SHA-256 `8602c26bffd2937e3841835fd8360ef8e974624a543e05977229fd3d062be231`). The game data is `SlayTheSpire2.pck`.

The earlier OpenGL trials are recorded in [steam-compatibility.md](steam-compatibility.md#stage-an-adaptation-on-the-xbox). They forced `--rendering-driver opengl3` through Mesa's D3D12 OpenGL driver. Builds `1.0.230.61` through `1.0.230.141` reached Steamworks, the model cache and FMOD. Each recorded one CPU-uploaded 1920 × 1080 startup frame, and none presented a GPU frame before SteamPrimary suspended.

## Direct3D 12 route

The publisher ships `launch_d3d12.bat`, `launch_vulkan.bat` and `launch_opengl.bat` beside the executable, so `--rendering-driver d3d12` is a supported configuration of this build. The Direct3D path can use the shared hosted DXGI presenter. Spider-Man and Hades already present 1920 × 1080 frames through it as shared GPU textures.

Godot's Direct3D 12 context loads `D3D12.dll`, `DXGI.dll` and `Dcomp.dll` dynamically and resolves `D3D12CreateDevice`, `CreateDXGIFactory2` and `DCompositionCreateDevice` with `GetProcAddress`. [Godot D3D12 context](https://github.com/godotengine/godot/blob/4.5.1-stable/drivers/d3d12/rendering_context_driver_d3d12.cpp#L102-L135) The executable contains the UTF-16 strings for all three libraries, so this build was compiled with `DCOMP_ENABLED`. With that option, Godot creates its swap chain with `IDXGIFactory2::CreateSwapChainForComposition`. It then calls `CreateTargetForHwnd`, `CreateVisual`, `IDCompositionVisual::SetContent`, `IDCompositionTarget::SetRoot` and `Commit`, and fails swap chain creation if any of these calls fails. [Godot swap chain and composition setup](https://github.com/godotengine/godot/blob/4.5.1-stable/drivers/d3d12/rendering_device_driver_d3d12.cpp#L2808-L2884)

A hosted game's windows exist only in `XboxWin32Window.dll`. The system compositor cannot target them, and before this change the hosted DXGI factory passed composition swap chains through unhosted. The shared adapters now cover that contract:

- **Hosted composition swap chains.** When virtual windows are active, `IDXGIFactory2::CreateSwapChainForComposition` returns the same hosted swap chain that `CreateSwapChainForHwnd` uses. It is created without a window, and its frames publish to the GPU mailbox. A composition chain still requires an explicit width and height, as on Windows. [DXGI adapter](../runtime/win32/dxgi.cpp)
- **Hosted DirectComposition device.** `XboxWin32Dxgi.dll` exports `DCompositionCreateDevice`. It supports the subset that shows one swap chain per window: virtual-window targets, visuals, `SetContent`, `SetRoot` and `Commit`. `Commit` binds each hosted chain to its target's window, which the chain uses for `GetHwnd` and close requests. Other methods return `E_NOTIMPL`. A real HWND or a visual from another device returns `E_INVALIDARG`, and a second target for the same window returns `DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED`.
- **Dynamic routing.** With `XBOX_WIN32_HOSTED_DXGI=1`, loader trace already redirected `GetProcAddress(dxgi, "CreateDXGIFactory*")`. It now also redirects `GetProcAddress(dcomp, "DCompositionCreateDevice")`. If `LoadLibraryW(L"dcomp.dll")` fails with `ERROR_MOD_NOT_FOUND`, it returns a counted reference to the hosted adapter instead. [Loader trace](../runtime/win32/loader-trace.c)
- **Steamworks handoff.** The OpenGL route initialized the publisher's Steamworks SDK against the running client when the WGL adapter created its context. The hosted DXGI factory now does the same on first creation for profiles with the `steam-win32-steam-sdk` marker, which excludes the legacy import adapter. Godot creates its rendering context before the game's managed code calls `SteamAPI_Init`.
- **Launcher and packager.** A `godot`/`d3d12` shadow profile can now carry the DXGI marker, the accessibility option and the Steam SDK marker. The packager requires the loader trace for that profile and rejects a Steam Core artifact that lacks hosted DirectComposition.

## Build-matched profile

[`profiles/2868840/26655341`](../profiles/2868840/26655341) holds the launch markers, companion list and adapter declaration. [`d3d12-bindings.json`](../profiles/2868840/26655341/d3d12-bindings.json) redirects 52 imports. It keeps the window, focus, storage-trace and input bindings from the [OpenGL map](../profiles/2868840-26655341-opengl3-bindings.json) and drops the WGL pixel-format hooks. `GetProcAddress` and the four `LoadLibrary` entry points go to loader trace. Cursor and coordinate conversion, `GetParent`, `GetSystemMetrics`, the blocking message calls and `GetDeviceCaps`/`DeleteDC` go to the window adapter. The three display-configuration queries go to the hosted display adapter. Applied offline with `--trim-dwarf`, the binding map produced a 91,738,624-byte image that retains all 745 import slots, 50 of them resolved to shared adapters. The recipe is 8,768 bytes.

The launch marker selects `--rendering-driver d3d12`, the Godot launcher, loader trace and `--accessibility disabled`. The companion list stages the 214 files beside the executable except the `.pck`, launch scripts and Steam Input configuration. Godot loads the installed pack through `--main-pack`. The hosted XInput marker covers SDL's `xinput1_4.dll` lookup for Godot 4.5's joypad backend.

```sh
profile_source="$PWD/profiles/2868840/26655341"
profile=$(mktemp -d)
(cd "$steam_install" && shasum -a 256 -c "$profile_source/SOURCE-SHA256SUMS") || exit 1
cp "$profile_source"/steam-win32-* "$profile/"
python3 -m build_tools.redirect_pe_imports --recipe --trim-dwarf "$steam_install/SlayTheSpire2.exe" \
  "$profile_source/d3d12-bindings.json" "$profile/steam-win32-shadow-2868840.xwp"
```

If no local Windows install exists, export the executable from the console. Run `inspect-game --source-profile` with a one-file profile holding the size and SHA-256 above, and add `--export-inputs`. Then package with `--surfaceless-profile 2868840=26655341 --win32-trial-profile "2868840=$profile"`.

## Pending console checks

Off-console results: the Python suite passes, `ruff` is clean, and the adapter sources pass a local mingw syntax check. The signed package, the Xbox launch and the frame measurement have not run yet. Each check needs a Series X with build `26655341` installed:

1. Observe Big Picture Play with `observe --app-id 2868840 --image-name SlayTheSpire2.exe`. Expect loader-trace lines `asset dxgi-factory=hosted` and `asset dcomp-device=hosted` (or `dcomp-library=hosted`). Expect DXGI trace lines `hosted-composition-chain`, `dcomp-target`, `dcomp-commit-bound`, `steam-handoff-ready` and `gpu-frame-submitted`. A failure here records the first failing operation.
2. Confirm the title screen in a Device Portal screenshot. Confirm controller navigation through hosted XInput and audio through FMOD.
3. Start a run, save, quit to the library and reload the save.
4. Measure 30 seconds of settled combat with `python3 -m tools.measure --require-gpu-60 --internal-resolution 1920x1080 --output-resolution 1920x1080`. The gate needs exclusively shared-texture frames, an average of at least 59.5 FPS and at least 57 frames in every rolling second.
