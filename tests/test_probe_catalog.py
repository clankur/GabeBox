import io
import json
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest.mock import Mock, patch

from probe.catalog import CASES, CONTROLLER_POINTER_PROBE_NAME, NATIVE_MOUSE_PROBE_NAME, PACKAGE_CASES
from probe.runner import run_case
from tools import xbox_deploy


def process_report(run_id, result):
    return f'probe=packaged-child-process\nrun_id={run_id}\n{result}\n'


class ProbeRunnerTests(unittest.TestCase):
    def test_named_pipe_ignores_previous_run_and_waits_for_both_checks(self):
        stale = process_report('1', 'exit_code_matches=yes\nipc=success')
        start = process_report('2', 'exit_code_matches=yes')
        finish = process_report('2', 'exit_code_matches=yes\nipc=success')
        read = Mock(side_effect=[process_report('1', 'status=starting'), stale, start, finish])
        launch = Mock(return_value=False)
        progress = Mock()
        with patch('probe.runner.time.sleep'):
            result = run_case(CASES['named-pipe'], read, launch, 20, progress)
        self.assertEqual(result.outcome, 'passed')
        self.assertEqual(result.evidence()['run_id'], '2')
        self.assertEqual([call.args[0] for call in progress.call_args_list], [start, finish])

    def test_partial_report_times_out_and_preserves_the_first_check(self):
        start = process_report('2', 'exit_code_matches=yes')
        read = Mock(side_effect=['', start])
        with patch('probe.runner.time.sleep'), patch('probe.runner.time.monotonic', side_effect=[0, 0, 2]):
            result = run_case(CASES['named-pipe'], read, lambda: False, 1, Mock())
        self.assertEqual(result.outcome, 'timeout')
        self.assertIn('no terminal named-pipe result', result.message)
        self.assertEqual(result.evidence()['exit_code_matches'], 'yes')

    def test_probe_catalog_is_available_without_portal_settings(self):
        output = io.StringIO()
        with patch('sys.argv', ['xbox_deploy', 'probe', '--list', '--json']), redirect_stdout(output):
            with patch.object(xbox_deploy, 'load_settings') as settings:
                xbox_deploy.main()
        self.assertEqual(set(json.loads(output.getvalue())), set(CASES))
        settings.assert_not_called()

    def test_steam_loader_distinguishes_completion_from_loading(self):
        report = ('probe=packaged-steam-core\nrun_id=3\nsteam_core=load-error\n'
                  'steam_error=0x0000007E\nstatus=steam-core-complete\n')
        for case, outcome in [('steam-core', 'passed'), ('steam-loader', 'failed')]:
            with self.subTest(case=case):
                result = run_case(CASES[case], Mock(side_effect=['', report]), lambda: False, 1, Mock())
                self.assertEqual(result.outcome, outcome)
                self.assertEqual(result.evidence()['steam_error'], '0x0000007E')

    def test_native_mouse_readiness_waits_for_a_fresh_foreground_report(self):
        def report(run_id, status, visible):
            return (f'probe=native-mouse-capture\nrun_id={run_id}\nstatus={status}\n'
                    f'visible={visible}\nregistered=0x3f\n')

        stale = report('1', 'mouse-capture-ready', '1')
        start = report('2', 'listening', '0')
        ready = report('2', 'mouse-capture-ready', '1')
        read = Mock(side_effect=[stale, stale, start, ready])
        progress = Mock()
        with patch('probe.runner.time.sleep'):
            result = run_case(CASES['native-mouse-ready'], read, lambda: False, 20, progress)
        self.assertEqual(result.outcome, 'passed')
        self.assertEqual(result.evidence()['run_id'], '2')
        self.assertEqual([call.args[0] for call in progress.call_args_list], [start, ready])

    def test_independent_mouse_readiness_checks_present_and_subscriptions(self):
        case = CASES['native-independent-mouse-ready']
        ready = ('probe=native-independent-mouse\nrun_id=2\nstatus=independent-mouse-ready\n'
                 'visible=1\npresented=1\nregistered=0xf\n')
        for present, subscriptions, outcome in [('1', '0xf', 'passed'), ('0', '0xf', 'failed'),
                                                  ('1', '0x7', 'failed')]:
            with self.subTest(present=present, subscriptions=subscriptions):
                report = ready.replace('presented=1', f'presented={present}').replace(
                    'registered=0xf', f'registered={subscriptions}')
                result = run_case(case, Mock(side_effect=['', report]), lambda: False, 1, Mock())
                self.assertEqual(result.outcome, outcome)
                self.assertEqual(result.evidence()['presented'], present)
        self.assertEqual(PACKAGE_CASES[NATIVE_MOUSE_PROBE_NAME], 'native-mouse-ready')

    def test_controller_pointer_waits_for_a_fresh_foreground_sample(self):
        def report(run_id, status, visible):
            return (f'run_id={run_id}\nprobe=controller-pointer\nstatus={status}\nvisible={visible}\n'
                    'raw_controllers=1\ngamepads=1\n')

        stale = report('1', 'controller-sampling', '1')
        start = report('2', 'controller-starting', '0')
        ready = report('2', 'controller-sampling', '1')
        read = Mock(side_effect=[stale, stale, start, ready])
        with patch('probe.runner.time.sleep'):
            result = run_case(CASES['controller-pointer'], read, lambda: False, 20, Mock())
        self.assertEqual(result.outcome, 'passed')
        self.assertEqual(result.evidence()['raw_controllers'], '1')
        self.assertEqual(PACKAGE_CASES[CONTROLLER_POINTER_PROBE_NAME], 'controller-pointer')

    def test_controller_pointer_fails_on_a_startup_error(self):
        error = 'run_id=3\nprobe=controller-pointer\nstatus=controller-error\nhr=80004005\n'
        result = run_case(CASES['controller-pointer'], Mock(side_effect=['', error]), lambda: False, 1, Mock())
        self.assertEqual(result.outcome, 'failed')

    def test_json_result_records_the_selected_case_and_failure(self):
        portal = Mock()
        last = process_report('2', 'exit_code_matches=yes\nipc=failed\nipc_stage=reply')
        portal.request.side_effect = [(200, b''), (200, b''), (200, last.encode())]
        package = 'XboxPortabilityProbe.ChildProcess_1.0.12.0_x64__fixture'
        app = {'PackageFullName': package, 'PackageRelativeId': 'fixture!App'}
        output = io.StringIO()
        with patch.object(xbox_deploy, 'find_installed_probe', return_value=app):
            with redirect_stdout(output), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                xbox_deploy.collect_probe(portal, 'named-pipe', '1.0.12.0', False, 20, json_output=True)
        self.assertEqual(error.exception.code, 1)
        evidence = json.loads(output.getvalue())
        self.assertEqual((evidence['case'], evidence['package'], evidence['outcome']),
                         ('named-pipe', package, 'failed'))
        self.assertEqual(evidence['observed']['ipc_stage'], 'reply')
