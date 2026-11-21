# Steam client startup on Windows

This test runs Valve’s AMD64 Windows client on an isolated Windows Server 2025 host without signing into a Steam account. It separates the installer or initial launch from a second launch after `steam.exe` and its web helpers have closed. The Windows baseline gives the Xbox port a reference process tree and shows which desktop modules load before the interface is available.

Valve’s public CDN served the same manifest as the existing import audit, build `1794536281` with SHA-256 `d5aa8724a30324cc3247bce8c6a983264926fbd34858f71f554a9a9e96570b0f`. The preparation script checks the manifest hash and each archive’s size and SHA-256. The Windows run also verifies the bootstrap executable’s Valve Authenticode signature. The repository and CI artifacts contain diagnostic records. The binaries remain in the runner’s scratch directory.

## Reproduce

Use a clean, disposable Windows VM with administrator access, PowerShell 7 and Python 3.11 or newer. Start the trace with a fresh work-directory path.

```powershell
./analysis/trace-steam-startup.ps1 -WorkDirectory "$env:TEMP/steam-startup" -PinnedClient
```

The workflow uses Windows Event Tracing to collect process starts, exits and image loads in order. The evidence records only Steam process identifiers, module basenames, window presence and Steam service state. Event timing shows when a module was mapped. It cannot establish which individual calls executed or whether every mapped module is required. Compare SHA-256 values in `client-build.json` with [the static import audit](client-imports.json) before applying older import results to this run. [Microsoft’s ETW documentation](https://learn.microsoft.com/en-us/windows/win32/etw/event-tracing-tools)

## Observations

- Valve’s ordinary installer passed Authenticode validation on the first runner. The test reached its startup deadline. The ETL event-log query returned no usable records, so this trial provides no evidence about the client’s runtime dependencies. [Initial workflow run](https://github.com/clankur/steam-xbox-devmode/actions/runs/44859772265)

The pinned client ran on Windows Server 2025 Datacenter build `26100`. All five executable and library hashes in the runtime inventory matched [the import audit](client-imports.json). The process trace showed a window owned by `steamwebhelper.exe` at the end of both startup stages. The launcher initially updated and relaunched itself. The second run started a fresh `steam.exe` after both the prior main process and its web helpers exited. [Successful workflow and artifacts](https://github.com/clankur/steam-xbox-devmode/actions/runs/44860130408)

| Time from cold-start trace | Observation |
| --- | --- |
| 4 ms | `steam.exe` started. |
| 8 ms | The main process mapped `user32.dll`, `win32u.dll` and `gdi32.dll`. |
| 63 ms | The main process mapped `SteamUI.dll`. |
| 103–116 ms | A child `steamwebhelper.exe` started and mapped `libcef.dll`. |
| 256 ms | The main process mapped `steamclient64.dll`. |
| 522 ms | A web helper also mapped `steamclient64.dll`. |

Steam Client Service was absent before the cold start and at the end of both runs. The capture contained no `SteamService.exe` launch. This establishes that this Windows build reached an unsigned-in web-helper window without the service on this VM. Installers, downloads, ownership checks and game launches were outside the trace.

## Port implications

The desktop `steam.exe` image from the same pinned release imports `CreateWindowExW` and `RegisterClassExW` directly. These are regular PE imports that the Windows loader binds when loading the unmodified process image. The UWP SDK link test could resolve neither desktop window symbol. The runtime result confirms that the desktop GUI stack maps at startup, before the genuine core DLL is loaded. [Static import audit](client-imports.json) · [SDK link results](uwp-link-results.json)

Madeira Dock demonstrates a separate integration point in its iOS/Wine environment. Its host runs Valve’s original `steamclient64.dll` and uses Valve’s own session and ownership checks independently of the desktop Steam interface. An Xbox host would still need to load the genuine client library within the supported app model and validate a library operation against Steam. The smallest test of that path is to load the pinned client on Series X and resolve its exported interface factory without signing in. [Madeira Dock](https://github.com/willfaust/madeira-dock) · [Valve’s Steamworks API](https://partner.steamgames.com/doc/sdk/api)
