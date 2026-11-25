---
name: xbox-queue
description: >-
  Take turns on the shared Xbox through the local coordination queue (xboxq). Use this
  skill before anything that deploys a package, launches or restarts an app, runs a
  probe case, drives the controller or keyboard, or otherwise changes what is running on
  the Series X — whether it is one scripted command or an interactive debugging session.
  Also use it to answer "is the Xbox free", "who is on the console", "queue this probe",
  "wait for the console", "release the Xbox", or when the user says they are playing and
  the console should be left alone. Several Claude and Codex sessions share one console,
  so reach for this even when the request only says "deploy", "launch", or "test it on
  the Xbox".
---

# Taking turns on the shared Xbox

One Series X serves every worktree and agent on this Mac. Each install replaces the
other sessions' profiles, and two operators launching at once corrupt each other's
evidence. The queue grants the console to one job at a time, in order. Full reference:
[`docs/XBOX-QUEUE.md`](../../../docs/XBOX-QUEUE.md).

## What needs a turn

- **Needs the queue:** `xbox_deploy deploy`, `probe`, `sdl-trial`, `lifetime`, launching
  or terminating apps, `xbox_remote_input`, anything that drives Big Picture or a game.
- **Free to run anytime:** read-only Device Portal queries — `fingerprint`, `status`,
  `report`, `inspect-*`, `observe` of a run someone else owns — and all off-console work.

## Find the CLI

```sh
python3 -m tools.xbox_queue status      # in a checkout that has tools/xbox_queue
~/.xboxq/bin/xboxq status               # any checkout, once the server has run
```

Below, `xboxq` means whichever of these works. If it reports the server is not reachable, start it once from a checkout that has the
module, preferably the main checkout rather than a worktree that may be cleaned up:
`python3 -m tools.xbox_queue server --detach`. Only one server can run per state
directory, so never pass a different `--port` or `XBOXQ_HOME` to get around a running
one. The dashboard is at <http://127.0.0.1:8765/>.

Jobs are labelled with your git branch. Set `XBOXQ_OWNER` (or `--owner`) if that is not
enough to tell sessions apart, and give every job a `--name` (title or probe case).

## Pick the shape

**One scripted step → `run`.** The worker runs the command in your checkout, streams
the log, and exits with the command's exit code (124 = timed out, 130 = cancelled):

```sh
xboxq run --name "named-pipe probe" --timeout 300 -- \
  python3 -m tools.xbox_deploy probe --case named-pipe --version 1.0.10.0 --json
```

A single argument after `--` is run as a shell string; several are quoted as one
command. It blocks while others are ahead, so launch it with Bash `run_in_background`
when the queue is busy or the step is long; you are notified when it exits. Processes
the command leaves running are stopped when it ends, so don't background a long-lived
helper inside a `run`; hold a lease for that. `submit` queues
without waiting; `wait <id>` and `logs <id>` follow up.

**An interactive session → `hold` a lease.**

```sh
xboxq hold --name "Hades loader trial" --ttl 1800   # returns once the console is yours
# ... deploy, launch, observe, screenshot, iterate — run commands directly ...
xboxq renew <id>                                     # before the TTL runs out
xboxq release <id>                                   # always, success or failure
```

While you hold a lease, run console commands directly — a `run` job would queue behind
your own lease. An unreleased lease expires after its TTL and the next job starts, so
renew during long sessions and stop touching the console once it has expired.

## Etiquette

- Keep turns short and fail fast: small `--timeout`/`--ttl`, an early screenshot, abort
  on the first wrong frame. Others are waiting.
- Report the wait once (`run` and `hold` print one line about who holds the console) —
  no repeated "still waiting" messages, no polling loops.
- When the user is playing, `xboxq pause --reason "user playing"`; nothing new starts
  and running work finishes. Only `resume` when the user says the console is free, and
  never resume a pause someone else set without asking.
- Cancel your own jobs that are no longer needed (`cancel <id>`); never cancel or release
  another session's job — message that session instead.
- Record the job or lease id alongside the build and run evidence it produced.
