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

Build the graphics runtime control with `--d3d12-pipeline-cache`. Its child links the same `runtime/win32/d3d12-pipeline-cache.c` as `XboxWin32LoaderTrace.dll` and checks whether the console's D3D12 runtime can persist pipelines between processes. It runs two paths: the `ID3D12PipelineLibrary` and the `GetCachedBlob`/`CachedPSO` fallback. For each path, it installs the shared hooks and creates a root signature and a trivial compute pipeline. That first creation is a miss, which the cache stores. The control then serializes the cache to TempState. A second copy of the child, started as a separate process, loads the file and creates the same pipeline, which must be a hit. The shader is a hand-assembled, checksummed `cs_5_0` container from [`pipeline-cache-shader.h`](graphics-runtime-control/pipeline-cache-shader.h), generated and verified by `tests/test_d3d12_pipeline_control.py`.

```sh
WIN32_CC=… probe/graphics-runtime-control/build.sh --d3d12-pipeline-cache WindowsApp.lib MESA_LAYOUT SDK_TEMPLATE.appx 1.0.0.0 build-dir
python3 -m tools.xbox_deploy probe --case graphics-control --version 1.0.0.0 --json
```

Read `child_exit_code`:

| Exit code | Meaning |
| --- | --- |
| `0x5CC0LBFF` | Both paths ran. `L` is the library result and `B` the cached-blob result. `FF` is `D3D12_FEATURE_DATA_SHADER_CACHE.SupportFlags`: 1 single PSO, 2 library, 4 in-process, 8 disk cache. |
| `L` or `B` = 1 | A pipeline was stored, serialized, reloaded in a second process and hit. |
| `L` = 2 / 3 / 4 / 5 | `CreatePipelineLibrary` returned `DXGI_ERROR_UNSUPPORTED`, `E_NOTIMPL`, the device has no `ID3D12Device1`, or another failure. The adapter then uses cached blobs. |
| `B` = 6 | The device does not report single-PSO cache support. Without the library as well, the adapter only times pipeline creation. |
| `0x5CSSMRRR` | Failed at stage `SS` on path `M` (1 library, 2 blobs, 0 setup), with the low 12 bits of the HRESULT or detail in `RRR`. Stages: 01 load `d3d12.dll`, 02 create the device, 03 install hooks, 04 serialize the root signature, 05 create it, 06 open the cache, 07 create the pipeline, 08 counters after the miss (`misses << 4 \| stores`), 09 write, 0A start the second process, 0B second process timed out, 0C second process failed (`its stage << 4 \| its low nibble`). |

A stage 07 failure in the setup process can mean the console's runtime rejects DXBC. The production cache keys whatever bytecode the title passes, such as Mesa's DXIL, so that result does not rule out the cache. It does mean the control needs a DXIL shader.

