import contextlib
import io
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile


prepare = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'probe' / 'prepare-steam-core.py'))


class LaunchOptionTests(unittest.TestCase):
    def test_target_specific_marker_and_legacy_launch_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            template = root / 'template.appx'
            with ZipFile(template, 'w') as archive:
                archive.writestr('AppxManifest.xml', '<Package><Identity Name="' + prepare['PROBE_ID'] +
                                 '" ProcessorArchitecture="x64" Version="1.0.1.0"/>'
                                 '<Applications><Application StartPage="index.html"/></Applications></Package>')
                archive.writestr('steam-signin.html', '')
            for index in (None, 0, 1, 0xffffffff):
                with self.subTest(index=index):
                    layout = root / str(index)
                    prepare['prepare_layout'](template, layout, '1.0.2.0', None, None, False, True,
                                              launch_app_id=427520, repeat_launch=True, install_before_launch=True,
                                              launch_option=index)
                    self.assertEqual((layout / 'steam-launch-app.txt').read_bytes(), b'427520\nrepeat\ninstall')
                    markers = list(layout.glob('steam-launch-option-*.txt'))
                    if index is None:
                        self.assertEqual(markers, [])
                    else:
                        self.assertEqual(markers, [layout / 'steam-launch-option-427520.txt'])
                        self.assertEqual(markers[0].read_text(), str(index))

    def test_rejects_unscoped_negative_and_oversize_options(self):
        for index, extra in [('1', []), ('-1', ['--launch-app-id', '427520']),
                             ('4294967296', ['--launch-app-id', '427520'])]:
            argv = ['prepare', 'absent.appx', 'layout', '--version', '1.0.2.0', '--steam-sign-in',
                    '--unicode-keyboard', '--valve-archive', 'unused.zip', '--launch-option', index, *extra]
            with self.subTest(argv=argv), patch('sys.argv', argv), contextlib.redirect_stderr(io.StringIO()) as stderr:
                with self.assertRaises(SystemExit) as error:
                    prepare['main']()
                self.assertEqual(error.exception.code, 2)
                self.assertIn('--launch-option requires', stderr.getvalue())
