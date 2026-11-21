# Series X Big Picture reliability

The second cold start of `1.0.230.19` produced a raw 1920×1080 Xbox output capture of the populated Valve Library. The first cold start exited before showing it. No production build in this ledger has passed the repeated-launch and game-return checks.

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

Signed APPX SHA-256 `b7b6d4f80f00f5fd705d0f75917cb22c26b367f3e2975ea40a8767093b6479ee`. The operator recorded each activation before launching. Both attempts began with zero matching package processes.

| Attempt | Native run ID | Whole-output evidence |
|---|---|---|
| tour-19-175915 | c799c37f-b908-48d1-abac-1e018f33e217 | Failed. The original popup was empty when the host and browser exited. |
| tour-19-180057 | 9c17f75d-1936-455d-944c-02c00c5ed7f2 | Passed. The Xbox showed a centered Welcome prompt, Valve's game details, then the unobscured Library with its selected tile and both footer corners visible. |

This yields 1/2 confirmed Library starts, with one consecutive success. Later observations of the live window within this same session count toward its interaction history.

## Game launches and returns

| Build and attempt | Title | Stock Steam action | Game on TV | Return and FHD measurement |
|---|---|---|---|---|
| 1.0.230.15 · tour-15-173517 | Dwarf Fortress | Player selected the game from Valve's Library. Matching game process started. | TV stayed black after launch. | Session ended. No verified return to the original Library or matched 30-second sample. |
| 1.0.230.17 · tour-17-174558 | Dwarf Fortress | Populated grid scrolled to the Dwarf tile. Tile activation and confirm were attempted. Stock Play execution remained unverified. | No matching game process or visible title. | No game round trip or sample. |

Completed circuits through `.18` are Dwarf Fortress 0/2 attempts, Slay the Spire 0 observed attempts and Stardew Valley 0 observed attempts. Earlier title screenshots and FHD measurements from the regular presenter and direct-launch QA do not enter the stock-UI count.

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

The first `.17` source file was absent before the request. The saved native receipt matched its dimensions, view and live browser process. The second request replaced the first, changed the screen contents and produced a new digest. The renderer frames contain the top tabs and both bottom corners. They do not establish final TV scaling. In the earlier `.12` whole-output image, the Welcome heading was centered at x≈1440 on a 1920-pixel-wide output whose center was x=960.

For `.19`, the operator fetched the raw PNG from Xbox Device Portal's `/ext/screenshot` endpoint. The Welcome heading spans x=806–1114, centered at x=960. The prompt block spans approximately y=512–575 on a 1080-pixel-high output. The later Library PNG includes the top search field, centered tab strip, clock, populated grid and complete bottom controls. Both source images remain uncropped.
