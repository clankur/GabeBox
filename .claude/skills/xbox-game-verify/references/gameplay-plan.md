# The gameplay plan

A verification session is a scripted play session with sampling attached. The script
matters because the same phases run again on the next build, and a difference between
two runs is only a regression if both runs did the same thing.

Contents: the phases · writing input scripts · adapting to the genre · what an
unattended run can and cannot settle · the audio gap.

## The phases

`scripts/verify-session.py` runs these in order. Each phase drives input with
`tools.xbox_remote_input` for its play window while sampling the hosted input mailbox
concurrently, then asks its question when a person is present. `--phase-set smoke`
keeps launch, world, wander and return for a recheck of a known title.

| # | Phase | Play | Sample | What happens | Sampled |
| --- | --- | ---: | ---: | --- | --- |
| 1 | `launch` | — | — | the observer waits for the Play request and collects startup artifacts | observation, optional checkpoints |
| 2 | `title` | 8 s | 6 s | one button press at the title screen | input, render, panel menu |
| 3 | `world` | 14 s | 10 s | start or continue until the game is being played | input, render |
| 4 | `wander` | 42 s | 30 s | traversal: two areas, both sticks, camera, a load boundary | input, render, **frame window** |
| 5 | `interact` | 42 s | 30 s | the primary verbs, five or more times | input, render, **frame window** |
| 6 | `systems` | 22 s | 20 s | inventory or map, change a setting, close, resume | input, render, panel menu |
| 7 | `keyboard` | 14 s | 12 s | type into a focused text field | input (`keyboard_ready`) |
| 8 | `audio` | 12 s | 10 s | trigger a named sound | input; audio checkpoints come from the collect step |
| 9 | `save` | 12 s | 10 s | save the way the game saves | input, render |
| 10 | `reload` | 14 s | 10 s | leave, reload that save, compare the state | input, render |
| 11 | `return` | 10 s | 6 s | exit to the library | input, panel menu |

Then one collect step for the whole session: `observe --run-id` for the artifacts and
the frame trace, `inspect-loader` for the exit code where the title is a staged Win32
trial, and `report --sdl-startup` for the SDL checkpoints.

Three constraints shaped those numbers, and they are worth keeping if you rewrite the
plan:

- **`inspect-input --seconds` is capped at 30**, so a sampled phase cannot exceed it.
  A longer phase needs more than one sample.
- **A measured window must fit inside the play window.** The 30 s frame window starts
  8 s after the phase begins, so the phase has to keep playing for at least 38 s — hence
  42 s for `wander` and `interact`.
- **Screenshots cost the game about half a second**, so each scene phase is bracketed
  rather than interrupted: one capture as the phase opens, one as it closes, both
  outside the measured window. Those captures are the evidence for whether the picture
  is the game at all — see `visual-checks.md` for the screening and the artifact
  taxonomy.

Capture a frame early — during `title` or `world`, not at the end. If it is black or
wrong, stop and report; the later phases cannot be interpreted anyway.

## Writing input scripts

The built-in cycles are deliberately generic — tap A, walk the sticks around, open a
menu — because they have to start somewhere on a title nobody has driven yet. They are
not a substitute for knowing the game. Once you know its verbs, write a script and pass
it in:

```sh
cat > scripts/hades-wander.txt <<'STEPS'
# Tartarus: leave the room, fight through the next, come back
hold:LS_UP:2  A  hold:LS_RIGHT:3  A*3  hold:LS_DOWN:2  X*2  hold:LS_LEFT:3
STEPS
python3 -m tools.xbox_remote_input --dry-run --script scripts/hades-wander.txt
verify-session.py ... --input-script wander=scripts/hades-wander.txt
```

`--dry-run` validates the grammar offline and prints the encoded messages, so a typo
costs nothing. Keep a script per title per phase next to the title's compatibility note,
so the next build is driven the same way.

Two rules make the samples worth anything:

- **Hold each input long enough to span a poll** — `hold:NAME:SECONDS`, not a tap —
  and let the sampler run while the input is in flight.
- **Record what the screen did**, from a screenshot or a person, in the same note as the
  sample. The mailbox tells you a serial was acknowledged; it cannot tell you the
  character moved.

## Adapting to the genre

The phases are roles, not a literal script:

- **Action or platformer**: wander = traverse two rooms and a transition; interact =
  attack, dodge, pick up, use, take damage.
- **Strategy or builder**: wander = pan and zoom across the map; interact = place,
  remove, queue, open a production chain; watch for a simulation-speed change.
- **Card or turn-based**: wander = move through the map or node graph; interact = play
  five cards or take five turns including an enemy turn and a reshuffle.
- **Narrative**: wander = walk two scenes; interact = advance dialogue, make a choice,
  open the journal.
- **Anything with a loading boundary**: cross it inside a sampled phase. Load
  transitions are where shared-texture and pacing problems surface.
- **Pointer-driven titles**: remote input's mouse messages do not raise host pointer
  events, so these cannot be driven this way. A person has to play them, or the pointer
  checks stay unobserved.

Pick scenes a later run can reproduce: a named room, a specific save, a fixed starting
loadout. "Act 1 combat with a fresh Ironclad" is a scene; "playing for a while" is not.

## What an unattended run can and cannot settle

With remote input, an unattended session genuinely verifies:

- that the game launched and reached its startup checkpoints,
- that controller and keyboard input were **delivered** — published by the host and
  acknowledged by the game-side adapter,
- presentation cadence, frame pacing and transport over a settled window,
- what the screen looked like, from screenshots taken during play, screened for black,
  flat, magenta, noisy, frozen and unlike-reference frames,
- the loader trace and the child's exit code for a staged Win32 trial.

It cannot settle, without someone watching or a later reading of the captures:

- whether the game **responded correctly** to the input — delivery is not response, and
  a pair of screenshots per phase is a thin proxy for it,
- whether anything is missing from a scene that otherwise screens clean: a character, a
  prop, a lighting pass, a UI layer,
- audio output,
- whether a reloaded save really matches the state that was saved,
- anything on a pointer-driven title.

So say which it was. "Driven by `tools.xbox_remote_input` with
`scripts/hades-wander.txt`; serial 4471 acknowledged at 14:03:21Z; `wander.png` shows
Zagreus mid-room" is evidence. "Controller input verified" is not.

## The audio gap

There is still no audio measurement in this project, so audio output rests on a person
listening. A minimal reporting facility that would change that:

1. **Sample what is actually submitted to the audio device** — peak and RMS over a short
   window, plus buffers submitted and underruns — in the shared audio path, so every
   title gets it rather than one.
2. **Publish counters the existing diagnostics can read**, in the same allowlisted
   record form the SDL and loader traces already use, so `report` and `observe` collect
   them with no new transport.
3. **Distinguish silence from absence**: a device that opened and submitted buffers of
   zeroes is a different finding from a device that never opened, and both differ from a
   title that never initialized audio.

Until then: report the SDL audio checkpoints or the loader trace's
`asset xaudio27-activation` record as *initialization*, an operator's confirmation of a
named sound as *output*, and `unobserved` when neither exists.
