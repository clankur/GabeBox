# Steam game compatibility on Xbox

Target compatibility across the full Steam game catalog through the app’s normal Install and Play flow. Follow Proton’s approach of composing reusable compatibility components and selecting them for each game. Use Xbox-supported APIs and validate their behavior on the console. [Valve Proton](https://github.com/ValveSoftware/Proton)

## Performance target

- Target sustained **60 FPS at native 1920 × 1080** during representative gameplay. Prefer hardware rendering and GPU-to-GPU presentation. Preserve the game’s simulation timing and controller responsiveness when changing frame pacing.
- Measure distinct frames actually presented on Xbox for at least 30 seconds after the scene stabilizes. Record internal render resolution, output resolution, average FPS, lowest rolling one-second frame count, and whether frames used shared GPU textures or CPU uploads. Use `measure.py --require-gpu-60` for the hardware path. Report startup stalls separately. [Native fullscreen presentation](docs/GAME-LAUNCH.md#native-fullscreen-presentation)
- Respect deliberate game-engine caps and fixed internal resolutions. Document the game-imposed limit and the best verified result. Keep limitations introduced by the compatibility layer visible as open performance work.

## Design for reuse

- **Treat missing Xbox functionality as a compatibility problem to investigate.** Trace the failing call on the console and identify the behavior the game relies on. Try a supported Xbox equivalent or compatible upstream component. Where needed, write a reusable wrapper or translation layer, or implement the required behavior directly. Before declaring a blocker, document the alternatives tested and the specific runtime limitation that remains.
- Preserve calling conventions, return values, ownership, and error behavior. Verify what the function does on Xbox even when its symbol resolves. For a confirmed optional feature, provide a bounded fallback that preserves its completion contract and document the fallback’s behavior. [Windows launch contract](docs/GAME-LAUNCH.md#windows-titles)
- **Fix the shared flow.** Use the affected game to reproduce the problem, trace the flow end to end, and put reusable behavior and diagnostics in the common layer. Apply this to install, launch, input, rendering, saves, and return to the library. Keep shared code independent of AppID, Steam account, install path, and console-specific values.
- Put title-specific launch options, dependency selection, and engine paths in declarative profiles pinned to the Steam build. Verify the source image before applying a recipe. Stage adaptations in a temporary copy of the player’s installed game. Keep published binaries, saves, and Steam library files intact.
- Let the authenticated Steam client own entitlements, downloads, redistributables, and cloud saves. Scope test and runtime state to the game and account. Preserve controller input, audio, save locations, and exit back to Big Picture through the normal launch path.

## Practices from Proton

- Keep adapter changes with their components. Pin upstream inputs and verify each affected component before assembling and signing an APPX. Put console controls in `probe/` and packaging in `build_tools/`. [Proton component declarations](https://github.com/ValveSoftware/Proton/blob/1f089f5610a882c8bfcb4b58a3c86ee88bb4b9f1/.gitmodules) · [Proton build targets](https://github.com/ValveSoftware/Proton/blob/1f089f5610a882c8bfcb4b58a3c86ee88bb4b9f1/README.md#building-proton)
- Apply title overrides at launch and make them reversible. Record the selected adapters and profile in diagnostics so a failure can be tied to the exact configuration. [Proton runtime options](https://github.com/ValveSoftware/Proton/blob/1f089f5610a882c8bfcb4b58a3c86ee88bb4b9f1/README.md#runtime-config-options)
- Coordinate writes to shared staging state. Track generated files so interrupted launches and cleanup preserve the player's install and saves. [Proton compatibility data and prefix locking](https://github.com/ValveSoftware/Proton/blob/1f089f5610a882c8bfcb4b58a3c86ee88bb4b9f1/proton#L578-L609)
- Pair a focused component reproduction with a normal Big Picture launch. Record the first failing operation, the Steam AppID and build, and the runtime and component revisions. [Proton debugging guidance](https://github.com/ValveSoftware/Proton/blob/1f089f5610a882c8bfcb4b58a3c86ee88bb4b9f1/README.md#debugging)
- Build console probes around reusable compatibility capabilities. Exercise the same adapter the normal launch uses and register single-package report checks in the probe catalog. Give each run an ID and record its package version, expected result, and first failed operation. [Xbox capability probes](probe/README.md)

Keep core Python commands runnable with the standard library. For a standalone research tool that needs third-party Python packages, prefer local script metadata and a script lockfile with `uv run --script`. [uv script dependencies](https://docs.astral.sh/uv/guides/scripts/#locking-dependencies)

## Existing tools by problem

| Problem | Start here |
| --- | --- |
| Missing DLL, import, or runtime dependency | Use `python3 -m build_tools.prepare_steam_core --inspect-pe` to build a control that checks which imports from a local Windows executable or DLL resolve on Xbox. [Installed Windows imports](docs/STEAM-CORE.md#inspect-installed-windows-imports) |
| Stage a reusable import adapter | Use `python3 -m build_tools.redirect_pe_imports --recipe` to generate a recipe from a verified source image. Stage the profile with `python3 -m build_tools.prepare_big_picture_library --win32-trial-profile` and sign the package. [Build-matched profile example](analysis/hades-compatibility.md#prepare-the-build-matched-profile) |
| Desktop windowing, graphics, or GPU sharing failure | Review the shared desktop, windowing, and DXGI adapters and compare the failure with the recorded presenter checkpoints. [Win32 runtime](runtime/win32) · [Native display results](analysis/big-picture-reliability.md#direct-native-game-display) |
| Xbox install, startup, or launch failure | Use `python3 -m tools.xbox_deploy inspect-game` for the installed manifest and add `--executable` to read the PE header. Use `deploy` to install a signed package and `report` to read saved checkpoints. Use `observe --startup` or `observe --app-id` to follow a run and collect diagnostics without controlling the Xbox. [Device Portal CLI](tools/xbox_deploy.py) · [Observer guide](docs/XBOX-DIAGNOSTICS.md) |
| Frame rate, pacing, or GPU transport | Collect native frame timestamps with the observer when the package enables them. Analyze a settled gameplay sample with `python3 -m tools.measure --require-gpu-60`. [Capture and measurement](docs/XBOX-DIAGNOSTICS.md#collect-and-measure) |
| Library coverage | Enable `python3 -m build_tools.prepare_big_picture_library --library-inventory` and read the exported title list with `python3 -m tools.xbox_deploy report --inventory`. Compare it with build-matched play evidence using the checks below. [Profile packager](build_tools/prepare_big_picture_library.py) · [Launch verification](docs/GAME-LAUNCH.md#verification) |

For Python changes, run `uv run --locked ruff check` and `uv run --locked python -m pytest`. Use `python3 -m unittest discover -s tests` when working with only the standard library. For JavaScript changes, run `node --test tests/*.test.js tests/big-picture-webview/*.test.js` as applicable. See the [launch test workflow](.github/workflows/steam-launch-tests.yml) for native checks.

## Verify before counting coverage

1. Reproduce the failure on the target Xbox. Record the Steam AppID and build ID, Xbox model and runtime build, and the first failing operation. Distinguish import resolution, runtime behavior, and visible presentation.
2. Exercise the shared flow with a focused control on Xbox, then rerun the affected game through Big Picture. Use a second title or an independent caller with a different usage pattern before claiming broad support. When evidence covers one title, record that scope.
3. Assign verification to a subagent separate from the implementation. Give it the acceptance criteria, exact build, and test setup. Have it run the relevant checks independently and report commands, observed results, regressions, and gaps. On a shared Xbox, coordinate one operator and have the verifier independently review the raw logs and frame traces. Record which checks still need a live run.
4. Count a title once per coverage report after verifying gameplay, controller input, audio, save and reload, and return to the library on the recorded game and runtime builds. Track installs, launches, and first frames as intermediate evidence. Recheck affected titles when a Steam build or shared runtime changes.
5. Verify free space on the app’s actual install volume before large downloads. Keep diagnostic trials isolated where practical, clean up generated staging files after the child exits, and get approval before destructive storage changes.
