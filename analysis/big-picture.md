# Steam Big Picture on Xbox

Valve ships Big Picture as a mode of the Steam client. This branch packages the complete worldwide Windows x64 release and asks Steam to start in gamepad mode. Steam owns the sign-in, library, settings, overlays and controller interface. The Xbox launcher’s job is to start the client and report what happens. [Valve’s Big Picture guide](https://help.steampowered.com/en/faqs/view/3725-76D3-3F31-FB63)

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

In a separate check on the same console, Steam Core `1.0.230.0` restored the Steam session. All three games opened from their **Play** buttons in that library. A screen capture confirmed each title screen. Each test game process was closed through Device Portal at its title screen and the app returned to the library. The native presenter recorded a 1920×1080 source for all three. Its built-in 30-second sample begins five seconds after the first distinct frame, so it can include loading.

| Game (Steam AppID) | Distinct frames | Sample seconds | Average FPS |
| --- | ---: | ---: | ---: |
| Slay the Spire (646570) | 1,645 | 30.014306 | 54.81 |
| Dwarf Fortress (975370) | 1,797 | 30.014743 | 59.87 |
| Stardew Valley (413150) | 1,797 | 30.006491 | 59.89 |

The native branch contained shared FHD revision `b79d1be`. The regular Steam Core release artifact matched the presenter, SDL and managed-adapter build inputs. Reanalyzing the earlier settled Slay title trace gave 1,798 distinct frames in 30 seconds (59.93 FPS). All 76 shared application payload files matched between that trial and the regular release by SHA-256. The trial added launch-selection and timing markers. Earlier scene-specific measurements also cover gameplay. [FHD performance results](../probe/GAME-LAUNCH.md#native-fullscreen-presentation)

## Steam Machine experience on Series X

Console acceptance starts with Valve’s interface appearing on the TV. From a cold launch, use a controller to navigate between two Big Picture screens, go back, open the Steam menu and enter Settings. The Windows reference can identify a Big Picture window on a desktop system. The Xbox trial must establish foreground presentation and input on the console. [Windows reference](windows-startup.md#native-big-picture-reference)

Feature parity also requires a signed-in session to survive restart, a library title to install and update in writable storage, and a game to launch and return to Big Picture with controller focus restored. Verify overlay, Steam Input, cloud saves, store and social features in the running client. The Xbox package directory is read-only, and the desktop client uses Windows windowing APIs. Both the client’s writable state and its window ownership need device evidence. The shared FHD presenter on the base branch displays hosted game frames. Steam’s Chromium window requires its own presentation check. [Windows startup measurements](windows-startup.md) · [Game presentation](../probe/GAME-LAUNCH.md)

## Release evidence

### Chromium on Xbox

Microsoft supports embedding Chromium in an Xbox UWP app through the WinUI 2 WebView2 control. A WebView2 host is a viable way to display web content in an Xbox-owned window. Steam’s gamepad bundle also expects Valve’s native `SteamClient` object to supply session state, create windows, dispatch controller events and issue library commands. A WebView2 port would need to connect that object to the running client. This branch first tests Valve’s existing `steamwebhelper.exe` and its bridge together. [Microsoft’s Xbox WebView2 announcement](https://github.com/MicrosoftEdge/WebView2Announcements/issues/78) · [WinUI 2 WebView2 setup for Xbox](https://learn.microsoft.com/en-us/microsoft-edge/webview2/get-started/winui2)

### Pinned Steam client

Valve’s Windows x64 manifest `1794536281` pins the `steamui_websrc_all` archive at 35,981,544 bytes with SHA-256 `c4ef0ef15369de9076657e67af5b218d95d71402ed3f92a1b3d096fb06a7b69b`. A local test in Chromium 148.0.7778.97 served the original assets in a fresh offline profile. The `steamui/index.html` entry loaded the shared libraries and `library.js`. Its first exception was `ReferenceError: SteamClient is not defined` at `library.js:5:38536`, when the code tried to register for a login-state change. The root stayed empty. Loading `sp.js` alone registered a webpack chunk and also left the root empty. [Pinned archive manifest](steam_client_win64.manifest)

The bundled source shows the next steps that require the bridge. Steam calls `SteamClient.UI.GetDesiredSteamUIWindows()` and subscribes to window changes. A `MainGamepadUI` request selects `CreateMainGamepadUIWindow`, and the gamepad component renders into the resulting popup. Those steps were identified in the source. The offline run stopped at the login-state call. A WebView2 implementation would need a genuine Steam-backed session and working window management to reach even that first Big Picture screen.

The desktop boot path also uses `steam.exe`, `SteamUI.dll`, and `steamwebhelper.exe`. The checked client binaries import desktop window APIs and Chromium Embedded Framework entry points. The earlier Steam Core prototype stages a subset of the client DLLs. The Big Picture package includes the web helper, CEF, UI bundles and the runtime that supplies Valve’s native bridge. [Measured imports](client-imports.json) · [Client staging](stage-pinned-client.py)

## Earlier custom Xbox library

The Xbox library fills the display after either a restored session or a new sign-in. Its controller-friendly artwork grid reuses the existing Steam library data, artwork cache, and Play and Download actions. D-pad navigation follows the visible rows and columns. Select a tile to focus its action. Back returns focus from the action to the selected title. The presentation uses the app’s own layout and includes no Valve client UI assets.

A local Chromium test with a fixture session verified post-sign-in rendering, movement across rows, action focus, the selected AppID reaching Play, and search showing the Download action for an uninstalled title. On Xbox Series X build `1.0.220.0`, the cached Steam session restored and the Device Portal screenshot showed the fullscreen Xbox library grid with artwork and its selected title's Download action.
