import json
import http.client
import os
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from unittest.mock import patch

from tools.xbox_queue import db
from tools.xbox_queue.__main__ import _command_text, _exit_code
from tools.xbox_queue.client import Client
from tools.xbox_queue.server import QueueServer, acquire_lock
from tools.xbox_queue.worker import Worker, process_started


def group_alive(pgid):
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False


class QueueTestCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        env = patch.dict(os.environ, {'XBOXQ_HOME': self.tmp.name})
        env.start()
        self.addCleanup(env.stop)
        self.conn = db.connect()
        self.addCleanup(self.conn.close)
        self.cwd = self.tmp.name

    def command(self, command, **kwargs):
        return db.create_job(self.conn, 'command', command=command, cwd=self.cwd, **kwargs)

    def lease(self, **kwargs):
        return db.create_job(self.conn, 'lease', **kwargs)

    def start_worker(self):
        worker = Worker(poll_interval=0.02)
        worker.start()
        self.addCleanup(worker.stop)
        return worker

    def wait_for(self, job_id, predicate, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            job = db.get_job(self.conn, job_id)
            if predicate(job):
                return job
            time.sleep(0.02)
        self.fail(f'job {job_id} stayed {db.get_job(self.conn, job_id).status}')


class StoreTests(QueueTestCase):
    def test_jobs_are_granted_in_submission_order_with_positions(self):
        first, second = self.lease(), self.command('true')
        self.assertEqual((first.position, second.position), (0, 1))
        self.assertEqual(db.claim_next(self.conn, db.DEFAULT_DEVICE).id, first.id)
        self.assertEqual(db.get_job(self.conn, second.id).position, 0)
        self.assertIsNotNone(db.get_job(self.conn, first.id).expires_at)

    def test_command_needs_an_absolute_working_directory(self):
        with self.assertRaises(ValueError):
            db.create_job(self.conn, 'command', command='true', cwd='relative')
        with self.assertRaises(ValueError):
            db.create_job(self.conn, 'command', command=' ', cwd=self.cwd)

    def test_terminal_status_is_not_overwritten(self):
        job = self.command('true')
        db.finish(self.conn, job.id, 'cancelled')
        self.assertEqual(db.finish(self.conn, job.id, 'completed', exit_code=0).status, 'cancelled')
        self.assertEqual(db.cancel(self.conn, job.id)[0], 'already_terminal')

    def test_lease_renewal_and_release(self):
        job = self.lease(limit_s=60)
        self.assertEqual(db.renew(self.conn, job.id, 120).limit_s, 120)
        db.claim_next(self.conn, db.DEFAULT_DEVICE)
        renewed = db.renew(self.conn, job.id, 300)
        self.assertGreater(renewed.expires_at, time.time() + 250)
        self.assertEqual(db.release(self.conn, job.id).status, 'released')
        with self.assertRaises(ValueError):
            db.renew(self.conn, job.id)
        with self.assertRaises(LookupError):
            db.renew(self.conn, self.command('true').id)

    def test_older_database_gains_new_columns(self):
        self.conn.close()
        path = Path(self.tmp.name) / 'old.db'
        old = sqlite3.connect(str(path))
        old.executescript(db.SCHEMA.replace('    pid_started TEXT,\n', ''))
        old.close()
        conn = db.connect(path)
        self.addCleanup(conn.close)
        job = db.create_job(conn, 'command', command='true', cwd=self.cwd)
        self.assertIsNone(db.get_job(conn, job.id).pid_started)

    def test_pause_is_per_device(self):
        db.set_paused(self.conn, 'xbox', 'user playing')
        self.assertEqual(db.paused(self.conn, 'xbox'), 'user playing')
        self.assertIsNone(db.paused(self.conn, 'other'))
        db.set_paused(self.conn, 'xbox', None)
        self.assertIsNone(db.paused(self.conn, 'xbox'))


class WorkerTests(QueueTestCase):
    def test_command_runs_in_its_checkout_and_records_exit_code(self):
        self.start_worker()
        job = self.command('pwd; echo job=$XBOXQ_JOB_ID; exit 3', owner='tester')
        done = self.wait_for(job.id, lambda j: j.terminal)
        self.assertEqual((done.status, done.exit_code), ('failed', 3))
        log = Path(done.log_path).read_text()
        self.assertIn(os.path.realpath(self.cwd), log)
        self.assertIn(f'job={job.id}', log)
        self.assertIn('for tester', log)

    def test_timeout_and_cancel_stop_the_process_group(self):
        self.start_worker()
        slow = self.command('sleep 30', limit_s=1)
        self.assertEqual(self.wait_for(slow.id, lambda j: j.terminal).status, 'timed_out')
        running = self.command('sleep 30 & sleep 30; wait')
        pid = self.wait_for(running.id, lambda j: j.pid is not None).pid
        self.assertEqual(db.cancel(self.conn, running.id)[0], 'kill_requested')
        done = self.wait_for(running.id, lambda j: j.terminal)
        self.assertEqual(done.status, 'cancelled')
        with self.assertRaises(ProcessLookupError):  # the background sleep went with its group
            os.killpg(pid, 0)

    def test_lease_blocks_later_jobs_until_released(self):
        self.start_worker()
        lease = self.lease(limit_s=60)
        command = self.command('true')
        self.wait_for(lease.id, lambda j: j.status == db.RUNNING)
        time.sleep(0.2)
        self.assertEqual(db.get_job(self.conn, command.id).status, db.PENDING)
        db.release(self.conn, lease.id)
        self.assertEqual(self.wait_for(command.id, lambda j: j.terminal).status, 'completed')

    def test_unreleased_lease_expires(self):
        self.start_worker()
        lease = self.lease(limit_s=1)
        self.assertEqual(self.wait_for(lease.id, lambda j: j.terminal).status, 'expired')

    def test_paused_queue_grants_nothing(self):
        db.set_paused(self.conn, db.DEFAULT_DEVICE, 'user playing')
        self.start_worker()
        job = self.command('true')
        time.sleep(0.2)
        self.assertEqual(db.get_job(self.conn, job.id).status, db.PENDING)
        db.set_paused(self.conn, db.DEFAULT_DEVICE, None)
        self.assertEqual(self.wait_for(job.id, lambda j: j.terminal).status, 'completed')

    def test_restart_fails_orphaned_commands_and_resumes_leases(self):
        orphan = self.command('true')
        db.claim_next(self.conn, db.DEFAULT_DEVICE)
        lease = self.lease(limit_s=60)
        db.claim_next(self.conn, db.DEFAULT_DEVICE)
        queued = self.command('true')
        self.start_worker()
        self.assertEqual(self.wait_for(orphan.id, lambda j: j.terminal).status, 'failed')
        time.sleep(0.2)
        self.assertEqual(db.get_job(self.conn, lease.id).status, db.RUNNING)
        self.assertEqual(db.get_job(self.conn, queued.id).status, db.PENDING)
        db.release(self.conn, lease.id)
        self.assertEqual(self.wait_for(queued.id, lambda j: j.terminal).status, 'completed')


class ServerTests(QueueTestCase):
    def setUp(self):
        super().setUp()
        self.httpd = QueueServer(0)
        thread = threading.Thread(target=self.httpd.serve_forever, kwargs={'poll_interval': 0.05}, daemon=True)
        thread.start()
        self.addCleanup(self.httpd.server_close)
        self.addCleanup(self.httpd.shutdown)
        self.url = f'http://127.0.0.1:{self.httpd.server_address[1]}'
        self.client = Client(self.url)

    def post(self, path, data, headers):
        request = urllib.request.Request(self.url + path, data=data, method='POST', headers=headers)
        try:
            with urllib.request.urlopen(request) as response:
                return response.status
        except urllib.error.HTTPError as error:
            return error.code

    def test_submit_run_and_read_log(self):
        self.start_worker()
        job = self.client.submit('echo queued-output', self.cwd, name='echo', owner='tester')
        self.assertEqual((job.kind, job.owner, job.name), ('command', 'tester', 'echo'))
        done = self.client.wait(job.id, poll_s=0.05, timeout_s=10)
        self.assertEqual(done.status, 'completed')
        text, size, status = self.client.log(job.id)
        self.assertIn('queued-output', text)
        self.assertEqual(status, 'completed')
        self.assertEqual(self.client.log(job.id, offset=size)[0], '')
        self.assertEqual(self.client.cancel(job.id)['action'], 'already_terminal')
        self.assertEqual([j.id for j in self.client.list(owner='tester')], [job.id])

    def test_lease_through_client(self):
        self.start_worker()
        lease = self.client.hold(self.cwd, owner='tester', ttl_s=60)
        granted = self.client.wait(lease.id, granted=True, poll_s=0.05, timeout_s=10)
        self.assertEqual(granted.status, db.RUNNING)
        self.assertEqual(self.client.state()['running']['id'], lease.id)
        self.assertEqual(self.client.release(lease.id).status, 'released')

    def test_pause_and_resume(self):
        self.assertEqual(self.client.pause('user playing')['paused'], 'user playing')
        self.assertIsNone(self.client.resume()['paused'])

    def test_rejects_foreign_hosts_cross_origin_and_form_posts(self):
        body = json.dumps({'kind': 'command', 'command': 'true', 'cwd': self.cwd}).encode()
        host = f'127.0.0.1:{self.httpd.server_address[1]}'
        self.assertEqual(self.post('/api/jobs', body, {'Host': 'attacker.example',
                                                       'Content-Type': 'application/json'}), 403)
        self.assertEqual(self.post('/api/jobs', body, {'Host': host, 'Origin': 'http://attacker.example',
                                                       'Content-Type': 'application/json'}), 403)
        self.assertEqual(self.post('/api/jobs', b'kind=command', {'Host': host,
                                                                  'Content-Type': 'text/plain'}), 415)
        self.assertEqual(self.post('/api/jobs', json.dumps({'shell': 'x'}).encode(),
                                   {'Host': host, 'Content-Type': 'application/json'}), 400)
        self.assertEqual(self.post('/api/jobs', json.dumps({'kind': 'command', 'command': ['true'],
                                                            'cwd': self.cwd}).encode(),
                                   {'Host': host, 'Content-Type': 'application/json'}), 400)
        self.assertEqual(db.list_jobs(self.conn), [])

    def test_negative_content_length_is_rejected(self):
        connection = http.client.HTTPConnection('127.0.0.1', self.httpd.server_address[1], timeout=5)
        self.addCleanup(connection.close)
        connection.putrequest('POST', '/api/jobs')
        connection.putheader('Content-Type', 'application/json')
        connection.putheader('Content-Length', '-1')
        connection.endheaders()
        self.assertEqual(connection.getresponse().status, 400)

    def test_dashboard_escapes_commands(self):
        self.command('echo <script>alert(1)</script>')
        with urllib.request.urlopen(self.url + '/') as response:
            page = response.read().decode()
        self.assertIn('&lt;script&gt;', page)
        self.assertNotIn('<script>alert', page)


class ExclusivityTests(QueueTestCase):
    """Regressions for ways a job could keep or take the console out of turn."""

    def stale_pending_read(self, job_id):
        """Make the first get_job see the job as pending, as if the worker claimed it just after."""
        stale = db.get_job(self.conn, job_id)
        db.claim_next(self.conn, db.DEFAULT_DEVICE)
        real = db.get_job
        calls = []

        def get_job(conn, wanted):
            calls.append(wanted)
            return stale if len(calls) == 1 else real(conn, wanted)
        return patch.object(db, 'get_job', side_effect=get_job)

    def test_cancel_that_loses_the_race_to_the_worker_stops_the_running_job(self):
        job = self.command('true')
        with self.stale_pending_read(job.id):
            action, current = db.cancel(self.conn, job.id)
        self.assertEqual(action, 'kill_requested')
        self.assertEqual((current.status, current.cancel_requested), (db.RUNNING, 1))

    def test_release_that_loses_the_race_to_the_grant_still_releases(self):
        lease = self.lease()
        with self.stale_pending_read(lease.id):
            self.assertEqual(db.release(self.conn, lease.id).status, 'released')

    def test_cancel_and_completion_stop_group_members_that_ignore_sigterm(self):
        self.start_worker()
        stubborn = "(trap '' TERM; sleep 30) & "
        with patch('tools.xbox_queue.worker.KILL_GRACE_S', 0.5):
            cancelled = self.command(stubborn + 'sleep 30; wait')
            pid = self.wait_for(cancelled.id, lambda j: j.pid is not None).pid
            db.cancel(self.conn, cancelled.id)
            self.assertEqual(self.wait_for(cancelled.id, lambda j: j.terminal).status, 'cancelled')
            self.assertFalse(group_alive(pid))

            finished = self.command(stubborn + 'echo done')
            pid = self.wait_for(finished.id, lambda j: j.pid is not None or j.terminal).pid
            done = self.wait_for(finished.id, lambda j: j.terminal)
        self.assertEqual(done.status, 'completed')
        self.assertIn('stopped processes the command left running', Path(done.log_path).read_text())
        if pid is not None:
            self.assertFalse(group_alive(pid))

    def start_session_process(self):
        proc = subprocess.Popen(['sleep', '30'], start_new_session=True)
        self.addCleanup(proc.wait)
        self.addCleanup(lambda: proc.poll() is None and proc.kill())
        return proc

    def start_orphan(self):
        """A session leader whose parent has exited, like a command a dead server left behind."""
        spawn = ('import subprocess; '
                 "print(subprocess.Popen(['sleep', '30'], start_new_session=True, stdin=subprocess.DEVNULL, "
                 'stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).pid)')
        pid = int(subprocess.run([sys.executable, '-c', spawn], capture_output=True, text=True).stdout)
        self.addCleanup(lambda: group_alive(pid) and os.killpg(pid, 9))
        return pid

    def orphan(self, pid, started):
        job = self.command('sleep 30')
        db.claim_next(self.conn, db.DEFAULT_DEVICE)
        db.set_pid(self.conn, job.id, pid, started)
        return job

    def test_recovery_stops_its_own_orphan_but_not_a_reused_pid(self):
        mine, stranger = self.start_orphan(), self.start_session_process()
        own = self.orphan(mine, process_started(mine))
        reused = self.orphan(stranger.pid, 'Thu Jan  1 00:00:00 1970')
        self.start_worker()
        self.assertIn('stopped its process group', self.wait_for(own.id, lambda j: j.terminal).note)
        self.assertIn('another process', self.wait_for(reused.id, lambda j: j.terminal).note)
        self.assertFalse(group_alive(mine))
        self.assertIsNone(stranger.poll())

    def test_only_one_server_owns_a_state_directory(self):
        first = acquire_lock()
        self.addCleanup(first.close)
        self.assertIsNone(acquire_lock())
        first.close()
        second = acquire_lock()
        self.assertIsNotNone(second)
        second.close()


class CommandLineTests(unittest.TestCase):
    def test_command_text_accepts_separator_and_single_strings(self):
        self.assertEqual(_command_text(['--', 'python3', '-m', 'tools.xbox_deploy', 'status']),
                         'python3 -m tools.xbox_deploy status')
        self.assertEqual(_command_text(['echo $PWD && true']), 'echo $PWD && true')
        with self.assertRaises(SystemExit):
            _command_text(['--'])

    def test_signal_deaths_exit_like_a_shell(self):
        job = db.Job(**{name: None for name in db.Job.__dataclass_fields__})
        job.status, job.exit_code = 'failed', -9
        self.assertEqual(_exit_code(job), 137)


if __name__ == '__main__':
    unittest.main()
