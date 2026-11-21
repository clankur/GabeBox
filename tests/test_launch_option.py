import contextlib
import io
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile


prepare = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'probe' / 'prepare-steam-core.py'))


def write_template(root, signin='', extra_entries=()):
    template = root / 'template.appx'
    with ZipFile(template, 'w') as archive:
        archive.writestr('AppxManifest.xml', '<Package><Identity Name="' + prepare['PROBE_ID'] +
                         '" ProcessorArchitecture="x64" Version="1.0.1.0"/>'
                         '<Applications><Application StartPage="index.html"/></Applications></Package>')
        archive.writestr('steam-signin.html', signin)
        for name in extra_entries:
            archive.writestr(name, 'template')
    return template


class LaunchOptionTests(unittest.TestCase):
    def test_slay_host_packages_matching_bridges_only_for_the_hosted_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            template = write_template(root, '<script src="steam-signin.js"></script>',
                                      ('steam-sdl-host.js', 'steam-sdl-gamepad.js',
                                       'slay-working-directory.dll', 'slay-sdl-display.dll'))
            runtime = root / 'runtime'
            runtime.mkdir()
            for name in ('SlayConsoleDiagnostic.class', 'SlayConsoleDiagnostic$1.class', 'slay-lwjgl-display.jar',
                         'slay-working-directory.dll', 'slay-sdl-display.dll', 'slay-font-atlas-limit.txt',
                         'SDL2.dll', 'LICENSE.SDL2'):
                (runtime / name).write_text('runtime')
            hosted = root / 'hosted'
            prepare['prepare_layout'](template, hosted, '1.0.2.0', None, None, False, True, slay_sdl_host=runtime)
            self.assertTrue((hosted / 'steam-sdl-host-646570.txt').is_file())
            self.assertIn('data-apps="646570"', (hosted / 'steam-signin.html').read_text())
            for name in runtime.iterdir():
                self.assertEqual((hosted / name.name).read_text(), 'runtime')

            standard = root / 'standard'
            prepare['prepare_layout'](template, standard, '1.0.2.0', None, None, False, True)
            for name in ('slay-working-directory.dll', 'slay-sdl-display.dll', 'steam-sdl-host-646570.txt'):
                self.assertFalse((standard / name).exists())

    def test_startup_snapshot_is_opt_in_and_title_scoped(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            template = write_template(root)
            for enabled in (False, True):
                layout = root / str(enabled)
                prepare['prepare_layout'](template, layout, '1.0.2.0', None, None, False, True,
                                          launch_app_id=975370, startup_snapshot=enabled)
                markers = [p.name for p in layout.glob('steam-startup-snapshot-*.txt')]
                self.assertEqual(markers, ['steam-startup-snapshot-975370.txt'] if enabled else [])
            for app_id, archive in ((None, None), (7760, Path('hosted.tgz'))):
                with self.assertRaisesRegex(ValueError, 'native Steam Play target'):
                    prepare['prepare_layout'](template, root / 'invalid', '1.0.2.0', None, None, False, True,
                                              launch_app_id=app_id, xcom_runtime_archive=archive, startup_snapshot=True)

    def test_target_specific_marker_and_legacy_launch_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            template = write_template(root)
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

    def test_multiple_library_options_do_not_require_an_automatic_launch(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            layout = root / 'layout'
            prepare['prepare_layout'](write_template(root), layout, '1.0.2.0', None, None, False, True,
                                      launch_options={427520: 1, 1145360: 0})
            self.assertFalse((layout / 'steam-launch-app.txt').exists())
            self.assertEqual((layout / 'steam-launch-option-427520.txt').read_text(), '1')
            self.assertEqual((layout / 'steam-launch-option-1145360.txt').read_text(), '0')

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

    def test_rejects_invalid_or_duplicate_library_options(self):
        base = ['prepare', 'absent.appx', 'layout', '--version', '1.0.2.0', '--steam-sign-in',
                '--unicode-keyboard', '--valve-archive', 'unused.zip']
        cases = [
            (['--steam-launch-option', '0:1'], 'expected APPID:INDEX'),
            (['--steam-launch-option', '427520:4294967296'], 'expected APPID:INDEX'),
            (['--steam-launch-option', '427520:1', '--steam-launch-option', '427520:0'], 'one Steam launch option'),
            (['--steam-launch-option', '427520:1', '--launch-app-id', '427520', '--launch-option', '0'],
             'one Steam launch option'),
            (['--steam-launch-option', '7760:1', '--xcom-dos-runtime', 'unused.tgz'], 'cannot be set for X-COM'),
        ]
        for args, message in cases:
            with self.subTest(args=args), patch('sys.argv', base + args), contextlib.redirect_stderr(io.StringIO()) as stderr:
                with self.assertRaises(SystemExit) as error:
                    prepare['main']()
                self.assertEqual(error.exception.code, 2)
                self.assertIn(message, stderr.getvalue())
