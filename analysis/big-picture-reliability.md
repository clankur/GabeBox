# Series X Big Picture reliability

Build `1.0.230.21` produced raw 1920×1080 Xbox output captures of the populated Valve Library on three consecutive application launches. The two preceding builds each reached it on one of three starts. No production build in this ledger has passed the complete repeated-launch and game-return checks.

## Release checks

Keep one entry per activation on the tested console, including startup failures, interruptions and attempts without a usable capture. Record the installed package version and SHA-256, the operator's attempt ID, process absence before activation, fresh host and renderer identities, and the outcome. A new package starts a separate series. Retain previous failures when reporting its success count and consecutive streak.

1. **Cold startup.** Three consecutive independent cold starts on the same signed production package must reach Valve's unobscured, populated All Games screen in the foreground. Each start needs evidence of the TV output, from an original console-output capture or direct TV observation. A fresh on-console WebView2 capture additionally verifies the content of Valve's renderer. Record its scope and dimensions. A route, process count or visible window rectangle alone cannot establish the displayed screen.
2. **Play and return.** Complete two Library → Play → visible title → Library cycles for each of Slay the Spire (646570), Dwarf Fortress (975370) and Stardew Valley (413150) on that package. For each attempt identify the stock Play activation, matching game process, recognizable game on the TV, and the foreground Valve Library after leaving the game. Record interruptions, black screens and unsuccessful Play attempts in the denominator.
3. **Presentation.** Correlate the game session with its own 1920×1080 presenter sample spanning at least 30 seconds. Report elapsed time, distinct frames actually presented and their rate. A source-frame sequence or a single first-frame notification measures a different stage. Neither supplies a sustained presentation measurement. Retain a raw title frame or direct TV confirmation alongside the sample.

## Cold starts

The table covers every controlled attempt in the operator's retained release ledger through `.18`. Run names identify separate activations. The two positive observations below distinguish the TV from the renderer because either can succeed while the other fails.

| Build | Attempt | Observation |
|---|---|---|
| 1.0.230.12 | parent-series-12 | Raw TV capture showed Valve's Welcome prompt over a blurred Library. Steam then exited. |
| 1.0.230.12 | tour-12-171753 | Host showed the retry screen without a live WebView. |
| 1.0.230.12 | tour-12-172007 | Interrupted for the next package installation. Outcome unverified. |
| 1.0.230.13 | tour-13-172235 | Game classification. App exited with the popup still empty. |
| 1.0.230.13 | tour-13-172447 | Game classification. No visible Library. The host returned to its retry state. |
| 1.0.230.13 | tour-13-172805 | App classification. Host and renderer exited. |
| 1.0.230.14 | tour-14-173115 | Game classification. Host exceeded its sampled 1 GiB allowance, then exited before capture. |
| 1.0.230.14 | tour-14-173247 | Game classification. App exited before a visible Library. |
| 1.0.230.15 | tour-15-173517 | Player confirmed the Library on TV and selected Dwarf Fortress. The TV screenshot request failed. |
| 1.0.230.16 | tour-16-174110 | Native primary-window composition failed before WebView2 startup. |
| 1.0.230.17 | tour-17-174437 | Host exited before a visible Library. |
| 1.0.230.17 | tour-17-174558 | Two new PNGs from the on-console foreground Valve popup showed different scroll positions. A whole-output TV image was unavailable. |
| 1.0.230.18 | tour-18-175104 | Host exited before a visible Library. |
| 1.0.230.18 | tour-18-175222 | Host exited before a visible Library. |

Across these 14 controlled attempts, the Library has one direct TV confirmation and one further start with foreground-renderer images. Twelve attempts lack either confirmation, including the interrupted `.12` trial. Per-build observed Library starts, including the renderer-only observation, are `.12` 0/3, `.13` 0/3, `.14` 0/2, `.15` 1/1, `.16` 0/1, `.17` 1/2 and `.18` 0/2. The two `.17` PNGs count once. An automatic restart after the `.15` game attempt is recorded as a session outcome and adds no independent cold start.

Earlier retained observations predate the controlled ledger. Integrated builds `.1` and `.3` failed during startup, `.5` showed first-time setup, and `.7` exited after setup. `series-9` showed setup followed by a failed native view restart, `series-10` showed a startup timeout, and `series-11` returned to Xbox Dev Home. None yielded a raw unobscured Library frame. Their historical attempt counts are incomplete. The regular FHD and direct-launch QA packages exercised other entry points. [Earlier console observations](big-picture.md)

### Build 1.0.230.19

Signed APPX SHA-256 `b7b6d4f80f00f5fd705d0f75917cb22c26b367f3e2975ea40a8767093b6479ee`. The operator recorded each activation before launching. All three attempts began with zero matching package processes.

| Attempt | Native run ID | Whole-output evidence |
|---|---|---|
| tour-19-175915 | c799c37f-b908-48d1-abac-1e018f33e217 | Failed. The original popup was empty when the host and browser exited. |
| tour-19-180057 | 9c17f75d-1936-455d-944c-02c00c5ed7f2 | Passed. The Xbox showed a centered Welcome prompt, Valve's game details, then the unobscured Library with its selected tile and both footer corners visible. |
| tour-19-181112 | ac954194-b60f-4dd6-b773-6ea8c7105ac5 | Failed. WebView2 reported an owner renderer out-of-memory failure at +61.4 s, before the Library became visible. |

This yields 1/3 confirmed Library starts. The longest streak was one, and the last attempt failed. Later observations of the same live window count toward its interaction history.

### Build 1.0.230.20

Signed APPX SHA-256 `ae750cafd15a57f7c567ec7bba8dbf2408dd19f8efc5954193c2aa51e4cae9dd`. Xbox Dev Home showed the installed package as a Game. Each activation had an empty package-process baseline.

| Attempt | Native run ID | Whole-output evidence |
|---|---|---|
| tour-20-181550 | ee8daa7a-6861-4172-af76-f88ddc2a1acb | Failed. At the timed foreground handoff, the usage was approximately 2.45 GB and the reported budget fell from 5 GiB to 1 GiB. App processes exited before the Library appeared. |
| tour-20-181836 | e9ba2fe9-4bd6-4ad4-92f2-372098dc31a1 | Failed. The internal route was selected but the app exited before an unobscured Library capture. |
| tour-20-182012 | fb60dedf-9749-43c4-88c8-4d24c25b9f08 | Passed. The official console output shows six columns of original Valve game tiles with tabs, search, clock and complete bottom controls. |

The observed Library count is 1/3, with one consecutive success. The durable original is `production-230-20/steam-big-picture-library-1920x1080.png` under the local console evidence directory.

### Build 1.0.230.21

Signed APPX SHA-256 `46775d5011227204fd7b63b83864d3d30265d5ec9962f00e46ea24a9d3487679`. Both initial activations had an empty package-process baseline.

| Attempt | Native run ID | Whole-output evidence |
|---|---|---|
| tour-21-182442 | f211d5af-411d-4c58-bdb2-445847e57e5b | Passed. Full six-column Valve Library with the selected first tile, game art, top tabs and bottom controls. |
| tour-21-182817 | 56b05851-91c7-4530-be8e-b41c3e499d7d | Passed. Populated full-screen Library, with Steam menu and B Back on its controller footer. |
| tour-21-183502 | 5737a53b-f3ca-4c30-b640-4161edd6d035 | Passed. Raw TV capture showed All Games 134, Installed 8, game art and controller footer. Only RuntimeBroker.exe remained at preflight. The app and its browser were absent. |

Three of three consecutive app activations produced the original Library. Two of the three had strictly empty package-process preflights. The durable first and second captures are `production-230-21/steam-big-picture-library-1920x1080.png` and `production-230-21/steam-big-picture-library-controller-1920x1080.png`.

### Build 1.0.230.22

Production APPX SHA-256 `246874ba556f2c3a6691f95be78e5c40a702d650fb7ea18540d5deb3134ba347`. All 1,175 original Valve assets match the previous signed build byte for byte.

| Attempt | Native run ID | Whole-output evidence |
|---|---|---|
| tour-22-183719 | cc3da6fe-67cf-4b3c-b8b1-c0ae24d7b235 | Passed from an empty package-process baseline. The TV shows All Games 134, Installed 8, game art and controller footer. |

The durable original is `production-230-22/steam-big-picture-library-controller-1920x1080.png`.

## Game launches and returns

| Build and attempt | Title | Stock Steam action | Game on TV | Return and FHD measurement |
|---|---|---|---|---|
| 1.0.230.15 · tour-15-173517 | Dwarf Fortress | Player selected the game from Valve's Library. Matching game process started. | TV stayed black after launch. | Session ended. No verified return to the original Library or matched 30-second sample. |
| 1.0.230.17 · tour-17-174558 | Dwarf Fortress | Populated grid scrolled to the Dwarf tile. Tile activation and confirm were attempted. Stock Play execution remained unverified. | No matching game process or visible title. | No game round trip or sample. |
| 1.0.230.19 · tour-19-180057 | Dwarf Fortress | Original green Play button activated. Matching game process and presenter were present. | Raw 1920×1080 console image showed the Dwarf Fortress title and main menu. | 1,797 distinct presented frames over 30.013639 s (59.87 FPS). A new run ID and disappearance of the package processes interrupted the session before a visible return to Library. |
| 1.0.230.20 · tour-20-182012 | Stardew Valley | Original game details led to a matched Stardew Valley game process. | Console output reached a white startup frame containing a pixel cursor and two icons. The title menu was unverified. | No matched 30-second presentation sample or visible return before the game and host processes disappeared. |
| 1.0.230.21 · tour-21-182442 | Slay the Spire | The operator activated the original green Play button. The expected game process appeared. | The first native frame was hidden and no Xbox capture established the game title. | The game process disappeared. No visible return or matched 30-second presentation sample. |
| 1.0.230.21 · tour-21-182817 | Dwarf Fortress | The operator activated the original green Play button and the matching game process appeared. | The raw Xbox frame shows the title menu, including About DF, Settings and Quit. | 1,797 distinct presented frames in 30.013570 s (59.87 FPS). The operator terminated the game process, then used Back from game details to reach the Library. A normal exit and return remain unverified. |
| 1.0.230.22 · tour-22-183719 | Slay the Spire | The original game details were captured and the matching JVM started. | No recognizable title frame was established by the foreground sample. | The primary media presenter advanced by two frames over 30.031600 s (0.067 FPS), reaching three total. The producer sequence remained at three across four later reads. A normal exit and return remain unverified. |

Completed circuits through `.18` are Dwarf Fortress 0/2 attempts, Slay the Spire 0 observed attempts and Stardew Valley 0 observed attempts. Earlier title screenshots and FHD measurements from the regular presenter and direct-launch QA do not enter the stock-UI count.

On `.19` Dwarf Fortress reached the game on TV on 1/1 attempt and completed 0/1 round trips. The player observed the pointer. No in-game overlay or return was independently confirmed in this run. Slay the Spire and Stardew Valley have no recorded `.19` Play attempts.

## Capture provenance

The `.17` APPX SHA-256 is `b5f596ce3a05f56272bc5194bde0fd1b4fa0f56bd9ca8aef904a927c88875509`. Its Valve UI payload matches the pinned assets in the staging cache. The native host captures the real Big Picture popup only while its signed parent is active, the popup is attached and visible, and the game's surface is in the background. It checks for a visible stock Library tile before and after capturing. The native call writes PNG bytes directly. The collector transfers those bytes from the installed package's LocalState. [Native foreground checks](../probe/big-picture-webview/renderer.cpp) · [Valve Library hit test](../probe/big-picture-webview/library-capture.h)

| Local receipt | Dimensions | SHA-256 |
|---|---:|---|
| `original-valve-library-17-1795301263.png`, initial Library | 2880×1620 | `d09b824b316ff732395b08a058e77319ad22cd65f6f42d8d096bb9e91d726040` |
| `original-valve-library-17-1795301352.png`, scrolled Library | 2880×1620 | `afb8ae3801bc3a5ff79ec22a6777ca5ade808a835ece0bf4cb0ee3a312fadf09` |
| `screen-36.png`, earlier .12 whole-output Welcome capture | 1920×1080 | `0856113e24840b9027d8dfabb7e1bbe58b07e10b7ee6f1bb029439dd91ba18fe` |
| `welcome.png`, .19 whole-output centered tour | 1920×1080 | `8ea2371933cf51a18f6087394098dd03874f0b7e2a044fdcce09e0c9e9424091` |
| `physical-tv-1795302175230899000.png`, .19 whole-output game details | 1920×1080 | `094fafc7ca951f3c1d7e5081f668b8560eb526d23b84661c14fbf5bbcaf58d09` |
| `physical-tv-1795302202507642000.png`, .19 whole-output stock Library | 1920×1080 | `b5b301247cc09624cbf48fc1ad2d2b954b11ec9a7a0c715c952eff3856e021ad` |
| `physical-tv-1795302269582823000.png`, .19 Dwarf Fortress details with Play | 1920×1080 | `789c56b960630c98110802dc6f5380e3aa096beb6716ee09b53d97aa9659fc27` |
| `physical-tv-1795302410685794000.png`, .19 Dwarf Fortress title and menu | 1920×1080 | `120584cf51757da46ee8b8917b2a1adb066b76ecd000cfc3770bac020c0c6c45` |
| `valve-998.png`, .20 whole-output stock Library | 1920×1080 | `f58de98cadb889a6ab8571c8374c54b25fc41de987fa5f82b2d21739b429815b` |
| `physical-tv-1795303418789208000.png`, .20 Stardew Valley early startup | 1920×1080 | `0b0d02baaa38c6b9aded2f7eeb97b74b8e1e1721e405958eee517912260bf7ba` |
| `valve-939.png`, .21 first cold start whole-output Library | 1920×1080 | `20749923c604b1726da90af87fd411be7e1141b76f002b22e974c29a4b67c42b` |
| `valve-1231.png`, .21 second cold start whole-output Library | 1920×1080 | `694353573676d3f050757b49edf487fb41784da592ee1ebeb0056e739712d5e9` |
| `valve-937.png`, .21 third application start whole-output Library | 1920×1080 | `e5609a5f21db9e2d1316610f65b9af70ef9ab1291f0bf383544b3fb5e7ae4dae` |
| `physical-tv-1795303954859752000.png`, .21 Dwarf Fortress title and menu | 1920×1080 | `ef5b70058a6c36ae82c83fcc2f57b4b374d87a5fa901b8d1ea0d8fdc81f83c1d` |
| `physical-tv-1795304092607387000.png`, .21 Library after assisted return | 1920×1080 | `58c0f1ea182e1f1ec02c051750c6c575030059c9b0342909679b93c76da9a5e5` |
| `valve-846.png`, .22 first cold start whole-output Library | 1920×1080 | `5cb377878cf552832c82154e9d6234fea7b4ae8a42a42daf3b0aaca8f057de47` |

The first `.17` source file was absent before the request. The saved native receipt matched its dimensions, view and live browser process. The second request replaced the first, changed the screen contents and produced a new digest. The renderer frames contain the top tabs and both bottom corners. They do not establish final TV scaling. In the earlier `.12` whole-output image, the Welcome heading was centered at x≈1440 on a 1920-pixel-wide output whose center was x=960.

For `.19`, the operator fetched the raw PNG from Xbox Device Portal's `/ext/screenshot` endpoint. The Welcome heading spans x=806–1114, centered at x=960. The prompt block spans approximately y=512–575 on a 1080-pixel-high output. The later Library PNG includes the top search field, centered tab strip, clock, populated grid and complete bottom controls. Both source images remain uncropped.

## Account and first-install coverage

The `.19` TV and frame receipts use a previously authenticated Steam account on the test console. A cold process start preserves saved credentials, game installations and browser storage. They do not verify a fresh installation, a second real account, or an account with no games.

The signed `.19` archive contains no Steam user directory, WebView2 profile, license catalog or automatic game-launch configuration. Inspection found no fixed individual SteamID in the app-authored JavaScript. The native client checks the live Steam identity against the requested identity before asking for its licenses. The broker rechecks the native owner around asynchronous library and launch operations. The three game-specific presenter adapters explain explicit AppIDs in the game path. [Native sign-in](../probe/ProbeNative.cpp) · [Broker](../probe/signin/steam-big-picture-broker.js)

The independent source audit found these limitations on the tested build:

- A zero license count cannot complete native sign-in. When metadata arrives for only some licenses before the 20-second deadline, the session library includes only named games. The array is acquired once for the session.
- The source captures a populated Library by hit-testing a real game tile. A legitimate empty Library needs separate evidence of Valve's empty state and confirmed account ownership.
- The native local cloud cache keys records by SteamID and checks the account again after storage access. Other stock browser storage uses a shared package WebView2 profile. The tested package also supplies completed device-setup defaults for Windows.Xbox on empty machine storage. Hardware account switching and truly fresh Steam first-run behavior remain unverified.
- The stock Sign Out dialog invokes `SteamClient.User.SignOutAndRestart()`. In the tested package only the legacy package page invokes native sign-out. Opening Big Picture while an old session exists reuses the prior broker, and failed catalog reads alone do not clear previously rendered tiles.

Fourteen local broker, startup and preference tests passed with isolated identities. A hardware account-switch check must confirm that the previous owner's titles disappear, that the new owner sees only their own catalog, and that unowned titles cannot launch. A first-install check must start with the test package's prior session and UI storage absent and proceed through actual QR authorization. No credentials or account identifiers are required in the resulting public receipt. [Broker tests](../tests/steam-big-picture-broker.test.js) · [Startup tests](../probe/big-picture-webview/core-startup.test.js) · [Preference tests](../probe/big-picture-webview/services.test.js)

Valve documents `LicensesUpdated_t` as a change notification for owned packages. It carries no payload. The documentation does not establish that it fires on an initial empty account or that all application metadata has arrived. The original Valve client also defines a typed CM license-list response with a result and license list. The current native callback reader does not receive that response. The Web API for owned games requires a key and depends on game-detail visibility. A successful connection or an elapsed timeout therefore cannot certify an empty private catalog through the existing native reader. [ISteamUser callbacks](https://partner.steamgames.com/doc/api/ISteamUser#LicensesUpdated_t) · [IPlayerService.GetOwnedGames](https://partner.steamgames.com/doc/webapi/IPlayerService#GetOwnedGames)

Build `.22` contains the stock sign-out binding and the CI-built native credential fix. Its Windows DLL SHA-256 is `75fe5ff246780b562023f44169b717c599f685a0077dcb5822ffd0ba917b23c3`. The first recorded production launch used the existing account. A separately signed fixture package with identity `XboxPortabilityProbe.SteamFresh23022` has SHA-256 `26696eef0163dbadd78acfb8a8ec98437c9c0ef33095b968481caedd9b8e842b`. The identity and two display labels are its only manifest changes, and all application payloads match production. Its distinct sandbox permits a first-install test without modifying the production account's data. Building this package alone supplies no evidence of fresh QR authorization.
