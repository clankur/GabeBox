from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit

from tools import xbox_deploy as cli, xbox_diagnostics as diagnostics


APP = {'PackageFullName': 'XboxPortabilityProbe.ChildProcess.SteamCore_1.2.3.4_x64__example'}
EPOCH_MS = 1_700_000_000_000
APP_ID = 42
OLD_BROWSER = '11111111-1111-1111-1111-111111111111'
NEW_BROWSER = '22222222-2222-2222-2222-222222222222'
MAILBOX = 'sdl-host-24-9821.bin'
RENDER_FIELDS = f'steam_launch_sdl_run=2274\r\nsteam_sdl_mailbox={MAILBOX}\r\nsteam_sdl_host_frame=presented\r\n'
VERIFIED_STAGE = 'steam_shadow_stage=verified-local-copy\r\n'
LIVE_STAGE = 'SteamWin32Trial (38)'
LIVE_TRACE = (diagnostics.TEMP + '\\' + LIVE_STAGE, 'loader-trace.txt')


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


def stage(name, created_ms):
    return {'Name': name, 'DateCreated': diagnostics.FILETIME_EPOCH + created_ms * 10_000}


def bmp(width=2, height=-1, bits=32):
    pixels = b'\x00' * (((width * bits + 31) // 32 * 4) * abs(height))
    return (struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0, 54)
            + struct.pack('<IiiHHIIiiII', 40, width, height, 1, bits, 0, len(pixels), 0, 0, 0, 0)
            + pixels)


def gpu_mailbox(generation=2, width=1920, sequence=25, format_code=28):
    raw = bytearray(diagnostics.GPU_MAILBOX_SIZE)
    diagnostics.GPU_HEADER.pack_into(raw, 0, diagnostics.GPU_MAGIC, generation, width, 1080, format_code, 0)
    raw[32:43] = b'do-not-save'
    diagnostics.GPU_STATUS.pack_into(raw, diagnostics.GPU_STATUS_OFFSET, sequence, sequence - 1, generation, 1)
    return bytes(raw)


def options(**overrides):
    args = dict(version='1.2.3.4', startup=False, app_id=APP_ID, run_id=None, image_name=None, timeout=8,
                frame_seconds=0, output=None, bootstrap_frame=False, require_checkpoint=[],
                fail_on_checkpoint=[], watch_checkpoint=[], checkpoint_timeout=2, checkpoint_timeouts=[])
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
            query = parse_qs(urlsplit(path).query)
            assert query['path'] == [diagnostics.TEMP]
            assert query['packagefullname'] == [APP['PackageFullName']]
            items = self.stages[min(self.step, len(self.stages) - 1)] if self.stages else []
            return 200, json.dumps({'Items': items}).encode()
        raise AssertionError(path)

    def pause(self, seconds):
        self.now += seconds

    def observe(self, args):
        return diagnostics.observe(args, self, APP, self.read, lambda raw: raw.decode(), PortalError,
                                   clock=lambda: self.now, wall_clock=lambda: EPOCH_MS / 1000, pause=self.pause)

    def inspect_render(self, args):
        return diagnostics.collect_render_state(args, self, APP, self.read, lambda raw: raw.decode(), PortalError)

    def inspect_loader(self, args):
        return diagnostics.collect_live_loader(args, self, APP, self.read, lambda raw: raw.decode(), PortalError)


class ObserverTests(unittest.TestCase):
    def test_source_diagnostics_include_build_and_relative_fingerprint_path(self):
        report = {'steam_source_check': 'file-mismatch', 'steam_source_build': '23986256',
                  'steam_source_file': 'asset_archive/toc'}
        self.assertEqual(diagnostics.game_snapshot(report, {}, {}, APP_ID)['probe'], report)
        for path in ('../toc', 'D:/private/toc', '/private/toc'):
            report['steam_source_file'] = path
            self.assertNotIn('steam_source_file', diagnostics.game_snapshot(report, {}, {}, APP_ID)['probe'])

    def test_startup_requires_fresh_native_and_browser_runs_and_filters_fields(self):
        fixture = Fixture([
            {diagnostics.PROBE_REPORT: probe(), diagnostics.WEBVIEW_REPORT: browser(OLD_BROWSER)},
            {diagnostics.PROBE_REPORT: probe(run=EPOCH_MS + 1), diagnostics.WEBVIEW_REPORT: browser(OLD_BROWSER)},
            {diagnostics.PROBE_REPORT: PortalError(409), diagnostics.WEBVIEW_REPORT: browser(NEW_BROWSER)},
            {diagnostics.PROBE_REPORT: probe(run=EPOCH_MS + 1, extra='steam_native_startup_title_metadata_ms=136\r\n'
                                            'steam_library_pending_metadata=14\r\n'),
             diagnostics.WEBVIEW_REPORT: browser(NEW_BROWSER)},
        ])
        result = fixture.observe(options(startup=True, app_id=None))
        self.assertEqual(fixture.now, 3)
        self.assertEqual(result['outcome'], 'gamepad-ready')
        self.assertEqual(result['run_id'], EPOCH_MS + 1)
        self.assertEqual(result['probe'], {'steam_native_startup_title_metadata_ms': '136',
                                          'steam_library_pending_metadata': '14'})
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
             'sdl-startup-2274.bmp': bmp(),
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
            self.assertEqual((output / 'bootstrap.bmp').read_bytes(), bmp())
            self.assertEqual(json.loads((output / 'observation.json').read_text()), result)
            self.assertNotIn('do-not-save', json.dumps(result))

    def test_loader_trace_preserves_late_fault_after_repeated_exports(self):
        report = ('steam_shadow_loader=export GetLogicalProcessorInformation ok\r\n' * 140 +
                  'steam_shadow_loader=fault breakpoint module=crs-client.dll rva=0x000187A9\r\n')
        self.assertEqual(diagnostics.embedded_loader_lines(report), [
            'export GetLogicalProcessorInformation ok',
            'fault breakpoint module=crs-client.dll rva=0x000187A9',
        ])

    def test_game_observation_keeps_late_records_from_large_live_trace(self):
        launch = EPOCH_MS + 1
        fault = b'fault access-violation module=game.exe rva=0x00123456 read near-null\r\n'
        ordinary = b''.join(f'export library-checkpoint-{index:05d} ok source=package\r\n'.encode()
                            for index in range(700))
        self.assertGreater(len(ordinary), diagnostics.TRACE_LIMIT)
        trace = fault + ordinary + fault + b'fault access-violation incomplete'
        fixture = Fixture([
            {diagnostics.PROBE_REPORT: probe()},
            {diagnostics.PROBE_REPORT: probe(launch, extra=VERIFIED_STAGE + RENDER_FIELDS), LIVE_TRACE: trace},
        ], stages=[[], [stage(LIVE_STAGE, launch)]])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'observation'
            result = fixture.observe(options(output=output))
            self.assertEqual(result['outcome'], 'frame-observed')
            saved = (output / 'loader-trace.txt').read_text().splitlines()
            self.assertEqual(len(saved), 128)
            self.assertEqual(saved[-1], fault.decode().strip())
            self.assertFalse(any('incomplete' in line for line in saved))
            self.assertIn((*LIVE_TRACE, diagnostics.LIVE_LOADER_LIMIT + 1), fixture.reads)

    def test_under_checkpoint_contract_repeating_frames_do_not_extend_the_deadline(self):
        launch = EPOCH_MS + 1
        boot, ready = 'asset startup-phase=boot', 'asset startup-phase=menu'
        report = probe(launch, extra=VERIFIED_STAGE + RENDER_FIELDS)
        first = boot.encode() + b' detail=0x1\r\n'
        fixture = Fixture([
            {diagnostics.PROBE_REPORT: probe()},
            {diagnostics.PROBE_REPORT: report, LIVE_TRACE: first},
            {diagnostics.PROBE_REPORT: report, LIVE_TRACE: first + first},
            {diagnostics.PROBE_REPORT: report, LIVE_TRACE: first + first + ready.encode()},
        ], stages=[[], [stage(LIVE_STAGE, launch)]])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'observation'
            result = fixture.observe(options(output=output, require_checkpoint=[boot, ready]))
            self.assertEqual(fixture.now, 3)
            self.assertEqual(result['outcome'], 'checkpoint-stalled')
            self.assertEqual(result['checkpoints'], {'reached': [boot], 'waiting_for': ready,
                                                    'records': [boot + ' detail=0x1'], 'source': 'live-loader',
                                                    'waiting_for_timeout_seconds': 2})
            self.assertIn('loader-trace.txt', result['artifacts'])

    def test_stage_deadline_and_latest_watched_field_identify_the_stall(self):
        launch = EPOCH_MS + 1
        gpu = 'dxgi gpu-frame-submitted'
        zone = 'asset menu-zone-flags=*'
        ready = zone + ' detail=0x3'
        first = 'asset menu-zone-flags=0x400 detail=0x0'
        latest = 'asset menu-zone-flags=0x402 detail=0x1'
        not_ready = 'asset menu-zone-flags=0x402 detail=0x30'
        report = probe(launch, extra=VERIFIED_STAGE + RENDER_FIELDS)
        fixture = Fixture([
            {diagnostics.PROBE_REPORT: report},
            {diagnostics.PROBE_REPORT: report},
            {diagnostics.PROBE_REPORT: report, LIVE_TRACE: f'{gpu}\r\n{first}\r\n'.encode()},
            {diagnostics.PROBE_REPORT: report, LIVE_TRACE: f'{gpu}\r\n{first}\r\n{not_ready}\r\n{latest}\r\n'.encode()},
        ], stages=[[stage(LIVE_STAGE, launch)]])
        result = fixture.observe(options(run_id=launch, require_checkpoint=[gpu, ready], checkpoint_timeouts=[5, 1],
                                         watch_checkpoint=[zone, 'asset never-emitted']))
        self.assertEqual(fixture.now, 3)
        self.assertEqual(result['outcome'], 'checkpoint-stalled')
        checks = result['checkpoints']
        self.assertEqual(checks['waiting_for'], ready)
        self.assertEqual(checks['waiting_for_timeout_seconds'], 1)
        self.assertEqual(checks['watched'], {zone: {'record': latest, 'source': 'live-loader'},
                                            'asset never-emitted': None})

    def test_checkpoint_value_wildcard_matches_one_nonempty_field(self):
        pattern = 'asset menu-zone-flags=* detail=0x3'
        self.assertTrue(diagnostics.valid_checkpoint_pattern(pattern))
        self.assertFalse(diagnostics.valid_checkpoint_pattern('asset menu-*=0x3'))
        self.assertFalse(diagnostics.valid_checkpoint_pattern('asset menu-zone-flags=**'))
        self.assertTrue(diagnostics.checkpoint_matches('asset menu-zone-flags=0xC02 detail=0x3 extra=1', pattern))
        self.assertFalse(diagnostics.checkpoint_matches('asset menu-zone-flags= detail=0x3', pattern))
        self.assertFalse(diagnostics.checkpoint_matches('asset menu-zone-flags=0xC02 extra=1 detail=0x3', pattern))

    def test_checkpoint_received_after_its_deadline_does_not_pass(self):
        launch = EPOCH_MS + 1
        gpu, ready = 'dxgi gpu-frame-submitted', 'asset menu-zone-ready'
        report = probe(launch, extra=VERIFIED_STAGE + RENDER_FIELDS)

        class SlowTrace(Fixture):
            def read(self, portal, app, folder, name, limit):
                raw = super().read(portal, app, folder, name, limit)
                if (folder, name) == LIVE_TRACE and self.step == 1:
                    self.now += 3
                return raw

        fixture = SlowTrace([
            {diagnostics.PROBE_REPORT: report, LIVE_TRACE: f'{gpu}\r\n'.encode()},
            {diagnostics.PROBE_REPORT: report, LIVE_TRACE: f'{gpu}\r\n{ready}\r\n'.encode()},
        ], stages=[[stage(LIVE_STAGE, launch)]])
        result = fixture.observe(options(run_id=launch, require_checkpoint=[gpu, ready]))
        self.assertEqual(result['outcome'], 'checkpoint-stalled')
        self.assertEqual(result['checkpoints']['reached'], [gpu])

    def test_attached_checkpoint_contract_requires_order_and_resets_budget_on_progress(self):
        launch = EPOCH_MS + 1
        boot, gpu, menu = 'asset startup-phase=boot', 'asset startup-phase=gpu', 'asset startup-phase=menu'
        report = probe(launch, extra=VERIFIED_STAGE + RENDER_FIELDS)
        records = [line.encode() + b'\r\n' for line in (boot, gpu, menu)]
        fixture = Fixture([
            {diagnostics.PROBE_REPORT: report, LIVE_TRACE: records[2] + records[0]},
            {diagnostics.PROBE_REPORT: report, LIVE_TRACE: records[2] + records[0] + records[1]},
            {diagnostics.PROBE_REPORT: report, LIVE_TRACE: records[2] + b''.join(records)},
        ], stages=[[stage(LIVE_STAGE, launch)]])
        result = fixture.observe(options(run_id=launch, require_checkpoint=[boot, gpu, menu]))
        self.assertEqual(fixture.now, 2)
        self.assertEqual(result['outcome'], 'checkpoints-observed')
        self.assertEqual(result['checkpoints']['reached'], [boot, gpu, menu])

    def test_checkpoint_failure_captures_the_record_before_the_process_exits(self):
        launch = EPOCH_MS + 1
        gpu = 'dxgi gpu-frame-submitted'
        removed = 'dxgi gpu-frame-error result=0x887A0005'
        running = VERIFIED_STAGE + 'steam_shadow_child=created\r\n'
        fixture = Fixture([
            {diagnostics.PROBE_REPORT: probe()},
            {diagnostics.PROBE_REPORT: probe(launch, extra=running),
             LIVE_TRACE: (removed + '\r\n').encode()},
        ], stages=[[], [stage(LIVE_STAGE, launch)]])
        result = fixture.observe(options(require_checkpoint=[gpu], fail_on_checkpoint=[removed]))
        self.assertEqual(fixture.now, 1)
        self.assertEqual(result['outcome'], 'checkpoint-failed')
        self.assertEqual(result['checkpoints']['failure'], removed)

    def test_checkpoint_failure_is_reported_when_it_first_appears_in_the_child_exit_report(self):
        launch = EPOCH_MS + 1
        gpu = 'dxgi gpu-frame-submitted'
        removed = 'dxgi gpu-frame-error result=0x887A0005'
        exit_file = f'steam-win32-shadow-{APP_ID}-exit-2274.txt'
        running = probe(launch, extra=f'steam_shadow_exit_report={exit_file}\r\n')
        exited = f'steam_shadow_child_status=exited\r\nsteam_shadow_loader={removed}\r\n'.encode()
        fixture = Fixture([{diagnostics.PROBE_REPORT: running, exit_file: exited}])
        result = fixture.observe(options(run_id=launch, require_checkpoint=[gpu], fail_on_checkpoint=[removed]))
        self.assertEqual(result['outcome'], 'checkpoint-failed')
        self.assertEqual(result['checkpoints']['failure'], removed)
        self.assertEqual(result['checkpoints']['failure_source'], 'exit-report')

    def test_checkpoint_contract_does_not_read_an_ambiguous_staging_folder(self):
        launch = EPOCH_MS + 1
        gpu = 'dxgi gpu-frame-submitted'
        fixture = Fixture([{
            diagnostics.PROBE_REPORT: probe(launch, extra=VERIFIED_STAGE + RENDER_FIELDS),
            LIVE_TRACE: (gpu + '\r\n').encode(),
        }], stages=[[stage(LIVE_STAGE, launch), stage('SteamWin32Trial (39)', launch + 1)]])
        result = fixture.observe(options(run_id=launch, require_checkpoint=[gpu]))
        self.assertEqual(result['outcome'], 'checkpoint-stalled')
        self.assertEqual(result['checkpoints']['source'], 'unavailable')
        self.assertNotIn((*LIVE_TRACE, diagnostics.LIVE_LOADER_LIMIT + 1), fixture.reads)

    def test_checkpoint_contract_rechecks_the_unique_staging_folder_after_read(self):
        launch = EPOCH_MS + 1
        gpu = 'dxgi gpu-frame-submitted'

        class StageRace(Fixture):
            def read(self, portal, app, folder, name, limit):
                raw = super().read(portal, app, folder, name, limit)
                if (folder, name) == LIVE_TRACE and len(self.stages[0]) == 1:
                    self.stages[0].append(stage('SteamWin32Trial (39)', launch + 1))
                return raw

        fixture = StageRace([{
            diagnostics.PROBE_REPORT: probe(launch, extra=VERIFIED_STAGE + RENDER_FIELDS),
            LIVE_TRACE: (gpu + '\r\n').encode(),
        }], stages=[[stage(LIVE_STAGE, launch)]])
        result = fixture.observe(options(run_id=launch, require_checkpoint=[gpu]))
        self.assertEqual(result['outcome'], 'checkpoint-stalled')
        self.assertEqual(result['checkpoints']['records'], [])
        self.assertIn((*LIVE_TRACE, diagnostics.LIVE_LOADER_LIMIT + 1), fixture.reads)

    def test_game_observation_waits_for_the_named_exit_report_to_append_its_trace(self):
        launch = EPOCH_MS + 1
        exit_file = f'steam-win32-shadow-{APP_ID}-exit-2274.txt'
        running = probe(launch, extra=f'steam_shadow_exit_report={exit_file}\r\n')
        exited = b'steam_shadow_child_status=exited\r\nsteam_shadow_child_exit_u32=1\r\n'
        fault = b'fault access-violation module=game.exe rva=0x00123456 read near-null'
        terminal = exited + b'steam_shadow_loader=' + fault + b'\r\nsteam_shadow_loader_trace=collected\r\n'
        fixture = Fixture([
            {diagnostics.PROBE_REPORT: running, exit_file: exited},
            {diagnostics.PROBE_REPORT: running, exit_file: PortalError(409)},
            {diagnostics.PROBE_REPORT: running, exit_file: terminal},
        ])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'observation'
            result = fixture.observe(options(run_id=launch, output=output))
            self.assertEqual(result['outcome'], 'game-exited')
            self.assertEqual(result['probe']['steam_shadow_child_exit_u32'], '1')
            self.assertEqual(fixture.now, 2)
            self.assertEqual((output / 'loader-trace.txt').read_bytes(), fault + b'\n')

    def test_game_observation_exit_trace_wait_respects_the_existing_deadline(self):
        launch = EPOCH_MS + 1
        exit_file = f'steam-win32-shadow-{APP_ID}-exit-2274.txt'
        fixture = Fixture([{
            diagnostics.PROBE_REPORT: probe(launch, extra=f'steam_shadow_exit_report={exit_file}\r\n'),
            exit_file: b'steam_shadow_child_status=exited\r\nsteam_shadow_child_exit_u32=1\r\n',
        }])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'observation'
            result = fixture.observe(options(run_id=launch, timeout=1, output=output))
            self.assertEqual(result['outcome'], 'game-exited')
            self.assertEqual(fixture.now, 1)
            self.assertFalse((output / 'loader-trace.txt').exists())

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

    def test_render_inspection_records_gpu_metadata_and_only_complete_captures(self):
        launch = EPOCH_MS + 1
        fixture = Fixture([{
            diagnostics.PROBE_REPORT: probe(launch, extra=RENDER_FIELDS),
            MAILBOX + '.gpu': gpu_mailbox(),
            'sdl-startup-2274.bmp': bmp(bits=24),
            'sdl-startup-2274-late.bmp': bmp()[:-1],
            'sdl-startup-2274-requested.bmp': bmp(),
            'sdl-startup-2274.txt': (b'win32_virtual_activation=active\r\nwin32_window=path/to/private\r\n'
                                     b'win32_window=part'),
        }])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'render'
            result = fixture.inspect_render(options(run_id=launch, output=output))
            self.assertEqual(result['app_id'], APP_ID)
            self.assertEqual(result['run_id'], launch)
            self.assertEqual(result['gpu_mailbox'], {
                'state': 'published', 'width': 1920, 'height': 1080, 'dxgi_format': 28, 'flip_y': False,
                'generation': 2, 'accepted_generation': 2, 'gpu_exchange_active': True, 'producer_sequence': 25,
            })
            self.assertEqual(result['screenshots'], {
                'bootstrap.bmp': {'state': 'captured', 'width': 2, 'height': 1},
                'bootstrap-late.bmp': {'state': 'invalid'},
                'requested.bmp': {'state': 'captured', 'width': 2, 'height': 1},
            })
            self.assertEqual((output / 'bootstrap.bmp').read_bytes(), bmp(bits=24))
            self.assertFalse((output / 'bootstrap-late.bmp').exists())
            self.assertEqual((output / 'requested.bmp').read_bytes(), bmp())
            self.assertEqual((output / 'sdl-startup.txt').read_text(), 'win32_virtual_activation=active\n')
            self.assertEqual(json.loads((output / 'observation.json').read_text()), result)
            self.assertNotIn('do-not-save', json.dumps(result))
            self.assertNotIn(MAILBOX, json.dumps(result))

    def test_render_inspection_rejects_a_different_launch_before_reading_artifacts(self):
        for report in (probe(EPOCH_MS, extra=RENDER_FIELDS), probe(EPOCH_MS + 1, APP_ID + 1, extra=RENDER_FIELDS)):
            fixture = Fixture([{diagnostics.PROBE_REPORT: report}])
            with self.assertRaisesRegex(ValueError, 'does not match'):
                fixture.inspect_render(options(run_id=EPOCH_MS + 1))
            self.assertEqual(fixture.reads, [(diagnostics.LOCAL, diagnostics.PROBE_REPORT, diagnostics.REPORT_LIMIT + 1)])

    def test_render_inspection_discards_artifacts_when_the_launch_changes_during_read(self):
        launch = EPOCH_MS + 1

        class LaunchChangesDuringRead(Fixture):
            def read(self, portal, app, folder, name, limit):
                data = super().read(portal, app, folder, name, limit)
                if name == 'sdl-startup-2274-late.bmp':
                    self.snapshots[0][diagnostics.PROBE_REPORT] = probe(launch + 1)
                return data

        fixture = LaunchChangesDuringRead([{
            diagnostics.PROBE_REPORT: probe(launch, extra=RENDER_FIELDS),
            'sdl-startup-2274-late.bmp': bmp(),
        }])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'render'
            with self.assertRaisesRegex(ValueError, 'launch changed'):
                fixture.inspect_render(options(run_id=launch, output=output))
            self.assertFalse(output.exists())

    def test_render_inspection_detects_torn_gpu_generations_without_requesting_screenshots(self):
        launch = EPOCH_MS + 1

        class GPUChangesDuringRead(Fixture):
            def read(self, portal, app, folder, name, limit):
                data = super().read(portal, app, folder, name, limit)
                if name == MAILBOX + '.gpu':
                    self.snapshots[0][name] = gpu_mailbox(generation=4)
                return data

        fixture = GPUChangesDuringRead([{
            diagnostics.PROBE_REPORT: probe(launch, extra=RENDER_FIELDS),
            MAILBOX + '.gpu': gpu_mailbox(),
        }])
        result = fixture.inspect_render(options(run_id=launch))
        self.assertEqual(result['gpu_mailbox'], {'state': 'updating'})
        self.assertFalse(any(name.endswith('.bmp') for _, name, _ in fixture.reads))

    def test_render_inspection_ignores_invalid_mailbox_names_and_binary_layouts(self):
        launch = EPOCH_MS + 1
        fixture = Fixture([{
            diagnostics.PROBE_REPORT: probe(launch, extra='steam_sdl_mailbox=..\\account.txt\r\n'),
        }])
        self.assertEqual(fixture.inspect_render(options(run_id=launch))['gpu_mailbox'], {'state': 'invalid'})
        self.assertTrue(all(name == diagnostics.PROBE_REPORT for _, name, _ in fixture.reads))
        for bad in (gpu_mailbox()[:-1], gpu_mailbox() + b'x', gpu_mailbox(width=0), gpu_mailbox(format_code=0)):
            self.assertEqual(diagnostics.gpu_mailbox_snapshot(bad), {'state': 'invalid'})
        self.assertEqual(diagnostics.gpu_mailbox_snapshot(gpu_mailbox(generation=1)), {'state': 'updating'})
        self.assertEqual(diagnostics.gpu_mailbox_snapshot(gpu_mailbox(generation=0)), {'state': 'inactive'})

    def test_live_loader_inspection_preserves_repeated_events_from_the_verified_copy(self):
        launch = EPOCH_MS + 1
        past = [stage(f'SteamWin32Trial ({number})', launch - 1 - number) for number in range(1, 38)]
        traced = (b'asset graphics-pso-before flags=0x00000040\r\n' * 2
                  + b'asset boot-wait-single timeout=0xFFFFFFFF enter\r\n'
                  + b'asset shell-request result=0x00000000\r\n')
        fixture = Fixture([{
            diagnostics.PROBE_REPORT: probe(launch, extra=VERIFIED_STAGE),
            LIVE_TRACE: traced + b'load C:\\Users\\private\\sample.dll\r\nasset boot-wait-split',
        }], stages=[[stage(LIVE_STAGE, launch + 882), *past]])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'loader'
            result = fixture.inspect_loader(options(run_id=launch, output=output))
            self.assertEqual(result['app_id'], APP_ID)
            self.assertEqual(result['run_id'], launch)
            self.assertEqual(result['loader_trace']['state'], 'observed')
            self.assertEqual(result['loader_trace']['records'], 4)
            self.assertEqual(result['loader_trace']['events']['graphics-pso-before']['count'], 2)
            self.assertEqual(result['loader_trace']['events']['boot-wait']['count'], 1)
            self.assertEqual(result['loader_trace']['events']['shell-request']['latest'],
                             ['asset shell-request result=0x00000000'])
            self.assertEqual((output / 'loader-trace.txt').read_bytes(), traced.replace(b'\r\n', b'\n'))
            self.assertEqual(json.loads((output / 'observation.json').read_text()), result)
            self.assertNotIn('private', json.dumps(result))
        self.assertEqual(fixture.reads, [(diagnostics.LOCAL, diagnostics.PROBE_REPORT, diagnostics.REPORT_LIMIT + 1),
                                         (*LIVE_TRACE, diagnostics.LIVE_LOADER_LIMIT + 1),
                                         (diagnostics.LOCAL, diagnostics.PROBE_REPORT, diagnostics.REPORT_LIMIT + 1)])

    def test_live_loader_inspection_rejects_stale_or_ambiguous_staging_directories(self):
        launch = EPOCH_MS + 1
        stale = [stage(LIVE_STAGE, launch - 1),
                 stage('SteamWin32Trial (39)', launch + diagnostics.TRIAL_CREATION_WINDOW_MS + 1)]
        ambiguous = [stage(LIVE_STAGE, launch), stage('SteamWin32Trial (39)', launch + 2)]
        for items, count in ((stale, 0), (ambiguous, 2)):
            fixture = Fixture([{diagnostics.PROBE_REPORT: probe(launch, extra=VERIFIED_STAGE)}], stages=[items])
            with self.assertRaisesRegex(ValueError, f'Found {count}'):
                fixture.inspect_loader(options(run_id=launch))
            self.assertTrue(all(name == diagnostics.PROBE_REPORT for _, name, _ in fixture.reads))

    def test_loader_inspection_reads_the_associated_exit_report_after_cleanup(self):
        launch = EPOCH_MS + 1
        exit_file = f'steam-win32-shadow-{APP_ID}-exit-2274.txt'
        exit_marker = f'steam_shadow_exit_report={exit_file}\r\n'
        fault = 'fault access-violation module=Game.exe rva=0x000AB123 read near-null'
        fixture = Fixture([{
            diagnostics.PROBE_REPORT: probe(launch, extra=VERIFIED_STAGE + exit_marker),
            exit_file: (f'steam_shadow_child_status=exited\r\nsteam_shadow_child_exit_u32=4294930433\r\n'
                        f'steam_shadow_loader={fault}\r\nsteam_shadow_loader={fault}\r\n'
                        'steam_shadow_loader=load C:\\Users\\private\\other.dll\r\n'
                        'steam_shadow_loader_trace=collected\r\n').encode(),
        }])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'loader'
            result = fixture.inspect_loader(options(run_id=launch, output=output))
            self.assertEqual(result['process_exit'], '0xFFFF7001')
            self.assertEqual(result['loader_trace']['source'], 'exit-report')
            self.assertEqual(result['loader_trace']['records'], 2)
            self.assertEqual((output / 'loader-trace.txt').read_text(), (fault + '\n') * 2)
        self.assertEqual([name for _, name, _ in fixture.reads],
                         [diagnostics.PROBE_REPORT, exit_file, diagnostics.PROBE_REPORT])

    def test_loader_inspection_rejects_exit_report_that_changes_during_capture(self):
        launch = EPOCH_MS + 1
        exit_file = f'steam-win32-shadow-{APP_ID}-exit-2274.txt'
        exit_marker = f'steam_shadow_exit_report={exit_file}\r\n'

        class ExitReportChanges(Fixture):
            def read(self, portal, app, folder, name, limit):
                data = super().read(portal, app, folder, name, limit)
                if name == exit_file:
                    self.snapshots[0][diagnostics.PROBE_REPORT] = probe(launch, extra=VERIFIED_STAGE +
                                                                      exit_marker.replace('2274', '2275'))
                return data

        fixture = ExitReportChanges([{
            diagnostics.PROBE_REPORT: probe(launch, extra=VERIFIED_STAGE + exit_marker),
            exit_file: b'steam_shadow_child_status=exited\r\nsteam_shadow_loader_trace=no-output\r\n',
        }])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'loader'
            with self.assertRaisesRegex(ValueError, 'changed during capture'):
                fixture.inspect_loader(options(run_id=launch, output=output))
            self.assertFalse(output.exists())

    def test_loader_inspection_requires_a_terminal_exit_trace_and_matching_hosted_run(self):
        launch = EPOCH_MS + 1
        exit_file = f'steam-win32-shadow-{APP_ID}-exit-2274.txt'
        pointer = VERIFIED_STAGE + f'steam_shadow_exit_report={exit_file}\r\n'
        for extra, expected in ((pointer, 'still pending'),
                                (pointer + 'steam_launch_sdl_run=2275\r\n', 'does not match the hosted launch')):
            fixture = Fixture([{
                diagnostics.PROBE_REPORT: probe(launch, extra=extra),
                exit_file: (b'steam_shadow_child_status=exited\r\n'
                            b'steam_shadow_loader=asset a-complete-record\r\n'),
            }])
            with self.assertRaisesRegex(ValueError, expected):
                fixture.inspect_loader(options(run_id=launch))

    def test_loader_inspection_rejects_exit_fallback_when_a_matching_stage_appears(self):
        launch = EPOCH_MS + 1
        exit_file = f'steam-win32-shadow-{APP_ID}-exit-2274.txt'

        class StageAppears(Fixture):
            def read(self, portal, app, folder, name, limit):
                data = super().read(portal, app, folder, name, limit)
                if name == exit_file:
                    self.stages[0] = [stage(LIVE_STAGE, launch)]
                return data

        fixture = StageAppears([{
            diagnostics.PROBE_REPORT: probe(launch, extra=VERIFIED_STAGE +
                                            f'steam_shadow_exit_report={exit_file}\r\n'),
            exit_file: b'steam_shadow_child_status=exited\r\nsteam_shadow_loader_trace=no-output\r\n',
        }], stages=[[]])
        with self.assertRaisesRegex(ValueError, 'staging directory changed'):
            fixture.inspect_loader(options(run_id=launch))

    def test_live_loader_reports_pipeline_request_ordinals_commit_and_failures(self):
        summary, _ = diagnostics.live_loader_snapshot(
            b'asset graphics-pso-before bytes=0x0000000000001000 flags=0x00000080 available-commit-kib=0x00000100\n'
            b'asset graphics-pso-before bytes=0x0000000000001000 flags=0x00000040 available-commit-kib=0x00000080\n'
            b'asset compute-pso-before bytes=0x0000000000000020 flags=0x00000002 available-commit-kib=0x00000080\n'
            b'asset graphics-pso-after bytes=0x0000000000001000 flags=0x8007000E available-commit-kib=0x00000040\n'
            b'asset graphics-pso-after bytes=0x0000000000001000 flags=0x00000000 available-commit-kib=0x00000060\n'
            b'asset compute-pso-after bytes=0x0000000000000020 flags=0x00000000 available-commit-kib=0x00000070\n'
            b'asset live-pipeline-graphics bytes=0x0000000000000090 flags=0x00001392 available-commit-kib=0x00000077\n'
            b'asset live-pipeline-graphics bytes=0x0000000000000099 flags=0x00000000 available-commit-kib=0x00000081\n')
        self.assertEqual(summary['pipelines'], {
            'graphics': {'requests_at_least': 128, 'latest_sampled_commit_kib': 96,
                         'latest_failure_hresult': '0x8007000E',
                         'latest_counter_sample': {'requests': 153, 'compiler_active': False,
                                                   'available_commit_kib': 129}},
            'compute': {'requests_at_least': 2, 'latest_sampled_commit_kib': 112},
        })

    def test_live_loader_inspection_requires_the_selected_run_and_a_verified_stage(self):
        launch = EPOCH_MS + 1
        for report in (probe(launch, APP_ID + 1, extra=VERIFIED_STAGE), probe(launch + 1, extra=VERIFIED_STAGE),
                       probe(launch)):
            fixture = Fixture([{diagnostics.PROBE_REPORT: report}], stages=[[stage(LIVE_STAGE, launch)]])
            with self.assertRaisesRegex(ValueError, 'does not (?:match|report)'):
                fixture.inspect_loader(options(run_id=launch))
            self.assertEqual(len(fixture.reads), 1)

    def test_live_loader_inspection_discards_data_when_the_report_or_stage_changes(self):
        launch = EPOCH_MS + 1

        class CaptureChanges(Fixture):
            def read(self, portal, app, folder, name, limit):
                data = super().read(portal, app, folder, name, limit)
                if (folder, name) == LIVE_TRACE:
                    self.pause(1)
                return data

        original_report = probe(launch, extra=VERIFIED_STAGE)
        original_stage = [stage(LIVE_STAGE, launch)]
        for report_after, stage_after, message in (
            (probe(launch, run=EPOCH_MS + 2, extra=VERIFIED_STAGE), original_stage, 'launch changed'),
            (original_report, [stage(LIVE_STAGE, launch + 1)], 'staging directory changed'),
            (original_report, original_stage + [stage('SteamWin32Trial (39)', launch)], 'staging directory changed'),
        ):
            fixture = CaptureChanges([
                {diagnostics.PROBE_REPORT: original_report, LIVE_TRACE: b'load example.dll ok\n'},
                {diagnostics.PROBE_REPORT: report_after},
            ], stages=[original_stage, stage_after])
            with tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / 'loader'
                with self.assertRaisesRegex(ValueError, message):
                    fixture.inspect_loader(options(run_id=launch, output=output))
                self.assertFalse(output.exists())

    def test_live_loader_inspection_caps_the_read_and_skips_an_incomplete_final_record(self):
        first = b'asset shell-request-enter\n'
        raw = first + b' ' * (diagnostics.LIVE_LOADER_LIMIT - len(first)) + b'\nasset shell-request-exit\n'
        summary, records = diagnostics.live_loader_snapshot(raw)
        self.assertTrue(summary['truncated'])
        self.assertEqual(summary['bytes_read'], diagnostics.LIVE_LOADER_LIMIT)
        self.assertEqual(summary['events']['shell-request']['count'], 1)
        self.assertEqual(records, ['asset shell-request-enter'])

    def test_export_game_inputs_publishes_only_after_hash_and_manifest_checks(self):
        manifest = (b'"AppState" { "appid" "42" "buildid" "123" "StateFlags" "4" '
                    b""" "installdir" "Example's Game" "InstalledDepots" { "12" { "buildid" "456" } } }""")
        payload = b'The user-owned fixture payload' * 2048
        requests = []

        class Portal:
            def __init__(self, file, final_manifest=manifest):
                self.file, self.final_manifest = file, final_manifest

            def request(self, method, path, **kwargs):
                self_query = parse_qs(urlsplit(path).query)
                requests.append((method, self_query))
                if self_query['filename'] == ['game.exe']:
                    assert 'receive' in kwargs
                    return 200, kwargs['receive'](io.BytesIO(self.file))
                assert self_query['filename'] == ['appmanifest_42.acf']
                return 200, self.final_manifest

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            profile = root / 'source.json'
            profile.write_text(json.dumps({'123': [{'path': 'x64/game.exe', 'size': len(payload),
                                                    'sha256': hashlib.sha256(payload).hexdigest()}]}))
            output = root / 'good'
            inputs = (APP, APP_ID, 'DevelopmentFiles', cli.DEVELOPER_STEAMAPPS_PATH, manifest, profile)
            with redirect_stdout(io.StringIO()):
                cli.export_game_inputs(Portal(payload), *inputs, output)
            self.assertEqual((output / 'x64/game.exe').read_bytes(), payload)
            self.assertEqual(requests[0][1]['path'], [cli.DEVELOPER_STEAMAPPS_PATH + r"\common\Example's Game\x64"])
            self.assertTrue(all(method == 'GET' for method, _ in requests))
            for bad_portal, message in (
                (Portal(payload[:-1]), 'Incomplete Xbox file'),
                (Portal(b'X' + payload[1:]), 'fingerprint mismatch'),
                (Portal(payload + b'x'), 'fingerprint mismatch'),
                (Portal(payload, manifest.replace(b'"123"', b'"124"')), 'installation changed'),
            ):
                bad = root / 'bad'
                with self.assertRaisesRegex(ValueError, message):
                    cli.export_game_inputs(bad_portal, *inputs, bad)
                self.assertFalse(bad.exists())

    def test_standard_cli_reuses_pinned_setup_and_get_only_requests(self):
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
                query = parse_qs(urlsplit(path).query)
                assert query['packagefullname'] == [APP['PackageFullName']]
                if path.startswith('/api/filesystem/apps/files?'):
                    assert query['path'] == [diagnostics.TEMP]
                    return 200, json.dumps({'Items': [stage(LIVE_STAGE, launch + 1)]}).encode()
                name = query['filename'][0]
                if name == 'loader-trace.txt':
                    assert query['path'] == [LIVE_TRACE[0]]
                    assert kwargs['read_limit'] == diagnostics.LIVE_LOADER_LIMIT + 1
                    return 200, b'asset boot-wait-single enter\r\n'
                assert query['path'] == [diagnostics.LOCAL]
                if name == diagnostics.PROBE_REPORT:
                    return 200, probe(launch, extra=RENDER_FIELDS + VERIFIED_STAGE)
                if name == MAILBOX:
                    ranges = {
                        'bytes=0-35': struct.pack('<9I', 0x53444C31, 2, 99, 0, 0, 2, 2, 1, 0),
                        'bytes=8294456-8294495': struct.pack('<II6iII', 4, 0x10001, 1, -2, 3, -4, -32768, 32767, 2, 1),
                        'bytes=20-23': struct.pack('<I', 2),
                        'bytes=8294456-8294459': struct.pack('<I', 4),
                    }
                    requested = kwargs['headers']['Range']
                    raw = ranges[requested]
                    header = 'bytes ' + requested.removeprefix('bytes=') + '/8303680'
                    response = SimpleNamespace(getheader=lambda key, default: header, read=lambda limit: raw[:limit])
                    return 206, kwargs['receive'](response)
                if name == MAILBOX + '.gpu':
                    assert kwargs['read_limit'] == diagnostics.GPU_MAILBOX_SIZE + 1
                    return 200, gpu_mailbox()
                if name in ('sdl-startup-2274.bmp', 'sdl-startup-2274-late.bmp'):
                    assert kwargs['read_limit'] == diagnostics.BMP_LIMIT + 1
                    return 200, bmp()
                return 200, b''

        command = ['xbox_deploy.py', 'observe', '--version', '1.2.3.4', '--app-id', str(APP_ID),
                   '--run-id', str(launch), '--frame-seconds', '0']
        settings = dict(XBOX_PORTAL_URL='https://xbox.invalid', XBOX_PORTAL_USERNAME='test_user',
                        XBOX_PORTAL_PASSWORD='test_secret', XBOX_PORTAL_CERT_SHA256=fingerprint)
        output, errors = io.StringIO(), io.StringIO()
        with (patch.object(cli, 'DevicePortal', Portal),
              patch.object(cli, 'load_settings', return_value=settings),
              patch('sys.argv', command), redirect_stdout(output), redirect_stderr(errors)):
            cli.main()
        self.assertEqual(json.loads(output.getvalue())['outcome'], 'frame-observed')
        self.assertEqual(errors.getvalue().strip(), 'attaching-to-launch')
        self.assertEqual(requests[0], None)
        self.assertTrue(all(method == 'GET' for method in requests[1:]))
        self.assertNotIn('test_secret', output.getvalue() + errors.getvalue())

        requests.clear()
        output, errors = io.StringIO(), io.StringIO()
        command = ['xbox_deploy.py', 'inspect-input', '--version', '1.2.3.4', '--app-id', str(APP_ID),
                   '--run-id', str(launch)]
        with (patch.object(cli, 'DevicePortal', Portal),
              patch.object(cli, 'load_settings', return_value=settings),
              patch('sys.argv', command), redirect_stdout(output), redirect_stderr(errors)):
            cli.main()
        result = json.loads(output.getvalue())
        self.assertEqual(requests, [None] + ['GET'] * 7)
        self.assertEqual(result['activity']['gamepad'], 'insufficient-samples')
        self.assertEqual(result['samples'][0]['gamepad'], {
            'state': 'unacknowledged', 'serial': 4, 'ack': 2, 'connected': True,
            'buttons': '0x00010001', 'axes': [1, -2, 3, -4, -32768, 32767],
        })
        self.assertEqual(result['samples'][0]['pointer']['state'], 'acknowledged')
        self.assertTrue(result['samples'][0]['keyboard_ready'])
        self.assertNotIn('test_secret', output.getvalue() + errors.getvalue())

        requests.clear()
        output, errors = io.StringIO(), io.StringIO()
        command = ['xbox_deploy.py', 'inspect-loader', '--version', '1.2.3.4', '--app-id', str(APP_ID),
                   '--run-id', str(launch)]
        with (patch.object(cli, 'DevicePortal', Portal),
              patch.object(cli, 'load_settings', return_value=settings),
              patch('sys.argv', command), redirect_stdout(output), redirect_stderr(errors)):
            cli.main()
        self.assertEqual(requests, [None, 'GET', 'GET', 'GET', 'GET', 'GET', 'GET'])
        self.assertEqual(json.loads(output.getvalue())['loader_trace']['events']['boot-wait']['count'], 1)
        self.assertNotIn('test_secret', output.getvalue() + errors.getvalue())
        requests.clear()
        output, errors = io.StringIO(), io.StringIO()
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / 'render'
            command = ['xbox_deploy.py', 'inspect-render', '--version', '1.2.3.4', '--app-id', str(APP_ID),
                       '--run-id', str(launch), '--output', str(destination)]
            with (patch.object(cli, 'DevicePortal', Portal),
                  patch.object(cli, 'load_settings', return_value=settings),
                  patch('sys.argv', command), redirect_stdout(output), redirect_stderr(errors)):
                cli.main()
            result = json.loads(output.getvalue())
            self.assertEqual(result['gpu_mailbox']['state'], 'published')
            self.assertEqual(result['gpu_mailbox']['producer_sequence'], 25)
            self.assertEqual((destination / 'bootstrap-late.bmp').read_bytes(), bmp())
            self.assertEqual((destination / 'bootstrap.bmp').read_bytes(), bmp())
            self.assertEqual(json.loads((destination / 'observation.json').read_text()), result)
        self.assertEqual(requests[0], None)
        self.assertTrue(all(method == 'GET' for method in requests[1:]))
        self.assertNotIn('test_secret', output.getvalue() + errors.getvalue())

    def test_host_input_sampling_distinguishes_activity_pending_ack_and_menu(self):
        launch = EPOCH_MS + 1

        class Portal:
            now = 0
            bad_range = False
            wrong_offset = False
            changes_launch = False

            def pause(self, seconds):
                self.now += seconds

            def request(self, method, path, **kwargs):
                assert method == 'GET'
                query = parse_qs(urlsplit(path).query)
                assert query['packagefullname'] == [APP['PackageFullName']]
                name = query['filename'][0]
                if name == diagnostics.PROBE_REPORT:
                    run = launch + 1 if self.changes_launch and self.now else launch
                    return 200, probe(run, extra=RENDER_FIELDS)
                if name == 'fhd-panel-input.txt':
                    markers = b'menu_chord=held\r\nmenu_request=ready\r\nmenu_request=rea' if self.now else b''
                    return 200, b'panel_input=ready\r\naccount_token=do-not-save\r\n' + markers
                assert name == MAILBOX
                step = round(self.now * 2)
                serial, ack, confirmed = ((2, 2, 2), (4, 2, 4), (6, 6, 6), (8, 6, 9))[step]
                data = {
                    'bytes=0-35': struct.pack('<9I', 0x53444C31, 0, 0, 0, 0, 0, 0, 0, 0),
                    'bytes=8294456-8294495': struct.pack('<II6iII', serial, 0x10000, 0, 0, 0, 0, -32768, -32768, ack, 1),
                    'bytes=20-23': bytes(4),
                    'bytes=8294456-8294459': struct.pack('<I', confirmed),
                }[kwargs['headers']['Range']]
                offset = kwargs['headers']['Range'].removeprefix('bytes=')
                header = ('bytes 2-37/' if self.wrong_offset else f'bytes {offset}/') + '8303680'

                def read(limit):
                    assert limit == len(data) + 1
                    return data

                response = SimpleNamespace(getheader=lambda key, default: header, read=read)
                return (200 if self.bad_range else 206), kwargs['receive'](response)

        args = options(run_id=launch, seconds=1.5, panel_menu=True)
        fake = Portal()
        result = cli.inspect_host_input(args, fake, APP, clock=lambda: fake.now, pause=fake.pause)
        self.assertEqual(result['activity'], {'gamepad': 'host-activity', 'pointer': 'host-idle'})
        self.assertEqual([sample['gamepad']['state'] for sample in result['samples']],
                         ['acknowledged', 'unacknowledged', 'acknowledged', 'updating'])
        self.assertNotIn('buttons', result['samples'][-1]['gamepad'])
        self.assertEqual(result['panel_menu']['counts'], {'panel_ready': 1, 'chord_held': 1, 'menu_request': 1})
        self.assertEqual(result['panel_menu']['added'], {'panel_ready': 0, 'chord_held': 1, 'menu_request': 1})
        self.assertNotIn('do-not-save', json.dumps(result))
        self.assertNotIn(MAILBOX, json.dumps(result))
        fake = Portal()
        fake.bad_range = True
        with self.assertRaisesRegex(ValueError, 'exact requested SDL mailbox range'):
            cli.inspect_host_input(args, fake, APP, clock=lambda: fake.now, pause=fake.pause)
        fake = Portal()
        fake.wrong_offset = True
        with self.assertRaisesRegex(ValueError, 'exact requested SDL mailbox range'):
            cli.inspect_host_input(args, fake, APP, clock=lambda: fake.now, pause=fake.pause)
        fake = Portal()
        fake.changes_launch = True
        with tempfile.TemporaryDirectory() as directory:
            args.output = Path(directory) / 'input'
            with self.assertRaisesRegex(ValueError, 'changed during input sampling'):
                cli.inspect_host_input(args, fake, APP, clock=lambda: fake.now, pause=fake.pause)
            self.assertFalse(args.output.exists())
