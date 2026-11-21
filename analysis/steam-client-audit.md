# Valve Steam client on Xbox Series X Developer Mode

This historical audit records the project’s initial findings. See the [project README](../README.md) for current setup and feature status.

**Result, 21 November 2026.** We found no buildable native Xbox port of Valve’s desktop Steam client in the public materials examined. The Windows client already has an x64 executable. Its operating-system dependencies, Steam’s closed client implementation, and the execution environment for downloaded PC games are the obstacles to an installable port. The repository contains the reproducible audit and a separate Xbox diagnostic. A native Steam client package and on-console gameplay remain unverified.

## What REA measured

We obtained Valve’s Windows x64 client manifest (version `1794536281`), downloaded three of the named archives, matched the byte counts and SHA-256 hashes in the manifest, and inspected five actual client components with REA 6.10.1. The Windows installer was audited separately because its architecture differs from the client that it installs. [Valve client manifest](https://client-update.akamai.steamstatic.com/steam_client_win64) · [Pinned archives and hashes](valve-release.json) · [Per-file results](client-imports.json)

| Component | Architecture | Selected observed dependencies | What a port would have to investigate |
| --- | --- | --- | --- |
| `steam.exe` | x64 | `CreateProcessW`, `CreateWindowExW` | Starting helpers and creating desktop windows |
| `steamwebhelper.exe` | x64 | `libcef.dll` functions including `cef_initialize` and `cef_execute_process` | Hosting Chromium and its process model for Steam’s interface |
| `steamclient64.dll` | x64 | `CreateProcessW`, `OpenSCManagerW`, `StartServiceW`, `ShellExecuteExW` | Process launch and integration with Windows services |
| `SteamUI.dll` | x64 | `RegisterClassExW`, `OpenSCManagerW`, `StartServiceW` | Desktop UI and service interactions |
| `SteamService.exe` | x86 | `CreateServiceW`, `StartServiceCtrlDispatcherW` | A separately shipped 32-bit Windows service |

An import establishes a binary dependency. It cannot tell us whether Steam exercises a particular code path during login or whether Valve could remove that dependency in an Xbox build. The measured x86 service alone does not establish a launch failure. [Installer-specific evidence](REPORT.md)

## What would be required to ship and play

1. **A build of Valve’s client for Xbox.** REA exposes binary structure and can produce pseudocode with an analysis backend. The public inputs we found are compiled releases. The public Steamworks SDK is for software integrating with Steam and depends on a running Steam client. We found no public Valve client build project or Xbox target. [REA capabilities](https://github.com/morluto/rea) · [Valve Steamworks API](https://partner.steamgames.com/doc/api/steam_api)
2. **A supported Xbox application and process model.** Microsoft documents a distinct API surface for Xbox game development. A Steam port would need implementations for its windowing, helper processes, updates, service-dependent operations and game launching, plus an Xbox package identity. Packaging the Windows files supplies none of these adaptations. [Xbox GDK overview](https://learn.microsoft.com/en-us/gaming/gdk/docs/gdk-dev/intro/introduction) · [Microsoft application process models](https://learn.microsoft.com/en-us/windows/apps/get-started/intro-pack-dep-proc)
3. **Local PC game compatibility.** Even an Xbox Steam UI that signs in and downloads an owned game would still need an execution environment for that game’s Windows or Linux build. The concrete acceptance test is Steam’s own library installing a game, launching it locally on a Series X, accepting input, saving, and reloading. Two games with different dependencies would provide initial evidence of library breadth. Neither test has been demonstrated here.
4. **Distribution rights for a modified Steam client.** Valve’s subscriber agreement restricts modification and redistribution, subject to its stated exceptions and applicable law. We identified no public grant covering a repackaged Steam client for Xbox. [Steam Subscriber Agreement](https://store.steampowered.com/subscriber_agreement/)

## Xbox workflow investigation

The [workflow report](xbox-workflows.md) maps the observed desktop dependencies to Xbox Developer Mode and partner GDK APIs. It covers WebView2, prerequisites, package updates, Steam depot downloads and game activation.

## Native translation tests

The [porting investigation](native-port-path.md) records which measured Steam imports link against Microsoft’s UWP SDK, what still needs an Xbox runtime test, and how to interpret a successful result. The [packaged process test](../probe/README.md) runs a single controlled experiment on a Series X in Developer Mode.

The [Windows startup trace](windows-startup.md) follows the same Valve client build from launch to a web-helper window. It records the desktop libraries loaded at startup and the service state on the test VM.

## Reproduce the audit

With Python 3.11 or later and the REA 6.10.1 CLI installed, run the following from the repository root. The client script downloads about 71 MB from Valve’s CDN. It verifies the pinned archive hashes and inspects the selected files in a temporary directory. It never launches them. A matching result produces no output from `diff`.

```sh
python3 analysis/audit-client.py > /tmp/steam-client-imports.json
diff -u analysis/client-imports.json /tmp/steam-client-imports.json
bash analysis/audit-steamsetup.sh /tmp/steam-installer-audit
```

Set `REA_BIN` to the CLI executable when it is outside your PATH. Use `--cache DIRECTORY` to read previously downloaded pinned archives. The separate installer script fetches the live installer URL, so a future Valve installer update can change its result.
