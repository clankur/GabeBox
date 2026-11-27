import runpy
from pathlib import Path
import unittest


measure = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'tools/measure.py'))


def trace(rows):
    return measure['HEADER'].pack(0x31444846, 2, 1000, measure['FILETIME_EPOCH'], 1000) + b''.join(
        measure['PRESENT'].pack(tick, sequence, index, mode)
        for index, (tick, sequence, mode) in enumerate(rows, 1)
    )


class StallDiagnosisTests(unittest.TestCase):
    frames = trace([(1000, 1, 2), (1017, 2, 2), (1117, 4, 2), (1134, 5, 2), (2000, 6, 2)])
    stall = 'dxgi stall qpc=1010 enter=1050 lock=1052 split=1057 present=1067 end=1090 prepace=0'

    def test_correlates_presenter_gap_with_clipped_producer_phases(self):
        stale = 'dxgi stall qpc=100 enter=101 lock=102 split=103 present=104 end=200 prepace=0'
        report = measure['diagnose_stalls'](self.frames, 0, None, 1, f'{stale}\r\n{self.stall}\r\n')
        self.assertEqual(report['producer_stall_records'], 2)
        self.assertEqual(report['longest_gaps'], [{
            'start_offset_s': .017, 'gap_ms': 100, 'gpu_source_sequence_delta': 2,
            'producer_stage_overlap_ms': [{'between_present_calls': 33, 'adapter_lock': 2, 'publication': 5,
                                           'pacing': 10, 'native_present': 23}],
        }])

    def test_reversed_pacing_run_labels_the_same_boundaries(self):
        log = 'steam_shadow_loader=' + self.stall.replace('prepace=0', 'prepace=1')
        report = measure['diagnose_stalls'](self.frames, 0, None, 1, log)
        stages, = report['longest_gaps'][0]['producer_stage_overlap_ms']
        self.assertEqual(stages['pacing'], 5)
        self.assertEqual(stages['publication'], 10)

    def test_old_traces_and_transport_changes_do_not_imply_a_cause(self):
        frames = trace([(1000, 5, 2), (1100, 1, 1), (1200, 3, 2), (2000, 4, 2)])
        report = measure['diagnose_stalls'](frames, 0, None, 1)
        self.assertIsNone(report['producer_stall_records'])
        self.assertEqual(len(report['longest_gaps']), 2)
        for gap in report['longest_gaps']:
            self.assertIsNone(gap['gpu_source_sequence_delta'])
            self.assertIsNone(gap['producer_stage_overlap_ms'])

    def test_nonmonotonic_producer_counters_are_rejected(self):
        log = self.stall.replace('lock=1052', 'lock=1045')
        with self.assertRaisesRegex(ValueError, 'Inconsistent producer stall timestamps'):
            measure['diagnose_stalls'](self.frames, 0, None, 1, log)

    def test_preserves_observed_resolution_and_separates_pre_scene_stalls(self):
        frames = trace([(1200, 1, 2), (2200, 2, 2), (3000, 3, 2), (3300, 4, 2), (4000, 5, 2)])
        report = measure['summarize'](frames, 2, None, 1, internal_resolution='1280x720', output_resolution='1920x1080')
        self.assertEqual(report['internal_resolution'], '1280x720')
        self.assertEqual(report['output_resolution'], '1920x1080')
        self.assertEqual(report['startup'], {'trace_start_to_first_present_ms': 200, 'pre_scene_worst_frame_interval_ms': 1000})
        self.assertEqual(report['worst_ms'], 300)


class StutterTests(unittest.TestCase):
    # 1000 ticks per second; anchor 1000 is the trace epoch (UNIX time 0).
    frames = trace([(1000 + 17 * i, i + 1, 2) for i in range(10)] +
                   [(1323, 11, 2), (1340, 12, 2), (1500, 13, 2), (1540, 14, 2), (1557, 15, 2), (2100, 16, 2)])
    log = ('asset pso-slow kind=graphics n=7 wait-ms=0 compile-ms=150 tid=0x0A04 hr=0x00000000 replay=1 qpc=1490\r\n'
           'asset pso-slow kind=compute n=8 wait-ms=0 compile-ms=12 tid=0x0A04 hr=0x00000000 replay=1 qpc=1530\r\n')

    def test_counts_long_frames_per_minute_without_a_log(self):
        report = measure['stutter'](self.frames, 0, None, 1)
        # 1153->1323 (170 ms), 1340->1500 (160 ms) and 1500->1540 (40 ms) exceed 33.3 ms.
        self.assertEqual(report['over_33ms_per_minute'], 180)
        self.assertEqual(report['over_100ms_per_minute'], 120)
        self.assertIsNone(report['by_cause'])

    def test_attributes_traced_compiles_and_excluded_windows(self):
        from datetime import datetime, timezone
        shot = (datetime.fromtimestamp(.2, timezone.utc), datetime.fromtimestamp(.25, timezone.utc))
        report = measure['stutter'](self.frames, 0, None, 1, self.log, [shot])
        causes = report['by_cause']
        self.assertEqual(causes['pipeline_compile'], {'over_33ms': 1, 'over_100ms': 1, 'ms': 160})
        self.assertEqual(causes['excluded'], {'over_33ms': 1, 'over_100ms': 1, 'ms': 170})
        # The 12 ms compile is below the 30 ms record threshold, so its frame stays unattributed.
        self.assertEqual(causes['unattributed'], {'over_33ms': 1, 'over_100ms': 0, 'ms': 40})
        self.assertEqual(report['over_33ms_per_minute'], 120)
        self.assertEqual(report['over_100ms_per_minute'], 60)
