"""Correlate a fresh console report with the selected probe's checks."""

from dataclasses import dataclass
import time


EVIDENCE_FIELDS = {
    'probe', 'run_id', 'status', 'create_process', 'win32_error',
    'child_exit_code', 'expected_exit_code', 'exit_code_matches',
    'ipc', 'ipc_stage', 'ipc_error', 'ipc_child_exit', 'exit_code',
    'steam_core', 'create_interface', 'steam_error',
    'hresult', 'relative_motion', 'uncaptured_motion', 'motion', 'callbacks', 'capture_hresult',
    'hide_cursor_hresult', 'release_capture_hresult', 'restore_cursor_hresult', 'remove_mouse_moved_hresult',
    'activation_start', 'activation_end', 'core_window_hresult', 'mouse_subscription_hresult',
    'mouse_capabilities_hresult', 'mouse_present', 'mouse_button_count',
    'document_visible', 'pointer_input', 'pointer_moves', 'pointer_down', 'pointer_wheel',
    'visible', 'registered', 'start_tick', 'hr', 'stage', 'presented',
}


def report_fields(report):
    return dict(line.split('=', 1) for line in report.splitlines() if '=' in line)


@dataclass(frozen=True)
class ProbeResult:
    outcome: str
    message: str
    report: str = ''

    def evidence(self):
        return {key: value for key, value in report_fields(self.report).items() if key in EVIDENCE_FIELDS}


def run_case(case, read_report, launch, timeout, progress):
    baseline = read_report()
    baseline_run = report_fields(baseline).get('run_id')
    launch_failed = launch()
    deadline = time.monotonic() + timeout
    last_report = ''
    while time.monotonic() < deadline:
        report = read_report(polling=True)
        if report.endswith('\n') and report not in (baseline, last_report):
            fields = report_fields(report)
            if case.accepts(fields, baseline_run):
                last_report = report
                progress(report)
                failure = case.failure(fields)
                if failure:
                    return ProbeResult('failed', failure, report)
                missing = [key for key, expected in case.checks if fields.get(key) != expected]
                if not missing:
                    return ProbeResult('passed', '', report)
                if case.terminal_status and fields.get('status') == case.terminal_status:
                    return ProbeResult('failed', 'Required evidence did not match: ' + ', '.join(missing), report)
        time.sleep(0.5)

    if last_report:
        fields = report_fields(last_report)
        if ('ipc', 'success') in case.checks and fields.get('exit_code_matches') == 'yes':
            return ProbeResult('timeout', 'The signed helper ran, but no terminal named-pipe result appeared.',
                               last_report)
        return ProbeResult('timeout', 'The probe wrote a startup checkpoint but did not finish the test.', last_report)
    message = f'No fresh {case.filename} appeared in LocalState.'
    if launch_failed:
        message += ' The remote launch also failed.'
    return ProbeResult('timeout', message)
