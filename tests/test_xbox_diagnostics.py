from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit

from probe import xbox_diagnostics as diagnostics


ROOT = Path(__file__).resolve().parents[1]
APP = {'PackageFullName': 'XboxPortabilityProbe.ChildProcess.SteamCore_1.2.3.4_x64__example'}
EPOCH_MS = 1_700_000_000_000
APP_ID = 42
OLD_BROWSER = '11111111-1111-1111-1111-111111111111'
NEW_BROWSER = '22222222-2222-2222-2222-222222222222'


class PortalError(ValueError):
    def __init__(self, status):
        self.status = status


def probe(launch=None, app_id=APP_ID, *, run=EPOCH_MS, extra=''):
    report = f'run_id={run}\r\naccount_token=do-not-save\r\n'
    if launch is not None:
        report += f'steam_launch_run_id={launch}\r\nsteam_launch_app_id={app_id}\r\n'
    return (report + extra).encode()


def browser(run, status='gamepad-ready'):
    return (f'run_id={run}\r\nstatus={status}\r\nowned_games_served=33\r\nstartup_elapsed_ms=shell_gamepad_ready:42\r\n'
            'renderer_error=https://example.invalid/do-not-save\r\n').encode()


def frame(launch, magic=0x31444846):
    return (diagnostics.HEADER.pack(magic, 2, 10_000_000, diagnostics.FILETIME_EPOCH + launch * 10_000, 12)
            + diagnostics.PRESENT.pack(120, 42, 1, 2))


def options(**overrides):
    args = dict(version='1.2.3.4', startup=False, app_id=APP_ID, run_id=None, image_name=None, timeout=8,
                frame_seconds=0, output=None, bootstrap_frame=False)
    args.update(overrides)
    return SimpleNamespace(**args)


class Fixture:
    def __init__(self, snapshots, stages=()):
        self.snapshots = snapshots
        self.stages = stages
        self.now = 0
        self.reads = []

    @property
    def step(self):
        return min(int(self.now), len(self.snapshots) - 1)

    def read(self, _, app, folder, name, limit):
        assert app == APP
        self.reads.append((folder, name, limit))
        raw = self.snapshots[self.step].get((folder, name), self.snapshots[self.step].get(name, b''))
        if isinstance(raw, PortalError):
            raise raw
        return raw[:limit]

    def request(self, method, path, **kwargs):
        assert method == 'GET'
        if path.startswith('/api/filesystem/apps/files?'):
            assert parse_qs(urlsplit(path).query)['path'] == [diagnostics.TEMP]
            items = self.stages[min(self.step, len(self.stages) - 1)] if self.stages else []
            return 200, json.dumps({'Items': items}).encode()
        raise AssertionError(path)

    def pause(self, seconds):
        self.now += seconds

    def observe(self, args):
        return diagnostics.observe(args, self, APP, self.read, lambda raw: raw.decode(), PortalError,
                                   clock=lambda: self.now, wall_clock=lambda: EPOCH_MS / 1000, pause=self.pause)


class ObserverTests(unittest.TestCase):
    def test_startup_requires_fresh_native_and_browser_runs_and_filters_fields(self):
        fixture = Fixture([
            {diagnostics.PROBE_REPORT: probe(), diagnostics.WEBVIEW_REPORT: browser(OLD_BROWSER)},
            {diagnostics.PROBE_REPORT: probe(run=EPOCH_MS + 1), diagnostics.WEBVIEW_REPORT: browser(OLD_BROWSER)},
            {diagnostics.PROBE_REPORT: PortalError(409), diagnostics.WEBVIEW_REPORT: browser(NEW_BROWSER)},
            {diagnostics.PROBE_REPORT: probe(run=EPOCH_MS + 1, extra='steam_native_startup_title_metadata_ms=136\r\n'),
             diagnostics.WEBVIEW_REPORT: browser(NEW_BROWSER)},
        ])
        result = fixture.observe(options(startup=True, app_id=None))
        self.assertEqual(fixture.now, 3)
        self.assertEqual(result['outcome'], 'gamepad-ready')
        self.assertEqual(result['run_id'], EPOCH_MS + 1)
        self.assertEqual(result['probe'], {'steam_native_startup_title_metadata_ms': '136'})
        self.assertNotIn('do-not-save', json.dumps(result))
        self.assertEqual(result['webview']['owned_games_served'], '33')
        self.assertEqual(result['webview']['startup_elapsed_ms'], 'shell_gamepad_ready:42')

    def test_game_matches_appid_retries_transient_report_and_collects_new_run_artifacts(self):
        launch = EPOCH_MS + 2
        exit_file = f'steam-win32-shadow-{APP_ID}-exit-2274.txt'
        temp_folder = diagnostics.TEMP + '\\SteamWin32Trial (2)'
        stable = ('steam_shadow_stage=verified-local-copy\r\nsteam_launch_sdl_run=2274\r\n'
                  f'steam_shadow_exit_report={exit_file}\r\n')
        fixture = Fixture([
            {diagnostics.PROBE_REPORT: probe(EPOCH_MS), 'fhd-present-times.bin': frame(EPOCH_MS)},
            {diagnostics.PROBE_REPORT: probe(EPOCH_MS + 1, APP_ID + 1)},
            {diagnostics.PROBE_REPORT: probe(launch, extra=stable + 'steam_launch_phase=child-created\r\n'),
             exit_file: PortalError(409), (temp_folder, 'loader-trace.txt'):
                 b'load example.dll ok source=package\r\nasset C:\\private-secret\r\n'},
            {diagnostics.PROBE_REPORT: PortalError(404)},
            {diagnostics.PROBE_REPORT: probe(launch, extra=stable + 'steam_sdl_host_frame=presented\r\n'),
             exit_file: b'steam_shadow_child_status=exited\r\nsteam_shadow_child_exit_u32=1\r\n'
                        b'steam_shadow_loader=dxgi native-present-interval result=0x00000000\r\n',
             'sdl-startup-2274.txt': b'sdl_gpu_interop=published\r\npath=C:\\private-secret\r\n',
             'sdl-startup-2274.bmp': b'BMcapture',
             'fhd-present-times.bin': frame(launch)},
        ], stages=[
            [{'Name': 'SteamWin32Trial', 'DateCreated': 1}],
            [],
            [{'Name': 'SteamWin32Trial (2)', 'DateCreated': 2}],
            [], [],
        ])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'observation'
            result = fixture.observe(options(output=output, bootstrap_frame=True))
            self.assertEqual(result['run_id'], launch)
            self.assertEqual(result['outcome'], 'game-exited')
            self.assertEqual(result['probe']['steam_shadow_child_exit_u32'], '1')
            self.assertEqual((output / 'sdl-startup.txt').read_text(), 'sdl_gpu_interop=published\n')
            self.assertEqual((output / 'loader-trace.txt').read_text(), 'load example.dll ok source=package\n'
                             'dxgi native-present-interval result=0x00000000\n')
            self.assertEqual((output / 'fhd-present-times.bin').read_bytes(), frame(launch))
            self.assertEqual((output / 'bootstrap.bmp').read_bytes(), b'BMcapture')
            self.assertEqual(json.loads((output / 'observation.json').read_text()), result)
            self.assertNotIn('do-not-save', json.dumps(result))

    def test_attach_keeps_last_checkpoint_but_rejects_stale_or_incomplete_trace(self):
        launch = EPOCH_MS + 1
        for bad_trace in (frame(launch - 1), frame(launch, magic=0), frame(launch)[:-2]):
            with self.subTest(bad_trace=bad_trace):
                fixture = Fixture([
                    {diagnostics.PROBE_REPORT: probe(launch, extra='steam_launch_phase=child-created\r\n'
                                                      'steam_shadow_exit_report=..\\unrelated.txt\r\n'),
                     'fhd-present-times.bin': bad_trace},
                    {diagnostics.PROBE_REPORT: PortalError(409), 'fhd-present-times.bin': bad_trace},
                ])
                with tempfile.TemporaryDirectory() as directory:
                    output = Path(directory) / 'result'
                    result = fixture.observe(options(run_id=launch, timeout=2, output=output))
                    self.assertEqual(result['run_id'], launch)
                    self.assertEqual(result['outcome'], 'observation-timeout')
                    self.assertEqual(result['probe']['steam_launch_phase'], 'child-created')
                    self.assertTrue((output / 'observation.json').is_file())
                    self.assertFalse((output / 'fhd-present-times.bin').exists())
                    self.assertTrue(all('unrelated' not in name for _, name, _ in fixture.reads))

    def test_unmatched_or_oversized_reports_do_not_become_observations(self):
        fake = Fixture([
            {},
            {diagnostics.PROBE_REPORT: b'x' * diagnostics.REPORT_LIMIT + b'\n',
             diagnostics.WEBVIEW_REPORT: browser(OLD_BROWSER)},
            {diagnostics.PROBE_REPORT: probe(run=EPOCH_MS - 60_000),
             diagnostics.WEBVIEW_REPORT: browser(NEW_BROWSER)},
        ])
        result = fake.observe(options(startup=True, app_id=None, timeout=4))
        self.assertEqual(result['outcome'], 'observation-timeout')
        self.assertNotIn('run_id', result)

    def test_fixed_frame_file_is_discarded_when_a_new_launch_starts_during_collection(self):
        launch = EPOCH_MS + 1

        class LaunchChangesDuringRead(Fixture):
            def read(self, portal, app, folder, name, limit):
                raw = super().read(portal, app, folder, name, limit)
                if name == 'fhd-present-times.bin':
                    self.snapshots[self.step][diagnostics.PROBE_REPORT] = probe(launch + 1, APP_ID + 1)
                return raw

        fixture = LaunchChangesDuringRead([
            {diagnostics.PROBE_REPORT: probe(launch, extra='steam_sdl_host_frame=presented\r\n'),
             'fhd-present-times.bin': frame(launch + 1)},
        ])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'observation'
            result = fixture.observe(options(run_id=launch, output=output))
            self.assertEqual(result['outcome'], 'frame-observed')
            self.assertEqual(result['run_id'], launch)
            self.assertFalse((output / 'fhd-present-times.bin').exists())

    def test_attach_to_a_superseded_run_returns_immediately(self):
        launch = EPOCH_MS + 1
        fixture = Fixture([{diagnostics.PROBE_REPORT: probe(launch + 1, APP_ID + 1)}])
        result = fixture.observe(options(run_id=launch))
        self.assertEqual(fixture.now, 0)
        self.assertEqual(result['outcome'], 'launch-superseded')
        self.assertEqual(result['requested_run_id'], launch)
        self.assertNotIn('run_id', result)

    def test_standard_cli_reuses_pinned_setup_and_get_only_requests(self):
        spec = importlib.util.spec_from_file_location('xbox_deploy_cli', ROOT / 'xbox-deploy.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        launch = EPOCH_MS + 1
        fingerprint = '1' * 64
        requests = []

        class Portal:
            def __init__(self, url, pin):
                self.authorization = None
                self.pinned = pin
                self.test_url = url

            def request(self, method=None, path=None, **kwargs):
                requests.append(method)
                if method is None:
                    assert self.authorization is None
                    assert self.test_url == 'https://xbox.invalid'
                    return self.pinned
                assert method == 'GET'
                assert self.authorization is not None
                if path.endswith('/packages'):
                    return 200, json.dumps({'InstalledPackages': [APP]}).encode()
                name = parse_qs(urlsplit(path).query)['filename'][0]
                if name == diagnostics.PROBE_REPORT:
                    return 200, probe(launch, extra='steam_sdl_host_frame=presented\r\n')
                return 200, b''

        command = ['xbox-deploy.py', 'observe', '--version', '1.2.3.4', '--app-id', str(APP_ID),
                   '--run-id', str(launch), '--frame-seconds', '0']
        settings = dict(XBOX_PORTAL_URL='https://xbox.invalid', XBOX_PORTAL_USERNAME='test_user',
                        XBOX_PORTAL_PASSWORD='test_secret', XBOX_PORTAL_CERT_SHA256=fingerprint)
        output, errors = io.StringIO(), io.StringIO()
        with (patch.object(module, 'DevicePortal', Portal),
              patch.object(module, 'load_settings', return_value=settings),
              patch('sys.argv', command), redirect_stdout(output), redirect_stderr(errors)):
            module.main()
        self.assertEqual(json.loads(output.getvalue())['outcome'], 'frame-observed')
        self.assertEqual(errors.getvalue().strip(), 'attaching-to-launch')
        self.assertEqual(requests[0], None)
        self.assertTrue(all(method == 'GET' for method in requests[1:]))
        self.assertNotIn('test_secret', output.getvalue() + errors.getvalue())
