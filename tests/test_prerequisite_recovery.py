import runpy
import unittest
from pathlib import Path
from unittest.mock import Mock, patch


cli = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'xbox-deploy.py'))
recover = cli['wait_for_repeat_launch']


def report(outcome, run=1, app=413150):
    return (f'steam_launch_run_id={run}\r\nsteam_launch_repeat_app_id={app}\r\n'
            f'steam_launch={outcome}\r\nstatus=steam-core-complete\r\n')


class PrerequisiteRecoveryTests(unittest.TestCase):
    def run_reports(self, results, calls):
        with patch.dict(recover.__globals__, wait_for_probe=calls):
            calls.side_effect = results
            recover(object(), '1.0.100.0', 413150, False, 300)

    def test_restarts_only_once_after_completed_shared_install(self):
        calls = Mock()
        self.run_reports([report('prerequisite-restart-required'), report('window-ready', run=2)], calls)
        self.assertEqual([call.kwargs['restart'] for call in calls.call_args_list], [False, True])

    def test_does_not_restart_for_other_outcomes(self):
        for outcome in ['install-path-error', 'dependency-install-incomplete', 'exited-during-startup', 'window-ready']:
            with self.subTest(outcome=outcome):
                calls = Mock()
                self.run_reports([report(outcome)], calls)
                self.assertEqual(calls.call_count, 1)

    def test_second_restart_request_or_stale_result_fails(self):
        first = report('prerequisite-restart-required')
        for second in [report('prerequisite-restart-required', run=2), report('window-ready', run=1)]:
            calls = Mock()
            with self.assertRaises(ValueError):
                self.run_reports([first, second], calls)
            self.assertEqual(calls.call_count, 2)

    def test_wrong_or_nonrepeat_game_cannot_trigger_restart(self):
        for first in [report('prerequisite-restart-required', app=105600), 'status=steam-core-complete\r\n']:
            calls = Mock()
            with self.assertRaises(ValueError):
                self.run_reports([first], calls)
            self.assertEqual(calls.call_count, 1)
