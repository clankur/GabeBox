---
name: xbox-game-verify
description: >-
  Independently verify that a Steam title actually plays on the Xbox, and produce the
  evidence a coverage report can be built from. Use this skill whenever the job is to
  check someone else's work rather than to write it: a verifier subagent given acceptance
  criteria, a request to confirm a title is playable, or any question of the form "does it
  really run", "is it 60 FPS", "did input reach the game", "is there audio", "did the save
  reload", "can it return to the library", or "can we count this title yet". Use it to run
  a deep gameplay session — enter the starting state, wander the world, interact with its
  systems, drive the controller and keyboard, and sample FPS, render state, input delivery
  and audio while that happens. Use it as well when a run has already happened and the
  artifacts need grading, when deciding whether evidence is strong enough to count a title
  once, and when a verification cannot be completed because a capability is missing, since
  naming the gap precisely is part of the result.
---

# Verifying a game on the Xbox

You are the independent verifier. `AGENTS.md` asks that verification come from someone
other than the implementer, and the value you add is refusal to overstate: you run the
checks, record what was observed, and name what was not. A verification that ends in
"three checks pass, two remain unobserved because the package reports no audio counters"
is a success. A verification that rounds that up to "verified" is a failure, and it costs
the project a title in the coverage report that will not survive the next recheck.

Everything the implementing session tells you is a claim, not a finding — including its
summary, its numbers and its confidence. Reproduce each one from the raw artifacts
yourself: re-measure the trace rather than quoting its FPS, read the observation and
loader records rather than its description of them, and confirm the package version,
Steam build and commit you are actually looking at match the ones it says it tested. A
claim you cannot reproduce is unverified, however plausible it sounds, and a claim that
quietly swaps one thing for another — an import that resolved for a function that
behaves, a first frame for a playable game, input delivered for input acted on — is the
most common way a title gets counted that should not have been. Where you can choose,
choose the scene most likely to break the claim rather than the one that was
demonstrated to you, and be especially careful with a result produced by tooling written
in the same change: prefer the project's shared commands or an independent caller.

Never implement the fix you are verifying. If you find the cause, report it; let the
implementer change the code, and verify the next build.

## Before touching the console

1. **The Xbox is shared, and this session drives it.** Take your turn through the
   console queue — the `xbox-queue` skill (`xboxq`), described in
   [docs/XBOX-QUEUE.md](../../../docs/XBOX-QUEUE.md) — before launching a game or sending
   input, and hold it for the whole session rather than per command. Read-only Device
   Portal queries are fine meanwhile; `deploy`, a launch, and every
   `tools.xbox_remote_input` step are not. If the user is playing, stay read-only until
   they say the console is free, and release the queue when you finish.
2. **Pin what you are verifying.** Steam AppID and build ID, package version, console
   model and OS build, the commit or branch the package was built from, and the
   acceptance criteria you were given. A verification that cannot name its build verifies
   nothing later.
3. **Confirm the portal works before planning a session.** `fingerprint`, then `status`.
   Settings live in a local `.env`; see the project notes for which one and whether its
   certificate pin is current.
4. **Know which checks this package can even answer.** The diagnostics are opt-in at
   package build time, so ask first: is there a frame trace, are captures enabled, is the
   title on the hosted input mailbox? `references/metrics.md` maps each check to the
   package feature it depends on. Checks whose evidence the build cannot produce are
   `unobserved`, not `failed`, and the distinction matters — one is a gap in the test,
   the other is a defect in the game.

## The session

Run the phases in `references/gameplay-plan.md`: cold start, title, entering the starting
state, wandering, interacting, systems and menus, keyboard text, audio, save, reload, and
return to the library. Depth is the point. A title screen and a first frame tell you the
loader worked; they say nothing about whether the game is playable, and most regressions
in this project have surfaced minutes into real play — a hitch during combat, a save that
does not reload, a pad that goes dead after a focus change.

`scripts/verify-session.py` runs that plan: for each phase it drives the game with
`tools.xbox_remote_input`, samples the hosted input and GPU mailboxes while that input is
in flight, brackets each scene phase with screenshots, records each phase's UTC window,
collects the run artifacts at the end, measures the settled windows, screens every
capture with `scripts/inspect-frames.py`, and writes `verification.json`, a
coverage-shaped `trial.json` and `report.md`. Start with
`--plan`, and `--dry-run` to see the exact commands without touching the console.

```sh
verify=.claude/skills/xbox-game-verify/scripts/verify-session.py
python3 "$verify" --plan
python3 "$verify" --version "$package_version" --app-id "$app_id" \
    --build-id "$steam_build" --target "$console_and_runtime" \
    --image-name "$exe_basename" --env-file "$xbox_env" \
    --internal-resolution 1920x1080 --output-resolution 1920x1080 \
    --input-script wander=scripts/"$title"-wander.txt \
    --reference analysis/"$title"-reference \
    --output verify-$(date -u +%Y%m%dT%H%M%SZ)
```

The built-in input cycles are generic by necessity; once you know the title's verbs,
write a script per phase and pass `--input-script PHASE=FILE`. Use `--no-drive` when a
person is playing instead, and run the phases by hand whenever the title needs it — the
script exists to keep the sampling and the timestamps honest, not to constrain how the
game is played.

## Driving input

`tools.xbox_remote_input` sends controller, keyboard and mouse input through Device
Portal's `/ext/remoteinput` WebSocket, and the presenter merges the gamepad codes into
the hosted pad, so a game reading hosted XInput receives them as controller input. A
session can therefore drive the game without anyone at the console. `inspect-input`
reads the hosted mailbox and says whether the game's adapter acknowledged each serial,
which is how delivery gets proven rather than assumed.

- **Drive and sample together.** Start the input, sample the mailbox while it is in
  flight, and hold each input long enough to span a poll (`hold:NAME:SECONDS`). A tap
  between polls reads as `host-idle` and looks like a dead pad.
- **Say exactly what was sent and what came back.** "Drove `scripts/hades-wander.txt`;
  serial 4471 acknowledged at 14:03:21Z; `wander.png` shows the character mid-room" is
  evidence. "Controller input verified" is not.
- **Delivery is not response.** The mailbox proves the game read the sample; only a
  capture or a person shows that the game did the right thing with it. Pair them.
- **Two limits**: Device Portal mouse messages raise no host pointer events, so a
  pointer-driven title still needs a person, and overlay screenshots return HTTP 500.
- **Validate a script offline first** with `--dry-run`, and `--check` opens and closes
  the socket without sending anything.

## What to measure

`references/metrics.md` has the commands, the fields that matter, and the pass criteria
for each of: frame rate and pacing, visible gameplay, controller delivery, keyboard
delivery, audio, save and reload, and return to the library. In short:

| Check | Evidence that settles it |
| --- | --- |
| Frame rate | `tools.measure` over a settled 30 s window: the average should be about 60 FPS, and the weakest gameplay window decides. Hitches are recorded as issues, not folded into the average |
| Gameplay is visible | a screenshot taken during play (`shot:` step or the endpoint — it does return real TV content), or the package's game capture; the startup BMP is a startup frame and persists all launch, so it is not evidence of the live scene |
| The picture is the game | captures from several scenes, screened with `scripts/inspect-frames.py` for black, flat, magenta, noisy, frozen or unlike-reference frames, and then looked at: objects, scenery, UI and colour all present and correct |
| Controller | `inspect-input` showing `host-activity` and an acknowledged serial while a named input was held, plus the on-screen response |
| Keyboard | the same, for the key queue, plus the text actually appearing in the game |
| Audio | initialization from the SDL audio checkpoints or the loader trace's `asset xaudio27-activation` record, plus a person confirming a named sound — this project measures no audio signal |
| The Steam menu | `inspect-input --panel-menu` recording a menu request during the window, plus captures showing the menu over the game and the game resuming afterwards |
| Save and reload | a save created in one session and its state present after a reload, named concretely |
| Return to library | the game exits, Big Picture is responsive, and the app stays open |

Capture an actual frame early — seconds into the run, not minutes — and abort if it is
wrong. Long waits for a late sample are how an hour gets spent on a build that was black
from the first frame.

Counting a title is a stricter bar than passing this session.
`tools.steam_compatibility coverage` reads trials whose checks are named `gameplay`,
`controller`, `audio`, `save-reload` and `return-to-library`, all true, with a matching
build, and under `--require-gpu-60` it also demands `performance_scene: "gameplay"` and
a sample reaching `gpu_60fps` at internal and output resolution exactly `1920x1080`. The
session runner writes `trial.json` in that shape; say plainly when a run passes the
average-FPS verdict but misses the strict gate.

## Reporting

Write the report as `report.md` beside the artifacts, and keep it to what happened:

- The build: AppID and Steam build, package version, console and OS build, commit.
- One row per check: `pass`, `fail` or `unobserved`, the evidence file or sample that
  settles it, and for a failure the first thing that went wrong, quoted.
- The numbers, in full, per measured window: average FPS, lowest rolling one-second
  count, longest frame interval, how many intervals ran past 33.33 ms, internal and
  output resolution, and whether frames used shared GPU textures or CPU uploads.
- The captures, and what they show: which scenes were photographed, what the screen
  flagged, and what you saw when you opened them. A rendering defect belongs in the
  verdicts with its capture attached, not as a footnote under a passing frame rate.
- Where your observation and the implementing session's claim disagree, both, quoted:
  their sentence, your evidence, and which raw artifact settles it. Do not split the
  difference or quietly adopt their wording.
- An issues list separate from the verdicts: every hitch, every rolling-second dip, every
  CPU upload, every flagged frame, each tied to the phase it happened in. A title that averages 60 and stalls
  for 450 ms in combat passes the frame-rate check and still has a defect worth fixing —
  both facts belong in the report, and burying either one makes it useless.
- What remains unobserved and exactly what would settle it — a package feature, another
  build, a longer session, a second title.
- Regressions against the previous verified build, if there was one.

Count a title as covered only when gameplay, controller, audio, save and reload, and
return to the library have all been observed on the recorded builds, and every measured
gameplay window averaged about 60 FPS or the game's own documented cap, with any
remaining hitches recorded as open performance work — and note whether the strict
coverage gate above is also met. Evidence from one title covers that title: before
claiming a shared runtime works in general, exercise a second title or an independent
caller with a different usage pattern, and record the scope when you have not. Installs, launches and first
frames are intermediate evidence. Recheck when the Steam build or the shared runtime
changes.

## When something blocks the run

Finish every check that does not depend on the blocker, then say what blocked and what it
would take to clear it. If the blocker is the console itself — held by another session,
in use by the user, unreachable — do the off-console half (artifact grading, trace
measurement of an earlier run, reading the package's declared diagnostics) and leave the
console checks named and pending. Report once; do not send repeated waiting messages.

## References

| File | Read it for |
| --- | --- |
| `references/metrics.md` | Per-check commands, field meanings, pass criteria, and what each check does not establish |
| `references/gameplay-plan.md` | The phase-by-phase play plan, per-genre adaptation, and the audio capability gap |
| `references/visual-checks.md` | Capturing and screening frames, the artifact taxonomy, reference sets, and judging whether the scene is the game |
| `scripts/verify-session.py` | Runs the plan, drives and samples each phase, measures the windows, screens the captures, writes the report |
| `scripts/inspect-frames.py` | Screens PNG and BMP captures for artifacts, frozen scenes and unlike-reference frames |
