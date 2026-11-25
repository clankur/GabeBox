import io
import re
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import Mock, patch

from tools import xbox_deploy as cli

ROOT = Path(__file__).resolve().parents[1]
BOUNDARY = '0123456789abcdef0123456789abcdef'


class CloudResolveUploadTests(unittest.TestCase):
    def test_builds_one_named_choice_file(self):
        filename, body = cli.cloud_resolve_upload(2868840, 'cloud', BOUNDARY)
        self.assertEqual(filename, 'steam-cloud-resolve-2868840.txt')
        text = body.decode()
        self.assertTrue(text.startswith(f'--{BOUNDARY}\r\n'))
        self.assertIn('filename="steam-cloud-resolve-2868840.txt"', text)
        self.assertIn('\r\n\r\ncloud\n\r\n', text)
        self.assertTrue(text.endswith(f'--{BOUNDARY}--\r\n'))

    def test_rejects_anything_but_a_known_choice(self):
        for app_id, keep, boundary in ((0, 'cloud', BOUNDARY), (1 << 32, 'local', BOUNDARY),
                                       (2868840, 'both', BOUNDARY), (2868840, 'cloud', 'x\r\n')):
            with self.assertRaises(ValueError):
                cli.cloud_resolve_upload(app_id, keep, boundary)

    def test_write_verifies_the_read_back(self):
        portal = Mock()
        app = {'PackageFullName': 'fixture'}
        with patch.object(cli, 'read_probe_report', return_value='local\r\n'), redirect_stdout(io.StringIO()):
            cli.write_cloud_resolve(portal, app, 2868840, 'local')
        method, path, body, headers = portal.request.call_args.args
        self.assertEqual(method, 'POST')
        self.assertIn('extract=false', path)
        self.assertIn(b'\r\n\r\nlocal\n\r\n', body)
        self.assertTrue(headers['Content-Type'].startswith('multipart/form-data; boundary='))
        with patch.object(cli, 'read_probe_report', return_value='cloud'):
            with self.assertRaises(ValueError):
                cli.write_cloud_resolve(portal, app, 2868840, 'local')


class CloudSyncReportTests(unittest.TestCase):
    def run_report(self, text):
        portal = Mock()
        portal.request.side_effect = lambda method=None, path=None, *rest, **options: (
            'a' * 64 if method is None else (200, text.encode()))
        argv = ['xbox_deploy', 'report', '--version', '1.0.244.3', '--steam-cloud-sync']
        output = io.StringIO()
        with patch.object(sys, 'argv', argv), patch.object(cli, 'load_settings', return_value={
                'XBOX_PORTAL_CERT_SHA256': 'a' * 64, 'XBOX_PORTAL_URL': 'https://xbox.test',
                'XBOX_PORTAL_USERNAME': 'u', 'XBOX_PORTAL_PASSWORD': 'p'}), \
                patch.object(cli, 'DevicePortal', return_value=portal), \
                patch.object(cli, 'find_installed_probe', return_value={'PackageFullName': 'fixture'}), \
                redirect_stdout(output):
            cli.main()
        return portal, output.getvalue()

    def test_reads_the_cloud_sync_report_from_local_state(self):
        text = ('steam_cloud_sync_run_id=134401\r\nsteam_cloud_sync_choice=none\r\n'
                'steam_cloud_sync_resolve_side=cloud\r\nsteam_cloud_sync_state_after_resolve=synchronized (2)\r\n'
                'steam_cloud_sync_decision=resolved-cloud-auto\r\nsteam_cloud_sync_upload_policy=allowed\r\n')
        portal, output = self.run_report(text)
        self.assertIn('filename=steam-cloud-sync-report.txt', portal.request.call_args.args[1])
        self.assertEqual(output, text)


class ObserverTests(unittest.TestCase):
    def test_launch_report_cloud_fields_reach_the_observer(self):
        from tools.xbox_diagnostics import game_snapshot

        probe = {'steam_cloud_sync': 'resolved-cloud-auto', 'steam_cloud_sync_upload_policy': 'allowed',
                 'steam_cloud_sync_exit_watch': 'started', 'steam_cloud_sync_run': '134401'}
        native = game_snapshot(probe, {}, {}, 2868840)['probe']
        self.assertEqual(native['steam_cloud_sync'], 'resolved-cloud-auto')
        self.assertEqual(native['steam_cloud_sync_upload_policy'], 'allowed')
        held = game_snapshot({'steam_cloud_sync_upload_policy': 'blocked:conflict-held'}, {}, {}, 2868840)['probe']
        self.assertEqual(held['steam_cloud_sync_upload_policy'], 'blocked:conflict-held')
        self.assertEqual(native['steam_cloud_sync_exit_watch'], 'started')


class NativeContractTests(unittest.TestCase):
    """The host's report keys and choice file name must match the tooling."""

    def test_native_names_match_the_tooling(self):
        native = (ROOT / 'runtime/steam/SteamCloudSync.h').read_text()
        self.assertIn(f'L"\\\\{cli.STEAM_CLOUD_SYNC_REPORT}"', native)
        self.assertIn('L"\\\\steam-cloud-resolve-"', native)
        for slot in ('IsCloudEnabledForAccount{23, 0x86d5f0}', 'IsCloudEnabledForApp{24, 0x86d770}',
                     'GetLastKnownSyncState{72, 0x7a1d10}', 'ResolveSyncConflict{77, 0x889c10}',
                     'SynchronizeApp{78, 0x8b36e0}', 'IsAppSyncInProgress{79, 0x86cc60}'):
            self.assertIn(slot, native)

    def test_conflicts_resolve_to_the_cloud_only_after_a_backup(self):
        native = (ROOT / 'runtime/steam/SteamCloudSync.h').read_text()
        policy = (ROOT / 'runtime/steam/steam-cloud-sync-policy.h').read_text()
        block = native[native.index('if (plan->decision == Decision::HeldConflict) {'):]
        backup = block.index('if (!(backedUp || backupLocal())) {')
        resolve = block.index('plan->storage.Resolve(plan->app, acceptLocal)')
        self.assertLess(backup, block.index('} else {'))
        self.assertLess(block.index('} else {'), resolve)
        self.assertIn('const bool acceptLocal = AcceptLocal(choice);', block)
        self.assertIn('return choice == Choice::Local;', policy)
        self.assertIn('return "resolved-cloud-auto";', policy)
        # Confirmation is the client's synchronized state, never a callback alone.
        after = policy[policy.index('inline Decision AfterResolution'):]
        self.assertIn('!wait.ran', after[:after.index('}')])
        self.assertIn('stateAfter != static_cast<std::int32_t>(State::Synchronized)', after[:after.index('}')])

    def test_every_win32_trial_launch_syncs_before_create_process(self):
        source = (ROOT / 'runtime/steam/ProbeNative.cpp').read_text()
        prepare = source.index('steam_cloud_sync::PrepareLaunch(')
        trial = source.index('} else if (!shadowExecutable.empty()) {')
        create = source.index('CreateProcessW(image.c_str()')
        watch = source.index('steam_cloud_sync::UploadAfterExit(')
        self.assertLess(prepare, trial)
        self.assertLess(create, watch)
        self.assertRegex(source[prepare - 600:prepare], re.compile(r'if \(!shadowExecutable\.empty\(\)\)'))


if __name__ == '__main__':
    unittest.main()
