# What a native translation would have to prove

The target is Valve’s desktop client compiled for Series X Developer Mode, with a game installed and launched by Steam itself. The experiments below test parts of that proposition independently. A native executable, an import that links, or a working UWP client/server sample are each narrower results than that target.

## The first distinction is linking versus execution

We linked ten selected imports from the REA inventory against Microsoft’s x64 UWP umbrella library, `WindowsApp.lib`, in SDK version `10.0.26100.8249`. Each experiment forced a reference to one imported symbol in an otherwise empty AppContainer DLL with no entry point. Each positive result was checked against the generated PE import table. Each symbol also resolved in its corresponding desktop import library. The x64 tests of names from the 32-bit service represent a hypothetical x64 rebuild. The comparison covers the selected system APIs, not all of Steam’s imports or Chromium’s own library.

| Resolved from WindowsApp.lib | Unresolved with that library |
| --- | --- |
| `CreateProcessA`, `CreateProcessW`, `OpenSCManagerW` | `CreateServiceW`, `StartServiceW`, `StartServiceCtrlDispatcherW`, `CreateWindowExW`, `RegisterClassExW`, `CreateDesktopW`, `ShellExecuteExW` |

The process APIs resolved via `api-ms-win-core-processthreads-l1-1-0.dll`. This makes a normal call to start a bundled helper a worthwhile hardware test. Successful linking grants no additional permissions. A successfully opened Service Control Manager handle would also say nothing about permission to register or start a Windows service. [Machine-readable results](uwp-link-results.json) · [Microsoft’s UWP API table](https://learn.microsoft.com/en-us/uwp/win32-and-com/win32-apis)

To reproduce the link test on Linux with Python 3.11+, LLVM lld-link and GNU objdump, download the pinned SDK package, then run the following in the repository root. The script checks the package hash, runs a desktop control for each symbol, and inspects each linked DLL. The published run used LLD 19.1.7. [Microsoft SDK package](https://www.nuget.org/packages/Microsoft.Windows.SDK.CPP.x64/10.0.26100.8249)

```sh
curl -fLO https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp.x64/10.0.26100.8249/microsoft.windows.sdk.cpp.x64.10.0.26100.8249.nupkg
python3 analysis/test-uwp-link.py microsoft.windows.sdk.cpp.x64.10.0.26100.8249.nupkg > /tmp/uwp-links.json
diff -u analysis/uwp-link-results.json /tmp/uwp-links.json
```

## An isolated function that can be translated

REA identified the real exported function `V_binarytohex` in Valve’s x64 `vstdlib_s64.dll`. We compared the pinned original machine code under bounded instruction emulation against a reference and tested an independent C implementation on the same 268 valid cases. All matched. The independent implementation also compiled to x64 COFF. This is evidence for one deterministic operation with no OS calls on the tested path. It supplies no evidence that a service, window, or game launch can be translated that way. [Function test and reproduction](leaf-test/REPORT.md)

## Test decisions in order

1. **[Packaged child on hardware](../probe/README.md).** The `1.0.9.0` UWP probe launched its fixed, signed x64 helper on a Series X in Developer Mode and collected the expected `0x000051A7` exit code on two CLI runs. This establishes a packaged-helper path for testing client-side workers. Running downloaded PC executables remains unverified.
2. **IPC and lifetime.** The `1.0.10.0` probe completed a same-package named-pipe exchange on two Xbox launches. The `1.0.12.0` probe kept one transaction pending while an owned control app took the foreground, observed suspension and resumption, and completed the original transaction after protocol activation returned to the same process. Both processes belonged to the lifetime package. Testing a request from a separate game package still requires a cross-package transport. Microsoft demonstrates UWP App Services on Xbox, including activation of a provider without its UI. Valve documents a separate RPC/IPC mechanism between the game library and Steam. [Probe evidence](../probe/DEVELOPMENT.md) · [Xbox App Service demonstration](https://blogs.windows.com/windowsdeveloper/2016/09/15/unity-interop-and-app-extensibility-app-dev-on-xbox-series/) · [Valve’s Steamworks technical details](https://partner.steamgames.com/doc/sdk/api)
3. **Valve client milestone.** Trace a cold start of the pinned Valve client on Windows to distinguish required startup dependencies from imports unused on that path. An Xbox client milestone requires suitable build inputs and a working replacement for each mandatory desktop dependency. First verify a genuine Steam library operation from the running Xbox client against an authorized Steam session. Then verify content selection, depot install, and updates independently of game launch. [Workflow mapping](xbox-workflows.md)
4. **Local game milestone.** Verify one game launched from Valve’s running client on the console. For a Steamworks-dependent title, verify normal `SteamAPI_Init`, controller input and save/reload. A second title with different dependencies tests reuse. If the only available launch API terminates the Steam process, a separate, supported client-lifetime design is still required. [Steamworks initialization](https://partner.steamgames.com/doc/api/steam_api) · [GDK launch behavior](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgame/functions/xlaunchnewgame)

## Ahead-of-time translation

Tools that can lift a Windows PE are useful for recovering an isolated operation. For example, rev.ng documents emitted C and an optional recompile artifact whose target is Linux x86-64 ELF. Its documented recompile command does not emit an Xbox package. Even correct native code for an individual function leaves the OS behavior invoked by the original imports unresolved. On the current public evidence, the near-term investigation can test specific functions and Xbox facilities. Building and distributing Valve’s whole client still requires a viable translation or source build for the client, validated replacement behavior, and applicable rights. [rev.ng artifact reference](https://docs.rev.ng/references/artifacts/) · [Valve distribution terms](https://store.steampowered.com/subscriber_agreement/)
