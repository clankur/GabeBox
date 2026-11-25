# Xbox capability probes

Each probe exercises a piece of the compatibility layer on an Xbox. The named-pipe case starts with the same helper-exit check as the process case and then requires a successful request and reply. The Steam loader case checks that the diagnostic completed, the DLL loaded, and its factory export resolved.

```sh
python3 -m tools.xbox_deploy probe --list
python3 -m tools.xbox_deploy probe --case named-pipe --version "$version" --json
```

Install the corresponding signed package before running a case. The JSON includes the installed package identity, expected checks, observed fields, and the outcome. A failed check or timeout gives a nonzero exit status. For a graphics variant, record the build option alongside the package version so the child exit code can be interpreted for that control.

| Capability | Console control |
| --- | --- |
| Startup, process creation, and named pipes | The packaged process test and startup control below. |
| Process lifetime across a foreground switch | The [multi-app lifetime control](DEVELOPMENT.md#test-a-foreground-switch), run with `xbox_deploy lifetime`. |
| Loader, SDL, XInput, desktop windowing, and DXGI | The [graphics runtime control](graphics-runtime-control/build.sh) and its build options. |
| Steam DLL loading and native Big Picture launcher | The [Steam Core Loader Probe](../docs/STEAM-CORE.md#run-the-on-console-sign-in) and [native launcher](../analysis/big-picture.md). |
| Direct3D 12 pipeline persistence (library and cached PSOs) | The [pipeline-cache control](#direct3d-12-pipeline-cache-control) (`--d3d12-pipeline-cache`). |
| WebView and window activation | The [primary XAML](big-picture-primary/), [WebView smoke](big-picture-webview/), [renderer integration](big-picture-integration-control/), and [windowed](big-picture-windowed/) controls. |

When adding a single-package control, reuse the relevant runtime adapter and report the operation's outcome and a per-launch `run_id`. Register the package and its terminal checks in [`catalog.py`](catalog.py). The [runner](runner.py) handles fresh reports, deadlines, and verdicts. Use [normal Big Picture Play](../docs/GAME-LAUNCH.md#verification) to verify a title after the control passes.

## Xbox packaged process test

This x64 UWP diagnostic calls `CreateProcessW` for one fixed executable bundled with the app. The helper returns `0x000051A7` when its entry point runs. The app displays the result and writes it to `LocalState/probe-report.txt`. It requires no Steam account, Valve files, or network access on the Xbox.

Version `1.0.9.0` was installed and launched by CLI on a Series X in Developer Mode. The report recorded `create_process=success`, `child_exit_code=0x000051A7`, and `exit_code_matches=yes`. [Signed Windows SDK build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44857081600) · [Developer workflow and hardware observations](DEVELOPMENT.md)

Version `1.0.10.0` also verifies a request and reply over one same-package named pipe. A successful report includes `ipc=success` and `ipc_child_exit=0x000051A8`. Add `--require-ipc` to make that result part of the CLI exit status. [Signed IPC build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44858026281)

### Test in Developer Mode

Configure the Device Portal HTTPS address and certificate fingerprint using the [setup guide](../README.md#setup). Supply a Portal username and password if your console has Portal authentication enabled.

Download the `xbox-probe-windows-sdk` artifact for the commit you want to test and verify `SHA256SUMS`. Use the signed APPX from that artifact with the CLI.

```sh
python3 -m tools.xbox_deploy deploy /path/to/PackagedProcessProbe-x64.appx --test-probe
python3 -m tools.xbox_deploy probe --version 1.0.10.0 --require-ipc
```

Use `--certificate path/to/publisher.cer` if the portal requests the package-signing certificate. The CLI verifies the installed version, launches the app, and waits for a fresh report. If needed, read `probe-report.txt` manually in Device Portal under **File explorer → LocalAppData → Packaged process test → LocalState**. [Device Portal app deployment API](https://learn.microsoft.com/en-us/windows/uwp/debug-test-perf/device-portal-api-core#app-deployment)

### Reading the result

- `exit_code_matches=yes` confirms that the bundled helper ran and returned its expected code on the tested console.
- `create_process=failure` reports the `CreateProcessW` error in `win32_error`.
- A fresh startup checkpoint without a final result identifies the last step the app reached. An unexpected helper exit code is a separate test result even when `CreateProcessW` succeeds.

The console result applies to the fixed helper in this package. Steam client startup and local gameplay remain unverified. The [original prerelease package](downloads/PackagedProcessProbe-x64.appx) is preserved for comparison with the earlier native startup path.

## Direct3D 12 pipeline-cache control

Build the graphics runtime control with `--d3d12-pipeline-cache`. Its child links the same `runtime/win32/d3d12-pipeline-cache.c` as `XboxWin32LoaderTrace.dll`. It checks, path by path, whether the console's D3D12 runtime can persist pipelines between processes without losing the device. A profile may select `mode=library` or `mode=blobs` only with a result from this control ([marker format](../docs/GAME-LAUNCH.md#persist-direct3d-12-pipelines-across-launches)).

The top process makes no D3D12 call. It starts one round process for the `ID3D12PipelineLibrary` path and then one for the `GetCachedBlob`/`CachedPSO` path. A path that removes the device, or whose process the runtime ends, therefore cannot affect the other. Each round creates its device the way the pinned Mesa does: an `ID3D12DeviceFactory` from `D3D12GetInterface` with `ALLOW_RETURNING_EXISTING_DEVICE | ALLOW_RETURNING_INCOMPATIBLE_EXISTING_DEVICE`, feature level `1_0_GENERIC` and then `11_0`, and `ID3D12Device3`. Without a factory it uses `D3D12CreateDevice` with the same levels. [Mesa `create_device`](https://github.com/XboxEmulationHub/mesa-uwp/blob/34732bbc207daea10bdf2fb969a4a0f7769ea1de/src/gallium/drivers/d3d12/d3d12_screen.cpp)

The round installs the shared hooks and selects the path as a profile would. It creates a root signature and a trivial compute pipeline. That first creation is a miss, which the cache handles with the calls the title path makes: the bind, the store and the serialization. The cache checks `GetDeviceRemovedReason` after each of them. A reload process then loads the file from TempState and creates the same pipeline, which must be a hit. The shader is a hand-assembled, checksummed `cs_5_0` container from [`pipeline-cache-shader.h`](graphics-runtime-control/pipeline-cache-shader.h), generated and verified by `tests/test_d3d12_pipeline_control.py`. The control covers compute pipelines only; graphics `Load` and `CachedPSO` calls on Mesa's DXIL are checked by the title run.

```sh
WIN32_CC=… probe/graphics-runtime-control/build.sh --d3d12-pipeline-cache WindowsApp.lib MESA_LAYOUT SDK_TEMPLATE.appx 1.0.0.0 build-dir
python3 -m tools.xbox_deploy probe --case graphics-control --version 1.0.0.0 --json
```

Read `child_exit_code`:

| Exit code | Meaning |
| --- | --- |
| `0x5CCXLBFF` | Both paths ran. `L` is the library result and `B` the cached-blob result. `FF` is `D3D12_FEATURE_DATA_SHADER_CACHE.SupportFlags`: 1 single PSO, 2 library, 4 in-process, 8 disk cache, 0x20 and 0x40 the shader-cache control flags. `X` is the cache call that preceded the first device removal: 0 none, 1 `GetAdapterLuid`, 2 `CheckFeatureSupport`, 3 `QueryInterface(ID3D12Device1)`, 4 `CreatePipelineLibrary`, 5 `StorePipeline`, 6 `Load*Pipeline`, 7 `Serialize`, 8 `GetCachedBlob`, 9 creation with `CachedPSO`, A `QueryInterface(ID3D12PipelineLibrary1)`, E the control's own creation, F unknown. |
| `L` or `B` = 1 | A pipeline was stored, serialized, reloaded in a second process and hit. Only this outcome lets a profile select the path. |
| `L` = 2 / 3 / 4 / 5 | `CreatePipelineLibrary` returned `DXGI_ERROR_UNSUPPORTED`, `E_NOTIMPL`, the device has no `ID3D12Device1`, or another failure without losing the device. |
| `B` = 6 | The device does not report single-PSO cache support. |
| `L` or `B` = 7 / 8 / 9 / A | The device was removed during that path: `DXGI_ERROR_DEVICE_REMOVED`, `DEVICE_HUNG`, `DEVICE_RESET` or `DRIVER_INTERNAL_ERROR`, reported by the cache's `GetDeviceRemovedReason` check or as the exit code of a process the runtime ended. |
| `L` or `B` = B | A round or reload process exited with another code; `X` comes from the guard file the cache left before the call it was in. |
| `0x5CSSMRRR` | Failed at stage `SS` on path `M` (1 library, 2 blobs, 0 setup), with the low 12 bits of the HRESULT or detail in `RRR`. Stages: 01 load `d3d12.dll`, 02 create the device, 03 install hooks, 04 serialize the root signature, 05 create it, 06 open the cache, 07 create the pipeline, 08 counters after the miss (`misses << 4 \| stores`), 09 write, 0A start a process, 0B a process timed out, 0C the reload process failed (`its stage << 4 \| its low nibble`). |

If `CreatePipelineLibrary` removes the device here as it did in Slay the Spire 2 on `1.0.246.1`, the code reads `0x5CC47B63`: `X` = 4, `L` = 7, and `B` reports the cached-blob path from its own process. Copy the full code into a profile marker only for a path whose nibble is 1.

A stage 07 failure in the setup process can mean the console's runtime rejects DXBC. The production cache keys whatever bytecode the title passes, such as Mesa's DXIL, so that result does not rule out the cache. It does mean the control needs a DXIL shader.

