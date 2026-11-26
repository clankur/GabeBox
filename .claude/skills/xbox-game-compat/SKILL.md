---
name: xbox-game-compat
description: >-
  Adapt a Steam game to this repository's Xbox compatibility layer, and diagnose any
  game that fails on a Series X in Developer Mode. Use this skill whenever work touches
  getting a Steam title to install, launch, render, accept input, save, or hold 60 FPS
  on Xbox: a game that exits during startup, a missing DLL or unresolved import, a
  loader error such as 0xC0000135, 0xC0000139, ERROR_MOD_NOT_FOUND or
  ERROR_CALL_NOT_IMPLEMENTED, a black screen or no first frame, a crash at a known RVA,
  dead controller input, no audio, broken saves, a failed return to the library, or
  frame pacing below the 60 FPS gate. Use it as well for the research half of that work
  — reverse engineering a game binary with REA, mining Valve's Proton and Wine for the
  behavior an Xbox adapter has to reproduce, writing or extending an XboxWin32* adapter,
  generating a hash-bound import recipe, pinning a build-matched profile, or deciding
  whether a title can be counted as covered. Reach for it even when the request names
  only a game ("why does Hades crash on the console", "get Factorio past the loader"),
  and even when the console is not reachable, since the audit, recipe and control work
  all happen off-console.
---

# Adapting a Steam game to the Xbox compatibility layer

A game runs on Xbox when every behavior it depends on is either supported by the
console or supplied by a reusable component of this layer, and when the adaptation is
bound to the exact Steam build it was audited against. Nothing here is finished by
reasoning about it: each step either produces console evidence or produces a named,
pending console check.

Read `AGENTS.md` at the repository root first — it holds the standing rules on reuse,
evidence and coverage, and this skill is how to apply them to one title.

## Orient before touching anything

1. **Pin the target.** Steam AppID, Steam build ID, the publisher's launch executable,
   and the exact Xbox package version and console model under test. Every artifact you
   produce is named after the AppID and build; an adaptation that is not pinned to a
   build is not reusable and not verifiable.
2. **Find the tooling.** The tree is being reorganized, so resolve the command names
   once instead of guessing:

   ```sh
   ls tools/xbox_deploy.py probe/prepare-steam-core.py 2>/dev/null
   ```

   On `main` everything is a module: `python3 -m tools.xbox_deploy`,
   `python3 -m tools.measure`, `python3 -m tools.steam_compatibility`,
   `python3 -m build_tools.redirect_pe_imports`,
   `python3 -m build_tools.prepare_big_picture_library`,
   `python3 -m build_tools.prepare_steam_core`. Adapters live in `runtime/win32/`,
   profiles in `profiles/APPID-BUILDID/`, guides in `docs/`. Older branches still carry
   the script layout (`probe/*.py`, `xbox-deploy.py`, `probe/win32-runtime/`); check
   before you copy a command from an old note, and run `--help` on anything you have not
   used this session, because flags move.
3. **Take your turn before driving the console.** Several sessions share one Series X:
   claim it through the console queue (the `xbox-queue` skill) before deploying,
   launching, or sending input, and release it when you finish. Read-only Device Portal
   queries are fine meanwhile.
4. **Check what you can reach.** Is Device Portal configured (`.env`,
   `tools.xbox_deploy fingerprint`)? Is the title installed on the console
   (`inspect-game`)? Is `rea` on PATH? Does a local Windows install of the game exist to
   audit? Say plainly which of these you have before planning around them.
5. **Find where the evidence lives.** Much of the recorded console history sits on
   feature branches and in the worktrees under `.claude/worktrees/`, not on `main` — a
   title you are about to investigate may already have twenty recorded passes and
   adapters that the current checkout lacks. Before deriving anything, search the other
   checkouts (`git log --all`, `git grep` across branches, the sibling worktrees) and
   say which branch an adapter or a finding comes from. Quote the file for any claim you
   attribute to the repository, and keep what the repository records separate from what
   you infer; cherry-picking existing work usually beats rediscovering it.

## The loop

Each pass through the loop moves the first failing operation later in the game's
startup. Expect several passes per title; the Hades and Factorio notes in `analysis/`
are what a long run of this loop looks like.

### 1. Reproduce and classify the first failing operation

Observe a real launch rather than inferring from a symptom. The observer does not drive
the console, so a person can select **Play** while it watches:

```sh
python3 -m tools.xbox_deploy observe --version "$version" --app-id "$app_id" \
    --image-name "$exe_basename" --frame-seconds 40 --timeout 180 --output run-1
python3 -m tools.xbox_deploy report --version "$version" --sdl-startup
```

Then place the failure in exactly one class, because the class selects the fix:

| Evidence | Class | What it means |
| --- | --- | --- |
| Child exit `0xC0000135` | dependency | a whole module is absent; look for a signed framework package or an app-local copy |
| Child exit `0xC0000139` | missing export | the module loaded, one function did not resolve: import adapter |
| `ERROR_MOD_NOT_FOUND` / `ERROR_PROC_NOT_FOUND` in the loader trace | dynamic load | a bare `LoadLibrary`/`GetProcAddress` the loader-trace adapter can satisfy |
| A resolved export returning `ERROR_CALL_NOT_IMPLEMENTED` or error 120 | stub behavior | the symbol exists on Xbox and does nothing; only a functional control proves the difference |
| Access violation at an RVA | contract violation | a call returned null or an unexpected value and the engine dereferenced it |
| No first frame, no fault | presentation | windowing, context, swap chain or presenter path |
| Frames present, FPS below gate | transport or pacing | CPU uploads vs shared textures, swap clock, publish cadence |
| No response to the pad | input | which XInput module the staged image imports, and whether the host mailbox is primed |
| Fails at save, config or crash reporting | desktop services | profile-folder, registry or reporting adapters |

While a launch is live, three read-only inspectors answer the follow-up questions
without disturbing it: `inspect-render` for the GPU mailbox and any captures the package
wrote, `inspect-input` for the hosted input mailbox and whether the game acknowledged a
sample, and `inspect-loader` for the live loader trace, pipeline compiles and the child
exit code. When the question is whether a startup stage is reached at all, gate it with
`observe --require-checkpoint` and let a named record and its budget answer instead of a
reading of the trace.

Record the Steam AppID and build, the Xbox package version and OS build, and the first
failing operation verbatim. Keep "the import resolved", "the call behaved", "a frame
appeared" and "the game played" as four separate claims; the repository's reports stay
trustworthy because they never merge them.

### 2. Learn the contract the game actually needs

Fixing the right thing requires the game's own usage, not the API's documentation. Use
REA on the installed Windows image to find the call site, the arguments, and — most
importantly — what the game does with each return value. A function whose failure the
game already branches around needs a cheap honest failure, not an implementation.

```sh
engine="$install/x64/Engine.dll"
rea inspect-pe-imports "$engine" --format csv \
    --filter-output normalized_result.imports --columns name
rea search "$engine" ImmGetIMEFileNameA --token-limit 2000
rea function "$engine" 0x370724 --token-limit 4000   # needs a native provider
```

Every REA command takes the target path first. The static inventory commands
(`inspect-pe-imports`, `inspect-native-exports`, `inspect-pe-load-config`, the managed
`inspect-managed-*` set) need no decompiler; `function`, `decompile`, `xrefs` and
procedure search need a configured provider and otherwise fail with
`provider_unavailable` — LLVM objdump covers disassembly in the meantime, which is why
the repository's own scanners take an `--objdump` path. See
`references/rea-cookbook.md` for provider setup, the managed (.NET) commands, the
delay-import and file-size limits, and how to keep REA's output small. Run REA work in
a subagent when the inventory is large; return the RVAs and the behavioral finding, not
the dumps.

Pair this with the on-console import report: a static import list says what the game
asks for, and `build_tools.prepare_steam_core --inspect-pe` plus the resulting control say what
the console answers.

### 3. Find who already implements that contract

Valve solved most of these contracts for Linux, and Wine's implementation is the
clearest available specification of what a Win32 function must do, including its error
cases. Look it up and cite it:

```sh
.claude/skills/xbox-game-compat/scripts/upstream-ref.sh ImmGetIMEFileNameA
.claude/skills/xbox-game-compat/scripts/upstream-ref.sh CM_Register_Notification --module dlls/cfgmgr32
.claude/skills/xbox-game-compat/scripts/upstream-ref.sh --repo proton --path proton --grep appid
```

The script keeps a sparse blobless clone of `ValveSoftware/wine` at `proton_11.0` (and
Proton, DXVK, vkd3d-proton), prints the definition, and emits a permalink pinned to the
commit it read — which is the citation form the `analysis/` notes use. Game engines are
reference material too: SDL, Mesa, Godot, libGDX and MonoGame show what the engine
expects on the other side of the call.

Treat upstream as a behavioral specification. Read it for the contract and implement
against Xbox APIs; if a component is actually reused, pin it, carry its license with it,
and verify its exports the way the packaged SDL and Mesa components already are. Map
Linux-side components onto their Xbox equivalents rather than importing them: see
`references/proton-reference.md`.

### 4. Implement, in order of preference

1. **An existing adapter already covers it.** Add the binding to the profile recipe and
   nothing else. Check `window.def`, `desktop.def`, `input.def`, `dxgi.def`,
   `crash-report.def` and `loader-trace.def` in the Win32 runtime before writing code.
2. **An existing adapter is close.** Extend that shared component so every title gets
   the behavior, and keep it free of AppID, account and install-path specifics.
3. **Nothing covers it.** Add a component beside its peers, export the original symbol
   names, preserve the calling convention, return values, ownership and error behavior,
   and give it a control that exercises it alone on the console.

Title-specific facts belong in the declarative profile, never in shared code: the
launcher name, the dynamically loaded engine image, the renderer selection, launch
arguments, and the markers that opt this build into a behavior. The import recipe binds
to the audited file's size and SHA-256, so staging refuses a changed build instead of
adapting the wrong bytes. `references/profiles-and-packaging.md` has the exact file
contract, the marker list, and the packaging and deployment commands.

Stage into a temporary copy. The player's Steam install, saves and signatures stay
untouched, and generated staging directories get cleaned up after the child exits —
a forgotten one filled the volume and produced `ERROR_DISK_FULL` on a later update.

### 5. Prove it

In this order, because each step is cheap compared with the next:

1. Local tests: `python3 -m unittest discover -s tests` and
   `node --test tests/*.test.js` (plus `uv run --locked python -m pytest` and
   `uv run --locked ruff check` where the project uses them).
2. A signed control on Xbox that exercises the new behavior alone. Controls encode the
   first failed stage in their exit code, which is what makes a one-line Device Portal
   result diagnostic.
3. The game through the ordinary Big Picture **Play** flow, observed as in step 1.
4. Frame measurement on a settled scene:
   `python3 -m tools.measure run-2/fhd-present-times.bin --start-utc "$t" --duration 30 --require-gpu-60`.
5. An independent verifier subagent, not the agent that implemented the change. The
   `xbox-game-verify` skill is that role's playbook: the gameplay session, the per-check
   evidence rules, and the report the coverage count is built from.

## Using subagents

The work parallelizes along the axes of the loop, and keeping heavy output out of the
main context is most of the benefit:

- **Audit agents**, one per candidate image (launcher, engine, companions), each running
  REA plus the repository's scan and returning imports that have no Xbox answer, with
  RVAs for the interesting ones.
- **Upstream agents**, one per failing contract, each returning the Wine or engine
  implementation as `file:line` plus a permalink and a short statement of the contract,
  including what it returns when the feature is unavailable.
- **A verifier agent** given the acceptance criteria, the exact package version and the
  test setup, which runs the checks itself and reports commands, observed results,
  regressions and gaps. Verification must not come from the agent that wrote the change.

Tell each agent to report evidence rather than conclusions: the command it ran, the
observed output, and what remains unobserved. If an agent cannot establish something,
the right answer is the named gap.

## When the console is out of reach

Most of this loop is off-console work, so do all of it and leave one clearly labelled
boundary. Finish the audit, the recipe, the profile, the adapter, the control, the
local tests and the signed package; then write the pending console checks as exact
commands with their acceptance criteria, in the compatibility note and in your reply.
Notify the user once that a console run is the only thing outstanding, and name what it
would establish. Never report an unrun check as passing, and never let the absence of
hardware stop the parts that do not need it.

## What counts as done

A title is covered once, per coverage report, after gameplay, controller input, audio,
save and reload, and return to the library are all observed on the recorded game build
and runtime build, with the frame sample meeting the 60 FPS gate at 1920 × 1080 or the
game's own documented cap. Installs, launches and first frames are intermediate
evidence worth recording and not sufficient. A Steam update or a shared-runtime change
invalidates the evidence for the affected titles — recheck them.

## Write it down where the next pass will find it

Add the result to the title's note in `analysis/` (or create one in the form of
`analysis/hades-compatibility.md`): the AppID and build, the Xbox package version, the
first failing operation, the adapter or profile change, the upstream reference with its
permalink, the measured numbers, and the checks that still need a live run. The next
pass starts from that note, and the coverage report is assembled from them.

## References

| File | Read it for |
| --- | --- |
| `references/rea-cookbook.md` | REA setup, the PE and managed commands worth running, output budgeting, limits |
| `references/proton-reference.md` | Which Proton/Wine component answers which failure, and the Xbox equivalent |
| `references/profiles-and-packaging.md` | Profile and marker file contract, recipes, packaging, deploy, observe, measure |
| `scripts/upstream-ref.sh` | Cited upstream lookups from a cached sparse Wine/Proton tree |
