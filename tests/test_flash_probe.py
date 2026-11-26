import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from tools import flash_probe as probe

ROOT = Path(__file__).resolve().parents[1]
# 2026-11-26T09:36:55Z as a FILETIME, at QPC 1000 with a 1 kHz counter.
FILETIME = probe.FILETIME_EPOCH + 1795685815 * 10_000_000


def frame(luma, white, low, high, cells=512):
    return {'luma': luma * 256, 'white': white, 'cells': cells, 'min': low, 'max': high}


def record(counter, sequence, kind, stats, flags=0, digest=0, slot=0):
    return probe.RECORD.pack(counter, sequence, kind, slot, flags, stats['luma'], stats['white'], stats['cells'],
                             stats['min'], stats['max'], digest)


def probe_file(rows):
    return probe.HEADER.pack(probe.MAGIC, 1, 1000, FILETIME, 1000, 16, 32, 235, 0) + b''.join(rows)


DARK, BRIGHT = frame(40, 0, 30, 50), frame(250, 500, 200, 255)


class DetectorTests(unittest.TestCase):
    """The same streams as tests/fhd-flash-probe.test.cpp."""

    def test_one_and_two_frame_flashes(self):
        flags = probe.detect([DARK] * 10 + [BRIGHT] + [DARK] * 4)
        self.assertEqual([i for i, f in enumerate(flags) if f], [10])
        self.assertEqual(flags[10], probe.FLASH)
        flags = probe.detect([DARK] * 10 + [BRIGHT] * 2 + [DARK] * 4)
        self.assertEqual([i for i, f in enumerate(flags) if f], [10, 11])

    def test_lasting_scene_change_is_not_a_flash(self):
        self.assertFalse(any(probe.detect([DARK] * 10 + [BRIGHT] * 10)))

    def test_white_blocks_over_a_steady_frame(self):
        steady = frame(90, 2, 20, 240)
        flags = probe.detect([steady] * 10 + [frame(95, 14, 20, 255)] + [steady] * 3)
        self.assertEqual([i for i, f in enumerate(flags) if f], [10])

    def test_uniform_white_needs_no_history(self):
        flags = probe.detect([DARK, DARK, frame(255, 512, 255, 255), DARK, DARK, DARK])
        self.assertEqual(flags[2], probe.UNIFORM_WHITE)


class AnalyzeTests(unittest.TestCase):
    def stream(self, sequence_flags):
        """Game and exchange records for sequences 1..n; sequence_flags maps a sequence to (game, exchange, output)."""
        rows = []
        for sequence in range(1, 21):
            game, exchange, output = sequence_flags.get(sequence, (0, 0, 0))
            tick = 1000 + sequence * 16
            game_hash = 0xA000 + sequence
            exchange_hash = game_hash if not (exchange & probe.MISMATCH) else 0xBEEF
            rows.append(record(tick, sequence, 1, BRIGHT if game else DARK, game, game_hash, sequence % 2))
            rows.append(record(tick + 2, sequence, 2, BRIGHT if exchange & 3 else DARK, exchange, exchange_hash, sequence % 2))
            rows.append(record(tick + 2, sequence, 3, BRIGHT if output else DARK, output, 0x1234, sequence % 2))
        return rows

    def test_white_rendered_by_the_game_is_attributed_to_the_game(self):
        report = probe.analyze(probe_file(self.stream({12: (1, 1, 1)})))
        self.assertEqual(report['white_frames'], 1)
        flash, = report['flashes']
        self.assertEqual((flash['sequence'], flash['origin'], flash['game_exchange_hash_match']), (12, 'game', True))
        self.assertEqual(flash['utc'], '2026-11-26T09:36:55.194Z')
        self.assertEqual(flash['exchange']['luma'], 250)
        self.assertEqual(flash['exchange']['white_fraction'], round(500 / 512, 4))
        self.assertEqual(report['origins'], {'game': 1})
        self.assertEqual(report['sources']['exchange']['white_runs'], 1)
        self.assertEqual(report['sources']['exchange']['offline_disagreements'], 0)
        self.assertEqual(report['hash_pairs'], 20)
        self.assertEqual(report['hash_mismatches'], 0)

    def test_pixels_changed_in_the_exchange(self):
        report = probe.analyze(probe_file(self.stream({9: (0, probe.FLASH | probe.MISMATCH, 1)})))
        flash, = report['flashes']
        self.assertEqual((flash['origin'], flash['game_exchange_hash_match']), ('exchange', False))
        self.assertEqual(flash['exchange']['flags'], ['flash', 'hash_mismatch'])
        self.assertEqual(report['mismatched_sequences'], [9])

    def test_white_only_after_the_presenter_draw(self):
        report = probe.analyze(probe_file(self.stream({7: (0, 0, 1)})))
        flash, = report['flashes']
        self.assertEqual(flash['origin'], 'presenter-draw')
        self.assertEqual(report['sources']['output']['white_frames'], 1)

    def test_missing_game_record_is_unattributed_and_present_index_is_added(self):
        rows = [r for r in self.stream({5: (0, 1, 1)}) if not (r[16] == 1 and struct.unpack_from('<Q', r, 8)[0] == 5)]
        trace = struct.pack('<IIQQQ', 0x31444846, 2, 1000, FILETIME, 1000) + b''.join(
            struct.pack('<QQII', 1000 + s * 16 + 5, s, s, 2) for s in range(1, 21))
        report = probe.analyze(probe_file(rows), trace)
        flash, = report['flashes']
        self.assertEqual((flash['origin'], flash['game'], flash['present_index']), ('unattributed', None, 5))
        self.assertEqual(flash['present_utc'], '2026-11-26T09:36:55.085Z')

    def test_captures_name_their_bitmaps(self):
        rows = self.stream({}) + [record(2000, 600, 4, DARK, 3, 0, 0), record(2100, 12, 4, BRIGHT, 1, 0, 1)]
        report = probe.analyze(probe_file(rows))
        self.assertEqual([c['file'] for c in report['captures']],
                         ['fhd-flash-20261126T093655-0-baseline-600.bmp', 'fhd-flash-20261126T093655-1-flash-12.bmp'])

    def test_rejects_other_files(self):
        with self.assertRaises(ValueError):
            probe.analyze(probe_file([]) + b'x')
        with self.assertRaises(ValueError):
            probe.analyze(b'\0' * probe.HEADER.size)
        with self.assertRaises(ValueError):
            probe.analyze(probe_file([record(1, 1, 9, DARK)]))

    def test_command_line_lists_flashes_and_can_require_none(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'fhd-flash-probe.bin'
            path.write_bytes(probe_file(self.stream({12: (1, 1, 1)})))
            command = [sys.executable, '-m', 'tools.flash_probe', str(path)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout)['origins'], {'game': 1})
            failed = subprocess.run(command + ['--require-none'], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(failed.returncode, 1)


if __name__ == '__main__':
    unittest.main()
