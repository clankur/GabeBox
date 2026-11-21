# Load the pinned Steam client library on Xbox

The Steam Core Loader Probe tests Valve’s original `steamclient64.dll` within an ordinary Xbox Developer Mode package. The goal is to reach the genuine client library independently of the Windows Steam launcher and its Chromium processes. The probe loads a known-good library first, attempts the Valve library, and checks for its `CreateInterface` export if the loader succeeds. It records each loader result in its own `probe-report.txt` in LocalState. It never calls the factory or signs into Steam.

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

## Series X observations

- Windows SDK template `1.0.13.0` and locally signed control `1.0.14.0` both loaded the owned library and found its export. With all three Valve files absent, `LoadPackagedLibrary` returned `0x7E` (`ERROR_MOD_NOT_FOUND`).
- The locally built `1.0.15.0` package installed, and Xbox found the core and both supporting Valve files. The owned library still loaded. The Valve core load instead returned `0x7F` (`ERROR_PROC_NOT_FOUND`). The probe never reached its export lookup. [Template build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44860592666)

- Instrumented package `1.0.18.0` reproduced the core’s `0x7F` and loaded `tier0_s64.dll` and `vstdlib_s64.dll` independently. Out of 518 distinct requested imports across 20 system modules, the only lookup returning no address was `USER32.dll!MapVirtualKeyExA`. Valve’s `steamclient64.dll` imports it directly. The pinned core had SHA-256 `669fa08e259277fbf5a1dfe15ce88afe734e89151194304b44d92f418dcc985b`. [Instrumented template build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44860919559)
- The Windows SDK template `1.0.19.0`, locally signed control `1.0.20.0`, and complete Valve-file package `1.0.21.0` produced the same isolated comparison. A DLL with only a regular import of `MapVirtualKeyExA` returned `0x7F`. An otherwise equivalent DLL with only `MapVirtualKeyExW` loaded. Neither test calls the imported function. The locally signed `1.0.21.0` still reported `MapVirtualKeyExA` as the one unresolved checked import, and the original Valve core remained unchanged. [Comparison template build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44861287286)

The results isolate the first loader obstacle in this pinned build. A Unicode input adaptation is a candidate for a compatible client, and its behavior still needs validation for character mappings, keyboard layouts and dead keys. The successful Unicode load establishes the availability of that entry point on the tested Xbox. It establishes no Steam session, library operation, or game launch. [Microsoft keyboard translation API](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-mapvirtualkeyexw)
