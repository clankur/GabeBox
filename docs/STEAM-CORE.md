# Load the pinned Steam client library on Xbox

The Steam Core Loader Probe tests Valve’s original `steamclient64.dll` within an ordinary Xbox Developer Mode package. The goal is to reach the genuine client library independently of the Windows Steam launcher and its Chromium processes. The probe loads a known-good library first, attempts the Valve library, and checks for its `CreateInterface` export if the loader succeeds. It records each loader result in its own `probe-report.txt` in LocalState. On an adapted build that loads successfully, the runtime probe requests the `SteamClient023` interface and tests both available ways to open a Steam pipe. It runs without a Steam account.

The workflow publishes an owned-code template as `xbox-steam-core-template`. Add the Valve files locally from the exact `bins_win64` archive in [the pinned manifest](../runtime/steam/pins/valve-release.json). The preparation script verifies the archive size and SHA-256 and extracts the core and both Valve dependencies, preserving their bytes. The locally signed APPX stays in the deployment workspace.

## Build and run the controls

The commands use Python 3.9 or later, LLVM objdump, Microsoft’s MSIX SDK CLI, OpenSSL, osslsigncode, and the existing [Device Portal settings](../probe/README.md). On macOS, `xcrun -f llvm-objdump` locates the LLVM tool from Xcode. Use a new work directory outside the checkout and the workflow run for the source under test.

```sh
trial_dir=$(mktemp -d)
run_id=YOUR_WORKFLOW_RUN_ID
xbox_env=/path/to/setup-workspace/.env
objdump_path=$(xcrun -f llvm-objdump)
gh run download "$run_id" --name xbox-steam-core-template --dir "$trial_dir/template"
(cd "$trial_dir/template" && shasum -a 256 -c SHA256SUMS)
template="$trial_dir/template/PackagedProcessProbe-x64.appx"

python3 xbox-deploy.py --env-file "$xbox_env" deploy "$template" --test-probe --timeout 30
python3 -m build_tools.prepare_steam_core "$template" "$trial_dir/control" --version 1.0.20.0
bash build_tools/package.sh --layout "$trial_dir/control" "$trial_dir/control-package"
python3 xbox-deploy.py --env-file "$xbox_env" deploy \
  "$trial_dir/control-package/PackagedProcessProbe-x64.appx" --test-probe --timeout 30

curl -fsSL \
  https://client-update.akamai.steamstatic.com/bins_win64.zip.119a215939f1426cff28bdf03a5c55fcd865720c \
  -o "$trial_dir/bins_win64.zip"
python3 -m build_tools.prepare_steam_core "$template" "$trial_dir/core" --version 1.0.21.0 \
  --valve-archive "$trial_dir/bins_win64.zip" --objdump "$objdump_path"
bash build_tools/package.sh --layout "$trial_dir/core" "$trial_dir/core-package"
(cd "$trial_dir/core-package" && shasum -a 256 -c SHA256SUMS)
python3 xbox-deploy.py --env-file "$xbox_env" deploy \
  "$trial_dir/core-package/PackagedProcessProbe-x64.appx" --test-probe --timeout 30
python3 xbox-deploy.py --env-file "$xbox_env" probe --steam-core --version 1.0.21.0 --timeout 30
```

Choose package versions above the template and anything already installed under the Steam Core identity. The terminal line `status=steam-core-complete` means the CLI collected the experiment. Check `control_load`, `steam_core`, and `create_interface` to determine what loaded. The system import inventory is generated locally from the three unchanged PE files. The probe uses the process’s normal DLL loader and resolves system exports by name or ordinal after the primary load attempt. It records system-module errors separately from missing exports. [Packaged library loading](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-loadpackagedlibrary) · [GetProcAddress](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress)

## Test the Unicode keyboard adaptation

Use a newer version of the same package identity and the SDK template with the runtime probe. As above, download and verify the template and Valve archive before preparing the layout.

```sh
python3 -m build_tools.prepare_steam_core "$template" "$trial_dir/unicode" --version 1.0.26.0 \
  --valve-archive "$trial_dir/bins_win64.zip" --objdump "$objdump_path" --unicode-keyboard
bash build_tools/package.sh --layout "$trial_dir/unicode" "$trial_dir/unicode-package"
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

## Run the on-console sign-in

The sign-in stage needs the account owner to approve a QR code in Steam Mobile. Start the app through the CLI after they are ready. The on-TV screen creates a request directly with Valve and supplies the resulting approval to the verified native Steam core in the same app. The CLI sees the stage, outcome, and subscription count.

```sh
python3 -m build_tools.prepare_steam_core "$template" "$trial_dir/signin" --version 1.0.34.0 \
  --valve-archive "$trial_dir/bins_win64.zip" --objdump "$objdump_path" \
  --unicode-keyboard --steam-sign-in
bash build_tools/package.sh --layout "$trial_dir/signin" "$trial_dir/signin-package"
(cd "$trial_dir/signin-package" && shasum -a 256 -c SHA256SUMS)
python3 xbox-deploy.py --env-file "$xbox_env" deploy \
  "$trial_dir/signin-package/PackagedProcessProbe-x64.appx" --test-probe --timeout 245
```

`status=steam-approval-ready` shows that the app drew the Valve code. In Steam Mobile, open Steam Guard, scan the code, and approve the sign-in named **Xbox Dev Mode Steam client**. If it expires, **Get a new code** starts another request. The report contains stage markers and counts. [Steam Mobile](https://store.steampowered.com/mobile)

To keep the approved session across package updates, select **Remember sign-in on this Xbox** before approving the code. After Valve verifies the account, the native component saves the refresh token using Windows data protection scoped to the current Windows user. Only protected bytes are written to the app’s nonroaming local cache. A subsequent launch of the same app under the same Xbox user tries that session with Valve first. An expired token, rejected session, or unavailable protected storage returns to Steam Guard. **Sign out** disables session reuse and removes the cached file. [Windows data protection](https://learn.microsoft.com/en-us/uwp/api/windows.security.cryptography.dataprotection.dataprotectionprovider) · [Windows app data](https://learn.microsoft.com/en-us/uwp/api/windows.storage.applicationdata)

On Series X, version `1.0.50.0` accepted an opted-in Steam Guard approval and reported `steam_session_cache=saved`. I upgraded the same package identity to `1.0.52.0` through the CLI and launched it. A fresh run reported `steam_session_cache=restored`, `steam_signin=authenticated`, and `steam_library_rendered=yes` without requesting another QR approval. Local UI tests also exercised saved-session fallback and sign-out. [Combined SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44868632573)

For an upgrade test, check the first report for `steam_session_cache=saved` after the owner opts in and signs in. Upgrade the package in place and look for `steam_session_cache=restored` together with `steam_signin=authenticated`. Reinstalling the package after removing app data requires a fresh approval. To check sign-out, sign out in the app and confirm the next launch draws a Steam Guard code. Reports contain session status only.

A verified library result requires Valve’s native user interface to report online, its account ID to match the approved account, and its subscription query to return a positive count. The client reads title metadata for those AppIDs and sends entries whose native Steam app type is Game to the on-console page. The page renders a searchable list and keeps the native session open until sign-out or app exit. `steam_library_named` counts all entries with names returned by Steam. `steam_library_visible` counts the games in the filtered library. `steam_library_rendered=yes` confirms the view rendered, including when the filtered library is empty. AppIDs, titles, account details and QR data stay off the CLI report. The QR renderer is the pinned MIT-licensed `qrcode-generator` browser bundle, included by the SDK workflow and used locally. [QR renderer source](https://github.com/kazuhikoarase/qrcode-generator)

In the library, use the controller D-pad to move between games. Right focuses the selected title’s available **Download** or **Play** button. Press **A** to activate the focused button, and use **B** to return from an action to the list or from a title to search. From search, Up reaches **Sign out** and Down returns to the first visible title. [UWP gamepad navigation](https://learn.microsoft.com/en-us/windows/uwp/ui-input/gamepad-and-remote-interactions)

For an installed copy of Slay the Spire, move Down from **Play** to **Cloud saves** and choose **Download and use saves**. The app uses the current Steam session to fetch and verify the Steam Cloud files. It backs up the existing game save folders under `LocalState/SteamCloud/646570/backup-*/backup`, installs the cloud files, and returns focus to **Play**. [Additional cloud save profiles](https://github.com/clankur/steam-xbox-devmode/issues/56)

Game rows load Steam's public small capsule artwork as they approach the viewport. The client schedules up to three image loads at once and releases decoded artwork more than a viewport away. It caches up to 256 entries in IndexedDB, with an 8 MiB cap on JPEG payloads and a 30-day freshness window. Missing art shows the game’s initial. [Steam graphical assets](https://partner.steamgames.com/doc/store/assets)

Read the library's aggregate artwork counters after the visible rows have loaded. `art_run_id` identifies the app process, and `art_active=0` marks a settled batch. A fresh app restart lets you compare `art_network_requests` with `art_disk_hits` on the same library viewport.

```sh
python3 xbox-deploy.py --env-file "$xbox_env" report --version 1.0.72.0 --artwork
```

On Series X, the `1.0.72.0` library rendered 134 games. The opening viewport displayed eight capsules from eight network requests, with no more than three artwork loads active at once. After a CLI restart, the same eight images came from disk with zero artwork requests. Both starts restored the saved Steam session. [SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44872841753)

On Series X, version `1.0.70.0` restored the saved Steam session after the CLI upgrade and rendered 134 games from 612 named entries. The preceding DLC-only filter had displayed 361 entries. The configured launch test found Slay the Spire in the filtered library and submitted it to Steam. [SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44872147072)

Read an existing report without restarting the app or interrupting the session.

```sh
python3 xbox-deploy.py --env-file "$xbox_env" report --version 1.0.38.0
```

## Direct Steam Cloud research

The Xbox sign-in sends the QR approval's Steam client refresh token to the pinned native client. Valve's partner `ICloudService` instead uses a Valve-issued OAuth client with `read_cloud` scoped to the requested AppID. The public `ISteamRemoteStorage::GetFileCount` API enumerates local files that Steam has synchronized. [Steam OAuth](https://partner.steamgames.com/doc/webapi_overview/oauth#steam_cloud) · [Steam Cloud Web API](https://partner.steamgames.com/doc/webapi/ICloudService) · [ISteamRemoteStorage](https://partner.steamgames.com/doc/api/ISteamRemoteStorage#GetFileCount)

The Steam client protocol has a separate Cloud service. Its protobuf describes `EnumerateUserFiles`, `GetAppFileChangelist` and `ClientFileDownload`. The file listing can include hashes, path prefixes and persistence states. The download response can include an HTTPS destination, request headers, raw and transfer sizes, and an encryption marker. SteamKit shows how authenticated client messages assign a source job ID and correlate the matching response. [Steam client Cloud schema](https://github.com/SteamTracking/Protobufs/blob/master/steam/steammessages_cloud.steamclient.proto) · [SteamKit client messaging](https://github.com/SteamRE/SteamKit/blob/master/SteamKit2/SteamKit2/Steam/Handlers/SteamUnifiedMessages/SteamUnifiedMessages.cs)

An offline inspection of the `1.0.220.0` package's `steamclient64.dll` (SHA-256 `5237b35064b7e1cf26754959643341fdf36c55ffefea54dcf20de6d62b46d822`) found RTTI-backed vtables with executable entries for `CAdapterSteamUnifiedMessages001`, `IClientUnifiedMessagesMap` and `IClientRemoteStorageMap`. It contains the two versioned client download and changelist method names and the `EnumerateUserFiles` request and response types. Valve's public client documentation still lists a unified messages getter, while its published Source SDK header labels it private and deprecated for `SteamClient020`. The Xbox package uses `SteamClient023`, so the getter signature and table need validation against the pinned build. [ISteamClient interface](https://partner.steamgames.com/doc/api/ISteamClient) · [Valve SDK header](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/public/steam/isteamclient.h)

The adapter uses SteamClient023's generic getter and the image-pinned unified-message adapter. It polls the response handle for `Cloud.EnumerateUserFiles#1`, downloads the files over HTTPS, and verifies each byte count and SHA-1 before handing the files to the restore code. The transport never writes to Steam Cloud.

The `1.0.223.0` Series X build authenticated with its saved Steam session and downloaded and verified 67 Slay the Spire files. A `1.0.224.0` run placed all 67 files in an isolated copy of the same save-folder layout and verified that an existing test file survived and was backed up. One earlier transfer returned an HTTP error and a fresh run succeeded. Both fixtures leave the real game folders untouched. [Xbox SDK build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44925402014)

The app maps Slay the Spire's cloud paths to the verified install's `preferences` and `saves` directories. It prepares the new directories before moving any active files. If a restore is interrupted, the launcher uses the journal to recover the previous files before starting the game. Use `xbox-deploy.py report --version VERSION --steam-cloud` to read the latest status. For a one-time verification package, add an empty `steam-cloud-preview.txt` to the local layout before signing. It fetches and verifies the files and restores them into temporary app storage. [Mega Crit save folders](https://steamcommunity.com/app/646570/discussions/0/1743353164094689674/#c1771511442697212245)

## Test an owned game download

Deploy a download-enabled build through the same CLI and approve Steam Guard. In the library, select a small owned game and choose **Download**. The client rechecks the account, app type and Steam license. It submits the request to Steam’s app manager using the library registered at `LocalState/SteamLibrary`.

```sh
python3 xbox-deploy.py --env-file "$xbox_env" report --version 1.0.40.0 --download
```

The app writes `steam-download-report.txt` when Steam changes state. `queued` records acceptance by Steam. `downloading` and `staging` reflect the app manager’s active title and install flags. `installed` requires Steam’s fully installed state and a reported install directory inside the app’s library. After an unsuccessful run, keep the last download report and check whether the library folder or any files appeared in Device Portal. The report records numeric state and error codes without exposing the selected title or account. [Steam depots](https://partner.steamgames.com/doc/store/application/depots) · [Device Portal file listing](https://learn.microsoft.com/en-us/windows/uwp/debug-test-perf/device-portal-api-core#get-files)

Version `1.0.40.0` successfully installed a Steam title in app-local storage on Series X. The report progressed from `downloading` to `installed` with state `4`. The on-device manifest recorded 608,111,824 downloaded bytes and result `0`. A separate Device Portal directory count confirmed 16,017 installed files totaling 803,964,902 bytes. An earlier title reached state `514`, then stalled. Its manifest recorded disk-write result `11` and zero downloaded bytes. [Download SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44865311046)

### Diagnose a paused download

When the selected title is paused, pressing **Download** submits the normal install request and asks Steam to move that title to the front of its queue. The report identifies the paused state and records caller-available and volume-free bytes before and after the attempt. The Steam result `11` means disk-write failure. Steam uses separate results for disk space and quota limits. [Microsoft volume and caller space API](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdiskfreespaceexw)

The CLI can inspect the title’s manifest and optionally the header of its Windows executable. It reads only the app's Steam library and samples the executable in memory without starting it. A PE machine type identifies the executable architecture. It does not validate the game’s dependencies or its platform support. [Microsoft PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)

```sh
python3 xbox-deploy.py --env-file "$xbox_env" inspect-game --version 1.0.46.0 \
  --app-id 646570 --executable jre/bin/javaw.exe
```

On Series X, `1.0.46.0` retried Slay the Spire and measured 30,867,873,792 caller-available bytes before and after the attempt. Steam stayed at state `514`, result `11`, and zero bytes downloaded. The bundled Java launcher was absent from the installed and staged folders. The Device Portal file listing could verify those folders, but the console returned HTTP `404` for its ETW provider endpoint and had no app-local Steam content log. The measured space rules out a full app volume during this attempt. The write operation that failed remains unidentified. [Recovery SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44867583238)

A later retry on the same build transferred 271,127,152 bytes and installed Slay the Spire in app-local storage. Its manifest reported state `4`, result `0`, and 541,513,970 bytes on disk. The CLI inspected `jre/bin/javaw.exe` and verified x64 PE32+. Steam rejected Play with result `17` before it reported a running process. The installed game manifest references depot `228983` in shared app `228980`. The app-local library had no manifest for that shared app.

## Test Steam’s normal launch path

The [game launch contract](GAME-LAUNCH.md) describes the common Steam checks, the supported launch backends, and how to verify another title.

Select a downloaded game in the library and choose **Play**. For a CLI launch test, prepare the sign-in package with `--launch-app-id APPID` for a game already installed in the Xbox app's own library. After Steam Guard approval, the probe makes one automatic launch attempt per package version. It refreshes the library and, if Steam needs to reopen a completed local install, asks the app manager to register the existing files. Launch requires Steam to confirm the current account owns the game and reports it fully installed in that library. The probe records Steam’s numeric response and whether the client observed a running game and ready window.

```sh
python3 -m build_tools.prepare_steam_core "$template" "$trial_dir/launch" --version 1.0.44.0 \
  --valve-archive "$trial_dir/bins_win64.zip" --objdump "$objdump_path" \
  --unicode-keyboard --steam-sign-in --launch-app-id 105600
bash build_tools/package.sh --layout "$trial_dir/launch" "$trial_dir/launch-package"
python3 xbox-deploy.py --env-file "$xbox_env" deploy \
  "$trial_dir/launch-package/PackagedProcessProbe-x64.appx" --test-probe --timeout 310
python3 xbox-deploy.py --env-file "$xbox_env" report --version 1.0.44.0
```

Add `--repeat-launch` when preparing an explicit `--launch-app-id` test package to run the same licensed launch check after each fresh app start. Once the current test ends, restart the installed probe and wait for a new report through Device Portal. A saved Steam session allows the test to repeat without a new QR approval. [Device Portal app controls](https://learn.microsoft.com/en-us/windows/uwp/debug-test-perf/device-portal-api-core#stop-a-modern-app)

```sh
python3 xbox-deploy.py --env-file "$xbox_env" probe --steam-core --version 1.0.68.0 --restart --timeout 150
```

Add `--install-before-launch` to a local `--launch-app-id` package to try the normal Steam download when the game has no complete manifest in the Xbox app library. The `report --download` command follows the install, and the launch report records its final checkpoint as `steam_launch_install`. A completed install continues to Play through Steam’s usual license and prerequisite checks. Keep the app in the foreground and allow enough CLI wait time for the selected download.

On Series X, `1.0.74.0` restored the saved Steam session and submitted the Hades install through this path. Steam paused with state `514` and no download progress, while the app volume reported 29,909,282,816 bytes available. The CLI recorded `steam_launch_install=stalled` and ended the run before the Play step. Hades has an x64 DirectX build for a launch test after content installation succeeds. [SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44873669512) · [Hades technical support](https://www.supergiantgames.com/faqs/hades/)

On Series X with `1.0.80.0`, the selected-game retry completed the 1,355,226,768-byte Dwarf Fortress transfer while Hades and Stardew Valley stayed paused. The first Play attempt hit an install-path error after shared prerequisite setup. A restart reused the completed installs, Steam accepted Play, and the game process exited with `0xC0000135` before opening a window.

Use the optional `--library-inventory` flag when preparing a local sign-in test package to inspect game names and AppIDs for candidate selection. Read the resulting app-local JSON with `report --inventory`. Treat this file as private library data.

The `1.0.76.0` Series X run provided 134 games for comparison. Dwarf Fortress was selected for its x64 Windows requirement and 500 MB published storage minimum. Package `1.0.78.0` submitted its install, then Steam paused with state `514`, update result `11`, and no transfer or installed executable. The app measured 29,909,069,824 bytes available. The temporary console inventory was removed after candidate selection. [Windows SDK build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44874202315) · [Dwarf Fortress system requirements](https://store.steampowered.com/app/975370/Dwarf_Fortress/#app_sysreq)

For a local test of the Microsoft x64 desktop C++ runtime, download the official `Microsoft.VCLibs.x64.14.00.Desktop.appx`. Pass it to `prepare-steam-core.py --vclibs-framework` and supply the same original signed APPX to `xbox-deploy.py deploy --dependency`. The prepare step checks the framework identity, architecture and DLL filenames. Xbox validates the dependency during installation. [Microsoft’s deployment instructions and download](https://learn.microsoft.com/en-us/windows/msix/msix-troubleshooting-guide)

The console accepted version `1.0.84.0` with framework version `14.0.33321.0`. Steam restored the saved session, reused the installed game and accepted Play. The captured process exited with `0xC0000139` (entry point missing) before a ready window. The change in error narrowed the loader investigation. [Microsoft’s NTSTATUS reference](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-erref/596a1078-e883-4972-9bbc-49e60bebca55)

An import probe in `1.0.85.0` resolved 24 system DLLs used by the installed Dwarf Fortress executable and its startup libraries. The only missing entry point in that check was SDL2's `imm32.dll!ImmGetIMEFileNameA`. The installed game's SDL2 reports version `2.26.2`. The SDL2 workflow builds upstream `2.32.10` with `SDL_DISABLE_WINDOWS_IME` and verifies that it omits this import. The rebuilt library provides all 117 SDL2 exports requested by the game and its SDL2_image library. [SDL2 Windows input source](https://github.com/libsdl-org/SDL/blob/release-2.32.10/src/video/windows/SDL_windowskeyboard.c) · [SDL2 build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44875975339)

In the local `1.0.88.0` trial, the console received the rebuilt SDL2 after the original file was backed up and verified. The Steam library rendered and the launch test wrote its starting checkpoint. It timed out without a final report or an observed Dwarf Fortress process. The original DLL was restored and verified.

The workflow also builds SDL2 `2.26.2`, the version shipped with Dwarf Fortress, using the same disabled Windows IME option. The original DLL reproduced `0xC0000139` on build `1.0.92.0`. An A/B test with the rebuilt `2.26.2` under the same Xbox app version wrote the `game-running` checkpoint from Steam, then timed out without a final report. Device Portal did not observe a sustained Dwarf Fortress process or verify a game window. [SDL2 2.26.2 release](https://github.com/libsdl-org/SDL/releases/tag/release-2.26.2) · [SDL build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44877353402)

### Shared SDL2 compatibility trials

Use the same SDL2 source workflow for games with this loader dependency. It produces versioned x64 artifacts with a license and checksum. The current profiles pair Dwarf Fortress with `2.26.2` and Stardew Valley with `2.30.9`. Keep each profile tied to the installed original DLL digest and the corresponding upstream build. Verify later window, renderer, audio and input behavior per game before releasing a profile for regular Play. [SDL2 workflow](../.github/workflows/xbox-sdl.yml)

Prepare the selected game's signed diagnostic with `--repeat-launch` and run it with its stock DLL first. Then pass the extracted workflow artifact to the same CLI. For example, the following command runs the Stardew profile on an already installed diagnostic package.

```sh
python3 xbox-deploy.py --env-file "$xbox_env" sdl-trial \
  --version "$version" --app-id 413150 --artifact "$sdl_artifact" --backup-dir "$trial_dir"
```

The command verifies the x64 artifact checksum, embedded SDL version and installed original. It stores the original in the backup directory, applies the rebuilt DLL for a single restart, and checks that the fresh report records Steam Play submission for the selected game. It restores and verifies the original even when launch verification fails. Use the same directory on repeat runs. If the console connection fails during restoration, rerun with the same arguments and `--restore-only`. A process that is forcibly killed can leave the backup directory's `.lock` file. Check that the first process has stopped before removing the lock for recovery.

The pinned app manager’s launch call takes a regular `CGameID`, a launch option index, a source value and an argument string. The probe defaults to index zero. To test Factorio’s publisher-configured x64 Windows executable, prepare the sign-in package with `--launch-app-id 427520 --launch-option 1 --repeat-launch`. The launch report records `steam_launch_option_index`. Other library games use index zero. The source stays zero and the argument string stays empty. The completion event must match the returned handle, game ID and pinned result layout. [Steam CGameID](https://partner.steamgames.com/doc/api/steam_api#CGameID) · [Earlier launch adapter observations](https://github.com/willfaust/madeira-dock/blob/main/docs/CLIENT_LAYOUTS.md)

Before Play, the launch probe reads the installed game’s dependency list from Steam. When Steam lists Common Redistributables (AppID `228980`), the probe asks Steam to download that shared component into the app-local library and waits for its own installed checkpoint. The eventual game launch still uses Steam’s normal checks. The report includes the prerequisite’s initial and final states and Steam’s response to the install request. [Valve Common Redistributables](https://partner.steamgames.com/doc/features/common_redist)

On Series X, `1.0.54.0` restored the saved Steam session and repeated the Slay the Spire launch through the CLI. The probe requested library index `1` in LocalState for Common Redistributables. Steam reported both the shared app’s install directory and its selected library under the read-only package at index `0`, before and after accepting the request. After accepting one explicit queue retry, Steam still reported index `0` and state `514` (`UpdateRequired` and `UpdatePaused`). The app-local library had no shared app manifest or staging directory. Device Portal observed no Java launcher. This run identifies the destination Steam retained for the shared prerequisite. The failed write or subsequent launch behavior has yet to be observed. [SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44869274097) · [UWP file access permissions](https://learn.microsoft.com/en-us/windows/uwp/files/file-access-permissions)

In a fresh app process with `1.0.60.0`, Steam found the already installed shared app at library index `1` in LocalState. It accepted Play with result `0` and briefly reported the game running, then reported no game before it saw a ready window. Device Portal sampled the process list during the same attempt and did not see the Java launcher. No startup log appeared in the game root, log folder, or Java runtime directory.

Version `1.0.62.0` matched a Steam-reported startup PID to the requested game and verified that its process was created after the launch request. It captured exit code `0x00000001`, and Steam again reported `exited-during-startup` without a ready window. The independent Device Portal sampler did not identify `javaw.exe`. The exit code is generic and the process image was unavailable in this run. [SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44870568288)

Version `1.0.64.0` reproduced exit code `1`. The UWP process diagnostics API returned no executable name for the captured process. The probe’s sampled peak memory was 153,772,032 bytes against a 1,073,741,824-byte app limit. These figures describe the probe's app memory budget and do not establish the game process's own allocation or the cause of exit. [SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44870903146) · [Windows process diagnostics](https://learn.microsoft.com/en-us/uwp/api/windows.system.diagnostics.processdiagnosticinfo)

For a local control build, add `--probe-java-runtime` with `--steam-sign-in --launch-app-id 646570`. After the normal Steam attempt exits during startup, this build runs the installed game's `jre/bin/java.exe -version` and records its process result. On Series X, `1.0.66.0` captured exit `1` from the Steam-reported process and exit `0` from the Java version command under the same 1 GiB app limit. The version command exercises Java startup. The game's Java arguments, graphics, and other runtime dependencies are still open. [SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44871184397) · [Game system requirements](https://store.steampowered.com/app/646570/Slay_the_Spire/#app_sysreq)

Version `1.0.68.0` completed the same experiment after deployment and after a CLI restart of the installed package. The second attempt restored Steam sign-in and wrote a new launch report. Both attempts measured a 1 GiB app limit and recorded exit `1` for Steam's startup process and exit `0` for Java's version command. [SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44871687494)

After the console owner changed this package’s App type to Game in Dev Home, a CLI restart of the same `1.0.68.0` package measured a 5 GiB limit. The Steam startup process again exited `1` before a ready window, and the Java version command again exited `0`. After upgrading to `1.0.70.0`, the probe reported a 1 GiB limit again. Check the measured limit after upgrades when comparing App and Game runs. [Xbox resource allocation](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation)

On Series X, `1.0.42.0` initially reported install state `1` after upgrading despite the existing completed Terraria manifest. A Steam install recheck recognized the files and reported zero bytes to download. Version `1.0.44.0` refreshed the library, recognized Terraria and submitted the launch request. Both the automatic attempt and a subsequent library launch received Steam result `29` (`Invalid platform`). The Steam running check stayed false, and Device Portal showed no Terraria process during the automatic attempt. The downloaded Terraria executable has PE machine `0x014c` and CLR flags `0x00000003`, identifying a managed Windows executable that requires 32-bit execution. Microsoft’s Xbox UWP guidance lists x86 as unsupported. [Launch SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44866820611) · [Xbox UWP architecture guidance](https://learn.microsoft.com/en-us/windows/uwp/xbox-apps/frequently-asked-questions#can-i-develop-and-publish-x86-apps-for-xbox)

## Series X observations

To identify an executable's architecture requirement on the console, use `inspect-game` with its installed path and AppID. Terraria's installed `Terraria.exe` reports `xbox_executable_requirement=requires-x86`. Managed IL without the 32-bit-required flag reports `managed-il`. Local play requires a publisher-supplied x64 build or an AnyCPU build with compatible runtime and native dependencies. The header check alone cannot establish that the game will launch.

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

The interactive `1.0.34.0` build showed a live code on the Series X and received the account owner’s approval through Steam Mobile. The pinned native client accepted the request, reported an online session whose account matched the approval, and returned a positive subscription count through `GetSubscribedApps`. The probe then signed out its native session. The report marked `steam_signin=authenticated` and `steam_library=verified`. Content installation and local game launch remain open. [Interactive SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44863836352)

Version `1.0.36.0` authenticated and enumerated subscriptions, then stopped at the metadata interface check. In the pinned image, the per-user interface construction stores `IClientAppsMap` at offset `0x50`, which matches engine slot 17. Version `1.0.38.0` verifies that getter, the returned interface table and its metadata methods. On Series X it requested missing metadata, received titles for most of the subscriptions, and reported `steam_library_rendered=yes`. The read-only CLI command confirmed the same report without launching again. [Library SDK template](https://github.com/clankur/steam-xbox-devmode/actions/runs/44864916465)

## Play the Steam DOS edition of X-COM

For a local interactive build, add `--xcom-dos-runtime /path/to/js-dos-6.22.60.tgz` to the normal `--steam-sign-in --unicode-keyboard --valve-archive …` preparation command. Select **X-COM: UFO Defense** in the library, use **Download** if needed, then **Play**. Other titles follow the existing Play path. The archive is the pinned upstream release. The builder verifies its SHA-512 digest and includes the static browser scripts, license and source links. [Upstream distribution](https://registry.npmjs.org/js-dos/-/js-dos-6.22.60.tgz) · [Corresponding source](https://github.com/caiiiycuk/js-dos/tree/d9d1045ef321bcde3f061dc8e686ded890147d6a)

Steam checks the live subscription for AppID 7760, its fully installed state, the completed manifest and the app-local install directory. The runtime reads the publisher's DOSBox configuration, mounts the installed `XCOM` folder as the DOS C drive and calls the installed `UFOCD.BAT`. The local runtime supports the verified publisher startup sequence. It uses the pinned interpreter's equivalent VGA mode and 16 MiB guest memory. Choose **Return to library** to close the session. Game writes remain in memory for this trial, and the adapter creates empty save-slot folders.

On Xbox Series X, `1.0.106.0` installed the original game from Steam. Steam's Windows DOSBox launcher returned result `29`. Version `1.0.108.0` ran a test command in the packaged DOS runtime. Version `1.0.112.0` verified the live entitlement and ran the game's original DOS intro, confirmed by two distinct frames retrieved from the Xbox canvas. The Steam installation files stayed in the console's application storage.

For a console acceptance trial, additionally supply `--launch-app-id 7760 --install-before-launch`. Only an automatic launch captures the DOS canvas after 8 and 18 seconds. Read the exact local PNG filenames from `steam_dos_frame_8s` and `steam_dos_frame_18s` in the probe report. Inspect the frames for the original game's title or menu before recording a successful visual launch. A guest-started checkpoint records entry into the emulator.

## Inspect installed Windows imports

Pass the local `UnityPlayer.dll` from the installed title to `prepare-steam-core.py --inspect-pe` when preparing a control layout. Repeat the option for additional PE files. The input binaries stay local. The layout contains only the generated import inventory and the probe. Package and deploy the control through the existing CLI, using the same signed VCLibs dependency as the game-launch comparison. Version `1.0.94.0` reported `opengl32.dll_system_load_error=0x0000007E` and the two missing User32 exports. This reports which imports could resolve in the Xbox control process.

Add `--readobj /path/to/llvm-readobj` to include delayed imports in the control. Pass the same option to `steam-compatibility.py scan` or `scan-library` to keep startup imports and delayed imports distinct when interpreting its results.

## Capture the Slay the Spire console diagnostic

For the publisher console diagnostic, prepare a package with `--slay-console --launch-app-id 646570` and a JDK supporting `javac --release 8` on the build machine’s PATH. The probe first requires Steam to accept Play and report a new process that exits with code 1. It then checks the current subscription, completed install and exact SlayTheSpire directory again. A small Java helper starts the publisher’s original `jre/bin/java.exe -jar desktop-1.0.jar` in that directory and captures stdout and stderr through Java’s ProcessBuilder. It stops its own diagnostic child after 20 seconds if still running. Read `report --version "$version" --slay-console` to see the child’s exit status, known launcher errors and exception types. These observations belong to the publisher’s diagnostic command. Steam’s original Play result remains in the main report. [Java 8 ProcessBuilder](https://docs.oracle.com/javase/8/docs/api/java/lang/ProcessBuilder.html)

On Xbox, the relative JAR argument failed even though the file was present. The helper now passes the verified install's absolute JAR path and sets the JVM's `user.dir` to the game directory. This reached the publisher's desktop launcher. The launcher still failed to write the relative `info.displayconfig` file and exited with code 1. The helper drains console output while retaining at most 64 KiB of the child's output and records whether the capture was truncated.

With the matching Windows SDK template, add `--slay-native-cwd` to set the JVM’s native working directory for the diagnostic. The Series X trial progressed past display configuration and reported an LWJGL native-library error. [Console evidence](https://github.com/clankur/steam-xbox-devmode/issues/37#issuecomment-7062225434)

## Trace the Stardew Valley .NET host

Stardew's installed Windows build contains the self-contained .NET 6.0.32 host. Add `--stardew-host-trace` when preparing a package to enable `COREHOST_TRACE` and write to a fresh `LocalState/stardew-host-<run>.log` during its Steam Play request. Examine a running or completed probe with `report --version "$version" --stardew-host`. The summary identifies the apphost, hostfxr, hostpolicy and CoreCLR stages found in up to 4 MiB, plus recognized library failures and HRESULTs. A managed-launch record places execution at the handoff to the game assembly. A missing trace leaves child environment inheritance and native loader startup unresolved. [Microsoft host tracing options](https://learn.microsoft.com/en-us/dotnet/core/tools/dotnet-environment-variables#dotnet_host_trace)

To distinguish managed entry from a failure during the game's startup, build `runtime/stardew/StardewStartupDiagnostic.csproj` with `dotnet build -c Release` and pass its `StardewStartupDiagnostic.dll` to `--stardew-startup-diagnostic` alongside `--stardew-host-trace`. The optionally packaged .NET startup hook runs during the same Steam Play request. The existing report command summarizes the hook entry, selected diagnostics, and process exit markers from its own per-run LocalState log. [Microsoft startup hook configuration](https://learn.microsoft.com/en-us/dotnet/core/tools/dotnet-environment-variables#dotnet_startup_hooks)

On Series X, build `1.0.136.0` reached the game's managed code with both SDL versions. The installed SDL 2.30.9 directly imports `imm32!ImmGetIMEFileNameA`, and this run ended with an SDL-related `TypeInitializationException`. The version-matched rebuild using `SDL_DISABLE_WINDOWS_IME` progressed to `Microsoft.Xna.Framework.Graphics.NoSuitableGraphicsDeviceException` with an inner `NullReferenceException`. This removes the observed SDL loader failure and leaves graphics-device creation unresolved. [SDL build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44899795043) · [Stardew Valley compatibility issue](https://github.com/clankur/steam-xbox-devmode/issues/32)
