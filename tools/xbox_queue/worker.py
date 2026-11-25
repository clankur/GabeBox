"""Background worker: grants the console to one queued job at a time.

A command job runs as a shell command in the submitter's checkout with its output in
the job log. A lease job holds the console until its owner releases it or it expires.
"""

from __future__ import annotations

import logging
import os
import signal
import subprocess
import threading
import time
from datetime import datetime
from typing import Optional

from tools.xbox_queue.db import (
    DEFAULT_DEVICE,
    RUNNING,
    Job,
    claim_next,
    connect,
    finish,
    get_job,
    list_jobs,
    paused,
    set_pid,
)

logger = logging.getLogger(__name__)
KILL_GRACE_S = 10


def _stamp(epoch: float) -> str:
    return datetime.fromtimestamp(epoch).strftime('%Y-%m-%d %H:%M:%S')


def process_started(pid: int) -> Optional[str]:
    """Start time of *pid* as ps reports it, or None when no such process exists."""
    try:
        out = subprocess.run(['ps', '-o', 'lstart=', '-p', str(pid)], capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return out.stdout.strip() or None


def _group_alive(pgid: int) -> bool:
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def _signal(pgid: int, sig: int) -> None:
    try:
        os.killpg(pgid, sig)
    except (ProcessLookupError, PermissionError):
        pass


def end_group(pgid: int, proc: Optional[subprocess.Popen] = None, grace_s: Optional[float] = None) -> bool:
    """Stop every process in the job's group, escalating to SIGKILL; True if any were running.

    The group outlives its leader, so this keeps signalling the group id until it is empty
    instead of watching only the leader.
    """
    if not _group_alive(pgid):
        if proc is not None:
            proc.wait()
        return False
    _signal(pgid, signal.SIGTERM)
    deadline = time.monotonic() + (KILL_GRACE_S if grace_s is None else grace_s)
    while time.monotonic() < deadline:
        if proc is not None:
            proc.poll()  # reap the leader so it does not linger as a zombie group member
        if not _group_alive(pgid):
            break
        time.sleep(0.05)
    else:
        _signal(pgid, signal.SIGKILL)
        for _ in range(100):
            if proc is not None:
                proc.poll()
            if not _group_alive(pgid):
                break
            _signal(pgid, signal.SIGKILL)
            time.sleep(0.05)
    if proc is not None:
        proc.wait()
    return True


class Worker:
    def __init__(self, device: str = DEFAULT_DEVICE, poll_interval: float = 0.5, server_url: Optional[str] = None):
        self.device = device
        self.poll_interval = poll_interval
        self.server_url = server_url
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name='xboxq-worker', daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=KILL_GRACE_S + 5)

    def _run(self) -> None:
        conn = connect()
        try:
            try:
                self._recover(conn)
            except Exception:
                logger.exception('xbox queue recovery failed')
            while not self._stop.is_set():
                job = None
                try:
                    job = None if paused(conn, self.device) is not None else claim_next(conn, self.device)
                    if job is None:
                        self._stop.wait(self.poll_interval)
                    elif job.kind == 'command':
                        self._run_command(conn, job)
                    else:
                        self._hold_lease(conn, job)
                except Exception as error:
                    logger.exception('xbox queue worker iteration failed')
                    if job is not None:
                        finish(conn, job.id, 'failed', note=f'queue worker error: {error!r}')
                    self._stop.wait(self.poll_interval)
        finally:
            conn.close()

    def _recover(self, conn) -> None:
        """Settle jobs a previous server left running before granting anything new."""
        for job in reversed(list_jobs(conn, statuses=[RUNNING], device=self.device)):
            if job.kind == 'lease':
                logger.info('resuming lease %d held by %s', job.id, job.owner)
                self._hold_lease(conn, job)
                continue
            note = 'queue server restarted while this command ran'
            leader = process_started(job.pid) if job.pid else None
            if job.pid and leader is not None and leader != job.pid_started:
                note += f'; pid {job.pid} now belongs to another process, left it alone'
            elif job.pid and end_group(job.pid):
                note += f'; stopped its process group {job.pid}'
            finish(conn, job.id, 'failed', note=note)

    def _hold_lease(self, conn, job: Job) -> None:
        logger.info('lease %d granted to %s', job.id, job.owner)
        while not self._stop.is_set():
            current = get_job(conn, job.id)
            if current is None or current.terminal:
                return
            if current.expires_at is not None and time.time() >= current.expires_at:
                finish(conn, job.id, 'expired', note='lease expired before it was released')
                logger.info('lease %d expired', job.id)
                return
            self._stop.wait(self.poll_interval)
        # Server shutdown leaves the lease running; the next worker resumes it.

    def _run_command(self, conn, job: Job) -> None:
        logger.info('running job %d for %s: %s', job.id, job.owner, job.command)
        with open(job.log_path, 'a', encoding='utf-8', errors='replace') as log:
            log.write(f'--- xboxq job {job.id} ({job.name or "unnamed"}) for {job.owner or "unknown"}\n'
                      f'--- cwd: {job.cwd}\n')
            if job.git_commit:
                log.write(f'--- git: {job.git_branch or "detached"} @ {job.git_commit}\n')
            log.write(f'--- started {_stamp(job.started_at)}, timeout {job.limit_s}s\n--- $ {job.command}\n')
            log.flush()
            if not job.cwd or not os.path.isdir(job.cwd):
                log.write('--- working directory no longer exists\n')
                finish(conn, job.id, 'failed', note='working directory no longer exists')
                return

            current = get_job(conn, job.id)
            if current is None or current.terminal or current.cancel_requested:
                log.write('--- cancelled before it started\n')
                finish(conn, job.id, 'cancelled', note='cancelled before it started')
                return

            env = dict(os.environ, XBOXQ_JOB_ID=str(job.id), PYTHONUNBUFFERED='1')
            if self.server_url:
                env['XBOXQ_SERVER'] = self.server_url
            try:
                proc = subprocess.Popen(job.command, shell=True, cwd=job.cwd, stdout=log, stderr=subprocess.STDOUT,
                                        stdin=subprocess.DEVNULL, env=env, start_new_session=True)
            except OSError as error:
                log.write(f'--- failed to start: {error}\n')
                finish(conn, job.id, 'failed', note=f'failed to start: {error}')
                return
            set_pid(conn, job.id, proc.pid, process_started(proc.pid))

            deadline = job.started_at + job.limit_s
            status, note = None, None
            while proc.poll() is None:
                current = get_job(conn, job.id)
                if self._stop.is_set():
                    status, note = 'failed', 'queue server stopped while this command ran'
                elif current is not None and current.cancel_requested:
                    status, note = 'cancelled', 'cancelled while running'
                elif time.time() >= deadline:
                    status, note = 'timed_out', f'exceeded its {job.limit_s}s timeout'
                if status:
                    break
                time.sleep(min(self.poll_interval, 0.25))
            # Stop the group either way: a background process the command left behind
            # must not keep driving the console during the next job.
            leftovers = end_group(proc.pid, proc)
            if status is None:
                status = 'completed' if proc.returncode == 0 else 'failed'
                if leftovers:
                    log.write('--- stopped processes the command left running\n')
            log.write(f'--- {status} (exit {proc.returncode}) at {_stamp(time.time())}'
                      f'{f": {note}" if note else ""}\n')
        finish(conn, job.id, status, exit_code=proc.returncode, note=note)
        logger.info('job %d %s (exit %s)', job.id, status, proc.returncode)
