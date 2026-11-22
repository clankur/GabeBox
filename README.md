# Steam on Xbox Series X Developer Mode

This branch opens Valve’s original Big Picture UI in Xbox WebView2 after the existing Steam QR sign-in. On Series X, builds `.21` and `.22` each opened the populated Library on three consecutive cold application starts. The compatibility layer connects Valve’s Play, Resume and Exit controls to the Xbox game presenter. The release ledger tracks whether each title appears, sustains 1920×1080 playback and returns to the Library through those controls. [Big Picture setup and controls](analysis/big-picture.md) · [Per-build Series X verification](analysis/big-picture-reliability.md)

The earlier Steam Core prototype provides a custom library and a fullscreen SDL player. The Series X results below were recorded with that prototype. [Verified prototype revision](https://github.com/clankur/steam-xbox-devmode/tree/547182a15be15939762c8f7ead8bbac236ab88ae)

## Setup

You’ll need an Xbox Series X in Developer Mode, an Xbox profile signed in on the console, and Xbox Device Portal enabled. Run the commands from the repository root on a computer that can reach the console. The deployment CLI requires Python 3.9 or later.

1. Copy `.env.example` to `.env`. Set `XBOX_PORTAL_URL` to the HTTPS address in Dev Home and `XBOX_PORTAL_USERNAME` to the Device Portal username.
2. Run `python3 xbox-deploy.py fingerprint`. Compare the SHA-256 fingerprint with the certificate presented by Device Portal in your browser, then set `XBOX_PORTAL_CERT_SHA256` in `.env`. The CLI prompts for your Device Portal password.
3. Build the `xbox-big-picture-template` from this branch, then prepare a signed local package with Valve’s client archives. The Big Picture guide includes the build commands, Device Portal launch command and expected startup reports. [Build and verify the Big Picture package](analysis/big-picture.md#build-the-native-big-picture-package)

To run the original packaged-process diagnostic, follow its separate deployment instructions. [Probe setup and report fields](probe/README.md)

### Try the earlier Steam Core prototype

Download the `xbox-steam-core-template` artifact from the **Build Xbox probe** run for your checkout. Local packaging requires LLVM objdump, `makemsix`, OpenSSL, and `osslsigncode`. Follow the Steam setup guide to add the hash-pinned Valve files and sign the APPX. Prepare the build with `--unicode-keyboard --steam-sign-in`, then approve the on-screen code in Steam Mobile. [Build runs](https://github.com/clankur/steam-xbox-devmode/actions/workflows/xbox-probe.yml) · [Steam setup guide](probe/STEAM-CORE.md)

To try the DOS runtime, also pass `--xcom-dos-runtime` with the pinned upstream archive when preparing the template. Sign in using a Steam account that owns X-COM: UFO Defense, then select **Download** and **Play** in the library. [X-COM setup](probe/STEAM-CORE.md#play-the-steam-dos-edition-of-x-com)

## Verified features

- **Console diagnostics.** The deployment CLI installs signed packages through Device Portal and reads fresh reports. The baseline probe verified same-package process startup and named-pipe communication. [Console probe](probe/README.md)
- **Steam sign-in and library.** Prototype builds verified Steam Guard QR approval, optional session reuse across an app upgrade, a searchable games-only library, controller navigation, and artwork caching. [Steam library setup and results](https://github.com/clankur/steam-xbox-devmode/blob/547182a15be15939762c8f7ead8bbac236ab88ae/probe/STEAM-CORE.md#run-the-on-console-sign-in)
- **Downloads and launch diagnostics.** Steam installed owned games into app-local storage. The prototype submits standard Steam Play requests, supports per-title launch options, and records startup checkpoints and process exits. [Download tests](https://github.com/clankur/steam-xbox-devmode/blob/547182a15be15939762c8f7ead8bbac236ab88ae/probe/STEAM-CORE.md#test-an-owned-game-download) · [Launch verification](https://github.com/clankur/steam-xbox-devmode/pull/47)
- **Fullscreen play.** The shared SDL player has displayed Slay the Spire, Dwarf Fortress, and Stardew Valley on Series X. Each title requires its version-matched runtime. [Player setup and console results](probe/GAME-LAUNCH.md)
- **Slay the Spire cloud saves.** From an installed game, select **Cloud saves** and then **Download and use saves**. The client verifies the downloaded files and backs up the existing save folders before replacement. [Restore setup and scope](probe/STEAM-CORE.md#direct-steam-cloud-research)
- **X-COM: UFO Defense.** On a Series X, the prototype checked Steam ownership and installation, then ran the installed DOS game in its packaged runtime and rendered frames from the original intro. [Series X results](https://github.com/clankur/steam-xbox-devmode/issues/44)

## Missing features and planned work

- Verify in-game Xbox controller input for X-COM and add persistent saves that survive an app restart. [X-COM follow-up](https://github.com/clankur/steam-xbox-devmode/issues/44)
- Fix game-path resolution after the first shared prerequisite install and refresh the competing AppID when the Steam download queue changes. [Prerequisite path](https://github.com/clankur/steam-xbox-devmode/issues/54) · [Download queue reporting](https://github.com/clankur/steam-xbox-devmode/issues/55)
- Investigate Windows game startup and graphics blockers. Use controlled renderer tests and title-by-title launch checks for the other Windows games under test. [Current compatibility issues](https://github.com/clankur/steam-xbox-devmode/issues) · [ANGLE experiment](https://github.com/clankur/steam-xbox-devmode/pull/53) · [Win32 experiment plan](https://github.com/clankur/steam-xbox-devmode/pull/51)
- Repeat the library and X-COM acceptance tests on a single build from `main`.

## Research archive

The original Steam client audit, import measurements, porting analysis, and reproduction commands are in the [historical audit](analysis/steam-client-audit.md). See the [probe development guide](probe/DEVELOPMENT.md) for the Xbox diagnostic build and deployment workflow.
