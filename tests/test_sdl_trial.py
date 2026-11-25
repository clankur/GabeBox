import hashlib
from argparse import Namespace
from pathlib import Path
from struct import pack_into
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import Mock, patch


from tools import xbox_deploy as cli


class SdlTrialTests(unittest.TestCase):
    def test_trial_checks_the_new_play_attempt_and_restores_the_original(self):
        original = b'publisher SDL2 fixture'
        original_hash = hashlib.sha256(original).hexdigest()
        image = bytearray(256)
        image[:2] = b'MZ'
        pack_into('<I', image, 0x3c, 64)
        image[64:68] = b'PE\0\0'
        pack_into('<H', image, 68, 0x8664)
        pack_into('<H', image, 84, 112)
        pack_into('<H', image, 88, 0x20b)
        pack_into('<IIII', image, 220, 0xFEEF04BD, 0x10000, 0x2001e, 0x90000)
        candidate = bytes(image)
        complete = 'status=steam-core-complete\r\n'
        submitted = 'steam_launch_submitted=yes\r\n' + complete
        cases = (
            ('timeout', ValueError('launch timed out'), 'launch timed out'),
            ('library only', complete, 'did not record Steam Play'),
            ('different game', 'steam_launch_app_id=975370\r\n' + submitted, 'did not record Steam Play'),
            ('Play never submitted', 'steam_launch_app_id=413150\r\n' + complete, 'did not record Steam Play'),
            ('current report', 'steam_launch_app_id=413150\r\n' + submitted, None),
            ('older report', 'steam_launch_repeat_app_id=413150\r\n' + submitted, None),
        )
        for name, result, error in cases:
            with self.subTest(name=name), TemporaryDirectory() as scratch:
                portal = Mock()
                portal.request.return_value = (200, b'steam_launch_repeat_app_id=413150\r\n')
                artifact = Path(scratch) / 'artifact'
                artifact.mkdir()
                (artifact / 'SDL2.dll').write_bytes(candidate)
                (artifact / 'SHA256SUMS').write_text(f'{hashlib.sha256(candidate).hexdigest()}  SDL2.dll\n')
                backup_dir = Path(scratch) / 'backup'
                args = Namespace(app_id=413150, version='1.0.140.0', artifact=artifact, backup_dir=backup_dir,
                                 timeout=20, restore_only=False, install_only=False, frame=None)
                outcome = {'side_effect': result} if isinstance(result, Exception) else {'return_value': result}
                with patch.dict(cli.SDL_TRIALS, {413150: ('Stardew Valley', '2.30.9', original_hash)}), \
                     patch.object(cli, 'find_installed_probe', return_value={'PackageFullName': 'fixture'}), \
                     patch.object(cli, 'read_game_file', side_effect=(original, candidate, candidate, original)), \
                     patch.object(cli, 'wait_for_probe', **outcome):
                    if error:
                        with self.assertRaisesRegex(ValueError, error):
                            cli.trial_sdl(portal, args)
                    else:
                        cli.trial_sdl(portal, args)
                backup = backup_dir / f'413150-SDL2-{original_hash[:12]}.dll'
                self.assertEqual(backup.read_bytes(), original)
                self.assertFalse(backup.with_suffix('.lock').exists())

                uploads = [call.args[2] for call in portal.request.call_args_list if call.args[0] == 'POST']
                self.assertEqual(len(uploads), 2)
                self.assertIn(candidate, uploads[0])
                self.assertIn(original, uploads[1])
