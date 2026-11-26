# Build-matched profiles, recipes and packaging

A profile is the declarative, signed description of one title's adaptation, pinned to
one Steam build. Shared code stays title-agnostic; everything specific to a game lives
here. The recipe inside it is bound to the audited image's size and SHA-256, so staging
refuses a changed build rather than adapting the wrong bytes.

Contents: where profiles live · the shadow profile · declarations and markers ·
companions · generating a recipe · packaging and signing · deploy and observe ·
staging hygiene · worked example.

## Where profiles live, and who validates them

Profiles live in `profiles/APPID-BUILDID/` and are passed to the packager as
`--win32-trial-profile APPID=DIRECTORY`. The staging rules are implemented in
`build_tools/stage_profiles.py` (`stage_win32_trial_profiles`), and that file is the
authority: the marker set grows as titles land, every marker is validated, and a profile
that breaks a rule fails the build rather than producing a quietly wrong package. Read
it before inventing a file name, and read it again when a rule here looks stale.

Older branches keep the same contract under `probe/win32-runtime/profiles/` with
script-style commands (`python3 probe/redirect-pe-imports.py`); the module form below is
what `main` uses.

## The shadow profile

`steam-win32-shadow-APPID.txt` is plain ASCII, one field per line, two to eight lines:

| Line | Field | Notes |
| --- | --- | --- |
| 1 | Steam build ID | digits; must match the installed manifest |
| 2 | launcher executable | `Something.exe`, the image Steam starts |
| 3 | renderer | empty, `opengl3`, or `d3d12`; non-empty requires line 4 |
| 4 | engine profile | empty, `godot`, or `unity` |
| 5 | image to patch | the `.exe` or `.dll` the recipe applies to, if not the launcher |
| 6 | source subdirectory | path within the install holding those files (e.g. `x64`) |
| 7 | `loader-trace` | enables the loader-trace adapter; several markers require it |
| 8 | engine option | currently `accessibility=disabled`, which needs an `opengl3` + `godot` profile and a matching hosted build |

The `unity` profile has its own preconditions — `opengl3`, an empty patched-image field,
a loader trace, a companion list, and a hosted surfaceless build pinned to the same
build ID. The validator states each of these; let it tell you, rather than guessing from
another title's profile.

## Declarations and markers

Two JSON declarations sit beside the shadow profile:

- **`steam-win32-adapters-APPID.json`** — `build_id` plus `required_adapters`, the list
  of adapter DLLs this title needs staged, *including ones loaded dynamically* that no
  import table would reveal. `AGENTS.md` asks for exactly this declaration, and it is
  how an adapter that only appears at runtime still ships.
- **`steam-win32-source-APPID.json`** — the source fingerprints for the build, also
  accepted as `--win32-source-profile APPID=FILE`. Several options refuse to stage
  without it.

Markers are empty files whose presence is the signal. The current families:

| Marker | Selects |
| --- | --- |
| `steam-win32-crash-report-APPID.txt` | the local crash-receipt fallback adapter |
| `steam-win32-dxgi-APPID.txt` | desktop DXGI swap chains through the native presenter; requires the game's default native renderer |
| `steam-win32-documents-APPID.txt` | the Documents folder adapter |
| `steam-win32-working-directory-APPID.txt` | setting the child's current directory to the verified install |
| `steam-win32-original-executable-APPID.txt` | answering the game's own `GetModuleFileNameW` with the original Steam path |
| `steam-win32-dynamic-companions-APPID.txt` | companions resolved at run time; needs the original-executable marker, a companion list and a loader trace |
| `steam-win32-storage-trace-APPID.txt` | the storage trace adapter; requires a loader trace |
| `steam-win32-d3d12-default-adapter-APPID.txt`, `steam-win32-streamline-unavailable-APPID.txt` | renderer options; each needs a source profile and a loader trace |
| `steam-win32-no-steam-relay-APPID.txt`, `steam-win32-steam-callback-trace-APPID.txt` | Steam title overrides in the loader trace: a no-op relay start-up (diagnostic only) and `SteamAPI_RunCallbacks` timing; each needs a source profile and a loader trace |
| `steam-win32-steam-relay-worker-APPID.txt`, `steam-win32-steam-relay-status-APPID.txt` | Steam title overrides in the loader trace: the real relay start-up on a worker thread, and `GetRelayNetworkStatus` changes and slow polls; each needs a source profile and a loader trace, and neither can be combined with the relay stub |
| `steam-win32-release-renderer-APPID.txt` | releases the renderer to a matching hosted build |
| `steam-win32-godot-shader-cache-APPID.txt` | `control` or `disabled` for a staged Godot OpenGL launcher |
| `steam-win32-steam-sdk-APPID.txt`, `steam-win32-steam-legacy-sdk-APPID.txt` | the staged publisher Steamworks SDK paths |

`steam-win32-arguments-APPID.txt` is the non-empty one: a single printable line, up to
2048 characters, appended as the child's command line — the place for publisher launch
options such as Hades's native gamepad flags.

`SOURCE-SHA256SUMS` records the audited source images so a staging run can be
reproduced, and the recipe itself carries the size and digest it enforces.

## Companions

`steam-win32-files-APPID.txt` lists the files, relative to the source subdirectory, that
staging copies alongside the patched image — the game's own DLLs, so the temporary copy
has a complete directory. A line that is just a path is copied unchanged. A line with a
tab and a short key (`[a-z0-9-]`, e.g. `Hades.exe\tlauncher`) also gets adapted: staging
verifies that file's hash and applies `steam-win32-shadow-APPID-KEY.xwp`. That is how a
launcher and its engine carry different recipes in one profile.

## Generating a recipe

```sh
profile_source="$PWD/profiles/1145360-10929685"
profile=$(mktemp -d)
(cd "$steam_install" && shasum -a 256 -c "$profile_source/SOURCE-SHA256SUMS") || exit 1
cp "$profile_source"/steam-win32-*.txt "$profile_source"/steam-win32-*.json "$profile/"

python3 -m build_tools.redirect_pe_imports --recipe "$steam_install/x64/EngineWin64s.dll" \
  "$profile_source/engine-bindings.json" "$profile/steam-win32-shadow-1145360.xwp"
python3 -m build_tools.redirect_pe_imports --recipe "$steam_install/x64/Hades.exe" \
  "$profile_source/launcher-bindings.json" "$profile/steam-win32-shadow-1145360-launcher.xwp"
```

The bindings JSON is the whole adaptation, and it is readable on purpose — original DLL
→ original function (or `#ordinal`) → replacement DLL:

```json
{
  "user32.dll": { "CreateWindowExW": "XboxWin32Window.dll" },
  "shell32.dll": { "#189": "XboxWin32Desktop.dll" },
  "xinput9_1_0.dll": { "XInputGetState": "XboxWin32Input.dll",
                       "XInputSetState": "xinput1_4.dll" }
}
```

Three things follow from that shape and are worth internalizing:

- A replacement can be another **system** DLL when the console already implements the
  function under a different module name — no adapter needed.
- Redirect the narrowest set that fixes the failure. Published recipes preserve every
  other import slot (one Factorio recipe redirected a single function and kept all 576),
  which is what keeps a trial interpretable.
- Adding a binding for a function the game never calls at startup changes nothing and
  costs a build; let REA and the loader trace choose the list.

### Constraints the generator enforces

Read these before hand-writing a bindings file, because each one fails the build rather
than degrading quietly:

- **Every binding must name an import the image really has.** A symbol the PE does not
  import is rejected with "The input PE does not import …", so the bindings list has to
  be derived from the audited import table, not from a guess about the engine.
- **x64 only.** A non-`0x8664` image is refused before imports are examined, which is
  why a 32-bit title is a different problem and not a recipe.
- **Unsigned, unbound input.** A bound import table is refused, and a signed source needs
  its audited full-file digest carried into the recipe (`--signed-source-sha256`) so the
  certificate is verified before the copy is adapted.
- **Window classes follow `CreateWindowExW`.** When that import is bound to the hosted
  window adapter, `RegisterClassW`, `RegisterClassExW` and `UnregisterClassW` are routed
  to the same adapter automatically, and a binding that sends them elsewhere is an
  error — the window handle and its class must live in one place.
- **A replacement may not be the original module**, and a binding may not be duplicated.

Without `--recipe` the same tool writes a full unsigned staged PE, useful for local
inspection but not what ships.

## Packaging and signing

```sh
python3 -m build_tools.prepare_big_picture_library "$base_appx" "$host_artifact" "$layout" \
  --version "$app_version" \
  --session-artifact "$session" --session-checksum "$session_sha" \
  --surfaceless-profile 1145360=10929685 \
  --win32-trial-profile "1145360=$profile" \
  --win32-runtime-template "$steam_core_appx"
bash build_tools/package.sh --layout "$layout" "$signed_dir"
(cd "$signed_dir" && shasum -a 256 -c SHA256SUMS)
```

`--win32-trial-profile` and `--win32-runtime-template` go together: the template is the
checksum-pinned x64 Steam Core artifact that supplies the native component, its WinRT
metadata, and the adapter DLLs. The packager stages only the adapters the profiles
select — through the adapters declaration, and through the markers and renderer fields
that imply one. If an adapter you expect is missing from the package, the profile did
not ask for it.

Increment the package version for every build you deploy, keep the package name and
publisher stable for in-place upgrades, and record the exact APPX digest you tested.

## Deploy and observe

```sh
python3 -m tools.xbox_deploy deploy "$signed_dir"/*.appx --timeout 120   # name it exactly
python3 -m tools.xbox_deploy inspect-game --version "$v" --app-id "$app_id" --executable x64/Hades.exe
python3 -m tools.xbox_deploy observe --version "$v" --app-id "$app_id" \
    --image-name Hades.exe --frame-seconds 40 --timeout 180 --output run-2
python3 -m tools.xbox_deploy inspect-loader --version "$v" --app-id "$app_id" --run-id "$run_id"
python3 -m tools.measure run-2/fhd-present-times.bin \
    --start-utc "$scene_start_utc" --duration 30 --require-gpu-60 \
    --internal-resolution 1920x1080 --output-resolution 1920x1080
```

`observe` never drives the console, so it is the right tool when a person is holding the
controller: start it, let them press **Play**, and collect `observation.json`,
`loader-trace.txt`, `sdl-startup.txt` and the native frame trace into one directory. It
also gates a startup directly — `--require-checkpoint`, `--watch-checkpoint`,
`--fail-on-checkpoint` with per-stage budgets — which turns "it seems to hang" into a
named record and a deadline. While a launch is live, `inspect-render`, `inspect-input`
and `inspect-loader` read its GPU mailbox, hosted input mailbox and loader trace without
disturbing it.

Take the 30-second sample after the scene settles, and keep Device Portal screenshots
outside the window: a capture stalls the game for about half a second.

## Staging hygiene

The launcher streams the original image through SHA-256 while writing a temporary copy,
re-checks entitlement and build before creating the child, and removes the directory
after the trial. Interrupted launches leave copies behind: check the app's `TempState`
for stale trial directories when a deployment reports `ERROR_DISK_FULL`, and confirm the
trial processes have exited before removing them. The player's Steam files, saves and
signatures are never modified — adaptations happen only in the copy.

## Worked example

`analysis/hades-compatibility.md` is the shortest complete example: audited source
digests, two recipes (engine and launcher), the arguments file for the publisher's
gamepad options, the crash-report marker, the adapters declaration, and
`--surfaceless-profile` plus `--win32-trial-profile` plus the runtime template, with the
measured result and its open checks. `analysis/steam-compatibility.md` shows the same
machinery over many builds, which is what the loop looks like at scale.
