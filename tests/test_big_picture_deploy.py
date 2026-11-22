import importlib.util
import unittest
from pathlib import Path
from unittest.mock import Mock, patch
from urllib.parse import parse_qs, urlsplit


spec = importlib.util.spec_from_file_location('xbox_deploy', Path(__file__).resolve().parents[1] / 'xbox-deploy.py')
cli = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cli)


def snapshot(run, status, extra=''):
    return f'probe=native-steam-big-picture\r\nrun_id={run}\r\nstatus={status}\r\n{extra}'.encode('utf-16')


class BigPictureDeployTests(unittest.TestCase):
    def watch(self, *snapshots):
        package = {'PackageFullName': 'XboxPortabilityProbe.BigPicture_1.0.0.0_x64__fixture',
                   'PackageRelativeId': 'BigPicture_fixture!App'}
        reports = iter(snapshots)

        def request(method, url):
            if method == 'POST':
                self.assertTrue(url.startswith('/api/taskmanager/app?'))
                return 200, b''
            query = parse_qs(urlsplit(url).query)
            self.assertEqual(query['filename'], ['big-picture-report.txt'])
            self.assertEqual(query['packagefullname'], [package['PackageFullName']])
            return 200, next(reports)

        portal = Mock()
        portal.request.side_effect = request
        with patch.object(cli, 'find_installed_probe', return_value=package) as installed, patch.object(cli.time, 'sleep'):
            try:
                return cli.wait_for_probe(portal, cli.BIG_PICTURE_NAME, '1.0.0.0', False, 20)
            finally:
                installed.assert_called_once_with(portal, cli.BIG_PICTURE_NAME, '1.0.0.0')

    def test_waits_for_fresh_observation_even_if_previous_run_changes(self):
        result = self.watch(snapshot('1', 'client-created'), snapshot('1', 'client-running'),
                            snapshot('2', 'client-created'), snapshot('2', 'client-running'))
        self.assertEqual(cli.report_fields(result)['run_id'], '2')

    def test_loader_failure_and_launcher_handoff_do_not_pass(self):
        for status, extra, error in [('process-error', 'win32_error=0x00000005\r\n', 'startup'),
                                     ('client-exited', 'exit_code=0x00000000\r\n', 'handoff')]:
            with self.subTest(status=status), self.assertRaisesRegex(ValueError, error):
                self.watch(b'', snapshot('3', status, extra))
