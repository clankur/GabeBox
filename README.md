# Steam on Xbox Series X Developer Mode

This project tests Steam sign-in, owned-game downloads, and local game launches on an Xbox Series X in Developer Mode. `main` contains the signed Xbox diagnostic and Steam core loader probe. The interactive Steam library and X-COM runtime were tested from feature branches. [Verified prototype revision](https://github.com/clankur/steam-xbox-devmode/tree/547182a15be15939762c8f7ead8bbac236ab88ae)

## Setup

You’ll need an Xbox Series X in Developer Mode, an Xbox profile signed in on the console, and Xbox Device Portal enabled. Run the commands from the repository root on a computer that can reach the console. The deployment CLI requires Python 3.9 or later.

1. Copy `.env.example` to `.env`. Set `XBOX_PORTAL_URL` to the HTTPS address in Dev Home and `XBOX_PORTAL_USERNAME` to the Device Portal username.
2. Run `python3 xbox-deploy.py fingerprint`. Compare the SHA-256 fingerprint with the certificate presented by Device Portal in your browser, then set `XBOX_PORTAL_CERT_SHA256` in `.env`. The CLI prompts for your Device Portal password.
3. Download the `xbox-probe-windows-sdk` artifact from a successful **Build Xbox probe** run for the revision you want to test. Verify its checksums and deploy the signed APPX. [Build runs](https://github.com/clankur/steam-xbox-devmode/actions/workflows/xbox-probe.yml)

   ```sh
   (cd /path/to/artifact && shasum -a 256 -c SHA256SUMS)
   python3 xbox-deploy.py deploy /path/to/artifact/PackagedProcessProbe-x64.appx --test-probe
   ```

   `exit_code_matches=yes` confirms the bundled helper ran. If Device Portal requests a signing certificate, rerun deployment with `--certificate` pointing to the artifact’s `PackagedProcessProbe.cer`. [Probe setup and report fields](probe/README.md)

### Try the Steam prototype

Check out the verified revision `547182a` and download its `xbox-steam-core-template` artifact. Local packaging requires LLVM objdump, `makemsix`, OpenSSL, and `osslsigncode`. Follow the Steam setup guide to add the hash-pinned Valve files and sign the APPX. Prepare the build with `--unicode-keyboard --steam-sign-in`, then approve the on-screen code in Steam Mobile. [Tested source](https://github.com/clankur/steam-xbox-devmode/tree/547182a15be15939762c8f7ead8bbac236ab88ae) · [Matching build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44881408709) · [Steam setup guide](https://github.com/clankur/steam-xbox-devmode/blob/547182a15be15939762c8f7ead8bbac236ab88ae/probe/STEAM-CORE.md)

To try the DOS runtime, also pass `--xcom-dos-runtime` with the pinned upstream archive when preparing the template. Sign in using a Steam account that owns X-COM: UFO Defense, then select **Download** and **Play** in the library. [X-COM setup](https://github.com/clankur/steam-xbox-devmode/blob/547182a15be15939762c8f7ead8bbac236ab88ae/probe/STEAM-CORE.md#play-the-steam-dos-edition-of-x-com)

## Verified features

- **Console diagnostics.** The deployment CLI installs signed packages through Device Portal and reads fresh reports. The baseline probe verified same-package process startup and named-pipe communication. [Console probe](probe/README.md)
- **Steam sign-in and library.** Prototype builds verified Steam Guard QR approval, optional session reuse across an app upgrade, a searchable games-only library, controller navigation, and artwork caching. [Steam library setup and results](https://github.com/clankur/steam-xbox-devmode/blob/547182a15be15939762c8f7ead8bbac236ab88ae/probe/STEAM-CORE.md#run-the-on-console-sign-in)
- **Downloads and launch diagnostics.** Steam installed owned games into app-local storage. The prototype submits standard Steam Play requests, supports per-title launch options, and records startup checkpoints and process exits. [Download tests](https://github.com/clankur/steam-xbox-devmode/blob/547182a15be15939762c8f7ead8bbac236ab88ae/probe/STEAM-CORE.md#test-an-owned-game-download) · [Launch verification](https://github.com/clankur/steam-xbox-devmode/pull/47)
- **X-COM: UFO Defense.** On a Series X, the prototype checked Steam ownership and installation, then ran the installed DOS game in its packaged runtime and rendered frames from the original intro. [Series X results](https://github.com/clankur/steam-xbox-devmode/issues/44)

## Missing features and planned work

- Verify in-game Xbox controller input for X-COM and add persistent saves that survive an app restart. [X-COM follow-up](https://github.com/clankur/steam-xbox-devmode/issues/44)
- Fix game-path resolution after the first shared prerequisite install and refresh the competing AppID when the Steam download queue changes. [Prerequisite path](https://github.com/clankur/steam-xbox-devmode/issues/54) · [Download queue reporting](https://github.com/clankur/steam-xbox-devmode/issues/55)
- Investigate Windows game startup and graphics blockers. A playable window remains unverified for the Windows titles under test. Begin with controlled renderer tests and title-by-title launch checks. [Current compatibility issues](https://github.com/clankur/steam-xbox-devmode/issues) · [ANGLE experiment](https://github.com/clankur/steam-xbox-devmode/pull/53) · [Win32 experiment plan](https://github.com/clankur/steam-xbox-devmode/pull/51)
- Bring the verified Steam prototype features into a reproducible build from `main`.

## Research archive

The original Steam client audit, import measurements, porting analysis, and reproduction commands are in the [historical audit](analysis/steam-client-audit.md). See the [probe development guide](probe/DEVELOPMENT.md) for the Xbox diagnostic build and deployment workflow.
