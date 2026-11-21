# Xbox packaged process test

This x64 UWP diagnostic answers one question: can this console start a second executable bundled with the same signed package using the documented `CreateProcessW` API and obtain the exit code the helper is programmed to return? It uses two small, original executables and requires no Steam account, Valve files, or network access. Its result applies to this package on the tested console OS.

## Test on a Series X in Developer Mode

1. From your signed-in GitHub account on your setup computer, open the [signed APPX](downloads/PackagedProcessProbe-x64.appx) and select **Download raw file**. The adjacent CER file is the public development signing certificate. The SHA256SUMS file identifies the exact release files.
2. In Xbox Dev Home, use the Device Portal address displayed by your console. In the apps manager, choose **Add / Deploy**, select the APPX, and install it. The diagnostic has no external framework package dependencies. If the Portal requests the signing certificate for this deployment, use the accompanying CER. Record the deployment error code if the package is rejected. [Microsoft deployment guide](https://learn.microsoft.com/en-us/windows/uwp/apps-for-xbox/development-options)
3. Launch **Packaged process test** on the Xbox. It performs one test, writes its result and exits. In Device Portal’s **File explorer**, open **LocalAppData**, find the probe package, open **LocalState**, and download `probe-report.txt`. Check its modification time after each run.
4. Report the console OS version, whether you ran in Developer Mode, and the reported result or exact error. A deployment or startup failure, or a missing report, is inconclusive about child execution.

## Reading the result

- A successful call and a matching helper exit code show that this packaged helper ran.
- A process-creation error identifies the failing API and its Windows error code. Record it before changing the package or console configuration.
- A timeout, missing exit code or unexpected exit code is a separate outcome. Investigate the helper and app lifecycle before drawing a conclusion about process creation.

The helper has a fixed path in the package. This experiment does not attempt to start downloaded software. It cannot establish that Valve’s browser helper can initialize, that Steam survives when a separate game takes foreground, or that a Steam depot can execute as an Xbox title.

The released package was packed with Microsoft’s MSIX SDK, signed with a self-signed development certificate, verified with osslsigncode, unpacked and compared byte-for-byte with the build staging files. REA verified that both executables are x64, that the parent imports `CreateProcessW`, and that the helper imports only `ExitProcess`. The release has not been run on Xbox or Windows.

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
