# Cached sign-in and Big Picture startup

The reported startup takes about 45 seconds. Earlier Series X trials identify two waits in this path. Package `1.0.230.124` took 16 ms to read and decrypt saved credentials, 109 ms to verify the bundled client image and 2,265 ms from logon to licensed AppIDs. It then spent 21,016 ms reading metadata for 627 licensed AppIDs and returned 135 named games. Fourteen unresolved AppIDs still had unknown types. The exact timing of the reported run is unmeasured. [Historical startup trace](big-picture.md#cached-sign-in-and-big-picture-startup-timing)

## Native catalog

Steam verifies the remembered identity and returns live entitlements before the host reads metadata. The metadata loop gates the first catalog on names for every licensed AppID, including tools and other entries the UI will filter out. It retries every 1,500 ms for up to 20 seconds. Keep waiting for an initial usable result when the account has no named games. On remembered sign-in with a usable batch, publish named games whose type and entitlement the current Steam session has confirmed. Continue the existing bounded wait for unresolved games or unknown types in the same session, then deliver newly identified games to the same broker and Valve's app-overview subscription. First QR and any empty result retain the full classification opportunity. Existing live ownership checks still govern Install and Play. [Native sign-in](../runtime/steam/ProbeNative.cpp) · [Ownership broker](../ui/signin/steam-big-picture-broker.js)

## Valve renderer

The pinned Valve UI release `1794536281` initializes three UI cloud-config namespaces in order. Its first-sync helper waits 10 seconds when a version is unknown, then permits its caller to proceed. A saved namespace document removed one wait on Series X `1.0.230.131`. Two missing documents still left about 22 seconds in post-login services and 48.416–50.751 seconds to native gamepad readiness. The UI renderer's connection manager is distinct from the genuine native client that verified sign-in. [Pinned release](../runtime/steam/pins/valve-release.json) · [Console service timings](big-picture.md#cached-sign-in-and-big-picture-startup-timing)

The pinned `40992` module schedules cloud sync when it creates the local namespace. `EnsureSubscribedToNamespace` awaits `WaitForSync` and ignores the result. Retain that wait, its state, and Valve's scheduled sync. Let the three known callers continue with local data after a short bound when the same account is verified and the UI connection manager confirms it is disconnected. Connected and unrecognized cases keep Valve's normal wait. Retain startup timing receipts for these early returns. The scope is UI configuration and preferences. Game saves use the separate native Steam Cloud path. [Scoped UI cache](../ui/big-picture-bridge/xbox-cloud-cache.js)

## Verification

The pinned Chromium fixture with a returning account and a disconnected UI connection manager initialized Valve's post-login services in 2,407 ms. It recorded local continuation for namespaces 3, 1 and 4, opened stock All Games, displayed a late catalog entry after the normal overview refresh, and submitted Play for the original installed fixture title. The fixture simulates the Xbox broker and uses the stock compiled Valve UI. The native account and title-metadata stages need a signed console run. [Pinned UI fixture](../tests/steam-big-picture-shell.cjs) · [Native-to-broker tests](../tests/steam-big-picture-broker.test.js)

Run the returning-account fixture with local pinned assets and the `--startup-local-cloud` argument. The focused Node tests cover a reconnect before the fallback, a late completion after an account change, and the host's startup diagnostics. The startup observer includes `steam_library_pending_metadata` at initial publication, `catalog_enriched` when the late scan finishes, and `cloud_local_namespace_*` when an offline UI wait takes the local path. Initial native gamepad readiness can precede the late catalog receipt, so also retain the final WebView report from that run. [Startup observer](../tools/xbox_diagnostics.py)

An independent A/B run reached the same stock Library and Play checkpoints in both trials. Valve's post-login renderer services took 30,158 ms with the original waits and 2,439 ms with the local continuation. The pinned UI, simulated account, and disconnected connection manager were held constant. The reviewer also passed 58 focused Node tests and 38 diagnostics tests. The Windows-target syntax check used an existing WinRT projection. The signed console build regenerated the metadata and exercised the new methods on Xbox.

## Series X results

On November 24, 2026, Xbox Series X OS `26100.9673` ran three cold process activations per package with the same remembered account and pinned Valve UI `1794536281`. The elapsed time starts at Device Portal activation and ends when the observer reads the fresh native gamepad-ready checkpoint.

| Package | Activations to gamepad-ready | Initial metadata stage | Visible games |
| --- | --- | --- | --- |
| `1.0.231.25` | 52.219 s, 52.094 s, 54.354 s | 21.015–21.016 s | 135 |
| `1.0.232.3` | 15.234 s, 14.757 s, 12.055 s | 16 ms | 135 |

The baseline package SHA-256 was `fb1f19956fbb2050cea94541729bfa5dc3944d65165feb1bb2d6f190e3017bb6`. The corrected package SHA-256 was `a1be4f576e69541884ab77ca1e4e5be0fa8cd46244cd8ea436a4dbeb3740c098`. All corrected runs restored the cached Steam account. The last run, `72077ff2-6920-4636-b55a-9c9f8021479a`, recorded 14 initially pending AppIDs, continued locally for UI configuration namespaces 3 and 4, and reached the late catalog checkpoint at 23.269 s on the shell clock. A full TV frame later showed 135 games under All Games. Stock Play reached Slay the Spire’s title screen on the same run.

The first signed candidate stalled on the account page. An early script receipt identified `Access is denied` while registering the legacy `WebUIApplication` activation event inside the XAML-hosted account page. The correction gates that event registration to the legacy StartPage. The XAML host continues to send activation arguments through LocalSettings. Broker regressions cover both entry points and event-registration failure. [Account page activation](../ui/signin/steam-signin.js) · [Broker tests](../tests/steam-big-picture-broker.test.js)

Further work includes first-time QR, sign-out and account replacement, a connection interruption, and a live catalog in which late metadata adds a visible game. The separate timing analysis describes the remaining startup stages and candidate measurements. [Startup latency opportunities](steam-ui-startup-latency-opportunities.md)

## Additional console cases

Compare at least three cold process activations on the same Series X and remembered account, using a signed package from each revision. Use the normal startup configuration for the timing run. The optional full inventory export and configured launch trials can wait for the late catalog. Start the startup observer before activation. Retain the package pins, runs, cache opening and live identity timing, first and final named game counts, the renderer's local namespace waits, stock Library route, gamepad-ready timing and a populated full TV frame. Confirm that late games appear under the same account and that the old account's catalog disappears on sign-out. Repeat first-time QR, an account with no visible titles, and a connection interruption. Exercise the original Install and Play controls after the library appears. [Xbox startup observation](../docs/XBOX-DIAGNOSTICS.md#observe-big-picture-startup)
