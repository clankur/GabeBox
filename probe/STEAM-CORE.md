# Load the pinned Steam client library on Xbox

The Steam Core Loader Probe tests Valve’s original `steamclient64.dll` within an ordinary Xbox Developer Mode package. The goal is to reach the genuine client library independently of the Windows Steam launcher and its Chromium processes. The probe loads a known-good library first, attempts the Valve library, and checks for its `CreateInterface` export if the loader succeeds. It records each loader result in its own `probe-report.txt` in LocalState. On an adapted build that loads successfully, the runtime probe requests the `SteamClient023` interface and tests both available ways to open a Steam pipe. It runs without a Steam account.

The workflow publishes an owned-code template as `xbox-steam-core-template`. Add the Valve files locally from the exact `bins_win64` archive in [the pinned manifest](../analysis/valve-release.json). The preparation script verifies the archive size and SHA-256 and extracts the core and both Valve dependencies, preserving their bytes. The locally signed APPX stays in the deployment workspace.

## Build and run the controls

The commands use Python 3.9 or later, LLVM objdump, Microsoft’s MSIX SDK CLI, OpenSSL, osslsigncode, and the existing [Device Portal settings](README.md). On macOS, `xcrun -f llvm-objdump` locates the LLVM tool from Xcode. Use a new work directory outside the checkout and the workflow run for the source under test.

```sh
trial_dir=$(mktemp -d)
run_id=YOUR_WORKFLOW_RUN_ID
xbox_env=/path/to/setup-workspace/.env
objdump_path=$(xcrun -f llvm-objdump)
gh run download "$run_id" --name xbox-steam-core-template --dir "$trial_dir/template"
(cd "$trial_dir/template" && shasum -a 256 -c SHA256SUMS)
template="$trial_dir/template/PackagedProcessProbe-x64.appx"

python3 xbox-deploy.py --env-file "$xbox_env" deploy "$template" --test-probe --timeout 30
python3 probe/prepare-steam-core.py "$template" "$trial_dir/control" --version 1.0.20.0
bash probe/package.sh --layout "$trial_dir/control" "$trial_dir/control-package"
python3 xbox-deploy.py --env-file "$xbox_env" deploy \
  "$trial_dir/control-package/PackagedProcessProbe-x64.appx" --test-probe --timeout 30

curl -fsSL \
  https://client-update.akamai.steamstatic.com/bins_win64.zip.119a215939f1426cff28bdf03a5c55fcd865720c \
  -o "$trial_dir/bins_win64.zip"
python3 probe/prepare-steam-core.py "$template" "$trial_dir/core" --version 1.0.21.0 \
  --valve-archive "$trial_dir/bins_win64.zip" --objdump "$objdump_path"
bash probe/package.sh --layout "$trial_dir/core" "$trial_dir/core-package"
(cd "$trial_dir/core-package" && shasum -a 256 -c SHA256SUMS)
python3 xbox-deploy.py --env-file "$xbox_env" deploy \
  "$trial_dir/core-package/PackagedProcessProbe-x64.appx" --test-probe --timeout 30
python3 xbox-deploy.py --env-file "$xbox_env" probe --steam-core --version 1.0.21.0 --timeout 30
```

Choose package versions above the template and anything already installed under the Steam Core identity. The terminal line `status=steam-core-complete` means the CLI collected the experiment. Check `control_load`, `steam_core`, and `create_interface` to determine what loaded. The system import inventory is generated locally from the three unchanged PE files. The probe uses the process’s normal DLL loader and resolves system exports by name or ordinal after the primary load attempt. It records system-module errors separately from missing exports. [Packaged library loading](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-loadpackagedlibrary) · [GetProcAddress](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress)

## Test the Unicode keyboard adaptation

Use a newer version of the same package identity and the SDK template with the runtime probe. As above, download and verify the template and Valve archive before preparing the layout.

```sh
python3 probe/prepare-steam-core.py "$template" "$trial_dir/unicode" --version 1.0.26.0 \
  --valve-archive "$trial_dir/bins_win64.zip" --objdump "$objdump_path" --unicode-keyboard
bash probe/package.sh --layout "$trial_dir/unicode" "$trial_dir/unicode-package"
(cd "$trial_dir/unicode-package" && shasum -a 256 -c SHA256SUMS)
python3 xbox-deploy.py --env-file "$xbox_env" deploy \
  "$trial_dir/unicode-package/PackagedProcessProbe-x64.appx" --test-probe --timeout 40
python3 xbox-deploy.py --env-file "$xbox_env" probe --steam-core --version 1.0.26.0 --timeout 40
```

The opt-in flag changes one byte of the local, hash-verified `steamclient64.dll` so that the verified keyboard call sites bind to `MapVirtualKeyExW`. The script checks that the resulting import inventory differs only at that symbol. Both supporting DLLs retain their original bytes. The original Valve archive remains intact. The derived DLL’s original Authenticode signature no longer validates. The local APPX uses the same Developer Mode package signing and deployment flow as the controls, and the CI artifact still contains only the owned template.

The runtime report identifies separate factory and pipe stages. A successful factory reports `steam_factory=success` and `steam_factory_code=0`. Direct pipe creation and release report `steam_pipe=open-and-release`. If the direct call returns zero, the probe invokes Steam’s user-and-pipe bootstrap. A signed-out bootstrap reports `steam_bootstrap=success` and `steam_logged_on=no`. The CLI accepts the final checkpoint regardless of the experimental outcome. [Valve factory type](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/public/tier1/interface.h) · [Valve SDK interface version](https://github.com/ValveSoftware/Proton/blob/proton_11.0/lsteamclient/steamworks_sdk_164/isteamclient.h) · [Steamworks pipe API](https://partner.steamgames.com/doc/api/ISteamClient)

## Verify account sign-in prerequisites

Prepare and deploy the current template with `--unicode-keyboard` and a version above the installed Steam Core Probe. The Steam Core package declares the UWP `internetClient` capability. The probe checks Valve’s public connection-manager directory over HTTPS, opens one WSS connection to a Valve server listed there, and closes it. It requests one QR approval challenge directly from Valve and discards the unapproved response. The report contains status and counts. It does not contain challenge URLs, request identifiers, or Steam account credentials. [Valve connection-manager directory](https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&cmtype=websockets) · [Steam Mobile sign-in](https://store.steampowered.com/mobile)

The runtime validates the derived library’s SHA-256 and the loaded vtable and method addresses before calling the pinned client interfaces. The `SteamClient023` factory establishes the public interface. The version-specific engine factory method returns an `IClientUser` for the bootstrap’s user and pipe. The probe reads its connection state and queries subscriptions only while signed out. A zero in that report describes the signed-out context. The Steam account’s library requires an authenticated session. [Earlier native client adapter research](https://github.com/willfaust/madeira-dock/blob/main/docs/CLIENT_LAYOUTS.md)

The pinned build uses user vtable slot 183 for `GetSubscribedApps`. Slot 182 belongs to `BIsSubscribedApp`. Their signatures differ. For an updated Valve DLL, confirm the image hash, RTTI table addresses, wrapper method names, and argument widths before updating the runtime table.

## Series X observations

- Windows SDK template `1.0.13.0` and locally signed control `1.0.14.0` both loaded the owned library and found its export. With all three Valve files absent, `LoadPackagedLibrary` returned `0x7E` (`ERROR_MOD_NOT_FOUND`).
- The locally built `1.0.15.0` package installed, and Xbox found the core and both supporting Valve files. The owned library still loaded. The Valve core load instead returned `0x7F` (`ERROR_PROC_NOT_FOUND`). The probe never reached its export lookup. [Template build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44860592666)

- Instrumented package `1.0.18.0` reproduced the core’s `0x7F` and loaded `tier0_s64.dll` and `vstdlib_s64.dll` independently. Out of 518 distinct requested imports across 20 system modules, the only lookup returning no address was `USER32.dll!MapVirtualKeyExA`. Valve’s `steamclient64.dll` imports it directly. The pinned core had SHA-256 `669fa08e259277fbf5a1dfe15ce88afe734e89151194304b44d92f418dcc985b`. [Instrumented template build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44860919559)
- The Windows SDK template `1.0.19.0`, locally signed control `1.0.20.0`, and complete Valve-file package `1.0.21.0` produced the same isolated comparison. A DLL with only a regular import of `MapVirtualKeyExA` returned `0x7F`. An otherwise equivalent DLL with only `MapVirtualKeyExW` loaded. Neither test calls the imported function. The locally signed `1.0.21.0` still reported `MapVirtualKeyExA` as the one unresolved checked import, and the original Valve core remained unchanged. [Comparison template build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44861287286)

The results isolate the first loader obstacle in this pinned build. Valve’s newer public beta `1795048523` also imports `MapVirtualKeyExA` directly. Its independently hash-checked core has SHA-256 `4a2d79520a72c55854c61c22cbfc53ec4e12cc13cc00e0c27d9b520648fe1c10`. [Valve public beta manifest](https://client-update.fastly.steamstatic.com/steam_client_publicbeta_win64)

Disassembly of the pinned stable core found ten direct calls through the `MapVirtualKeyExA` import. Each passed mode zero, `MAPVK_VK_TO_VSC`, which maps a virtual key to a scan code. A Unicode input adaptation is a candidate for a compatible client, and its behavior still needs validation for character mappings, keyboard layouts and dead keys. The successful Unicode load establishes the availability of that entry point on the tested Xbox. It establishes no Steam session, library operation, or game launch. [Microsoft keyboard translation API](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-mapvirtualkeyexw)

The loader-only `1.0.22.0` package with the local Unicode adaptation installed and loaded the derived Valve core on Series X. `GetProcAddress` found `CreateInterface`. The single-byte adaptation changed the core’s SHA-256 to `5237b35064b7e1cf26754959643341fdf36c55ffefea54dcf20de6d62b46d822`. This first test only inspected the export.

The adapted `1.0.24.0` Xbox probe called the factory and obtained a non-null `SteamClient023` interface with return code `0`. The direct `Steam_CreateSteamPipe` call returned `0`. The same factory and direct-pipe sequence on clean Windows Server build `26100`, using the original verified Valve DLLs and without a running Steam desktop process, also returned a valid interface and a zero pipe handle. [Runtime SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44861937129) · [Windows control](https://github.com/clankur/steam-xbox-devmode/actions/runs/44862185635)

The adapted `1.0.26.0` Xbox probe then called `Steam_CreateGlobalUser`. It obtained a positive Steam user handle and pipe handle, reported `steam_logged_on=no`, released the user, and verified that the pipe closed. This is a signed-out native Steam core session on the Series X. Account authentication, library access, installation and local game launch remain open. [Bootstrap SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44862298020)

Running the same optional bootstrap with only the three core DLLs on the Windows test VM emitted missing-`SDL3.dll` and missing-`video64.dll` messages before the process failed. The Windows control defaults to the isolated factory and direct-pipe check. Use `analysis/steam-core-control.ps1 -Bootstrap` on a disposable Windows VM to re-test that separate path. [Windows bootstrap trial](https://github.com/clankur/steam-xbox-devmode/actions/runs/44862297963)

On Series X, package `1.0.28.0` received a successful Valve directory response and established a WSS connection. Package `1.0.30.0` repeated the network result and verified the pinned engine and user interface addresses. The genuine signed-out subscription query returned `0` and the native user reported no active Steam connection. [Network SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44862803331) · [Session ABI SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44863003791)

Package `1.0.32.0` verified the same native ABI and signed-out query, and Valve’s authentication endpoint returned HTTP `200` with a valid approval challenge. The package discarded the challenge. No sign-in, account data, entitlement query, or library retrieval occurred. [QR preflight SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44863218500)
