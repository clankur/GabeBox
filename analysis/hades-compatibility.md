# Hades on Xbox Series X

Hades Steam AppID `1145360`, Windows build `10929685`, launches from **Play** in Big Picture on Xbox Series X. The Xbox host stages its DirectX launcher and engine from the player's Steam install, then presents the game at 1920 × 1080 through shared GPU textures. Hosted XInput delivers gamepad state to the game. The player confirmed that pressing B twice kept Hades and Steam open on Xbox build `1.0.230.79`.

During live play on Xbox build `1.0.230.106`, the final 30 seconds of the native presenter trace contained 1,681 distinct hardware frames and zero CPU uploads. It averaged **56.03 FPS**, with **25 frames in the slowest rolling second** and a **450.37 ms hitch**. The trace confirms uneven frame delivery. It cannot resolve whether the longest pause originated in the game or in the shared-texture and presentation pipeline. Sustained 60 FPS, save and reload, and a full return-to-Library cycle still need validation.

## Prepare the build-matched profile

Install the Windows version of Hades through Steam. Set `steam_install` to the local Hades directory and run these commands from the repository root. The source checksums are for Steam build `10929685`.

```sh
profile_source="$PWD/profiles/1145360-10929685"
profile=$(mktemp -d)
(cd "$steam_install" && shasum -a 256 -c "$profile_source/SOURCE-SHA256SUMS") || exit 1
cp "$profile_source"/steam-win32-*.txt "$profile/"
cp "$profile_source/steam-win32-adapters-1145360.json" "$profile/"
python3 -m build_tools.redirect_pe_imports --recipe "$steam_install/x64/EngineWin64s.dll" \
  "$profile_source/engine-bindings.json" "$profile/steam-win32-shadow-1145360.xwp"
python3 -m build_tools.redirect_pe_imports --recipe "$steam_install/x64/Hades.exe" \
  "$profile_source/launcher-bindings.json" "$profile/steam-win32-shadow-1145360-launcher.xwp"
```

Follow the [single-window Big Picture build](big-picture.md#reproduce-the-single-window-series-x-build). Use the Steam Core template, FHD presenter and primary XAML host artifacts built from this checkout. The regular FHD base provides the SDL and Mesa dependencies for the shared hosted package and declares the x64 Visual C++ UWP and UWPDesktop frameworks. Add the following options to the Big Picture packaging command before signing.

```sh
--surfaceless-profile 1145360=10929685 \
--win32-trial-profile "1145360=$profile"
```

Install the signed package and the framework dependencies if needed. Sign in with an account that owns Hades, install it in the Xbox Steam library, and select **Play**. The launcher rechecks the entitlement, installed build and original binary hashes. The signed package contains the runtime adapters and recipes. Game binaries come from Steam on the console, and staging leaves the Steam depot files intact.

The profile supplies Hades' native gamepad options and the original image names. It requires the `ktmw32.dll` transaction adapter from the same signed Steam Core artifact. The runtime also provides desktop window calls, DXGI presentation, hosted XInput and local crash reporting. This checkout's profile is pinned to the Windows x64 DirectX executable. [Supergiant's gamepad and launch guidance](https://www.supergiantgames.com/faqs/hades/)
