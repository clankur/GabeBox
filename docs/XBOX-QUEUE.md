# Shared Xbox queue

Several sessions and worktrees share one Series X. Installing a package replaces the profiles other sessions staged, and overlapping launches mix their evidence. `tools.xbox_queue` serializes console work through one FIFO queue on this Mac, in the shape of [runq](https://github.com/clankur/runq): a local server with an integrated worker, a client, a CLI, and a dashboard. It uses only the Python standard library.

## Start the server

```sh
python3 -m tools.xbox_queue server --detach
```

The server listens on `http://127.0.0.1:8765`, or on `--port`; it does not read `XBOXQ_SERVER`. It keeps state in `~/.xboxq` (`queue.db`, `logs/<id>.log`, `server.log`), or in `XBOXQ_HOME` when that variable is set. The directory is private to your user. Only one server can own a state directory: a second `server` command reports the running one and exits. On startup the server writes `~/.xboxq/bin/xboxq`, a launcher that runs this checkout's CLI with `python3`. Older checkouts without `tools/xbox_queue` can use the launcher. Start the server from a long-lived checkout, such as the main one. A worktree that is later removed breaks the launcher. Clients find the server through `--server`, then `XBOXQ_SERVER`, then the default address. The dashboard at `/` refreshes every three seconds.

## Two kinds of turn

| Kind | Command | What the worker does |
| --- | --- | --- |
| Command job | `run` (waits) or `submit` (returns) | Runs the shell command in the submitter's checkout and writes its output to the job log. On timeout (default 15 minutes) or cancel, it stops the whole process group, escalating to SIGKILL. When the command exits, it also stops any processes the command left running, so none of them carry into the next turn. |
| Lease | `hold` | Grants the console to the holder until it is released, cancelled, or expires. The TTL defaults to 20 minutes and is reset by `renew`. |

Both kinds wait in the same queue, so a held lease delays the command jobs behind it. Limits are capped at four hours.

```sh
python3 -m tools.xbox_queue run --name "named-pipe probe" --timeout 300 -- \
  python3 -m tools.xbox_deploy probe --case named-pipe --version 1.0.10.0 --json
python3 -m tools.xbox_queue hold --name "Hades loader trial" --ttl 1800
python3 -m tools.xbox_queue renew 12
python3 -m tools.xbox_queue release 12
```

After `--`, a single argument is a shell string (`run -- 'make && ./probe'`), and several arguments are quoted as one command (`run -- python3 -m tools.xbox_deploy status`). `run` exits with the command's exit code. It returns 124 after a timeout, 130 after a cancel, and 128 plus the signal number when a signal killed the command. Interrupting `run`, or `hold` while it waits, cancels the job. Once `hold` has returned, end the lease with `release`. Run `status` to see who holds the console, and `list` for active jobs with the last few results. Use `logs [-f] <id>` to read a job log, `wait <id>` to block until a job finishes or a lease is granted, and `cancel <id>` to drop a job. `pause --reason …` stops granting new turns, for example while someone plays on the console. Work that is already running continues. Use `resume` to start granting turns again.

Each job records its owner, from `--owner`, then `XBOXQ_OWNER`, then the git branch, then the directory name. It also records the checkout, branch, and commit, with `+dirty` added when tracked files have uncommitted changes. The command receives `XBOXQ_JOB_ID`.

## Job states

`pending` → `running` → one terminal state:

- Command jobs end as `completed` (exit 0), `failed`, `timed_out`, or `cancelled`.
- Leases end as `released`, `expired`, or `cancelled`.

When the server restarts, it resumes a running lease. A running command is marked `failed`, because the worker no longer supervises it. If its recorded process still has the same start time, the server stops the process group before it grants the next turn. A reused process ID is left alone. Cancelling a job the worker has just started stops it rather than reporting a dequeue.

## HTTP API

| Method and path | Purpose |
| --- | --- |
| `GET /api/state` | Device, pause reason, the job that holds the console, and the pending jobs |
| `GET /api/jobs?status=a,b&owner=&limit=` | Newest jobs first |
| `POST /api/jobs` | Submit `{kind, command, cwd, name, owner, limit_s, git_branch, git_commit}` |
| `GET /api/jobs/<id>` | One job, with `position` while it is pending |
| `GET /api/jobs/<id>/log?offset=N` or `?tail=N` | Log bytes. `X-Log-Size` gives the next offset, and `X-Job-Status` gives the job status. |
| `POST /api/jobs/<id>/cancel`, `/renew` `{limit_s}`, `/release` | Change a job |
| `POST /api/pause` `{reason}`, `POST /api/resume` | Stop or resume granting turns |

The server runs shell commands as your user for any local process that can reach the loopback address, so it binds only to that address. It rejects a `Host` header other than `127.0.0.1:<port>` or `localhost:<port>`, which protects against DNS rebinding. It also rejects cross-origin and non-JSON writes, so a web page cannot submit a job.

## Scope

The queue coordinates the tools that honor it. `xbox_deploy` does not check for a lease yet. The queue grants turns only and does not reset the console between jobs. Each job or lease holder leaves the console at the library when it finishes.
