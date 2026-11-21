# Steam game launch contract

The Steam library and ordinary Play workflow work across titles. Whether a particular game can run depends on its installed build and the Xbox runtime available to it. X-COM is the verified DOS example. Factorio uses ordinary Steam Play with its publisher's Windows x64 launch option and currently exits in the Windows loader.

## Common game checks

1. Restore or establish the Steam session and use Steam's library metadata to identify the selected AppID.
2. Install through Steam into the app's own writable library. Steam must report the install complete before launch. For a hosted game also verify the completed local manifest and exact title directory.
3. Recheck the live session, subscription, installed library path, and Steam prerequisites in the native component before starting either backend.
4. Record the AppID and chosen backend in the launch report. Distinguish an accepted request, a captured process, and visible game rendering. The installed state only establishes that the content is available.

## Windows titles

The default route calls Steam Play for the selected AppID. Use the publisher's Windows x64 launch option when the default does not select the right executable. Pass `--steam-launch-option APPID:INDEX` once per title to the local package builder. Options are available through ordinary library Play and each signed marker applies only to its AppID. The older `--launch-app-id APPID --launch-option INDEX` form still configures the automatic trial title.

Check a failed launch against the installed executable's architecture, supported Windows API imports, runtime dependencies, and graphics backend. `inspect-game --executable` reads the installed PE header. The loader and process checkpoints show where the request stopped. Any runtime dependency added to the package should come from the original signed redistributable. Keep changes scoped to the verified title and depot.

## Hosted titles

Register an adapter only when a bundled runtime can execute the verified installed format. An adapter declares its AppID, backend, native authorization method, success marker, and start function. The dispatcher opens a hosted runtime only after that method verifies the selected install. A refused or failed authorization returns to the library with its diagnostic.

For DOS, the native component allowlists the exact supported AppID and directory. The fixed profile in `signin/xcom-dos.js` records the publisher's DOS mount, startup command, supported configuration and file exclusions. The host handles file copying, process lifetime, return to the library, and optional local trial snapshots. When adding a DOS title, verify its owned depot, define a separate fixed profile, and extend the native allowlist to that exact title and install path. Keep publication of the runtime separate from the user's game files. Input and save persistence also need their own validation for each supported backend.

## Verification

Run these from the repository root. GitHub Actions runs the same tests without Steam credentials or game files.

```sh
node --test tests/*.test.js
python3 -m unittest discover -s tests
```

The tests cover native Play and two synthetic hosted profiles, authorization refusal, launch-option layout, DOS files and empty save slots, and stale callbacks after a game is closed. On Xbox, verify each backend with a real installed title. For hosted games, capture a recognizable frame from the automatic trial and exercise input and return to the library before marking those behaviors verified. For Windows titles, keep the actual Steam result and process exit in the compatibility record.

### Native process startup snapshot

For a local native Steam Play trial, add `--launch-app-id 975370 --startup-snapshot` when preparing the package. The option binds the diagnostic to that AppID. Before submitting Play, the probe waits 50 ms on its own process and records the control outcome and the probe PID. A healthy control reports `steam_launch_snapshot_control=running`.

Once Steam identifies a newly created process for the selected AppID, the probe writes its PID, creation time, available executable basename, and immediate Windows wait result at `captured-process`. It then waits up to one second on the same handle on a background thread and writes `sampled-process` before returning to Steam's callback and window polling. `running` describes an unsignaled handle at the sample time. `exited` includes the actual exit code. Neither outcome establishes a game window.

For Dwarf Fortress, compare the stock and rebuilt SDL2 on the same installed package. Use the captured PID and creation time to correlate Device Portal observations. If only the first snapshot survives, record its exact fields and whether the probe PID still exists before restarting the app.

The Series X control with the stock SDL2 and no Visual C++ framework dependency exited immediately with `0xC0000135`. Adding the official signed desktop Visual C++ framework as a package dependency changed the startup exit to `0xC0000139`. The loader control identified `imm32!ImmGetIMEFileNameA` as unavailable. Keep the framework dependency in both arms of an SDL comparison.

With the version-matched SDL 2.26.2 rebuild using `SDL_DISABLE_WINDOWS_IME`, the same package captured `Dwarf Fortress.exe` and found its process handle still running at the one-second sample. The trial did not reach a confirmed game window. Restore the publisher's SDL2 after the experiment and verify its original hash. The next comparison should record whether the process reaches SDL window creation or renderer initialization. [Dwarf Fortress compatibility results](https://github.com/clankur/steam-xbox-devmode/issues/35)

The shared SDL trial artifacts can write initialization, window, OpenGL and renderer checkpoints when started under `--startup-snapshot`. Read a run with `report --version "$version" --sdl-startup`. Use `--startup-sdl-renderer software` or `--startup-sdl-renderer direct3d11` to request a renderer in a separate signed test. `--startup-sdl-dummy-video` requests the SDL offscreen test driver. `--startup-sdl-working-directory` asks SDL's main entry point to set the child process's current directory to the verified app-local game installation. The native report records requested settings and the SDL trace records which startup calls returned.

Use `--startup-wait-only` with `--startup-snapshot` and `--repeat-launch` to classify a fast exit on the captured Windows process handle. The probe writes up to twelve 500 ms samples and records its app memory usage and budget when available. It skips Steam's window query during that observation. The final status `process-wait-complete` describes the diagnostic, and the sampled Windows exit code describes the process.
