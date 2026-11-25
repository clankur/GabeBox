"""SQLite state for the shared Xbox queue: jobs, leases, and the pause switch.

A job is either a `command` the worker runs on this Mac against the console, or a
`lease` that grants one session exclusive, interactive use of the console until it
releases the lease or the lease expires. Both wait in one FIFO queue per device.
"""

from __future__ import annotations

import os
import sqlite3
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Optional

DEFAULT_DEVICE = 'xbox'
KINDS = ('command', 'lease')
PENDING = 'pending'
RUNNING = 'running'
TERMINAL = ('completed', 'failed', 'cancelled', 'timed_out', 'released', 'expired')
DEFAULT_TIMEOUT = 15 * 60
DEFAULT_TTL = 20 * 60
MAX_SECONDS = 4 * 60 * 60


def home() -> Path:
    """State directory shared by every checkout on this Mac (override with XBOXQ_HOME)."""
    return Path(os.environ.get('XBOXQ_HOME') or Path.home() / '.xboxq')


@dataclass
class Job:
    id: int
    kind: str
    device: str
    status: str
    name: Optional[str]
    owner: Optional[str]
    command: Optional[str]
    cwd: Optional[str]
    git_branch: Optional[str]
    git_commit: Optional[str]
    limit_s: int  # command timeout, or lease time-to-live per grant or renewal
    created_at: float
    started_at: Optional[float]
    finished_at: Optional[float]
    expires_at: Optional[float]
    cancel_requested: int
    exit_code: Optional[int]
    pid: Optional[int]
    pid_started: Optional[str]  # `ps -o lstart` of pid, so recovery never signals a reused pid
    log_path: Optional[str]
    note: Optional[str]
    position: Optional[int] = None  # pending jobs ahead of this one; filled in for API responses

    @property
    def terminal(self) -> bool:
        return self.status in TERMINAL

    def to_dict(self) -> dict:
        return asdict(self)


SCHEMA = """\
CREATE TABLE IF NOT EXISTS jobs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL,
    device TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending',
    name TEXT,
    owner TEXT,
    command TEXT,
    cwd TEXT,
    git_branch TEXT,
    git_commit TEXT,
    limit_s INTEGER NOT NULL,
    created_at REAL NOT NULL,
    started_at REAL,
    finished_at REAL,
    expires_at REAL,
    cancel_requested INTEGER NOT NULL DEFAULT 0,
    exit_code INTEGER,
    pid INTEGER,
    pid_started TEXT,
    log_path TEXT,
    note TEXT
);
CREATE TABLE IF NOT EXISTS control (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
"""


def connect(path: Optional[Path] = None) -> sqlite3.Connection:
    base = home()
    (base / 'logs').mkdir(parents=True, exist_ok=True)
    base.chmod(0o700)  # job logs and commands stay private to this user
    conn = sqlite3.connect(str(path or base / 'queue.db'), timeout=10, isolation_level=None)
    conn.row_factory = sqlite3.Row
    conn.execute('PRAGMA journal_mode=WAL')
    conn.executescript(SCHEMA)
    columns = {row['name'] for row in conn.execute('PRAGMA table_info(jobs)')}
    if 'pid_started' not in columns:  # databases from before process start times were recorded
        conn.execute('ALTER TABLE jobs ADD COLUMN pid_started TEXT')
    return conn


def _job(conn: sqlite3.Connection, row: sqlite3.Row) -> Job:
    job = Job(**dict(row))
    if job.status == PENDING:
        job.position = conn.execute(
            "SELECT COUNT(*) FROM jobs WHERE status = 'pending' AND device = ? AND id < ?", (job.device, job.id)
        ).fetchone()[0]
    return job


def _clamp(seconds: int) -> int:
    return max(1, min(int(seconds), MAX_SECONDS))


def create_job(
    conn: sqlite3.Connection,
    kind: str,
    device: str = DEFAULT_DEVICE,
    command: Optional[str] = None,
    cwd: Optional[str] = None,
    name: Optional[str] = None,
    owner: Optional[str] = None,
    git_branch: Optional[str] = None,
    git_commit: Optional[str] = None,
    limit_s: Optional[int] = None,
) -> Job:
    if kind not in KINDS:
        raise ValueError(f'kind must be one of {", ".join(KINDS)}')
    if kind == 'command' and not (command and command.strip() and cwd):
        raise ValueError('a command job needs a command and an absolute working directory')
    if cwd is not None and not os.path.isabs(cwd):
        raise ValueError('cwd must be an absolute path')
    default = DEFAULT_TIMEOUT if kind == 'command' else DEFAULT_TTL
    cur = conn.execute(
        'INSERT INTO jobs (kind, device, name, owner, command, cwd, git_branch, git_commit, limit_s, created_at)'
        ' VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)',
        (kind, device, name, owner, command if kind == 'command' else None, cwd, git_branch, git_commit,
         _clamp(limit_s or default), time.time()),
    )
    return get_job(conn, cur.lastrowid)


def get_job(conn: sqlite3.Connection, job_id: int) -> Optional[Job]:
    row = conn.execute('SELECT * FROM jobs WHERE id = ?', (job_id,)).fetchone()
    return _job(conn, row) if row else None


def list_jobs(
    conn: sqlite3.Connection,
    statuses: Optional[list] = None,
    device: Optional[str] = None,
    owner: Optional[str] = None,
    limit: Optional[int] = None,
) -> list:
    clauses, params = [], []
    if statuses:
        clauses.append(f'status IN ({",".join("?" for _ in statuses)})')
        params.extend(statuses)
    if device:
        clauses.append('device = ?')
        params.append(device)
    if owner:
        clauses.append('owner = ?')
        params.append(owner)
    where = ' WHERE ' + ' AND '.join(clauses) if clauses else ''
    tail = ' LIMIT ?' if limit else ''
    if limit:
        params.append(int(limit))
    rows = conn.execute(f'SELECT * FROM jobs{where} ORDER BY id DESC{tail}', params).fetchall()
    return [_job(conn, row) for row in rows]


def claim_next(conn: sqlite3.Connection, device: str) -> Optional[Job]:
    """Move the oldest pending job on *device* to running. Leases get their expiry here."""
    conn.execute('BEGIN IMMEDIATE')
    try:
        row = conn.execute(
            "SELECT * FROM jobs WHERE status = 'pending' AND device = ? ORDER BY id LIMIT 1", (device,)
        ).fetchone()
        if row is None:
            conn.execute('COMMIT')
            return None
        job = _job(conn, row)
        now = time.time()
        expires = now + job.limit_s if job.kind == 'lease' else None
        log_path = str(home() / 'logs' / f'{job.id}.log') if job.kind == 'command' else None
        conn.execute(
            "UPDATE jobs SET status = 'running', started_at = ?, expires_at = ?, log_path = ? WHERE id = ?",
            (now, expires, log_path, job.id),
        )
        conn.execute('COMMIT')
    except BaseException:
        conn.execute('ROLLBACK')
        raise
    return get_job(conn, job.id)


def set_pid(conn: sqlite3.Connection, job_id: int, pid: int, started: Optional[str]) -> None:
    conn.execute('UPDATE jobs SET pid = ?, pid_started = ? WHERE id = ?', (pid, started, job_id))


def finish(conn: sqlite3.Connection, job_id: int, status: str, exit_code: Optional[int] = None,
           note: Optional[str] = None) -> Optional[Job]:
    """Record a terminal status unless the job already has one."""
    assert status in TERMINAL, status
    conn.execute(
        f"UPDATE jobs SET status = ?, exit_code = ?, note = COALESCE(?, note), finished_at = ?, pid = NULL"
        f" WHERE id = ? AND status NOT IN ({','.join('?' for _ in TERMINAL)})",
        (status, exit_code, note, time.time(), job_id, *TERMINAL),
    )
    return get_job(conn, job_id)


def _end_pending(conn: sqlite3.Connection, job_id: int, note: str) -> bool:
    """Cancel a job only while it is still pending, so a concurrent claim always wins cleanly."""
    cur = conn.execute(
        "UPDATE jobs SET status = 'cancelled', note = ?, finished_at = ? WHERE id = ? AND status = 'pending'",
        (note, time.time(), job_id),
    )
    return cur.rowcount == 1


def cancel(conn: sqlite3.Connection, job_id: int) -> tuple:
    """Returns (action, job): not_found, dequeued, kill_requested, lease_ended or already_terminal."""
    job = get_job(conn, job_id)
    if job is None:
        return 'not_found', None
    if job.status == PENDING and _end_pending(conn, job_id, 'cancelled before it started'):
        return 'dequeued', get_job(conn, job_id)
    job = get_job(conn, job_id)  # the worker may have claimed it in the meantime
    if job.terminal:
        return 'already_terminal', job
    if job.kind == 'lease':
        return 'lease_ended', finish(conn, job_id, 'cancelled', note='lease cancelled')
    conn.execute('UPDATE jobs SET cancel_requested = 1 WHERE id = ?', (job_id,))
    return 'kill_requested', get_job(conn, job_id)


def renew(conn: sqlite3.Connection, job_id: int, seconds: Optional[int] = None) -> Job:
    """Extend a lease. A pending lease gets the new TTL when it is granted."""
    job = get_job(conn, job_id)
    if job is None or job.kind != 'lease':
        raise LookupError(f'lease {job_id} not found')
    if job.terminal:
        raise ValueError(f'lease {job_id} is already {job.status}; queue a new one with hold')
    seconds = _clamp(seconds or job.limit_s)
    if job.status == PENDING:
        conn.execute('UPDATE jobs SET limit_s = ? WHERE id = ?', (seconds, job_id))
    else:
        conn.execute('UPDATE jobs SET limit_s = ?, expires_at = ? WHERE id = ? AND status = ?',
                     (seconds, time.time() + seconds, job_id, RUNNING))
    return get_job(conn, job_id)


def release(conn: sqlite3.Connection, job_id: int) -> Job:
    job = get_job(conn, job_id)
    if job is None or job.kind != 'lease':
        raise LookupError(f'lease {job_id} not found')
    if job.status == PENDING and _end_pending(conn, job_id, 'released before it was granted'):
        return get_job(conn, job_id)
    return finish(conn, job_id, 'released')  # no-op when it is already terminal


def paused(conn: sqlite3.Connection, device: str) -> Optional[str]:
    row = conn.execute('SELECT value FROM control WHERE key = ?', (f'pause:{device}',)).fetchone()
    return row[0] if row else None


def set_paused(conn: sqlite3.Connection, device: str, reason: Optional[str]) -> None:
    if reason is None:
        conn.execute('DELETE FROM control WHERE key = ?', (f'pause:{device}',))
    else:
        conn.execute('INSERT OR REPLACE INTO control (key, value) VALUES (?, ?)', (f'pause:{device}', reason))
