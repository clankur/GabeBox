# Xbox probe development

The probe tests one `CreateProcessW` call against a fixed helper packaged with the app. The helper is expected to return `0x000051A7`. Its runtime evidence comes from `probe-report.txt` in the app’s `LocalState` on the target Xbox. Deployment status, a splash screen, and a return to Dev Home are separate observations.

## Build, deploy, and collect a result

Run these commands from the repository root with the Xbox in Developer Mode, signed in to an Xbox profile, and with Device Portal enabled. Use Python 3.9 or later for the CLI and GitHub CLI to download the build. The [Windows SDK workflow](../.github/workflows/xbox-probe.yml) runs on pushes that touch the probe or workflow. It sets `StartPage` to the packaged `index.html`, builds `ProbeNative.dll` and its WinRT metadata, generates `resources.pri` with `MakePri.exe`, packages with `MakeAppx.exe`, and signs with `SignTool.exe`.

Use the Git-ignored `.env` documented in [the probe README](README.md). `--env-file` lets a checkout use the setup workspace’s existing file without copying it into the repository. Keep saved credentials in the local file and share only the CLI’s diagnostic output.

For an updated build, increment `Identity Version` in [`Package.appxmanifest`](Package.appxmanifest) so the install can be identified on the console. Keep the package name and publisher consistent during an in-place upgrade. Use the workflow run whose commit matches the change being tested. Download the signed packages to separate directories because the artifacts use the same filenames.

```sh
trial_dir=$(mktemp -d)
run_id=YOUR_WORKFLOW_RUN_ID
xbox_env=/path/to/setup-workspace/.env
gh run download "$run_id" --name xbox-probe-windows-sdk --dir "$trial_dir/probe"
gh run download "$run_id" --name xbox-probe-fresh-identity --dir "$trial_dir/control"
(cd "$trial_dir/probe" && shasum -a 256 -c SHA256SUMS)
(cd "$trial_dir/control" && shasum -a 256 -c SHA256SUMS)
python3 -m tools.xbox_deploy --env-file "$xbox_env" deploy \
  "$trial_dir/control/PackagedProcessProbe-x64.appx" --test-probe --timeout 30
python3 -m tools.xbox_deploy --env-file "$xbox_env" deploy \
  "$trial_dir/probe/PackagedProcessProbe-x64.appx" --test-probe --timeout 30
```

The control installs as **Packaged process startup control** beside the main package. Both use the same HTML startup and signer. The control omits the WinRT extension, component, metadata, and child binaries, so its report checks HTML activation and storage on the console. If Device Portal explicitly asks for a certificate during deployment, retry with `--certificate` and the artifact’s `.cer` path. The `.cer` is for package signing. The separate `XBOX_PORTAL_CERT_SHA256` setting identifies the Device Portal HTTPS service.

`deploy --test-probe` waits for installation, looks up the version from the APPX manifest in the live package inventory, requests a launch, and waits for a changed report. The Portal can return HTTP `202` before registration completes. Save the CLI output and the SHA-256 of the APPX you tested alongside the branch commit and console OS build. [Device Portal API](https://learn.microsoft.com/en-us/windows/uwp/debug-test-perf/device-portal-api-core)

For another run of the installed build, use `probe` without uploading again. Omit `--control` to launch **Packaged process test**. Use `--manual-launch` only when reproducing a TV-only launch symptom or recovering from a remote launch error. Start the watcher before launching from Dev Home so it can record the existing file as its baseline.

```sh
python3 -m tools.xbox_deploy --env-file "$xbox_env" probe --control --timeout 30
python3 -m tools.xbox_deploy --env-file "$xbox_env" probe --version 1.0.9.0 --timeout 30
```

Substitute the installed version in those commands. `--manual-launch` also works with `deploy --test-probe`. The HTML report includes a timestamp as `run_id` so two runs with the same result still produce distinct reports.

### Test a foreground switch

Use the signed `xbox-probe-lifetime` and `xbox-probe-fresh-identity` artifacts from the same workflow run. The lifetime package has its own identity and runs beside the regular process test. The control shows when its document becomes visible.

```sh
gh run download "$run_id" --name xbox-probe-lifetime --dir "$trial_dir/lifetime"
(cd "$trial_dir/lifetime" && shasum -a 256 -c SHA256SUMS)
python3 -m tools.xbox_deploy --env-file "$xbox_env" deploy \
  "$trial_dir/lifetime/PackagedProcessProbe-x64.appx"
python3 -m tools.xbox_deploy --env-file "$xbox_env" lifetime --version 1.0.12.0 --dwell 15 --timeout 25
```

Install the matching control using the earlier command. The CLI starts the lifetime app, waits for a pending helper request in `lifetime-report.txt`, then switches to the control and requires its fresh visibility report. After the dwell, it checks that the transaction is still pending and requests a return to the lifetime app. The CLI uploads the control’s current run ID to its LocalState. That app requests the lifetime package’s registered URI and records a diagnostic in `protocol-return.txt` if the return fails. Use `--portal-return` to reproduce Device Portal’s app launcher path. The finish path writes a new challenge to the original pipe handle. The signed helper encodes the challenge and its startup token in its exit code, which the parent verifies on the original process handle. The CLI also compares the two reports’ run, parent, helper, and channel identifiers. Its process inventory checks show whether the original PIDs were listed before switching, at the control’s first visible report, and before returning. Capture the complete CLI output with the build run and dwell duration.

A reload, changed identifier, early reply, or incomplete result fails the check. Inspect the final available checkpoint to distinguish a cold start from a blocked return or a lost pipe. The report includes visibility, background, suspension, and resumption callbacks separately. A completed transaction establishes continuity across the observed app switch. Execution while the app was away from the foreground and communication across packages need their own tests. The CLI stops the dedicated lifetime app during cleanup. [Named-pipe transactions](https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-callnamedpipew) · [UWP app lifecycle](https://learn.microsoft.com/en-us/windows/uwp/launch-resume/app-lifecycle)

### Rebuild the original native executable host

Use `bash build_tools/package.sh /path/to/x64/WindowsApp.lib NEW_OUTPUT_DIR` when isolating the earlier native launcher in `probe.c`. That build uses Clang, `lld-link`, `makemsix`, OpenSSL, and `osslsigncode`, and emits the signed APPX, public development certificate, and checksums. The prior cross-build used `c/um/x64/WindowsApp.lib` from `Microsoft.Windows.SDK.CPP.x64` version `10.0.26100.8249`, including on an ARM64 build host. The HTML-to-WinRT package comes from the Windows SDK workflow above. [Windows SDK package](https://www.nuget.org/packages/Microsoft.Windows.SDK.CPP.x64/10.0.26100.8249) · [Microsoft MSIX SDK](https://github.com/microsoft/msix-packaging)

See the [Steam Core Loader Probe](../docs/STEAM-CORE.md) to stage the genuine Valve client library locally and reproduce its loader result on the console.

## Interpret the feedback

| Observation | What it establishes |
| --- | --- |
| Deployment succeeded and the intended version appears in package inventory | That build registered on the console. |
| Fresh startup checkpoint | The app reached the named stage and wrote to `LocalState`. Capture the stage and HRESULT, when present. |
| `status=control-ready` | The control launched its local HTML and wrote the final result to `LocalState`. |
| `status=web-started` | The main package’s HTML started and wrote a checkpoint before it called the native component. |
| `status=native-component-error` | JavaScript caught an error during component lookup, activation, or invocation. Capture `hresult` and `message`. |
| `status=child-module-load-error` or `status=child-module-export-error` | The WinRT component ran and could not load or resolve the helper’s packaged library. Capture `win32_error`. |
| `create_process=failure` | The startup path reached `CreateProcessW`. Capture `win32_error`. |
| `exit_code_matches=yes` | The same-package helper returned `0x000051A7` on this console build. |
| `ipc=success` | The second signed helper sent a request on a per-run local named pipe, verified the reply, and returned `0x000051A8`. |
| `ipc=failed` | Capture `ipc_stage`, `ipc_error`, and `ipc_child_exit` when present. The process-only result appears earlier in the same report. |
| Timeout, unchanged file, or HTTP `404` for `probe-report.txt` | The watcher obtained no fresh report. Child execution remains unverified. |

The last available checkpoint shows how far execution got. The HTML also displays the result or a report-write error on the TV. If a launch returns to Dev Home without a fresh report, record how far the UI got and the exact Portal launch response. Check for an existing probe dump in Device Portal’s Crash data and record whether one appeared. Preserve the build and console evidence before changing the manifest, launch path, or storage implementation.

## Observed baseline

- The signed `process-probe-v0.1.0` package registered on a Series X in Developer Mode on OS build `26100.9673.amd64fre.xb_flt_2610ge.261116-2200`. Dev Home showed `0x80070002` on launch, and `LocalState` contained no report. Both executables and the manifest-referenced assets were present in the APPX.
- In-place update `1.0.1.0` registered over the CLI. After manual launch, Dev Home briefly opened the app and returned to the dashboard. The Portal’s remote launch returned `0x8027025B`. The app’s actual `LocalState` folder was empty, the direct report download returned HTTP `404`, and the Portal listed no crash dump for this probe.
- `1.0.2.0` moved `CreateProcessW` into a fixed packaged library loaded after app startup and changed the report opener to `CreateFile2`. It registered. Remote launch returned `0x8027025B` and a manual launch showed no error. No report or probe crash dump appeared.
- `1.0.3.0`, produced by the Windows SDK workflow, contained `resources.pri` and `AppxMetadata/CodeIntegrity.cat`. It registered and reproduced the remote launch error with no report. The original packaging route alone cannot explain the failure observed across both builds.
- `1.0.4.0` processed the window’s pending activation events and gave the shell time before running the helper and exiting. It still returned `0x8027025B` remotely with no report, including when the public package-signing certificate was uploaded with it.
- The `1.0.5.0` Windows SDK build replaced the C startup host with C++/WinRT, waited for the activation event, and awaited `StorageFolder.CreateFileAsync` and `FileIO.WriteTextAsync`. It registered, remote launch returned `0x8027025B`, and the report endpoint still returned HTTP `404` after a manual Dev Home launch.
- The `1.0.5.0` fresh-identity control registered beside the main app. Its `probe.exe`, `child.exe`, and `child-process.dll` matched the main artifact from the same build byte for byte. Its remote launch also returned `0x8027025B`, and both report endpoints returned HTTP `404` after a brief console screen appeared. This reproduced the failure without updating the existing app registration. [Control build and artifacts](https://github.com/clankur/steam-xbox-devmode/actions/runs/44855896834)
- The HTML-only `1.0.6.0` build used the same control identity. Device Portal launched it remotely and retrieved `status=control-ready` from `LocalState`. This confirmed remote launch and report collection for that package on the same console and narrowed the original failure to native executable startup. The exact loader or activation failure remains unknown.
- `1.0.7.0` applied the HTML startup path to the main diagnostic. Remote launch and file retrieval succeeded twice. The probe reported `create_process=success`, `wait_result=0x00000000`, and `child_exit_code=0xC000007B` (`STATUS_INVALID_IMAGE_FORMAT`). The component and file writer ran even though the helper returned a loader error. [Build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44856763230)
- `1.0.8.0` retained an actual x64 relocation table in `child.exe` while preserving AppContainer and ASLR. It again wrote a report with `child_exit_code=0xC000007B`. [Build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44856940304)
- `1.0.9.0` built `child.exe` with Microsoft’s compiler and linker, embedded a Windows manifest, and signed the executable individually before signing the APPX. The console reported `create_process=success`, `child_exit_code=0x000051A7`, and `exit_code_matches=yes`. A second CLI launch without reinstalling produced a fresh report with the same result. These helper-image changes were tested together. [Build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44857081600)
- `1.0.10.0` retained the first helper check and added a second signed helper that exchanges two four-byte messages over `\\.\pipe\LOCAL\`. The app correlates the request with the PID returned by `CreateProcessW`, and confirms reply validation through the exit code on that process handle. Windows’ pipe client PID query is absent from the UWP umbrella library. Two CLI launches produced fresh reports with `exit_code_matches=yes`, `ipc=success`, and `ipc_child_exit=0x000051A8`. [Build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44858026281) · [Microsoft named-pipe scope](https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-createnamedpipew)

- `1.0.11.0` opened a pending pipe transaction, and the control reported foreground visibility. The Xbox process inventory still listed both original PIDs after 15 seconds away. The Portal’s app launcher returned with new parent, helper, and channel IDs on both trials. Its Resume action returned HTTP 500 with `Element not found` in a separate trial. [Build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44858820731)
- `1.0.12.0` used the control’s registered-protocol return to complete the same pipe transaction after 15 and 25 seconds away. Runs `1795230363213` and `1795230419817` preserved the run, parent, helper, and channel IDs and reported both `observed_suspending=yes` and `observed_resuming=yes`. The challenge-derived helper exit codes were `0x5100C1E8` and `0x5100EEEF`. The main package from the same build also returned `0x000051A7` and `ipc=success`. [Build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44859225111)

Run `python3 -m tools.xbox_deploy probe --version 1.0.10.0 --require-ipc` to repeat the hardware check. Requiring IPC makes the CLI wait for a terminal pipe result even if a partial read contains the process-only result. That command checks the immediate exchange in one package. Use the lifetime command above to test what happens across a foreground switch.

If the exact failure in the old executable host needs to be identified, debug the archived `1.0.5.0` package from Visual Studio on the Series X. Break first at `wWinMain`, then at the activation callback and `RunDiagnostic`. Record the first HRESULT from the runtime or storage call and preserve the exact tested APPX. [Debug an installed UWP package](https://learn.microsoft.com/en-us/visualstudio/debugger/debug-installed-app-package?view=visualstudio)
