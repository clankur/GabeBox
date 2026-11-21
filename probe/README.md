# Xbox packaged process test

For the build, CLI deployment, and console diagnosis loop, see [Developer workflow](DEVELOPMENT.md).

This x64 UWP diagnostic answers one question: can this console start a second executable bundled with the same signed package using the documented `CreateProcessW` API and obtain the exit code the helper is programmed to return? It uses two small, original executables and requires no Steam account, Valve files, or network access. Its result applies to this package on the tested console OS.

## Test on a Series X in Developer Mode

1. From your signed-in GitHub account on your setup computer, open the [signed APPX](downloads/PackagedProcessProbe-x64.appx) and select **Download raw file**. The adjacent CER file is the public development signing certificate. The SHA256SUMS file identifies the exact release files.
2. In Xbox Dev Home, use the Device Portal address displayed by your console. In the apps manager, choose **Add / Deploy**, select the APPX, and install it. The diagnostic has no external framework package dependencies. If the Portal requests the signing certificate for this deployment, use the accompanying CER. Record the deployment error code if the package is rejected. [Microsoft deployment guide](https://learn.microsoft.com/en-us/windows/uwp/apps-for-xbox/development-options)
3. Launch **Packaged process test** on the Xbox. It performs one test, writes its result and exits. In Device Portal’s **File explorer**, open **LocalAppData**, find the probe package, open **LocalState**, and download `probe-report.txt`. Check its modification time after each run.
4. Report the console OS version, whether you ran in Developer Mode, and the reported result or exact error. A deployment or startup failure, or a missing report, is inconclusive about child execution.

### Deploy from the command line

From the repository root, copy `.env.example` to `.env` and set `XBOX_PORTAL_URL` to the HTTPS address shown in Dev Home and `XBOX_PORTAL_USERNAME` to the Device Portal username. Run `python3 xbox-deploy.py fingerprint` and compare its output with the SHA-256 fingerprint in the browser’s certificate viewer for the portal. Put the verified value in `XBOX_PORTAL_CERT_SHA256`. The CLI requires Python 3.9 or later and prompts for the Device Portal password. To deploy noninteractively, set `XBOX_PORTAL_PASSWORD` in the Git-ignored `.env` and restrict file access with `chmod 600 .env`.

```sh
python3 xbox-deploy.py deploy probe/downloads/PackagedProcessProbe-x64.appx
```

Pass the path to your signed APPX when testing a rebuild. If the portal requires the package-signing certificate, pass `--certificate path/to/publisher.cer`. `python3 xbox-deploy.py status` shows the console’s latest installation result. [Device Portal app deployment API](https://learn.microsoft.com/en-us/windows/uwp/debug-test-perf/device-portal-api-core#app-deployment)

## Series X launch investigation

`process-probe-v0.1.0` registered successfully on Xbox Series X in Developer Mode, on OS build `26100.9673.amd64fre.xb_flt_2610ge.261116-2200`. Dev Home returned `0x80070002` when opening it and the package’s `LocalState` contained no report. The APPX contains both executables and all assets referenced by the manifest. The launch error does not identify the missing file or path. [Microsoft error reference](https://learn.microsoft.com/en-us/windows/win32/appxpkg/troubleshooting)

The `1.0.1.0` source activates `CoreWindow` before running the probe. The original view left the window inactive. The candidate also attempts to write `status=core-application-starting` from the executable entry point and reports an HRESULT if obtaining or activating the window fails. Both x64 executables were linked against the pinned Windows SDK. The Xbox registered `1.0.1.0` through the CLI. Dev Home briefly opened the app and returned to the dashboard without displaying an error. Device Portal’s remote launch returned `0x8027025B`, and `LocalState` still contained no report after the manual launch. Child execution is unverified. [Microsoft CoreWindow example](https://learn.microsoft.com/en-us/uwp/api/windows.ui.core.corewindow?view=winrt-22621)

For a launch retry, verify that the console has version `1.0.1.0` and check for a fresh `probe-report.txt` even if Dev Home shows an error. A startup checkpoint confirms that user code reached that stage. A missing report also leaves open a failure to write `LocalState`. If launch still fails, test the same staging files packaged and signed with Windows SDK `MakeAppx` and `SignTool` to isolate the packaging step. [Windows SDK signing guide](https://learn.microsoft.com/en-us/windows/msix/package/sign-app-package-using-signtool)

## Reading the result

- A successful call and a matching helper exit code show that this packaged helper ran.
- A process-creation error identifies the failing API and its Windows error code. Record it before changing the package or console configuration.
- A timeout, missing exit code or unexpected exit code is a separate outcome. Investigate the helper and app lifecycle before drawing a conclusion about process creation.

The helper has a fixed path in the package. This experiment does not attempt to start downloaded software. It cannot establish that Valve’s browser helper can initialize, that Steam survives when a separate game takes foreground, or that a Steam depot can execute as an Xbox title.

The v0.1.0 package was packed with Microsoft’s MSIX SDK, signed with a self-signed development certificate, verified with osslsigncode, unpacked and compared byte-for-byte with the build staging files. REA verified that both executables are x64, that the parent imports `CreateProcessW`, and that the helper imports only `ExitProcess`.

## Rebuild

Run `build.sh` with Clang, lld-link and the x64 Windows SDK umbrella library. The build emits a staging directory containing two PE executables, manifest, and tile assets. Then pack with the Microsoft MSIX SDK and sign with your own development key matching the publisher in the manifest. The release signing private key is not a build input.

```sh
bash probe/build.sh /path/to/WindowsApp.lib /tmp/probe-build
makemsix pack -d /tmp/probe-build/package -p /tmp/probe-build/unsigned.appx
osslsigncode sign -certs dev-cert.pem -key dev-key.pem -h sha256 \
  -in /tmp/probe-build/unsigned.appx -out /tmp/probe-build/signed.appx
osslsigncode verify -CAfile dev-cert.pem -in /tmp/probe-build/signed.appx
```

For the development certificate, use a code-signing certificate whose subject matches `CN=Xbox Portability Probe`. Supply `CLANG` and `LLD_LINK` if your tools have nonstandard names or paths. [Microsoft MSIX SDK](https://github.com/microsoft/msix-packaging)
