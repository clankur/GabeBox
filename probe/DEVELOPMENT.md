# Xbox probe development

The probe tests one `CreateProcessW` call against a fixed helper packaged with the app. The helper is expected to return `0x000051A7`. Its runtime evidence comes from `probe-report.txt` in the app’s `LocalState` on the target Xbox. Deployment status, a splash screen, and a return to Dev Home are separate observations.

## Build, deploy, and collect a result

Run these commands from the repository root with the Xbox in Developer Mode, signed in to an Xbox profile, and with Device Portal enabled. Use Python 3.9 or later. The build uses Clang, `lld-link`, the x64 `WindowsApp.lib` from the Windows SDK, `makemsix` from Microsoft’s MSIX SDK, OpenSSL, and `osslsigncode`. The prior build used `c/um/x64/WindowsApp.lib` from `Microsoft.Windows.SDK.CPP.x64` version `10.0.26100.8249`. Use the x64 umbrella library even on an ARM64 build host. [Windows SDK package](https://www.nuget.org/packages/Microsoft.Windows.SDK.CPP.x64/10.0.26100.8249) · [Microsoft MSIX SDK](https://github.com/microsoft/msix-packaging)

Use the Git-ignored `.env` documented in [the probe README](README.md). `--env-file` lets a checkout use the setup workspace’s existing file without copying it into the repository. Keep saved credentials in the local file and share only the CLI’s diagnostic output.

For an updated build, increment `Identity Version` in [`Package.appxmanifest`](Package.appxmanifest) so the install can be identified on the console. Keep the package name and publisher consistent during an in-place upgrade. Build to a new output directory outside the checkout.

```sh
trial_dir=$(mktemp -d)
build_dir="$trial_dir/probe"
bash probe/package.sh /path/to/x64/WindowsApp.lib "$build_dir"
(cd "$build_dir" && shasum -a 256 -c SHA256SUMS)
python3 xbox-deploy.py --env-file /path/to/setup-workspace/.env deploy \
  "$build_dir/PackagedProcessProbe-x64.appx" --test-probe --timeout 30
```

`package.sh` runs the existing cross-build, creates and signs the APPX, and emits the package, public certificate, and checksums. It uses a temporary development signing key. If Device Portal explicitly asks for a certificate during deployment, retry with `--certificate` and the generated `.cer` path. The `.cer` is for package signing. The separate `XBOX_PORTAL_CERT_SHA256` setting identifies the Device Portal HTTPS service.

`deploy --test-probe` waits for installation, looks up the version from the APPX manifest in the live package inventory, requests a launch, and waits for a changed report. The Portal can return HTTP `202` before registration completes. Save the CLI output and the SHA-256 of the APPX you tested alongside the branch commit and console OS build. [Device Portal API](https://learn.microsoft.com/en-us/windows/uwp/debug-test-perf/device-portal-api-core)

The [Windows SDK build workflow](../.github/workflows/xbox-probe.yml) runs on pushes that touch the probe. It produces the same artifact filenames and adds `resources.pri` with `MakePri.exe`, packages with `MakeAppx.exe`, and signs with `SignTool.exe`. Download the artifact for the intended commit with `gh run download RUN_ID --name xbox-probe-windows-sdk --dir BUILD_DIR`, verify `SHA256SUMS`, and use the same CLI command to deploy it. This gives a packaging comparison when the cross-built APPX installs but fails to activate.

For another run of the installed build, use `probe` without uploading again. If remote launch fails or the launch needs to be observed on the TV, start the watcher first and launch **Packaged process test** once from Dev Home when prompted.

```sh
python3 xbox-deploy.py --env-file /path/to/setup-workspace/.env probe \
  --version 1.0.1.0 --manual-launch --timeout 30
```

Substitute the installed version in that command. `--manual-launch` also works with `deploy --test-probe`. Starting the watcher before launch lets it record the existing file as its baseline.

## Interpret the feedback

| Observation | What it establishes |
| --- | --- |
| Deployment succeeded and the intended version appears in package inventory | That build registered on the console. |
| Fresh startup checkpoint | The executable reached the named stage and wrote to `LocalState`. Capture the stage and HRESULT, when present. |
| `create_process=failure` | The startup path reached `CreateProcessW`. Capture `win32_error`. |
| `exit_code_matches=yes` | The same-package helper returned `0x000051A7` on this console build. |
| Timeout, unchanged file, or HTTP `404` for `probe-report.txt` | The watcher obtained no fresh report. Child execution remains unverified. |

The last available checkpoint narrows the failure to the next observable stage. If a manual pass returns to Dev Home without a fresh report, record how far the UI got and the exact Portal launch response. Check for an existing probe dump in Device Portal’s Crash data and record whether one appeared. Preserve the build and console evidence before changing the manifest, launch path, or storage implementation.

## Observed baseline

- The signed `process-probe-v0.1.0` package registered on a Series X in Developer Mode on OS build `26100.9673.amd64fre.xb_flt_2610ge.261116-2200`. Dev Home showed `0x80070002` on launch, and `LocalState` contained no report. Both executables and the manifest-referenced assets were present in the APPX.
- In-place update `1.0.1.0` registered over the CLI. After manual launch, Dev Home briefly opened the app and returned to the dashboard. The Portal’s remote launch returned `0x8027025B`. The app’s actual `LocalState` folder was empty, the direct report download returned HTTP `404`, and the Portal listed no crash dump for this probe.
- `1.0.2.0` moved `CreateProcessW` into a fixed packaged library loaded after app startup and changed the report opener to `CreateFile2`. It registered. Remote launch returned `0x8027025B` and a manual launch showed no error. No report or probe crash dump appeared.
- `1.0.3.0`, produced by the Windows SDK workflow, contained `resources.pri` and `AppxMetadata/CodeIntegrity.cat`. It registered and reproduced the remote launch error with no report. The original packaging route alone cannot explain the failure observed across both builds.

Possible causes at this baseline include activation stopping before the first checkpoint and the app reaching the checkpoint while its report write fails. A fresh checkpoint, a precise startup HRESULT, or a matching helper exit code provides stronger evidence than the dashboard behavior alone.
