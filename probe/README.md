# Xbox packaged process test

This x64 UWP diagnostic calls `CreateProcessW` for one fixed executable bundled with the app. The helper returns `0x000051A7` when its entry point runs. The app displays the result and writes it to `LocalState/probe-report.txt`. It requires no Steam account, Valve files, or network access on the Xbox.

Version `1.0.9.0` was installed and launched by CLI on a Series X in Developer Mode. The report recorded `create_process=success`, `child_exit_code=0x000051A7`, and `exit_code_matches=yes`. [Signed Windows SDK build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44857081600) · [Developer workflow and hardware observations](DEVELOPMENT.md)

Version `1.0.10.0` also verifies a request and reply over one same-package named pipe. A successful report includes `ipc=success` and `ipc_child_exit=0x000051A8`. Add `--require-ipc` to make that result part of the CLI exit status. [Signed IPC build](https://github.com/clankur/steam-xbox-devmode/actions/runs/44858026281)

## Test on a Series X in Developer Mode

Copy `.env.example` to `.env` and set `XBOX_PORTAL_URL` to the HTTPS address shown in Dev Home and `XBOX_PORTAL_USERNAME` to the Device Portal username. Run `python3 xbox-deploy.py fingerprint`, compare it with the browser’s TLS certificate fingerprint for the portal, and set `XBOX_PORTAL_CERT_SHA256`. Python 3.9 or later prompts for the Device Portal password. For noninteractive use, set `XBOX_PORTAL_PASSWORD` in the Git-ignored `.env` and restrict it with `chmod 600 .env`.

Download the `xbox-probe-windows-sdk` artifact for the commit you want to test and verify `SHA256SUMS`. Use the signed APPX from that artifact with the CLI.

```sh
python3 xbox-deploy.py deploy /path/to/PackagedProcessProbe-x64.appx --test-probe
python3 xbox-deploy.py probe --version 1.0.10.0 --require-ipc
```

Use `--certificate path/to/publisher.cer` if the portal requests the package-signing certificate. The CLI verifies the installed version, launches the app, and waits for a fresh report. If needed, read `probe-report.txt` manually in Device Portal under **File explorer → LocalAppData → Packaged process test → LocalState**. [Device Portal app deployment API](https://learn.microsoft.com/en-us/windows/uwp/debug-test-perf/device-portal-api-core#app-deployment)

## Reading the result

- `exit_code_matches=yes` confirms that the bundled helper ran and returned its expected code on the tested console.
- `create_process=failure` reports the `CreateProcessW` error in `win32_error`.
- A fresh startup checkpoint without a final result identifies the last step the app reached. An unexpected helper exit code is a separate test result even when `CreateProcessW` succeeds.

The console result applies to the fixed helper in this package. Steam client startup and local gameplay remain unverified. The [original prerelease package](downloads/PackagedProcessProbe-x64.appx) is preserved for comparison with the earlier native startup path.
