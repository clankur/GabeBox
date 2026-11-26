import io
import json
import re
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import Mock, patch

from tools import xbox_deploy as cli

ROOT = Path(__file__).resolve().parents[1]
BOUNDARY = '0123456789abcdef0123456789abcdef'


EXPIRES = 1795727625  # 2026-11-26T21:13:45Z
LINE = 'local account=22202 expires=2026-11-26T21:13:45Z\n'


def listing(*names, kind=16):
    return json.dumps({'Items': [{'Name': name, 'Type': kind} for name in names]}).encode()


class CloudResolveUploadTests(unittest.TestCase):
    def test_binds_the_choice_to_an_account_and_expiry(self):
        self.assertEqual(cli.cloud_resolve_line('local', 22202, EXPIRES), LINE)
        # The C++ policy test parses this exact line as pending for that account.
        policy_test = (ROOT / 'tests/steam-cloud-sync-policy.test.cpp').read_text()
        self.assertIn(f'EvaluateChoice("{LINE[:-1]}\\n", account, now)', policy_test)
        self.assertIn('constexpr std::uint32_t account = 22202;', policy_test)

    def test_builds_one_named_choice_file(self):
        filename, body, line = cli.cloud_resolve_upload(2868840, 'local', 22202, EXPIRES, BOUNDARY)
        self.assertEqual(filename, 'steam-cloud-resolve-2868840.txt')
        self.assertEqual(line, LINE)
        text = body.decode()
        self.assertTrue(text.startswith(f'--{BOUNDARY}\r\n'))
        self.assertIn('filename="steam-cloud-resolve-2868840.txt"', text)
        # The file ends in a newline before the CRLF that belongs to the delimiter.
        self.assertIn(f'\r\n\r\n{LINE}\r\n--{BOUNDARY}--\r\n', text)
        self.assertTrue(text.endswith(f'--{BOUNDARY}--\r\n'))

    def test_rejects_anything_but_a_known_choice(self):
        for app_id, keep, account, expires, boundary in (
                (0, 'cloud', 22202, EXPIRES, BOUNDARY), (1 << 32, 'local', 22202, EXPIRES, BOUNDARY),
                (2868840, 'both', 22202, EXPIRES, BOUNDARY), (2868840, 'cloud', 22202, EXPIRES, 'x\r\n'),
                (2868840, 'local', 0, EXPIRES, BOUNDARY), (2868840, 'local', 1 << 32, EXPIRES, BOUNDARY),
                (2868840, 'local', 22202, 0, BOUNDARY)):
            with self.assertRaises(ValueError):
                cli.cloud_resolve_upload(app_id, keep, account, expires, boundary)

    def portal(self, accounts):
        portal = Mock()
        portal.request.side_effect = lambda method, path, *rest: (200, accounts if method == 'GET' else b'')
        return portal

    def test_write_derives_the_account_and_verifies_the_read_back(self):
        portal = self.portal(listing('22202', 'anonymous', '0', '4294967296'))
        app = {'PackageFullName': 'fixture'}
        with patch.object(cli.time, 'time', return_value=EXPIRES - 24 * 3600), \
                patch.object(cli, 'read_probe_report', return_value=LINE), redirect_stdout(io.StringIO()):
            cli.write_cloud_resolve(portal, app, 2868840, 'local')
        listed, posted = (entry.args for entry in portal.request.call_args_list)
        self.assertEqual(listed[0], 'GET')
        self.assertIn('/api/filesystem/apps/files?', listed[1])
        self.assertIn('SteamUserData', listed[1])
        method, path, body, headers = posted
        self.assertEqual(method, 'POST')
        self.assertIn('extract=false', path)
        self.assertIn(f'\r\n\r\n{LINE}\r\n'.encode(), body)
        self.assertTrue(headers['Content-Type'].startswith('multipart/form-data; boundary='))
        with patch.object(cli.time, 'time', return_value=EXPIRES - 24 * 3600), \
                patch.object(cli, 'read_probe_report', return_value='local\n'):
            with self.assertRaises(ValueError):
                cli.write_cloud_resolve(portal, app, 2868840, 'local')

    def test_the_account_must_be_named_or_unambiguous(self):
        app = {'PackageFullName': 'fixture'}
        for accounts, account in ((listing('22202', '22203'), None), (listing(), None),
                                  (listing('22202'), 22203), (listing('22202', kind=32), None)):
            with self.assertRaises(ValueError):
                cli.write_cloud_resolve(self.portal(accounts), app, 2868840, 'local', account)
        portal = self.portal(listing('22202', '22203'))
        with patch.object(cli.time, 'time', return_value=EXPIRES - 3600), \
                patch.object(cli, 'read_probe_report', return_value='cloud account=22203 expires=2026-11-26T21:13:45Z\n'), \
                redirect_stdout(io.StringIO()):
            cli.write_cloud_resolve(portal, app, 2868840, 'cloud', 22203, hours=1)
        self.assertIn(b'\r\n\r\ncloud account=22203 expires=2026-11-26T21:13:45Z\n\r\n', portal.request.call_args.args[2])

    def test_expiry_is_bounded(self):
        app = {'PackageFullName': 'fixture'}
        for hours in (0, -1, cli.MAX_CLOUD_CHOICE_HOURS + 1):
            with self.assertRaises(ValueError):
                cli.write_cloud_resolve(self.portal(listing('22202')), app, 2868840, 'local', hours=hours)
        # The host rejects anything further than seven days away.
        self.assertLessEqual(cli.MAX_CLOUD_CHOICE_HOURS, 7 * 24)


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
        self.assertIn('const bool acceptLocal = AcceptLocal(side);', block)
        self.assertIn('return choice == Choice::Local;', policy)
        self.assertIn('return "resolved-cloud-auto";', policy)
        # Confirmation is the client's synchronized state, never a callback alone.
        after = policy[policy.index('inline Decision AfterResolution'):]
        self.assertIn('!wait.ran', after[:after.index('}')])
        self.assertIn('stateAfter != static_cast<std::int32_t>(State::Synchronized)', after[:after.index('}')])

    def test_the_choice_is_checked_at_every_launch_and_consumed_only_when_resolved(self):
        native = (ROOT / 'runtime/steam/SteamCloudSync.h').read_text()
        prepare = native[native.index('PrepareLaunch(std::shared_ptr<Plan> plan'):]
        read = prepare.index('ReadChoice(choicePath, static_cast<std::uint32_t>(plan->account))')
        discard = prepare.index('if (DiscardAtLaunch(choice.file.status)) DiscardChoice(')
        # Before any skip that follows a live session, and before the download.
        self.assertLess(read, discard)
        for skip in ('skip("interface-mismatch")', 'skip("account-cloud-disabled")', 'skip("app-cloud-disabled")',
                     'plan->storage.Request(plan->app, Download)'):
            self.assertLess(discard, prepare.index(skip))
        conflict = prepare[prepare.index('if (plan->decision == Decision::HeldConflict) {'):]
        consume = conflict.index('if (IsResolved(plan->decision)) ConsumeChoice(')
        self.assertLess(conflict.index('plan->decision = AfterResolution(side,'), consume)
        self.assertEqual(native.count('ConsumeChoice(choicePath'), 1)
        self.assertNotIn('MoveFileFromAppW(choicePath', prepare)

    def test_only_a_running_upload_holds_back_the_next_launch(self):
        native = (ROOT / 'runtime/steam/SteamCloudSync.h').read_text()
        self.assertNotIn('exitSyncActive', native)
        upload = native[native.index('UploadAfterExit(winrt::handle process'):]
        self.assertLess(upload.index('resume_on_signal(process.get())'), upload.index('ExitUploadActive active;'))
        self.assertLess(upload.index('if (!UploadAllowed(plan->decision))'), upload.index('ExitUploadActive active;'))
        self.assertLess(upload.index('ExitUploadActive active;'), upload.index('plan->storage.Request(plan->app, Upload)'))

    def test_backups_are_pruned_only_after_a_complete_backup(self):
        native = (ROOT / 'runtime/steam/SteamCloudSync.h').read_text()
        self.assertIn('if (backup.ok && std::strcmp(backup.status, "copied") == 0)\n'
                      '            PruneBackups(localState, plan->account, plan->app, plan->runId, report);', native)
        policy = (ROOT / 'runtime/steam/steam-cloud-sync-policy.h').read_text()
        self.assertIn('if (run == currentRun || run >= now || now - run < KeepBackupTicks) continue;', policy)

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
