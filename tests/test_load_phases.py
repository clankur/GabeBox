import json
import runpy
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

phases = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'tools/load_phases.py'))

HZ = 1000
LAUNCH = datetime(2026, 11, 28, 4, 33, 23, tzinfo=timezone.utc).timestamp()
ANCHOR = 10_000_000  # presenter anchor tick at LAUNCH


def tick(seconds):
    return ANCHOR + round(seconds * HZ)


def presents(times):
    filetime = round(LAUNCH * 10_000_000) + phases['FILETIME_EPOCH']
    rows = b''.join(phases['PRESENT'].pack(tick(t), index, index, 2) for index, t in enumerate(times, 1))
    return phases['HEADER'].pack(0x31444846, 2, HZ, filetime, ANCHOR) + rows


def utc(seconds):
    return datetime.fromtimestamp(LAUNCH + seconds, timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


class LoadPhaseTests(unittest.TestCase):
    def run_folder(self, directory, *, probe=True):
        folder = Path(directory)
        (folder / 'launch.utc').write_text(utc(0) + '\n')
        (folder / 'continue.utc').write_text(utc(100) + '\n')
        if probe:
            (folder / 'probe-report.txt').write_text(
                f'run_id={round(LAUNCH * 1000)}\r\nsteam_launch_run_id={round((LAUNCH + 27.5) * 1000)}\r\n')
        # Console local time is UTC-8; the game starts 31.2 s after launch (04:33:54.200 UTC).
        (folder / 'game.log').write_text(
            '20:33:54:200 (00005004) > [Startup] Marvel\'s Spider-Man Remastered\n'
            '20:33:54:600 (00005004) > [Render] File \\xbox0.pso contains 2322 pipelines and 0 duplicates\n')
        trace = [
            'asset pso-parallel slots=4 boost=2 boost-min-mib=768 priority=1',
            f'asset pso-slow kind=graphics n=1 wait-ms=10 compile-ms=40 tid=0x05E0 hr=0x00000000 replay=2 sh=0x1 qpc={tick(40)}',
            'asset live-thread-render-state=0x00001B14 detail=0x00000000',
            f'asset pso-replay-compile ms=50 sh=0x2 qpc={tick(60)}',
            f'asset pso-slow kind=graphics n=2 wait-ms=0 compile-ms=35 tid=0x05E0 hr=0x00000000 replay=2 sh=0x3 qpc={tick(90)}',
            'asset live-thread-lobby-save-state=0x00000000 detail=0x02020202',
            'asset live-thread-lobby-save-state=0x00000001 detail=0x00000000',
            f'asset pso-slow kind=graphics n=3 wait-ms=900 compile-ms=60 tid=0x1B14 hr=0x00000000 replay=2 sh=0x4 qpc={tick(103)}',
            'asset pso-slots n=512 base=4 boost=2 min-mib=768 boosted=40 urg=3 urg-over=1 urg-wait-ms=5 '
            'urg-max-ms=4 wait-ms=900 bg=20 bg-wait-ms=100 peak=7',
            f'asset pso-slow kind=graphics n=4 wait-ms=0 compile-ms=150 tid=0x1B14 hr=0x00000000 replay=1 sh=0x5 qpc={tick(130)}',
            f'asset pso-slow kind=compute n=5 wait-ms=20 compile-ms=200 tid=0x05E0 hr=0x00000000 replay=1 sh=0x6 qpc={tick(150)}',
            f'asset perf-sec s=99 qpc={tick(170)} cpu-ms=1',
        ]
        (folder / 'trace.txt').write_text('\r\n'.join(trace) + '\r\n')
        # Loading frames from 37 s, menu, a 1 s world-load gap at 104 s, then steady 60 fps from 110 s.
        frames = [37 + i / 20 for i in range(20 * 66)]           # 37..103 s at 20 fps
        frames += [104.5, 106, 107.2, 108.9]                      # world load
        frames += [110 + i / 60 for i in range(60 * 60)]          # 110..170 s at 60 fps
        (folder / 'fhd-present-times.bin').write_bytes(presents(frames))
        return folder

    def test_phases_from_existing_records(self):
        with tempfile.TemporaryDirectory() as directory:
            inventory = {'SteamWin32Trial (9)': {'created': round((LAUNCH - 60) * 1e7) + phases['FILETIME_EPOCH']},
                         'SteamWin32Trial (10)': {'created': round((LAUNCH + 28.6) * 1e7) + phases['FILETIME_EPOCH']}}
            report = phases['analyze'](self.run_folder(directory), inventory)
        self.assertEqual(report['play_source'], 'probe-report')
        self.assertEqual(report['phases_s'], {'play': 27.5, 'stage_created': 28.6, 'game_log_start': 31.2,
                                              'first_present': 37.0, 'menu': 90.0, 'continue': 100.0,
                                              'gameplay': 110.0})
        self.assertEqual(report['play_to_menu_s'], 62.5)
        self.assertEqual(report['continue_to_gameplay_s'], 10.0)
        self.assertEqual(report['intervals_s']['first_present_to_menu'], 53.0)
        self.assertEqual(report['game_warmup_pipelines'], 2322)
        self.assertEqual(report['render_thread'], '0x1B14')
        self.assertEqual(report['pso_config'], {'slots': 4, 'boost': 2, 'boost-min-mib': 768, 'priority': 1})
        self.assertEqual(report['pso_slots_summary']['urg-over'], 1)
        self.assertEqual(report['compiles']['boot'], {'title_compiles': 2, 'title_compile_ms': 75, 'title_wait_ms': 10,
                                                      'replay_compiles': 1, 'replay_compile_ms': 50})
        self.assertEqual(report['compiles']['menu_to_gameplay']['render_thread_wait_ms'], 900)
        stalls = report['gameplay_compile_stalls']
        self.assertEqual(stalls['render_thread'], [150])
        self.assertEqual((stalls['worker_count'], stalls['worker_worst_ms']), (1, 220))
        self.assertEqual(stalls['traced_minutes'], 1.0)

    def test_hidden_warmup_and_replay_hits(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = self.run_folder(directory)
            (folder / 'game.log').write_text(
                '20:33:54:200 (00005004) > [Startup] Marvel\'s Spider-Man Remastered\n'
                '20:33:54:300 (00005004) > [Render] File xbox0.pso is missing, no pipelines read.\n'
                '20:33:54:310 (00005004) > [Render] File \\xbox0.pso is missing, no pipelines read.\n')
            with (folder / 'trace.txt').open('a') as trace:
                trace.write('asset pso-parallel slots=4 boost=2 boost-min-mib=768 priority=1 replay-threads=6 '
                            'bg-reserve=2 skip-warmup=1\r\n')
                trace.write(f'asset pso-replay hit kind=graphics n=7 waited=0 ms=0 tid=0x05E0 qpc={tick(50)}\r\n')
                trace.write(f'asset pso-replay hit kind=graphics n=8 waited=1 ms=3 tid=0x05E0 qpc={tick(102)}\r\n')
                trace.write(f'asset perf-sec s=4 qpc={tick(45)} cpu-ms=900 pso-n=12 pso-ms=800\r\n')
            report = phases['analyze'](folder)
        self.assertEqual(report['game_warmup_pipelines'], 0)
        self.assertEqual(report['pso_config']['skip-warmup'], 1)
        self.assertEqual(report['pso_config']['replay-threads'], 6)
        self.assertEqual(report['compiles']['boot']['replay_hits'], 1)
        self.assertEqual(report['compiles']['boot']['perf_title_compiles'], 12)
        self.assertEqual(report['compiles']['menu_to_gameplay']['replay_hits'], 1)

    def test_estimates_play_from_the_stage_without_a_probe_report(self):
        with tempfile.TemporaryDirectory() as directory:
            inventory = {'SteamWin32Trial (10)': {'created': round((LAUNCH + 28.6) * 1e7) + phases['FILETIME_EPOCH']}}
            report = phases['analyze'](self.run_folder(directory, probe=False), inventory)
        self.assertEqual(report['play_source'], 'estimated-from-stage')
        self.assertAlmostEqual(report['phases_s']['play'], 27.55, delta=.06)

    def test_timestamp_files_stand_in_without_a_presenter_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = self.run_folder(directory)
            (folder / 'fhd-present-times.bin').unlink()
            (folder / 'menu.utc').write_text(utc(95))
            (folder / 'gameplay.utc').write_text(utc(120))
            report = phases['analyze'](folder)
        self.assertEqual(report['phase_sources'], {'menu': 'menu.utc', 'gameplay': 'gameplay.utc'})
        self.assertEqual((report['phases_s']['menu'], report['phases_s']['gameplay']), (95.0, 120.0))
        self.assertIsNone(report['phases_s']['first_present'])

    def test_gameplay_waits_for_steady_frames_after_the_load_gap(self):
        frames = [tick(t) for t in [0, .5] + [1 + i / 60 for i in range(60 * 3)] + [4.5] +
                  [5 + i / 60 for i in range(60 * 8)]]
        self.assertEqual(phases['gameplay_start'](frames, HZ, tick(0)), tick(5))
        self.assertIsNone(phases['gameplay_start'](frames, HZ, tick(9)))

    def test_cli_reports_the_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = self.run_folder(directory)
            import contextlib
            import io
            import sys
            output = io.StringIO()
            argv = sys.argv
            sys.argv = ['load_phases', str(folder), '--budget', '90']
            try:
                with contextlib.redirect_stdout(output):
                    phases['main']()
            finally:
                sys.argv = argv
        report = json.loads(output.getvalue())
        self.assertEqual(report['play_to_gameplay_unattended_s'], 72.5)
        self.assertTrue(report['within_budget'])


if __name__ == '__main__':
    unittest.main()
