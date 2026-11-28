from pathlib import Path
import json
import re
import unittest


PROFILE = Path(__file__).resolve().parents[1] / 'profiles/1817070/23986256'
TEXT_FILE_OFFSET = 0xA00  # .text RVA 0x1000 is stored at file offset 0x600 in build 23986256


class SpiderProfileOverlayTests(unittest.TestCase):
    def test_every_overlay_manifest_names_a_present_replacement(self):
        manifests = sorted(PROFILE.glob('*overlay.json'))
        self.assertTrue(manifests)
        for manifest in manifests:
            entries = json.loads(manifest.read_text(encoding='utf-8'))
            self.assertIsInstance(entries, list, manifest.name)
            ranges = []
            for entry in entries:
                self.assertEqual(set(entry), {'offset', 'expected_sha256', 'replacement'}, manifest.name)
                self.assertRegex(entry['expected_sha256'], r'\A[0-9a-f]{64}\Z')
                self.assertTrue(re.fullmatch(r'[A-Za-z0-9_-]+\.[A-Za-z0-9]+', entry['replacement']))
                size = (PROFILE / entry['replacement']).stat().st_size
                self.assertGreater(size, 0, entry['replacement'])
                ranges.append((entry['offset'], entry['offset'] + size))
            ranges.sort()
            for (_, end), (start, _) in zip(ranges, ranges[1:]):
                self.assertLessEqual(end, start, f'{manifest.name} overlaps itself')

    def test_texture_quality_medium_sets_the_settings_default(self):
        entries = json.loads((PROFILE / 'texture-quality-medium-overlay.json').read_text(encoding='utf-8'))
        self.assertEqual(len(entries), 1)
        # mov dword ptr [rdi+0x50], 1 at RVA 0x1CFAD1E: Texture Quality default Medium (0 Low .. 3 Very High).
        self.assertEqual(entries[0]['offset'] + TEXT_FILE_OFFSET, 0x1CFAD1E)
        self.assertEqual((PROFILE / entries[0]['replacement']).read_bytes(), bytes.fromhex('c7475001000000'))


    def test_light_grid_slot_cap_stays_at_1024(self):
        entries = json.loads((PROFILE / 'light-grid-slots-1024-overlay.json').read_text(encoding='utf-8'))
        self.assertEqual(len(entries), 1)
        # mov r14d, 0x400 at RVA 0x17233A6: LightGridManager slot count min(requested, 1024) instead of 8191.
        # Reverting it added about 135 MiB of commit and exhausted it in the Fisk Tower area (device removed).
        self.assertEqual(entries[0]['offset'] + TEXT_FILE_OFFSET, 0x17233A6)
        self.assertEqual((PROFILE / entries[0]['replacement']).read_bytes(), bytes.fromhex('41be00040000'))


if __name__ == '__main__':
    unittest.main()
