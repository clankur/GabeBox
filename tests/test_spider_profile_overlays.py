from pathlib import Path
import json
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / 'profiles/1817070/23986256'
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

    def test_half_refresh_overlay_turns_on_the_games_30_fps_vsync(self):
        entries = json.loads((PROFILE / 'half-refresh-vsync-overlay.json').read_text(encoding='utf-8'))
        self.assertEqual(len(entries), 1)
        # mov word ptr [rdi+0x38], 0x0101 at RVA 0x1CFAC69: VSync on (+0x39) and Half refresh rate (+0x38).
        # The game then presents with SyncInterval 2. Optional; the play build does not select it.
        self.assertEqual(entries[0]['offset'] + TEXT_FILE_OFFSET, 0x1CFAC69)
        self.assertEqual((PROFILE / entries[0]['replacement']).read_bytes(), bytes.fromhex('66c747380101'))

    def test_play_build_pins_overlays_and_loader_budget(self):
        build = json.loads((PROFILE / 'play-build.json').read_text(encoding='utf-8'))
        overlays = build['recipe_overlays']
        self.assertEqual(len(overlays), len(set(overlays)))
        for name in overlays + list(build['optional_recipe_overlays']):
            self.assertTrue((PROFILE / name).is_file(), name)
        self.assertIn('texture-quality-medium-overlay.json', overlays)
        self.assertIn('light-grid-slots-1024-overlay.json', overlays)
        self.assertNotIn('half-refresh-vsync-overlay.json', overlays)
        loader = build['loader']
        self.assertEqual(loader['mode'], 'play')
        script = (ROOT / loader['build']).read_text(encoding='utf-8')
        width, height = loader['hosted_render']
        flags = (f'budget_flags="-DSOUNDBANK_MIB={loader["soundbank_mib"]} '
                 f'-DHOSTED_RENDER_WIDTH={width} -DHOSTED_RENDER_HEIGHT={height}"')
        self.assertIn(flags, script)
        self.assertRegex(script, r'play\) mode_flags="[^"]*\$budget_flags[^"]*-DNO_PERF_SEC"')
        source = (ROOT / 'analysis/spider-man-play-loader/trace.c').read_text(encoding='utf-8')
        # The loader reports the smaller hosted monitor before the game queries displays.
        self.assertIn('#if defined(HOSTED_RENDER_WIDTH) && defined(HOSTED_RENDER_HEIGHT)', source)
        self.assertIn('L"XBOX_SDL_RENDER_WIDTH", L"XBOX_SDL_RENDER_HEIGHT"', source)
        self.assertIn('{ "SoundBankAlloc", "asset engine-pool-SoundBankAlloc bytes=0x", SOUNDBANK_MIB }', source)


if __name__ == '__main__':
    unittest.main()
