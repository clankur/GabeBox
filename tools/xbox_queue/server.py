"""Local HTTP server: REST API, read-only dashboard, and the integrated worker.

The server binds to 127.0.0.1 and runs submitted shell commands, so it rejects
requests whose Host is not this loopback address and cross-origin or non-JSON writes.
"""

from __future__ import annotations

import fcntl
import html
import json
import logging
import os
import re
import signal
import stat
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional
from urllib.parse import parse_qs, urlsplit

from tools.xbox_queue.db import (
    DEFAULT_DEVICE,
    PENDING,
    RUNNING,
    TERMINAL,
    cancel,
    connect,
    create_job,
    get_job,
    home,
    list_jobs,
    paused,
    release,
    renew,
    set_paused,
)
from tools.xbox_queue.worker import Worker

logger = logging.getLogger(__name__)
DEFAULT_PORT = 8765
REPO_ROOT = Path(__file__).resolve().parents[2]
SUBMIT_FIELDS = ('kind', 'command', 'cwd', 'name', 'owner', 'git_branch', 'git_commit', 'limit_s')


def _check_types(body: dict) -> None:
    for key, value in body.items():
        expected = int if key == 'limit_s' else str
        if value is not None and type(value) is not expected:
            raise ApiError(400, f'{key} must be {"an integer" if expected is int else "a string"} or null')


def acquire_lock():
    """Hold an exclusive lock on the state directory; None when another server owns it."""
    home().mkdir(parents=True, exist_ok=True)
    handle = open(home() / 'server.lock', 'a')
    try:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        handle.close()
        return None
    return handle


class ApiError(Exception):
    def __init__(self, status: int, detail: str):
        super().__init__(detail)
        self.status = status
        self.detail = detail


def _state(conn, device: str) -> dict:
    running = list_jobs(conn, statuses=[RUNNING], device=device)
    pending = list_jobs(conn, statuses=[PENDING], device=device)
    return {
        'device': device,
        'paused': paused(conn, device),
        'running': running[0].to_dict() if running else None,
        'pending': [job.to_dict() for job in reversed(pending)],
        'server_pid': os.getpid(),
        'repo': str(REPO_ROOT),
    }


def _log_tail(path: Optional[str], offset: int, tail: Optional[int]) -> tuple:
    if not path or not os.path.exists(path):
        return b'', 0
    with open(path, 'rb') as handle:
        if tail is None:
            handle.seek(max(0, offset))
            data = handle.read()
            return data, max(0, offset) + len(data)
        data = handle.read()
    lines = data.splitlines(keepends=True)
    return b''.join(lines[-tail:] if tail else []), len(data)


class Handler(BaseHTTPRequestHandler):
    server_version = 'xboxq'
    protocol_version = 'HTTP/1.1'

    def log_message(self, format, *args):
        logger.debug('%s %s', self.address_string(), format % args)

    def do_GET(self):
        self._dispatch('GET')

    def do_POST(self):
        self._dispatch('POST')

    def _send(self, status: int, body: bytes, content_type: str, headers: Optional[dict] = None) -> None:
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def _json(self, status: int, payload) -> None:
        self._send(status, json.dumps(payload).encode(), 'application/json')

    def _dispatch(self, method: str) -> None:
        port = self.server.server_address[1]
        if self.headers.get('Host') not in (f'127.0.0.1:{port}', f'localhost:{port}'):
            return self._json(403, {'detail': 'unexpected Host header'})
        body = {}
        if method == 'POST':
            origin = self.headers.get('Origin')
            if origin and urlsplit(origin).netloc != self.headers.get('Host'):
                return self._json(403, {'detail': 'cross-origin requests are not accepted'})
            if not (self.headers.get('Content-Type') or '').startswith('application/json'):
                return self._json(415, {'detail': 'send application/json'})
            try:
                length = int(self.headers.get('Content-Length') or 0)
                if length < 0:
                    raise ValueError('negative Content-Length')
                body = json.loads(self.rfile.read(length) or b'{}')
            except ValueError:
                return self._json(400, {'detail': 'body is not valid JSON'})
            if not isinstance(body, dict):
                return self._json(400, {'detail': 'body must be a JSON object'})
        url = urlsplit(self.path)
        query = {key: values[-1] for key, values in parse_qs(url.query).items()}
        conn = connect()
        try:
            self._route(conn, method, url.path, query, body)
        except ApiError as error:
            self._json(error.status, {'detail': error.detail})
        except (LookupError, ValueError, TypeError) as error:
            self._json(404 if isinstance(error, LookupError) else 400, {'detail': str(error)})
        except Exception as error:
            logger.exception('request failed: %s %s', method, self.path)
            self._json(500, {'detail': f'internal error: {error!r}'})
        finally:
            conn.close()

    def _job_or_404(self, conn, job_id: int):
        job = get_job(conn, job_id)
        if job is None:
            raise ApiError(404, f'job {job_id} not found')
        return job

    def _route(self, conn, method: str, path: str, query: dict, body: dict) -> None:
        device = self.server.device
        match = re.fullmatch(r'/api/jobs/(\d+)(/[a-z]+)?', path)
        job_id = int(match.group(1)) if match else None
        action = match.group(2) if match else None

        if method == 'GET' and path in ('/', '/index.html'):
            return self._send(200, render_dashboard(conn, device).encode(), 'text/html; charset=utf-8')
        if method == 'GET' and path == '/api/state':
            return self._json(200, _state(conn, device))
        if method == 'GET' and path == '/api/jobs':
            statuses = [s for s in query.get('status', '').split(',') if s] or None
            jobs = list_jobs(conn, statuses=statuses, device=query.get('device') or device,
                             owner=query.get('owner'), limit=int(query.get('limit') or 50))
            return self._json(200, [job.to_dict() for job in jobs])
        if method == 'POST' and path == '/api/jobs':
            unknown = set(body) - set(SUBMIT_FIELDS)
            if unknown:
                raise ApiError(400, f'unknown fields: {", ".join(sorted(unknown))}')
            _check_types(body)
            job = create_job(conn, device=device, **body)
            return self._json(201, get_job(conn, job.id).to_dict())
        if method == 'POST' and path in ('/api/pause', '/api/resume'):
            reason = str(body.get('reason') or 'paused') if path == '/api/pause' else None
            set_paused(conn, device, reason)
            return self._json(200, _state(conn, device))
        if job_id is not None and method == 'GET' and action is None:
            return self._json(200, self._job_or_404(conn, job_id).to_dict())
        if job_id is not None and method == 'GET' and action == '/log':
            job = self._job_or_404(conn, job_id)
            tail = query.get('tail')
            data, size = _log_tail(job.log_path, int(query.get('offset') or 0), int(tail) if tail else None)
            return self._send(200, data, 'text/plain; charset=utf-8',
                              {'X-Log-Size': str(size), 'X-Job-Status': job.status})
        if job_id is not None and method == 'POST' and action == '/cancel':
            outcome, job = cancel(conn, job_id)
            if outcome == 'not_found':
                raise ApiError(404, f'job {job_id} not found')
            return self._json(409 if outcome == 'already_terminal' else 200,
                              {'action': outcome, 'job': job.to_dict()})
        if job_id is not None and method == 'POST' and action == '/renew':
            _check_types({'limit_s': body.get('limit_s')})
            return self._json(200, renew(conn, job_id, body.get('limit_s')).to_dict())
        if job_id is not None and method == 'POST' and action == '/release':
            return self._json(200, release(conn, job_id).to_dict())
        raise ApiError(404, f'no route for {method} {path}')


class QueueServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, port: int, device: str = DEFAULT_DEVICE):
        super().__init__(('127.0.0.1', port), Handler)
        self.device = device


def _when(epoch: Optional[float]) -> str:
    return datetime.fromtimestamp(epoch).strftime('%H:%M:%S') if epoch else ''


def _detail(job) -> str:
    now = time.time()
    if job.status == RUNNING and job.kind == 'lease' and job.expires_at:
        return f'expires in {max(0, int(job.expires_at - now)) // 60} min'
    if job.status == RUNNING and job.started_at:
        return f'running {int(now - job.started_at)}s'
    if job.status == PENDING:
        return f'{job.position} ahead'
    return job.note or (f'exit {job.exit_code}' if job.exit_code is not None else '')


def render_dashboard(conn, device: str) -> str:
    reason = paused(conn, device)
    active = list_jobs(conn, statuses=[RUNNING, PENDING], device=device)
    recent = list_jobs(conn, statuses=list(TERMINAL), device=device, limit=25)
    rows = []
    for job in sorted(active, key=lambda job: job.id) + recent:
        what = job.command if job.kind == 'command' else 'interactive lease'
        rows.append(
            f'<tr class="{job.status}"><td>{job.id}</td><td>{job.status}</td><td>{html.escape(job.owner or "")}</td>'
            f'<td>{html.escape(job.name or "")}</td><td><code>{html.escape(what or "")}</code></td>'
            f'<td>{_when(job.created_at)}</td><td>{_when(job.started_at)}</td>'
            f'<td>{html.escape(_detail(job))}</td></tr>'
        )
    banner = f'<p class="paused">Paused: {html.escape(reason)}</p>' if reason is not None else ''
    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="refresh" content="3"><title>Xbox queue</title>
<style>
:root {{ --bg:#fff; --fg:#1b1b1b; --muted:#666; --line:#ddd; --run:#e6f4ea; --wait:#fff8e1; --warn:#fdecea; }}
@media (prefers-color-scheme: dark) {{
  :root {{ --bg:#141414; --fg:#e8e8e8; --muted:#999; --line:#333; --run:#173322; --wait:#332b10; --warn:#3a1a17; }}
}}
body {{ background:var(--bg); color:var(--fg); font:14px/1.4 -apple-system, system-ui, sans-serif; margin:16px; }}
table {{ border-collapse:collapse; width:100%; }} td, th {{ border-bottom:1px solid var(--line); padding:4px 8px;
  text-align:left; vertical-align:top; }}
th {{ color:var(--muted); font-weight:500; }} code {{ word-break:break-all; }}
tr.running {{ background:var(--run); }} tr.pending {{ background:var(--wait); }}
tr.failed, tr.timed_out, tr.expired {{ background:var(--warn); }}
.paused {{ background:var(--warn); padding:8px; }} .wrap {{ overflow-x:auto; }}
</style></head><body>
<h1>Xbox queue · {html.escape(device)}</h1>{banner}
<div class="wrap"><table><tr><th>ID</th><th>Status</th><th>Owner</th><th>Name</th><th>Command</th><th>Queued</th>
<th>Started</th><th>Detail</th></tr>
{''.join(rows) or '<tr><td colspan="8">No jobs yet.</td></tr>'}
</table></div></body></html>"""


def write_launcher(port: int) -> Path:
    """Write ~/.xboxq/bin/xboxq so checkouts without this module can still reach the queue."""
    launcher = home() / 'bin' / 'xboxq'
    launcher.parent.mkdir(parents=True, exist_ok=True)
    script = REPO_ROOT / 'tools' / 'xbox_queue' / '__main__.py'
    launcher.write_text(f'#!/bin/sh\nXBOXQ_SERVER="${{XBOXQ_SERVER:-http://127.0.0.1:{port}}}" '
                        f'exec python3 "{script}" "$@"\n')
    launcher.chmod(launcher.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return launcher


def _interrupt(*_):
    raise KeyboardInterrupt


def serve(port: int = DEFAULT_PORT, device: str = DEFAULT_DEVICE) -> None:
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(message)s')
    lock = acquire_lock()
    if lock is None:
        raise SystemExit(f'Another Xbox queue server already owns {home()}; see {home() / "server.json"}.')
    httpd = QueueServer(port, device)
    url = f'http://127.0.0.1:{port}'
    worker = Worker(device=device, server_url=url)
    launcher = write_launcher(port)
    (home() / 'server.json').write_text(json.dumps(
        {'url': url, 'pid': os.getpid(), 'device': device, 'repo': str(REPO_ROOT), 'launcher': str(launcher)}))
    signal.signal(signal.SIGTERM, _interrupt)
    worker.start()
    logger.info('xbox queue for %s serving %s (state in %s)', device, url, home())
    try:
        httpd.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
        worker.stop()
        lock.close()
        logger.info('xbox queue stopped')
