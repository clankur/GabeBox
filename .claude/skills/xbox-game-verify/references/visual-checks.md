# Does the picture look like the game?

A frame counter proves the pipeline is moving. It says nothing about whether the scene
is the game's scene. This layer's characteristic failures are visual: an atlas page that
decoded at the wrong size, a world that renders black while the UI draws fine, geometry
that never arrives because a shader failed to compile, scenery with missing-texture
magenta where a format was not supported. All of those present as a game that "runs".

So treat the captures as evidence in their own right, and say what you saw.

Contents: capturing · screening · the artifact taxonomy · reference sets · judging
"this is not the game".

## Capturing

```sh
python3 -m tools.xbox_remote_input shot:open.png hold:LS_UP:2 hold:LS_RIGHT:2 shot:close.png
```

A Device Portal screenshot returns real 1920 × 1080 TV content and costs the game about
half a second, so bracket the measured window rather than shooting inside it — one
capture as a phase opens, one as it closes. The session runner does this for the title
and scene phases automatically.

Capture in more than one scene. A single screenshot of a menu is the weakest possible
visual evidence: menus often draw correctly while the world does not, which is exactly
the failure mode worth catching. Aim for the title, the first playable scene, a
traversal, an interaction, and whatever the title's distinctive visual feature is —
its lighting, its particle effects, its map, its card art.

For a title whose package writes its own capture, the game-capture request produces
`big-picture-game-video.png`; for an SDL-hosted title the one-shot startup BMP is a
startup frame only, and it persists for the whole launch, so it is never evidence about
a later scene.

## Screening

`scripts/inspect-frames.py` reads the captures a session collected and reports, per
frame, the statistics that catch the mechanical failures:

```sh
python3 .claude/skills/xbox-game-verify/scripts/inspect-frames.py verify-run \
    --reference analysis/hades-reference --json frames.json
```

| Flag | What it usually means |
| --- | --- |
| `black-frame` | the game or the presenter produced nothing; confirm which by reading the backbuffer in-process, as the Spider-Man work did |
| `flat-frame` | one colour covers the frame: a clear with no draws |
| `blown-out` | white-out, usually a format or gamma mismatch |
| `magenta-blocks` | the classic missing-texture fill: an asset or format the loader rejected |
| `low-detail` | almost nothing is drawn, though the frame is not black — missing geometry or a failed pass |
| `noise` | high-frequency garbage: uninitialized or corrupted texture memory |
| `static-scene` | consecutive captures are identical while movement input was being driven — the scene is frozen, or frames are stale |
| `unlike-reference` | the frame does not resemble any known-good capture of this title |

Thresholds are starting points and can be moved with `--set NAME=VALUE`; a dark game
will sit near `detail_low` legitimately. `--motion-phase` names the phases whose frames
must change between captures (a title screen is legitimately still, and flagging it
would teach the reader to ignore the flag that matters).

**A clean screen is not a pass.** It cannot see a missing NPC, the wrong skybox, an
inventory that failed to draw, or scenery from the wrong biome. It narrows hundreds of
frames to the few worth opening. Open the flagged ones, and a sample of the clean ones.

## The artifact taxonomy

When you do see something wrong, name the symptom and the component it points at, so
the implementer starts somewhere:

| What you see | Where it usually comes from |
| --- | --- |
| World black, UI correct | the world's render pass or its shader pipeline, not the presenter — the presenter is clearly delivering the UI |
| Everything black, presenter reports frames | the game's backbuffer; read it in-process before blaming the transport |
| Missing-texture magenta on specific assets | the asset decoder: a format or size the packaged decoder rejects, as with the half-size atlas decode for the Slay the Spire JPEG pages |
| Textures present but wrong size or stretched | atlas coordinates kept while the page was rescaled, or a viewport and logical-size mismatch |
| Objects missing, world otherwise right | draw calls dropped on a failed pipeline creation; check the loader trace's pipeline counts and the last failure HRESULT |
| Geometry flickering or z-fighting | depth buffer format or precision in the translated swap chain |
| Colours washed out or oversaturated | swap chain format and colour space: sRGB versus UNORM across the shared texture |
| UI misplaced or clipped at the edges | the virtual window's size versus the game's logical rendering size |
| Scene correct but frozen | publish cadence or a stale shared-texture generation; cross-check `inspect-render` producer sequence and the frame trace |
| Torn or interleaved halves | a frame published while the consumer was reading it: fence or generation handling |

Record the AppID, build, package version and the capture file with every finding, and
say which scene it was taken in. "Magenta blocks on the Act 1 map in `wander-open.png`,
build 10929685, package 1.0.232.60" is actionable; "textures look wrong" is not.

## Reference sets

A comparison needs something to compare against. Keep a small set of known-good captures
per title — from a verified Xbox build, or from the game on Windows at the same
resolution and scene — next to the title's compatibility note, and pass it as
`--reference`. Four or five frames covering the distinctive scenes is enough.

The comparison is coarse by design: a 16 × 16 luma signature, which tolerates small
camera differences and still catches a frame that is a different thing entirely. It
cannot tell you a specific object is missing — that is a job for your eyes on the two
images side by side.

When a reference set does not exist yet, create it from the first build that a person
confirms looks right, and say in the report that the comparison baseline starts there.

## Judging "this is not the game"

Three questions, in order, and answer them from the captures rather than from memory:

1. **Is the scene present at all?** Geometry, characters, background, effects, UI — list
   what should be on screen for the scene you captured and check each is there.
2. **Is it drawn correctly?** Colour, scale, placement, layering, text legibility. A
   detail that is subtly wrong — a UI drawn at half scale, a scene missing its lighting
   pass — is a defect even when nothing is obviously broken.
3. **Does it match the game?** Compare with the reference set, the publisher's own
   material, or the same scene on Windows. If you cannot tell, say you cannot tell and
   name what would settle it.

If the answer to any of these is no, the title is not playable in the sense this project
counts, whatever the frame rate says. Record it as a failure with the capture attached,
not as a note under a passing result.
