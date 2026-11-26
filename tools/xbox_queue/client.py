"""Python client for the shared Xbox queue server."""

from __future__ import annotations

import json
import os
import subprocess
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Optional
from urllib.parse import urlencode

from tools.xbox_queue.db import RUNNING, Job

DEFAULT_SERVER = 'http://127.0.0.1:8765'
START_HINT = 'Start it with: python3 -m tools.xbox_queue server --detach'


class QueueError(Exception):
    def __init__(self, status: int, detail: str):
        super().__init__(detail)
        self.status = status
        self.detail = detail


def server_url(explicit: Optional[str] = None) -> str:
    return (explicit or os.environ.get('XBOXQ_SERVER') or DEFAULT_SERVER).rstrip('/')


def _git(cwd: str, *args: str) -> Optional[str]:
    try:
        result = subprocess.run(['git', *args], cwd=cwd, capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return (result.stdout.strip() or None) if result.returncode == 0 else None


def git_context(cwd: str) -> dict:
    """Branch and commit of the submitting checkout, with a marker for uncommitted changes."""
    commit = _git(cwd, 'rev-parse', 'HEAD')
    if commit and _git(cwd, 'status', '--porcelain', '--untracked-files=no'):
        commit += '+dirty'
    branch = _git(cwd, 'rev-parse', '--abbrev-ref', 'HEAD')
    return {'git_branch': None if branch == 'HEAD' else branch, 'git_commit': commit}


def default_owner(cwd: str) -> str:
    """XBOXQ_OWNER, else the checkout's branch, else the directory name."""
    return os.environ.get('XBOXQ_OWNER') or git_context(cwd)['git_branch'] or Path(cwd).name


def _job(data: dict) -> Job:
    return Job(**data)


class Client:
    def __init__(self, server: Optional[str] = None):
        self.server = server_url(server)

    def _request(self, method: str, path: str, body: Optional[dict] = None, raw: bool = False):
        data = json.dumps(body).encode() if body is not None else None
        request = urllib.request.Request(self.server + path, data=data, method=method)
        if data is not None:
            request.add_header('Content-Type', 'application/json')
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                payload = response.read()
                if raw:
                    return payload, dict(response.headers)
                return json.loads(payload)
        except urllib.error.HTTPError as error:
            try:
                detail = json.loads(error.read()).get('detail', '')
            except ValueError:
                detail = error.reason
            raise QueueError(error.code, detail) from None
        except (urllib.error.URLError, ConnectionError) as error:
            raise QueueError(0, f'Xbox queue server is not reachable at {self.server} ({error}). {START_HINT}') \
                from None

    def submit(self, command: str, cwd: str, name: Optional[str] = None, owner: Optional[str] = None,
               timeout_s: Optional[int] = None) -> Job:
        cwd = os.path.abspath(cwd)
        body = {'kind': 'command', 'command': command, 'cwd': cwd, 'name': name,
                'owner': owner or default_owner(cwd), 'limit_s': timeout_s, **git_context(cwd)}
        return _job(self._request('POST', '/api/jobs', body))

    def hold(self, cwd: str, name: Optional[str] = None, owner: Optional[str] = None,
             ttl_s: Optional[int] = None) -> Job:
        cwd = os.path.abspath(cwd)
        body = {'kind': 'lease', 'cwd': cwd, 'name': name, 'owner': owner or default_owner(cwd),
                'limit_s': ttl_s, **git_context(cwd)}
        return _job(self._request('POST', '/api/jobs', body))

    def get(self, job_id: int) -> Job:
        return _job(self._request('GET', f'/api/jobs/{job_id}'))

    def list(self, statuses: Optional[list] = None, owner: Optional[str] = None, limit: int = 50) -> list:
        query = {'limit': limit}
        if statuses:
            query['status'] = ','.join(statuses)
        if owner:
            query['owner'] = owner
        return [_job(item) for item in self._request('GET', f'/api/jobs?{urlencode(query)}')]

    def log_bytes(self, job_id: int, offset: int = 0, tail: Optional[int] = None) -> tuple:
        """Returns (data, next_offset, job_status)."""
        query = {'offset': offset} if tail is None else {'tail': tail}
        data, headers = self._request('GET', f'/api/jobs/{job_id}/log?{urlencode(query)}', raw=True)
        return data, int(headers.get('X-Log-Size', 0)), headers.get('X-Job-Status')

    def log(self, job_id: int, offset: int = 0, tail: Optional[int] = None) -> tuple:
        """Returns (text, next_offset, job_status)."""
        data, size, status = self.log_bytes(job_id, offset, tail)
        return data.decode('utf-8', errors='replace'), size, status

    def cancel(self, job_id: int) -> dict:
        try:
            return self._request('POST', f'/api/jobs/{job_id}/cancel', {})
        except QueueError as error:
            if error.status == 409:
                return {'action': 'already_terminal', 'job': self.get(job_id).to_dict()}
            raise

    def renew(self, job_id: int, ttl_s: Optional[int] = None) -> Job:
        return _job(self._request('POST', f'/api/jobs/{job_id}/renew', {'limit_s': ttl_s}))

    def release(self, job_id: int) -> Job:
        return _job(self._request('POST', f'/api/jobs/{job_id}/release', {}))

    def state(self) -> dict:
        return self._request('GET', '/api/state')

    def pause(self, reason: str) -> dict:
        return self._request('POST', '/api/pause', {'reason': reason})

    def resume(self) -> dict:
        return self._request('POST', '/api/resume', {})

    def wait(self, job_id: int, granted: bool = False, timeout_s: Optional[float] = None,
             poll_s: float = 2.0) -> Job:
        """Block until the job is terminal, or until it starts when *granted* is set."""
        deadline = None if timeout_s is None else time.monotonic() + timeout_s
        while True:
            job = self.get(job_id)
            if job.terminal or (granted and job.status == RUNNING):
                return job
            if deadline is not None and time.monotonic() >= deadline:
                return job
            time.sleep(poll_s)
