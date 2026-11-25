"""Queue tests and probes for the shared Xbox so only one session drives it at a time.

Batch work:   submit / run a shell command; the worker runs it in your checkout.
Interactive:  hold a lease, drive the console yourself, then release it.
"""

from __future__ import annotations

import argparse
import codecs
import json
import os
import shlex
import signal
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

if not __package__:  # run as a script path, e.g. from ~/.xboxq/bin/xboxq in an older checkout
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.xbox_queue.client import Client, QueueError, server_url  # noqa: E402
from tools.xbox_queue.db import DEFAULT_DEVICE, DEFAULT_TIMEOUT, DEFAULT_TTL, PENDING, RUNNING, home  # noqa: E402

EXIT_CODES = {'completed': 0, 'released': 0, 'cancelled': 130, 'timed_out': 124, 'expired': 125}


def _when(epoch) -> str:
    return datetime.fromtimestamp(epoch).strftime('%H:%M:%S') if epoch else '-'


def _describe(job) -> str:
    what = job.command if job.kind == 'command' else 'interactive lease'
    if job.status == PENDING:
        detail = f'{job.position} ahead'
    elif job.status == RUNNING and job.kind == 'lease':
        detail = f'until {_when(job.expires_at)}'
    elif job.status == RUNNING:
        detail = f'since {_when(job.started_at)}'
    else:
        detail = job.note or (f'exit {job.exit_code}' if job.exit_code is not None else '')
    name = f' [{job.name}]' if job.name else ''
    return f'#{job.id:<4} {job.status:<10} {job.owner or "-"}{name}: {what}' + (f'  ({detail})' if detail else '')


def _exit_code(job) -> int:
    if job.status == 'failed' and job.exit_code is not None and job.exit_code < 0:
        return 128 - job.exit_code  # killed by a signal, as a shell would report it
    if job.status == 'failed':
        return job.exit_code or 1
    return EXIT_CODES.get(job.status, 0)


def _command_text(parts: list) -> str:
    if parts and parts[0] == '--':
        parts = parts[1:]
    if not parts:
        raise SystemExit('Give the command to run after --, e.g. submit -- python3 -m tools.xbox_deploy status')
    return parts[0] if len(parts) == 1 else shlex.join(parts)


def _announce_wait(client: Client, job) -> None:
    """One line about what this job waits behind; no repeated progress messages."""
    state = client.state()
    holder = state.get('running')
    if holder and holder['id'] == job.id:
        holder = None
    if not (job.position or holder or state.get('paused') is not None):
        return
    parts = [f'queued #{job.id}, {job.position} ahead']
    if holder:
        parts.append(f'console held by #{holder["id"]} ({holder["owner"]})')
    if state.get('paused') is not None:
        parts.append(f'queue paused: {state["paused"]}')
    print('; '.join(parts), file=sys.stderr, flush=True)


def _cancel_on_interrupt(client: Client, job_id: int):
    def handler(*_):
        client.cancel(job_id)
        print(f'\ncancelled #{job_id}', file=sys.stderr)
        raise SystemExit(130)
    signal.signal(signal.SIGINT, handler)
    signal.signal(signal.SIGTERM, handler)


def _follow(client: Client, job_id: int, poll_s: float = 1.0):
    decoder = codecs.getincrementaldecoder('utf-8')(errors='replace')
    offset = 0
    while True:
        data, offset, status = client.log_bytes(job_id, offset=offset)
        finished = status not in (PENDING, RUNNING)
        if finished:  # read whatever the worker wrote after the status changed
            more, offset, _ = client.log_bytes(job_id, offset=offset)
            data += more
        sys.stdout.write(decoder.decode(data, final=finished))
        sys.stdout.flush()
        if finished:
            return client.get(job_id)
        time.sleep(poll_s)


def cmd_server(args) -> int:
    url = f'http://127.0.0.1:{args.port}'
    try:
        state = Client(url).state()
        print(f'Xbox queue already running at {url} (pid {state["server_pid"]}, {state["repo"]})')
        return 0
    except QueueError:
        pass
    from tools.xbox_queue.server import acquire_lock, serve

    lock = acquire_lock()
    if lock is None:
        try:
            other = json.loads((home() / 'server.json').read_text())
        except (OSError, ValueError):
            other = {}
        print(f'Xbox queue already running for {home()} at {other.get("url", "an unknown address")} '
              f'(pid {other.get("pid", "?")}); not starting another', file=sys.stderr)
        return 1
    lock.close()
    if not args.detach:
        serve(args.port, args.device)
        return 0
    home().mkdir(parents=True, exist_ok=True)
    repo = Path(__file__).resolve().parents[2]
    with open(home() / 'server.log', 'a') as log:
        subprocess.Popen([sys.executable, '-m', 'tools.xbox_queue', 'server', '--port', str(args.port),
                          '--device', args.device], cwd=repo, stdout=log, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, start_new_session=True)
    for _ in range(50):
        time.sleep(0.1)
        try:
            Client(url).state()
            print(f'Xbox queue started at {url}; log {home() / "server.log"}; '
                  f'launcher {home() / "bin" / "xboxq"}')
            return 0
        except QueueError:
            continue
    print(f'Xbox queue did not start; see {home() / "server.log"}', file=sys.stderr)
    return 1


def cmd_submit(args, client: Client) -> int:
    job = client.submit(_command_text(args.command), args.cwd, name=args.name, owner=args.owner,
                        timeout_s=args.timeout)
    if not args.follow:
        print(json.dumps(job.to_dict()) if args.json else _describe(job))
        return 0
    _cancel_on_interrupt(client, job.id)
    if job.status == PENDING:
        _announce_wait(client, job)
    job = _follow(client, job.id)
    print(_describe(job), file=sys.stderr)
    return _exit_code(job)


def cmd_hold(args, client: Client) -> int:
    job = client.hold(args.cwd, name=args.name, owner=args.owner, ttl_s=args.ttl)
    print(f'lease #{job.id}', flush=True)
    if args.no_wait:
        return 0
    _cancel_on_interrupt(client, job.id)
    if job.status == PENDING:
        _announce_wait(client, job)
    job = client.wait(job.id, granted=True, timeout_s=args.wait_timeout)
    if job.status == RUNNING:
        print(f'granted lease #{job.id} until {_when(job.expires_at)}; renew with: renew {job.id}; '
              f'release with: release {job.id}')
        return 0
    if job.status == PENDING:
        client.cancel(job.id)
        print(f'lease #{job.id} was not granted within {args.wait_timeout}s; cancelled it', file=sys.stderr)
        return 124
    print(_describe(job), file=sys.stderr)
    return 1


def cmd_wait(args, client: Client) -> int:
    job = client.get(args.id)
    job = client.wait(args.id, granted=job.kind == 'lease', timeout_s=args.timeout)
    print(_describe(job))
    if job.kind == 'lease' and job.status == RUNNING:
        return 0
    return 124 if not job.terminal else _exit_code(job)


def cmd_list(args, client: Client) -> int:
    statuses = args.status.split(',') if args.status else None if args.all else [PENDING, RUNNING]
    jobs = client.list(statuses=statuses, owner=args.owner, limit=args.limit)
    if not args.all and not args.status:
        jobs += client.list(statuses=['completed', 'failed', 'cancelled', 'timed_out', 'released', 'expired'],
                            owner=args.owner, limit=5)
    if args.json:
        print(json.dumps([job.to_dict() for job in jobs], indent=2))
    else:
        print('\n'.join(_describe(job) for job in jobs) or 'queue is empty')
    return 0


def cmd_status(args, client: Client) -> int:
    if args.id is not None:
        print(json.dumps(client.get(args.id).to_dict(), indent=2))
        return 0
    state = client.state()
    if args.json:
        print(json.dumps(state, indent=2))
        return 0
    print(f'device {state["device"]} via {client.server} (server repo {state["repo"]})')
    if state['paused'] is not None:
        print(f'PAUSED: {state["paused"]}')
    running = state['running']
    print(f'console: {"held by #%d %s (%s)" % (running["id"], running["owner"], running["kind"]) if running else "free"}')
    print(f'pending: {len(state["pending"])}')
    return 0


def cmd_logs(args, client: Client) -> int:
    if args.follow:
        _follow(client, args.id)
        return 0
    text, _, _ = client.log(args.id, tail=args.tail)
    sys.stdout.write(text)
    return 0


def cmd_cancel(args, client: Client) -> int:
    result = client.cancel(args.id)
    print(f'{result["action"]}: {_describe(client.get(args.id))}')
    return 0


def cmd_renew(args, client: Client) -> int:
    print(_describe(client.renew(args.id, args.ttl)))
    return 0


def cmd_release(args, client: Client) -> int:
    print(_describe(client.release(args.id)))
    return 0


def cmd_pause(args, client: Client) -> int:
    client.pause(args.reason)
    print(f'queue paused: {args.reason} (running work continues; nothing new starts)')
    return 0


def cmd_resume(args, client: Client) -> int:
    client.resume()
    print('queue resumed')
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog='xboxq', description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--server', help=f'queue URL (default: XBOXQ_SERVER or {server_url()})')
    commands = parser.add_subparsers(dest='action', required=True)

    server = commands.add_parser('server', help='run the queue server and worker on this Mac')
    server.add_argument('--port', type=int, default=8765)
    server.add_argument('--device', default=DEFAULT_DEVICE)
    server.add_argument('--detach', action='store_true', help='start in the background and return')

    def job_options(sub, limit_flag, default, help_text):
        sub.add_argument('--name', help='short label, e.g. the title or probe case')
        sub.add_argument('--owner', help='who is asking (default: XBOXQ_OWNER, else the git branch)')
        sub.add_argument('--cwd', default=os.getcwd(), help='checkout to run in (default: current directory)')
        sub.add_argument(limit_flag, type=int, default=default, help=help_text)

    for verb, follow in (('submit', False), ('run', True)):
        sub = commands.add_parser(verb, help='queue a command and return' if not follow else
                                  'queue a command, stream its log, and exit with its exit code')
        job_options(sub, '--timeout', DEFAULT_TIMEOUT, 'seconds before the worker kills it')
        if not follow:
            sub.add_argument('--json', action='store_true')
        sub.add_argument('command', nargs=argparse.REMAINDER, help='-- followed by the command')
        sub.set_defaults(follow=follow, handler=cmd_submit)

    hold = commands.add_parser('hold', help='queue for exclusive interactive use; returns once granted')
    job_options(hold, '--ttl', DEFAULT_TTL, 'seconds the lease lasts unless renewed')
    hold.add_argument('--no-wait', action='store_true', help='print the lease id and return immediately')
    hold.add_argument('--wait-timeout', type=float, help='give up (and cancel) if not granted in this many seconds')
    hold.set_defaults(handler=cmd_hold)

    wait = commands.add_parser('wait', help='block until a command finishes or a lease is granted')
    wait.add_argument('id', type=int)
    wait.add_argument('--timeout', type=float)
    wait.set_defaults(handler=cmd_wait)

    renew = commands.add_parser('renew', help='extend a lease')
    renew.add_argument('id', type=int)
    renew.add_argument('--ttl', type=int, help='new time-to-live in seconds (default: the lease TTL)')
    renew.set_defaults(handler=cmd_renew)

    release = commands.add_parser('release', help='give the console back')
    release.add_argument('id', type=int)
    release.set_defaults(handler=cmd_release)

    listing = commands.add_parser('list', help='show active jobs and the last few results')
    listing.add_argument('--all', action='store_true')
    listing.add_argument('--status', help='comma-separated statuses')
    listing.add_argument('--owner')
    listing.add_argument('--limit', type=int, default=50)
    listing.add_argument('--json', action='store_true')
    listing.set_defaults(handler=cmd_list)

    status = commands.add_parser('status', help='who holds the console, or one job as JSON')
    status.add_argument('id', type=int, nargs='?')
    status.add_argument('--json', action='store_true')
    status.set_defaults(handler=cmd_status)

    logs = commands.add_parser('logs', help='print a command job log')
    logs.add_argument('id', type=int)
    logs.add_argument('-f', '--follow', action='store_true')
    logs.add_argument('--tail', type=int)
    logs.set_defaults(handler=cmd_logs)

    cancel = commands.add_parser('cancel', help='dequeue a job, stop a running command, or end a lease')
    cancel.add_argument('id', type=int)
    cancel.set_defaults(handler=cmd_cancel)

    pause = commands.add_parser('pause', help='stop granting the console (e.g. while someone plays)')
    pause.add_argument('--reason', default='paused by operator')
    pause.set_defaults(handler=cmd_pause)
    commands.add_parser('resume', help='resume granting the console').set_defaults(handler=cmd_resume)
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    if args.action == 'server':
        return cmd_server(args)
    try:
        return args.handler(args, Client(args.server))
    except QueueError as error:
        print(f'xboxq: {error.detail}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
