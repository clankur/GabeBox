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

    def test_lists_every_gap_over_the_threshold_with_its_utc_time(self):
        report = measure['presenter_gaps'](self.frames, 0, None, 1, 50)
        self.assertEqual(report['count'], 1)
        self.assertEqual(report['gaps'], [{'utc': '1970-01-01T00:00:00.017+00:00', 'start_offset_s': .017,
                                           'gap_ms': 100, 'gpu_source_sequence_delta': 2}])
        self.assertEqual(measure['presenter_gaps'](self.frames, 0, None, 1, 16)['count'], 3)

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
