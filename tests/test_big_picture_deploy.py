import hashlib
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import Mock, patch
from urllib.parse import parse_qs, urlsplit


from tools import xbox_deploy as cli


def snapshot(run, status, extra=''):
    return f'probe=native-steam-big-picture\r\nrun_id={run}\r\nstatus={status}\r\n{extra}'.encode('utf-16')


class BigPictureDeployTests(unittest.TestCase):
    def test_settings_still_default_to_the_checkout_root(self):
        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            tools = root / 'tools'
            tools.mkdir()
            (root / '.env').write_text('XBOX_PORTAL_USERNAME=checkout\n')
            (tools / '.env').write_text('XBOX_PORTAL_USERNAME=tools-directory\n')
            alternate = root / 'alternate.env'
            alternate.write_text('XBOX_PORTAL_USERNAME=alternate\n')
            with patch.object(cli, '__file__', str(tools / 'xbox_deploy.py')), patch.dict('os.environ', {}, clear=True):
                self.assertEqual(cli.load_settings()['XBOX_PORTAL_USERNAME'], 'checkout')
                self.assertEqual(cli.load_settings(alternate)['XBOX_PORTAL_USERNAME'], 'alternate')

    def test_status_can_use_a_pinned_portal_without_credentials(self):
        certificate = b'certificate fixture'
        settings = {
            'XBOX_PORTAL_URL': 'https://xbox.invalid:11443',
            'XBOX_PORTAL_CERT_SHA256': hashlib.sha256(certificate).hexdigest(),
        }
        with (patch('sys.argv', ['xbox_deploy.py', 'status']),
              patch.object(cli, 'load_settings', return_value=settings),
              patch.object(cli.getpass, 'getpass') as prompt,
              patch.object(cli.http.client, 'HTTPSConnection') as https):
            connection = https.return_value
            connection.sock.getpeercert.return_value = certificate
            response = connection.getresponse.return_value
            response.status = 200
            response.getheaders.return_value = []
            response.read.return_value = b'{"Success": true}'

            cli.main()
            prompt.assert_not_called()
            connection.request.assert_called_once_with(
                'GET', '/api/app/packagemanager/state', body=None, headers={'CSRF-Token': 'Fetch'})

            response.status = 401
            with self.assertRaisesRegex(ValueError, 'requires authentication'):
                cli.main()

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
