from contextlib import redirect_stderr, redirect_stdout
import errno
import hashlib
import http.client
import io
import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit

from tools import device_portal, xbox_capture as capture, xbox_deploy as cli
from tools.device_portal import CertificateChanged, PortalError, PortalRetry, PortalUnavailable

APP = {'PackageFullName': 'XboxPortabilityProbe.ChildProcess.SteamCore_1.2.3.4_x64__example',
       'PackageRelativeId': 'XboxPortabilityProbe.ChildProcess.SteamCore_example!App'}
FILETIME_2026 = 134_090_000_000_000_000


class Clock:
    def __init__(self):
        self.now = 0.0
        self.pauses = []

    def pause(self, seconds):
        self.pauses.append(seconds)
        self.now += seconds


def png(width=1920, height=1080):
    return capture.PNG_SIGNATURE + struct.pack('>I4sII', 13, b'IHDR', width, height) + b'\x08\x06\x00\x00\x00rest'


class RetryTests(unittest.TestCase):
    def retry(self, clock, **kwargs):
        return PortalRetry(clock=lambda: clock.now, wall_clock=lambda: 1_795_633_733 + clock.now,
                           pause=clock.pause, **kwargs)

    def test_transient_errors_are_retried_with_backoff_and_the_gap_is_recorded(self):
        clock = Clock()
        retry = self.retry(clock)
        failures = iter([TimeoutError(errno.ETIMEDOUT, 'Operation timed out'), ConnectionResetError(), None])

        def read():
            error = next(failures)
            if error:
                raise error
            return 'ok'

        self.assertEqual(retry.call(read), 'ok')
        self.assertEqual(clock.pauses, [0.5, 1])
        summary = retry.summary()
        self.assertEqual(summary['retries'], 2)
        gap, = summary['gaps']
        self.assertEqual(gap, {'start_utc': '2026-11-25T19:08:53.000Z', 'end_utc': '2026-11-25T19:08:54.500Z',
                               'seconds': 1.5, 'error': f'TimeoutError errno={errno.ETIMEDOUT}',
                               'failed_attempts': 2, 'skipped_reads': 0})
        self.assertIsNone(self.retry(Clock()).summary())

    def test_exhausted_retries_skip_the_read_and_later_reads_try_once_until_recovery(self):
        clock = Clock()
        retry = self.retry(clock)
        attempts = []

        def down():
            attempts.append(clock.now)
            raise http.client.RemoteDisconnected('closed')

        with self.assertRaises(PortalUnavailable):
            retry.call(down)
        self.assertEqual(clock.pauses, [0.5, 1, 2, 4])
        with self.assertRaises(PortalUnavailable):
            retry.call(down)
        self.assertEqual(len(attempts), 6)
        summary = retry.summary()
        self.assertEqual((summary['retries'], summary['skipped_reads'], summary['gaps']), (4, 2, []))
        self.assertEqual(summary['unrecovered_gap']['failed_attempts'], 6)
        self.assertEqual(retry.call(lambda: 'back'), 'back')
        self.assertEqual(retry.summary()['gaps'][0]['skipped_reads'], 2)

    def test_deadline_bounds_the_backoff(self):
        clock = Clock()
        retry = self.retry(clock, deadline=1.2)

        def down():
            raise TimeoutError()

        with self.assertRaises(PortalUnavailable):
            retry.call(down)
        self.assertEqual(clock.pauses, [0.5])

    def test_certificate_changes_http_errors_and_local_errors_are_never_retried(self):
        for error in (CertificateChanged('a' * 64, 'b' * 64), PortalError(500, 'GET returned HTTP 500'),
                      FileExistsError(errno.EEXIST, 'exists'), ValueError('bad')):
            clock = Clock()
            retry = self.retry(clock)
            with self.assertRaises(type(error)):
                retry.call(lambda: (_ for _ in ()).throw(error))
            self.assertEqual(clock.pauses, [])
            self.assertIsNone(retry.summary())

    def test_only_get_requests_are_retried(self):
        clock = Clock()
        retry = self.retry(clock)
        calls = []

        class Portal:
            def request(self, method, path, **kwargs):
                calls.append(method)
                raise ConnectionRefusedError()

        with self.assertRaises(ConnectionRefusedError):
            retry.request(Portal(), 'POST', '/api/taskmanager/app')
        with self.assertRaises(PortalUnavailable):
            retry.request(Portal(), 'GET', '/api/os/info')
        self.assertEqual(calls, ['POST'] + ['GET'] * 5)

    def test_pinned_transport_reports_a_new_certificate_without_trusting_it(self):
        pinned = hashlib.sha256(b'old certificate').hexdigest()
        observed = hashlib.sha256(b'certificate after reboot').hexdigest()
        portal = device_portal.DevicePortal('https://xbox.invalid:11443', pinned)
        with patch.object(device_portal.http.client, 'HTTPSConnection') as https:
            connection = https.return_value
            connection.sock.getpeercert.return_value = b'certificate after reboot'
            for _ in range(2):
                with self.assertRaises(CertificateChanged) as raised:
                    portal.request('GET', '/api/os/info')
                self.assertEqual(raised.exception.observed, observed)
                self.assertIn(observed, str(raised.exception))
                self.assertIn('after some reboots', str(raised.exception))
            connection.request.assert_not_called()
        self.assertEqual(portal.fingerprint, pinned)

    def test_cli_names_the_new_fingerprint_when_the_pin_check_fails(self):
        settings = {'XBOX_PORTAL_URL': 'https://xbox.invalid', 'XBOX_PORTAL_CERT_SHA256': '1' * 64}

        class Portal:
            def __init__(self, url, pin):
                pass

            def request(self, method=None, path=None, **kwargs):
                assert method is None
                return '2' * 64

        with (patch.object(cli, 'DevicePortal', Portal), patch.object(cli, 'load_settings', return_value=settings),
              patch('sys.argv', ['xbox_deploy.py', 'status'])):
            with self.assertRaisesRegex(CertificateChanged, '2{64}.*Confirm that this fingerprint'):
                cli.main()


class ProcessPortal:
    def __init__(self, snapshots, clock):
        self.snapshots = snapshots
        self.clock = clock
        self.requests = []

    def request(self, method, path, **kwargs):
        self.requests.append((method, path))
        assert method == 'GET' and path == capture.PROCESSES
        snapshot = self.snapshots[min(int(self.clock.now), len(self.snapshots) - 1)]
        if isinstance(snapshot, Exception):
            raise snapshot
        return 200, json.dumps({'Processes': snapshot}).encode()


def process(image, pid, commit_mib, package=APP['PackageFullName'], cpu=1.25):
    row = {'ImageName': image, 'ProcessId': pid, 'CPUUsage': cpu, 'PrivateWorkingSet': commit_mib * 1024 * 1024 // 2,
           'TotalCommit': commit_mib * 1024 * 1024, 'UserName': 'XBOX\\do-not-save'}
    if package:
        row['PackageFullName'] = package
    return row


class SamplerTests(unittest.TestCase):
    def test_samples_package_and_named_processes_with_events_and_unavailable_samples(self):
        clock = Clock()
        steam = process('SteamPrimary.exe', 10, 100)
        game = process('Game.exe', 20, 900, package=None, cpu=12.5)
        other = process('Dashboard.exe', 30, 50, package='Xbox.Dashboard_1_x64__x')
        # The portal stops answering at 3 s; the retry at 3.5 s fails and the next would pass the 4 s end.
        portal = ProcessPortal([[steam, other], [steam, game], [steam], TimeoutError(), TimeoutError()], clock)
        progress = []
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'samples.jsonl'
            summary = capture.sample_processes(portal, APP['PackageFullName'], ['game.exe', 'WerFault.exe'], 4, 1,
                                               output, clock=lambda: clock.now, wall_clock=lambda: 0,
                                               pause=clock.pause, progress=progress.append)
            rows = [json.loads(line) for line in output.read_text().splitlines()]
        self.assertEqual(summary['samples'], 3)
        self.assertEqual(summary['unavailable_samples'], 2)
        self.assertEqual(summary['peak_package_commit_mib'], 100.0)
        self.assertEqual([(entry['image'], entry['samples'], entry['peak_total_commit_mib'], entry['max_cpu_percent'])
                          for entry in summary['processes']], [('Game.exe', 1, 900.0, 12.5),
                                                               ('SteamPrimary.exe', 3, 100.0, 1.2)])
        self.assertEqual([(row['event'], row['image']) for row in rows if 'event' in row],
                         [('started', 'Game.exe'), ('exited', 'Game.exe')])
        self.assertEqual(sum(bool(row.get('unavailable')) for row in rows), 2)
        self.assertNotIn('Dashboard.exe', json.dumps(rows))
        self.assertNotIn('do-not-save', json.dumps(rows) + json.dumps(summary))
        self.assertEqual(rows[0], {'utc': '1970-01-01T00:00:00.000Z', 'package_commit_mib': 100.0, 'processes': [
            {'image': 'SteamPrimary.exe', 'pid': 10, 'cpu_percent': 1.2, 'private_working_set_mib': 50.0,
             'total_commit_mib': 100.0, 'packaged': True}]})
        self.assertEqual((summary['portal']['retries'], summary['portal']['skipped_reads']), (1, 2))
        self.assertEqual(clock.pauses, [1, 1, 1, 0.5, 0.5])
        self.assertTrue(any(stage.startswith('portal-unreachable') for stage in progress))


class ScreenshotTests(unittest.TestCase):
    def test_saves_scheduled_screenshots_and_records_failures_without_stopping(self):
        clock = Clock()
        responses = iter([(200, png()), PortalError(500, 'GET returned HTTP 500'), (200, b'not a png')])

        class Portal:
            def request(self, method, path, **kwargs):
                assert (method, path) == ('GET', capture.SCREENSHOT)
                clock.now += 0.5
                response = next(responses)
                if isinstance(response, Exception):
                    raise response
                return response

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'shots'
            result = capture.capture_screenshots(Portal(), output, 45, 45, 3, clock=lambda: clock.now,
                                                 wall_clock=lambda: 1_795_633_733 + clock.now, pause=clock.pause)
            self.assertEqual((output / 'shot-001.png').read_bytes(), png())
            index = [json.loads(line) for line in (output / 'screenshots.jsonl').read_text().splitlines()]
            self.assertEqual(sorted(path.name for path in output.iterdir()), ['screenshots.jsonl', 'shot-001.png'])
        self.assertEqual(index, result['screenshots'])
        self.assertEqual(result['saved'], 1)
        first, second, third = index
        self.assertEqual((first['offset_s'], first['width'], first['height'], first['capture_ms']), (45, 1920, 1080, 500))
        self.assertEqual(first['sha256'], hashlib.sha256(png()).hexdigest())
        self.assertEqual(first['start_utc'], '2026-11-25T19:09:38.000Z')
        self.assertEqual(second['error'], 'http-500')
        self.assertEqual(second['offset_s'], 90)
        self.assertEqual(third['error'], 'not-a-png')
        self.assertEqual(clock.pauses[0], 45)


class FileTests(unittest.TestCase):
    def test_lists_recent_package_files_recursively(self):
        listings = {
            r'\TempState': [{'Name': 'SteamWin32Trial (163)', 'Type': 16, 'DateCreated': FILETIME_2026 + 10},
                            {'Name': 'old.txt', 'Type': 32, 'FileSize': 3, 'DateCreated': 1, 'DateModified': 1}],
            r'\TempState\SteamWin32Trial (163)': [
                {'Name': 'loader-trace.txt', 'Type': 32, 'FileSize': 15938, 'DateCreated': FILETIME_2026 + 20,
                 'DateModified': FILETIME_2026 + 30},
                {'Name': '..\\escape', 'Type': 32}],
        }

        class Portal:
            def request(self, method, path, **kwargs):
                query = parse_qs(urlsplit(path).query)
                assert path.startswith('/api/filesystem/apps/files?') and query['knownfolderid'] == ['LocalAppData']
                assert query['packagefullname'] == [APP['PackageFullName']]
                return 200, json.dumps({'Items': listings[query['path'][0]]}).encode()

        found = capture.list_files(Portal(), APP, r'\TempState', recursive=True, since=FILETIME_2026)
        self.assertEqual([entry['path'] for entry in found],
                         [r'\TempState\SteamWin32Trial (163)', r'\TempState\SteamWin32Trial (163)\loader-trace.txt'])
        self.assertEqual(found[1]['bytes'], 15938)
        self.assertEqual(found[1]['created_utc'], capture.filetime_utc(FILETIME_2026 + 20))
        self.assertEqual(len(capture.list_files(Portal(), APP, r'\TempState')), 2)

    def test_streams_one_file_and_rejects_paths_outside_the_package_folders(self):
        data = b'audio window=1 peak-milli=29 audible=15/100\r\n' * 1000

        class Portal:
            def request(self, method, path, **kwargs):
                query = parse_qs(urlsplit(path).query)
                assert query['path'] == [r'\TempState\SteamWin32Trial (163)']
                assert query['filename'] == ['loader-trace.txt']
                stream = io.BytesIO(data)
                return 200, kwargs['receive'](SimpleNamespace(read=stream.read))

        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / 'loader-raw.txt'
            result = capture.get_file(Portal(), APP, r'\TempState\SteamWin32Trial (163)', 'loader-trace.txt', destination)
            self.assertEqual(destination.read_bytes(), data)
            self.assertEqual(result['sha256'], hashlib.sha256(data).hexdigest())
            small = Path(directory) / 'small.txt'
            with self.assertRaisesRegex(ValueError, 'larger than 10 bytes'):
                capture.get_file(Portal(), APP, r'\TempState\SteamWin32Trial (163)', 'loader-trace.txt', small, 10)
            self.assertFalse(small.exists())
        for folder in (r'\TempState\..\LocalState', 'TempState', r'C:\Windows', r'\SteamLibrary', r'\TempState\a:b'):
            with self.assertRaises(ValueError):
                capture.package_folder(folder)
        for name in ('a\\b', '..', 'a/b', ''):
            with self.assertRaises(ValueError):
                capture.file_name(name)


class RelaunchTests(unittest.TestCase):
    def portal(self, running_polls):
        calls = []
        polls = iter(running_polls)

        class Portal:
            def request(self, method, path, **kwargs):
                calls.append((method, path.split('?')[0]))
                if path == '/api/resourcemanager/processes':
                    count = next(polls)
                    return 200, json.dumps({'Processes': [
                        {'ImageName': 'SteamPrimary.exe', 'ProcessId': 1, 'PackageFullName': APP['PackageFullName']},
                        {'ImageName': 'msedgewebview2.exe', 'ProcessId': 2, 'PackageFullName': APP['PackageFullName']},
                        {'ImageName': 'Dashboard.exe', 'ProcessId': 3, 'PackageFullName': 'Xbox.Dashboard'},
                    ][:count] + [{'ImageName': 'Dashboard.exe', 'ProcessId': 3}]}).encode()
                return 200, b''

        return Portal(), calls

    def test_relaunch_waits_for_the_package_to_stop_before_activating(self):
        portal, calls = self.portal([2, 1, 0])
        clock = Clock()
        result = cli.relaunch_package(portal, APP, 30, clock=lambda: clock.now, pause=clock.pause)
        self.assertEqual(result['stopped_processes'], ['SteamPrimary.exe', 'msedgewebview2.exe'])
        self.assertEqual(result['stop_seconds'], 0.5)
        self.assertEqual([method for method, _ in calls], ['GET', 'DELETE', 'GET', 'GET', 'POST'])
        self.assertEqual(calls[-1], ('POST', '/api/taskmanager/app'))

    def test_relaunch_does_not_activate_a_package_that_will_not_stop(self):
        portal, calls = self.portal([2] * 100)
        clock = Clock()
        with self.assertRaisesRegex(ValueError, 'still has running processes after 3s .*not activated'):
            cli.relaunch_package(portal, APP, 3, clock=lambda: clock.now, pause=clock.pause)
        self.assertNotIn('POST', [method for method, _ in calls])

    def test_cli_relaunch_and_read_only_commands_select_the_installed_steam_core(self):
        settings = {'XBOX_PORTAL_URL': 'https://xbox.invalid', 'XBOX_PORTAL_CERT_SHA256': '1' * 64}
        methods = []

        class Portal:
            def __init__(self, url, pin):
                self.pin = pin

            def request(self, method=None, path=None, **kwargs):
                methods.append(method)
                if method is None:
                    return self.pin
                if path.endswith('/packages'):
                    return 200, json.dumps({'InstalledPackages': [APP]}).encode()
                if path == '/api/resourcemanager/processes':
                    return 200, json.dumps({'Processes': []}).encode()
                if path.startswith('/api/filesystem/apps/files?'):
                    return 200, json.dumps({'Items': [{'Name': 'loader-trace.txt', 'Type': 32, 'FileSize': 4}]}).encode()
                return 200, b''

        def run(*command):
            output = io.StringIO()
            with (patch.object(cli, 'DevicePortal', Portal), patch.object(cli, 'load_settings', return_value=settings),
                  patch('sys.argv', ['xbox_deploy.py', *command]), redirect_stdout(output), redirect_stderr(io.StringIO())):
                cli.main()
            return json.loads(output.getvalue())

        self.assertEqual(run('relaunch', '--version', '1.2.3.4')['package'], APP['PackageFullName'])
        self.assertEqual(methods, [None, 'GET', 'GET', 'DELETE', 'GET', 'POST'])
        methods.clear()
        listed = run('list-files', r'\TempState\SteamWin32Trial (163)')
        self.assertEqual(listed[0]['path'], r'\TempState\SteamWin32Trial (163)\loader-trace.txt')
        self.assertTrue(all(method in (None, 'GET') for method in methods))
        with tempfile.TemporaryDirectory() as directory:
            existing = Path(directory) / 'samples.jsonl'
            existing.touch()
            for command in (['sample-processes', '--seconds', '0', '--output', str(Path(directory) / 'new.jsonl')],
                            ['sample-processes', '--seconds', '5', '--output', str(existing)],
                            ['sample-processes', '--seconds', '5', '--image-name', '..\\x.exe',
                             '--output', str(Path(directory) / 'new.jsonl')],
                            ['screenshots', '--count', '0', '--output', str(Path(directory) / 'shots')],
                            ['list-files', r'\TempState\..'],
                            ['get-file', r'\TempState', 'a\\b', '--output', str(Path(directory) / 'out')],
                            ['relaunch', '--stop-timeout', '0']):
                with (patch('sys.argv', ['xbox_deploy.py', *command]), redirect_stderr(io.StringIO()),
                      self.assertRaises(SystemExit)):
                    cli.main()


if __name__ == '__main__':
    unittest.main()
