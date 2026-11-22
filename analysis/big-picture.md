# Steam Big Picture on Xbox

Valve ships Big Picture as a mode of the Steam client. The integrated Xbox app keeps the existing QR sign-in and opens Valve's original UI in WebView2 after authentication. The complete shell still needs Valve's native client services. A separate diagnostic packages the worldwide Windows x64 release and starts it in gamepad mode to test whether the desktop client can supply those services on Xbox. [Valve’s Big Picture guide](https://help.steampowered.com/en/faqs/view/3725-76D3-3F31-FB63)

## Build the native Big Picture package

Use the `xbox-big-picture-template` artifact built from this branch. Its package identity is `XboxPortabilityProbe.BigPicture`, so it can coexist with the Steam Core prototype. The template contains the Xbox launcher. The local build adds the Valve client under `ValveClient/`. [Template workflow](../.github/workflows/xbox-big-picture.yml) · [Layout builder](../probe/prepare-big-picture.py)

From the repository root, with Python 3.9 or newer, Bash, `makemsix`, OpenSSL and `osslsigncode` available, run the following commands. Use fresh directories for the layout and signed output.

```sh
(cd /path/to/xbox-big-picture-template && shasum -a 256 -c SHA256SUMS)
python3 probe/prepare-big-picture.py /path/to/xbox-big-picture-template/BigPictureTemplate-x64.appx ./big-picture-layout
bash probe/package.sh --layout ./big-picture-layout ./big-picture-signed
(cd ./big-picture-signed && shasum -a 256 -c SHA256SUMS)
```

The builder verifies the archived Valve manifest and each client archive against its SHA-256. To build offline, pass `--archive-cache /path/to/cache` to the Python command. The cache must hold the content-addressed ZIP filenames from the manifest. [Pinned release](valve-release.json) · [Archive list](steam_client_win64.manifest)

### Local verification

On November 21, 2026, the x64 launcher cross-compiled and linked against WindowsApp.lib and uuid.lib on macOS. The local test template used ILAsm to generate the WinRT metadata for the two launcher methods. The Windows SDK workflow uses MIDL for that metadata and MakePri for the resource index. The local build verified the launcher DLL and APPX signatures. All 6,631 files under `ValveClient/` in the signed APPX matched the pinned payload byte for byte.

## Check startup on Xbox

### Test the original UI in Xbox WebView2

Download `xbox-big-picture-webview-x64` and `xbox-big-picture-template` from this branch's Windows SDK builds. The standalone device test runs the pinned Valve document without an account. To build it, pass the extracted renderer artifact directory to the layout builder.

```sh
python3 probe/prepare-big-picture.py /path/to/BigPictureTemplate-x64.appx ./webview-layout --webview-host /path/to/xbox-big-picture-webview-x64 --version 1.0.0.3
bash probe/package.sh --layout ./webview-layout ./webview-signed
python3 xbox-deploy.py deploy ./webview-signed/SteamBigPicture-x64.appx --certificate ./webview-signed/SteamBigPicture.cer
```

Open **Steam Big Picture** in Dev Home, then read the two reports. The first checks WinRT activation. The second records navigation, script startup and display of the gamepad window.

```sh
python3 xbox-deploy.py report --big-picture-activation --version 1.0.0.3
python3 xbox-deploy.py report --big-picture-webview --version 1.0.0.3
```

### Build the compatibility layer for the signed-in FHD app

The Xbox package supplies the Steam client calls that Valve's JavaScript makes after QR sign-in. It loads the original UI from Valve release `1794536281`. The window and display bindings use the Xbox WebView2 composition controller. The SteamID and owned games come from the live Steam session. Play uses the same entitlement check and native presenter as the regular shared-FHD build. [Adapter source](../probe/big-picture-bridge) · [Signed-in host](../probe/signin/steam-big-picture-broker.js)

| Valve interface | Xbox implementation |
| --- | --- |
| User and Apps | Active QR session, owned app overviews serialized with Valve's protobuf, and default launch of an installed game. A session change revokes the old catalog. |
| UI, Browser and Window | Gamepad window request, WebView2 popup attachment, foreground focus, primary display bounds and scaling. |
| Input and System.UI | Standard Gamepad API buttons and axes, direction changes, and releases on disconnect. Guide input depends on whether Xbox exposes that button to the app. |
| System | Reads the host's retail-demo and assigned-access settings for restricted presentation. If neither setting can be read, the adapter requests restricted presentation. |
| URL and Storage | Resolves Valve's pinned URL definitions and public CDN roots, dispatches supported local screen-navigation routes, and stores supported UI preferences under the current account. |

The following command reproduces the application layout for build `1.0.230.24` from source revision `30745ea`. It takes the signed regular FHD `1.0.230.0` APPX, the locally cached Valve archives and the three extracted Windows artifacts listed below. The hashes passed to the builder identify the artifacts’ `SHA256SUMS` files. The packager signs the assembled layout locally. [WebView2 renderer](https://github.com/clankur/steam-xbox-devmode/actions/runs/44962090368) · [Native Steam session](https://github.com/clankur/steam-xbox-devmode/actions/runs/44961774442) · [Primary-window FHD presenter](https://github.com/clankur/steam-xbox-devmode/actions/runs/44963224024)

```sh
python3 probe/prepare-big-picture-library.py /path/to/regular/PackagedProcessProbe-x64.appx \
  /path/to/xbox-big-picture-webview-x64 ./integrated-layout --version 1.0.230.24 \
  --session-artifact /path/to/xbox-steam-core-template \
  --session-checksum edcad827351329e9b775a0b8ffd825850d3dc78ddecac962060f0de881f11d04 \
  --presenter-artifact /path/to/xbox-fhd-present-x64 \
  --presenter-checksum ddaea77306d6ff8bece0c7af6067bdc4aeaae5b856b89c735194f3a820a4d1f4 \
  --archive-cache /path/to/valve-archives
bash probe/package.sh --layout ./integrated-layout ./integrated-signed
python3 xbox-deploy.py deploy ./integrated-signed/PackagedProcessProbe-x64.appx --certificate ./integrated-signed/PackagedProcessProbe.cer
python3 xbox-deploy.py report --steam-core-webview --version 1.0.230.24
```

The integrated package uses the existing Steam Core identity and its installed games. Open Steam Core in Xbox Dev Home, then run the report command. Each cold start produces a new `run_id`. Save the original Xbox Device Portal screenshot at the Library and retain the run ID with it. During a game, `game_frame_sample` and `game_frame_latest_sample` record the first and latest complete foreground intervals. The Xbox media sink can deliver frame notifications faster than the TV refreshes on this build. For a picture of the decoded game video, place an empty `big-picture-capture-game.request` in the package’s `LocalState` folder while the game is in front. Match the new `big-picture-game-video.png` to the same run, AppID and `game_frame_media_capture` receipt. The TV and video-element captures measure different stages. [Verification criteria and results](big-picture-reliability.md)

### Test the Windows Steam bootstrap

After configuring Device Portal, deploy and launch the signed package. [Device Portal setup](../README.md#setup)

```sh
python3 xbox-deploy.py deploy ./big-picture-signed/SteamBigPicture-x64.appx --certificate ./big-picture-signed/SteamBigPicture.cer --test-probe --timeout 60
python3 xbox-deploy.py report --big-picture --version 1.0.0.0
```

The package opens a minimal packaged start page to activate the native launcher through the same Windows Web Application host that ran on the Series X in earlier tests. The launcher starts `ValveClient\steam.exe -gamepadui` with `ValveClient` as its working directory. It records the result in `LocalState/big-picture-report.txt`. Use `python3 xbox-deploy.py probe --big-picture --restart --timeout 60` for a fresh run. [Xbox activation history](../probe/DEVELOPMENT.md#observed-baseline)

| Observation | What it establishes |
| --- | --- |
| `launching` | The Xbox start page reached the native launch step. |
| `client-created` followed by `client-running` | The initial Steam process survived the observation interval. Inspect the TV for the interface. |
| `client-exited` | The initial process exited. The exit code and any visible handoff are needed to interpret the result. |
| A status ending in `-error` | The report identifies the stage and error code for that attempt. |

### Series X deployment and game checks

On November 21, 2026, Device Portal installed Big Picture `1.0.0.0`. Both the initial launch and a cold restart created `ValveClient/steam.exe`, which exited with `0xffffff98` (−104). The pinned bootstrap executable associates that return value with failure to initialize the update UI or create its initial window. The console capture was blank, the subsequent process list showed the Xbox host with no surviving Steam client or web helper, and the app’s LocalState contained only the launcher report. The failure occurs before the library appears. Which Windows call failed is still unknown.

The standalone WebView2 package `1.0.0.3` ran on the same Series X. The Microsoft-built renderer activated, created the composition controller and loaded Valve’s original `/index.html`. Device Portal reported live `msedgewebview2.exe` processes. Valve's first JavaScript exception in this account-free test was `SteamClient is not defined`. All 866 UI, sound and movie files matched Valve's pinned archives. [Windows renderer build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44943347786)

The integrated `1.0.230.1` package restored the existing QR-authenticated session, verified the Steam library, found 134 visible games and handed control to the WebView2 host. On that console the original Valve page reached `document-loaded` and raised `Cannot read properties of undefined (reading 'IsSteamInTournamentMode')`. The adapter implements app overviews and game launch. The shell's native System interface and additional services described below remain unresolved. The host returned to a startup error message. Valve's home, library, menus, and Play button were unavailable. [Windows build for the signed-in test](https://github.com/clankur/steam-xbox-devmode/actions/runs/44943721428)

On the same Series X, integrated build `1.0.230.3` restored the QR session, served 134 owned games to Valve and reported `gamepad-window-created`. The player heard Steam's boot sound. The TV capture showed Valve's own React error view naming `System.RegisterForBatteryStateChanges`. The Home and Library views had not appeared at this point. [Windows build for the popup mapping](https://github.com/clankur/steam-xbox-devmode/actions/runs/44947202496)

In a separate check on the same console, Steam Core `1.0.230.0` restored the Steam session. All three games opened from their **Play** buttons in that library. A screen capture confirmed each title screen. Each test game process was closed through Device Portal at its title screen and the app returned to the library. The native presenter recorded a 1920×1080 source for all three. Its built-in 30-second sample begins five seconds after the first distinct frame, so it can include loading.

| Game (Steam AppID) | Distinct frames | Sample seconds | Average FPS |
| --- | ---: | ---: | ---: |
| Slay the Spire (646570) | 1,645 | 30.014306 | 54.81 |
| Dwarf Fortress (975370) | 1,797 | 30.014743 | 59.87 |
| Stardew Valley (413150) | 1,797 | 30.006491 | 59.89 |

The native branch contained shared FHD revision `b79d1be`. The regular Steam Core release artifact matched the presenter, SDL and managed-adapter build inputs. Reanalyzing the earlier settled Slay title trace gave 1,798 distinct frames in 30 seconds (59.93 FPS). All 76 shared application payload files matched between that trial and the regular release by SHA-256. The trial added launch-selection and timing markers. Earlier scene-specific measurements also cover gameplay. [FHD performance results](../probe/GAME-LAUNCH.md#native-fullscreen-presentation)

After testing the Valve shell, the console received the regular FHD payload as an in-place update to `1.0.230.2`. Every application file matched the signed `1.0.230.0` release. Only the versioned manifest, block map and signature differed. Steam restored its session and 134 visible games. From a fresh launch of the regular library for each game, selecting **Play** reached all three title screens. Each game returned to the library after its test process closed. The Series X remained signed in.

| Fresh run on `1.0.230.2` | Resolution | Distinct frames | Sample seconds | Average FPS |
| --- | --- | ---: | ---: | ---: |
| Slay the Spire | 1920×1080 | 1,686 | 30.000430 | 56.20 |
| Dwarf Fortress | 1920×1080 | 1,799 | 30.013815 | 59.94 |
| Stardew Valley | 1920×1080 | 1,798 | 30.001369 | 59.93 |

## Steam Machine experience on Series X

The repeated-run ledger records each cold launch, authenticated Library capture and game attempt by package version. It includes failed launches, the original image hashes and the evidence needed for a release verdict. [Series X reliability ledger](big-picture-reliability.md)

### Game classification and the Library startup crash

Xbox Dev Home showed **App type → Game** for the installed Steam Core package. On build `1.0.230.19`, one of three cold starts displayed the Library, and the third emitted WebView2’s `out-of-memory` process failure before the Library appeared. The raw Dev Home capture for build `1.0.230.20` has SHA-256 `42e60ad428e08d639b6224d15a91cf2853db4e69a5a25f34389b62fd616c2328`. Microsoft documents separate foreground allowances for UWP apps and games on Series X|S. [Xbox system resource allocation](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation)

Keeping the primary Xbox window in front during startup on build `.20` preserved a measured 5 GiB limit while Steam opened its secondary window and received the signed-in library. In two cold starts, the host then brought that window forward by timeout, at 2,447,257,600 and 2,481,258,496 bytes of total app usage. The limit immediately after the switch was 1,073,741,824 bytes. Both sessions ended before an unobscured Library capture. Build `.21` samples memory before switching windows and lets WebView2 release unused data while it prepares the Library. The release ledger tracks the result of hardware trials on that package.

Build `.21` reached the original Library on three consecutive application launches on Series X. The third preflight contained a runtime broker. All three started with the previous game's process, browser and application host absent. [Per-run observations](big-picture-reliability.md)

### First sign-in and subsequent sessions

The existing Xbox QR flow authenticates the account. The host passes its validated SteamID to the renderer and opens a separate WebView2 profile for that account. Valve records its own guided-tour preference. Startup waits for the player to finish the original tour before switching to All Games. Valve’s Sign Out confirmation returns to the existing sign-in screen. The native client can retain a valid remembered sign-in when Steam temporarily fails to return the catalog. [QR host](../probe/signin/steam-signin.js) · [WebView2 profiles](../probe/big-picture-webview/renderer.cpp) · [Account tests](../tests/steam-big-picture-shell.cjs)

### Background Steam and the running game

Build `.22` adds a presentation path that draws real 1920×1080 SDL frames onto Direct3D surfaces and supplies them to a live video element in the primary Xbox window. The existing FHD receiver handles GPU and CPU frames. The verifier checks the media pipeline’s rendered-frame counter and a separate TV capture of the game. [Primary-window playback](../probe/signin/steam-sdl-native-view.js) · [Native media source](../probe/fhd/media-source.cpp) · [Microsoft MediaStreamSource](https://learn.microsoft.com/en-us/uwp/api/windows.media.core.mediastreamsource)

With an authenticated fixture in offline Chromium, Valve's own game list accepted a running AppID, navigated to `/app/42/overlay/controller` and displayed Resume game, Controller settings, View game details and Exit game. Activating the original Resume button issued the Xbox foreground request for AppID 42. When the host reported the process had exited, Valve cleared its running-game state and returned to the previous screen. This test uses Valve's pinned client UI with a simulated Xbox host. [Original-shell test](../tests/steam-big-picture-shell.cjs)

Valve's shared context owns the gamepad window, its main menu and Quick Access. On the Xbox compatibility branch, the native presenter takes foreground focus when a hosted game produces its first frame. The WebView2 host keeps the same Valve windows alive and asks WebView2 to suspend hidden content. Pressing **View + Menu** in a hosted game requests the running game's Valve controls. The bridge publishes the confirmed AppID in Valve's app-overview protobuf and calls the registered `open/apprunningcontrols` handler. Valve's Resume command switches focus back to that game's existing presenter. The Xbox host supports one active hosted game. To switch titles, exit the current game using Valve's controls and select the next title after the game has exited. These input and focus transitions require verification on each console build. [Window and game lifecycle](../probe/big-picture-webview/big-picture-webview.js) · [Steam navigation](../probe/big-picture-bridge/xbox-url.js) · [Valve's overlay documentation](https://partner.steamgames.com/doc/features/overlay)

On Series X test build `1.0.230.4`, a diagnostic requested launches through Valve's `SteamClient.Apps.RunGame` interface using the restored Steam session. Dwarf Fortress reached its title screen at 1920×1080 and delivered 1,767 distinct frames in 30.001 seconds (58.90 FPS). The physical stick and A generated SDL events, but the tester saw no movement or menu response. The updated FHD presenter draws the emulated pointer in the game view for this title. Slay the Spire opened a native frame and then exited. Stardew Valley reached the ConcernedApe splash after changing Steam Core's Dev Home classification from App to Game, then returned to Dev Home. Neither title produced a valid 30-second sample on that build. These checks exercised the launch API. The original on-screen Play button and a round trip through the running-game menu require separate testing. [App and game resource allocations](https://learn.microsoft.com/en-us/windows/uwp/xbox-apps/system-resource-allocation)

Series X build `1.0.230.6` opened Dwarf Fortress and Stardew Valley through Valve’s launch API. At their title screens, the native presenter recorded 1,783 and 1,748 distinct 1920×1080 frames over 30 seconds, respectively (59.43 and 58.27 FPS). Dwarf Fortress displayed the new virtual cursor. Both games returned to Valve’s Welcome screen when their test processes closed. A physical controller response with the visible cursor and a round trip through the in-game menu still need confirmation. Slay the Spire displayed one frame and exited before its title screen.

On `1.0.230.7`, a fresh Series X launch supplied 134 owned titles to Valve and reached `setup-ready`. The app host and all six WebView2 processes then exited before the Library appeared. The app measured a 5 GiB startup allocation. Device Portal had no matching crash dump or Windows Error Reporting entry. The last recorded renderer warning concerned Steam’s audio feature query. These observations establish the exit but do not identify its cause.

On `1.0.230.9`, the console showed Valve’s language setup and Steam requested a client restart. The next native view creation returned `0x800700AA` (resource in use). The renderer now restarts the signed Steam document in the existing Xbox view. It also forwards Xbox pointer coordinates into the visible WebView2 composition surface and reads physical controllers through Windows.Gaming.Input.

On `1.0.230.10`, Dev Home confirmed the Game classification and Steam Core measured a 5 GiB startup allowance. The signed-in broker returned 134 titles. The TV flashed white and the host’s 45-second readiness timer expired at `app-initialized`. The host then displayed its retry message. In offline Chromium the same branch opened Valve’s All Games tab and its Play control without opening the Steam menu first. The console run did not reach that screen.

On `1.0.230.12`, a raw 1920×1080 console frame showed Valve’s Welcome tour. Its heading and controller prompt were centered near (1440, 810) on a screen whose center is (960, 540). The native composition host now applies the reciprocal of the Xbox rasterization scale when presenting WebView2 and accounts for the same transform when forwarding pointer input. The tour still obscured the library in that capture.

On `1.0.230.13`, the signed-in broker supplied 134 titles. One Game-classified run reported `RenderProcessExited` at 61.8 seconds and the host showed its retry screen. The highest sampled aggregate private working set in another Game run was 1,116,930,048 bytes. The game’s measured startup memory allowance was 5,368,709,120 bytes. A separate App-classified run lost the host and its WebView2 processes at 53.6 seconds. These measurements identify a renderer exit in one run. They leave the underlying cause open. Microsoft distinguishes the failed process type from the reason for its exit. [WebView2 process failure events](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/process-related-events)

Two Game-classified runs on `1.0.230.14` measured the WinRT memory limit at each stage. Both reported 5 GiB at startup and immediately after the first view switch, then 1 GiB by the time Valve created its Big Picture popup in the secondary view. The first run recorded 1,135,697,920 bytes of app memory usage against the 1,073,741,824-byte limit when Valve selected the Library route. It lost the app processes before a library frame could be captured. The second run also exited. This establishes that the app exceeded its reduced allowance in the first run. Neither run retained an OS termination reason. Microsoft documents an expanded memory partition for foreground Xbox games with the `expandedResources` capability. [Xbox Game Mode resources](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/app-capability-declarations#restricted-capabilities)

On `1.0.230.15`, Xbox accepted the expanded-resources manifest. The popup still reported a 1 GiB limit. On one launch the user saw the Steam Library on the TV and selected Dwarf Fortress. The console started the matching game process and the presenter reported a first frame, but the TV stayed black and the session subsequently ended. The Device Portal screenshot request also failed during that session. There is no verified library image or Dwarf Fortress frame-rate measurement from that run.

Build `1.0.230.16` tested attaching the compositor directly to the primary HTML application window so that Steam could keep its Game allocation. Xbox returned `E_INVALIDARG` (`0x80070057`) at composition startup before creating any WebView2 process. The trusted host remained alive. Integrated builds reverted to the separate native Steam window used by `1.0.230.15`.

The existing Xbox QR screen handles sign-in and restores a remembered session. After authentication, the product entry point is Valve's full Big Picture shell. Valve owns home, library, search, game details, Play, downloads, controller settings, Steam menus, store, friends and the in-game overlay. Returning from a game must restore focus to Valve's interface. The Xbox backend can perform the existing entitlement checks, download, launch and fullscreen presentation underneath those controls.

For each of Slay the Spire (646570), Dwarf Fortress (975370) and Stardew Valley (413150), open the title from Valve's library, press its Play button, verify its title screen and return to the same signed-in Big Picture session. Record the native presenter resolution and frame sample for each game. The separate Steam Core runs above establish a baseline for this check.

Console acceptance starts with Valve’s interface appearing on the TV. From a cold launch, use a controller to navigate between two Big Picture screens, go back, open the Steam menu and enter Settings. The Windows reference can identify a Big Picture window on a desktop system. The Xbox trial must establish foreground presentation and input on the console. [Windows reference](windows-startup.md#native-big-picture-reference)

Feature parity also requires a signed-in session to survive restart, a library title to install and update in writable storage, and a game to launch and return to Big Picture with controller focus restored. Verify overlay, Steam Input, cloud saves, store and social features in the running client. The Xbox package directory is read-only, and the desktop client uses Windows windowing APIs. Both the client’s writable state and its window ownership need device evidence. The shared FHD presenter on the base branch displays hosted game frames. Steam’s Chromium window requires its own presentation check. [Windows startup measurements](windows-startup.md) · [Game presentation](../probe/GAME-LAUNCH.md)

## Release evidence

### Chromium on Xbox

Microsoft supports embedding Chromium in an Xbox UWP app through WinUI 2 WebView2. The renderer on this branch creates a WebView2 composition controller in an Xbox-owned window. Steam’s gamepad bundle expects Valve’s native `SteamClient` object to supply session state, create windows, dispatch controller events and issue library commands. The standalone and signed-in trials above measure how far each layer runs on Series X. [Microsoft’s Xbox WebView2 announcement](https://github.com/MicrosoftEdge/WebView2Announcements/issues/78) · [WinUI 2 WebView2 setup for Xbox](https://learn.microsoft.com/en-us/microsoft-edge/webview2/get-started/winui2)

### Pinned Steam client

Valve’s Windows x64 manifest `1794536281` pins the `steamui_websrc_all` archive at 35,981,544 bytes with SHA-256 `c4ef0ef15369de9076657e67af5b218d95d71402ed3f92a1b3d096fb06a7b69b`. A local test in Chromium 148.0.7778.97 served the original assets in a fresh offline profile. The `steamui/index.html` entry loaded the shared libraries and `library.js`. Its first exception was `ReferenceError: SteamClient is not defined` at `library.js:5:38536`, when the code tried to register for a login-state change. The root stayed empty. Loading `sp.js` alone registered a webpack chunk and also left the root empty. [Pinned archive manifest](steam_client_win64.manifest)

The bundled source shows the next steps that require the bridge. Steam calls `SteamClient.UI.GetDesiredSteamUIWindows()` and subscribes to window changes. A `MainGamepadUI` request selects `CreateMainGamepadUIWindow`, and the gamepad component renders into the resulting popup. Those steps were identified in the source. The offline run stopped at the login-state call. A WebView2 implementation would need a genuine Steam-backed session and working window management to reach even that first Big Picture screen.

A separate offline Chromium run of the original entry with the in-client configuration reached initialization of Valve's own services. The WebUI transport requested connection information for SteamUI and the client library. The shared-connection service requested a native connection handle to receive logon and message callbacks. Controller initialization requested a Steam Input service operation, and the UI store requested focus notifications. Several services catch initialization failures and allow the page to continue. A loaded document or a mounted library tile cannot establish that the full shell or its controls are working. Valve describes how the running Steam client serves its APIs over IPC in the [Steamworks architecture documentation](https://partner.steamgames.com/doc/sdk/api).

The desktop boot path also uses `steam.exe`, `SteamUI.dll`, and `steamwebhelper.exe`. The checked client binaries import desktop window APIs and Chromium Embedded Framework entry points. The earlier Steam Core prototype stages a subset of the client DLLs. The Big Picture package includes the web helper, CEF, UI bundles and the runtime that supplies Valve’s native bridge. [Measured imports](client-imports.json) · [Client staging](stage-pinned-client.py)

## Earlier custom Xbox library

The Xbox library fills the display after either a restored session or a new sign-in. Its controller-friendly artwork grid reuses the existing Steam library data, artwork cache, and Play and Download actions. D-pad navigation follows the visible rows and columns. Select a tile to focus its action. Back returns focus from the action to the selected title. The presentation uses the app’s own layout and includes no Valve client UI assets.

A local Chromium test with a fixture session verified post-sign-in rendering, movement across rows, action focus, the selected AppID reaching Play, and search showing the Download action for an uninstalled title. On Xbox Series X build `1.0.220.0`, the cached Steam session restored and the Device Portal screenshot showed the fullscreen Xbox library grid with artwork and its selected title's Download action.
