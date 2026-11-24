"""Named Xbox probe cases composed from console report checks."""

from dataclasses import dataclass


PROBE_NAME = 'XboxPortabilityProbe.ChildProcess'
PROBE_CONTROL_NAME = PROBE_NAME + '.Control'
GRAPHICS_CONTROL_NAME = PROBE_NAME + '.GraphicsControl'
PROBE_STEAM_CORE_NAME = PROBE_NAME + '.SteamCore'
BIG_PICTURE_NAME = 'XboxPortabilityProbe.BigPicture'
PROBE_REPORT = 'probe-report.txt'
BIG_PICTURE_REPORT = 'big-picture-report.txt'

CHILD_EXIT = (('exit_code_matches', 'yes'),)
PIPE_EXCHANGE = CHILD_EXIT + (('ipc', 'success'),)


@dataclass(frozen=True)
class ProbeCase:
    package: str
    label: str
    description: str
    checks: tuple[tuple[str, str], ...]
    filename: str = PROBE_REPORT
    report_kind: str = ''
    require_run_id: bool = False
    terminal_status: str = ''

    def accepts(self, fields, baseline_run):
        run_id = fields.get('run_id')
        return (
            (not self.report_kind or fields.get('probe') == self.report_kind)
            and (not self.require_run_id or bool(run_id))
            and (not run_id or run_id != baseline_run)
        )

    def failure(self, fields):
        if CHILD_EXIT[0] in self.checks and fields.get('exit_code_matches') == 'no':
            return 'The probe ran, but the helper did not return its expected exit code.'
        if ('ipc', 'success') in self.checks and fields.get('ipc') == 'failed':
            return 'The signed helper ran, but the named-pipe exchange did not succeed.'
        status = fields.get('status', '')
        if self.report_kind == 'native-steam-big-picture' and status == 'client-exited':
            return (f"The Steam launcher exited ({fields.get('exit_code', 'unknown')}). "
                    'Check the Xbox display for a possible handoff.')
        if status.endswith(('error', 'failed')):
            return 'The probe reported a startup or test error above.'
        return ''


CASES = {
    'activation': ProbeCase(
        PROBE_CONTROL_NAME, 'Packaged process startup control',
        'Start the HTML control and write a LocalState report.',
        (('status', 'control-ready'),), report_kind='packaged-startup-control', require_run_id=True,
    ),
    'process': ProbeCase(
        PROBE_NAME, 'Packaged process test',
        'Run the packaged helper and verify its exit code.',
        CHILD_EXIT, report_kind='packaged-child-process',
    ),
    'named-pipe': ProbeCase(
        PROBE_NAME, 'Packaged process test',
        'Verify the helper exit code and a named-pipe request and reply.',
        PIPE_EXCHANGE, report_kind='packaged-child-process', require_run_id=True,
    ),
    'graphics-control': ProbeCase(
        GRAPHICS_CONTROL_NAME, 'Graphics runtime control',
        'Check the result of the graphics helper in the selected package build.',
        CHILD_EXIT, report_kind='packaged-child-process', require_run_id=True,
    ),
    'steam-core': ProbeCase(
        PROBE_STEAM_CORE_NAME, 'Steam Core Loader Probe',
        'Wait for the Steam Core diagnostic to finish.',
        (('status', 'steam-core-complete'),),
    ),
    'steam-loader': ProbeCase(
        PROBE_STEAM_CORE_NAME, 'Steam Core Loader Probe',
        'Load the packaged Steam client DLL and resolve CreateInterface.',
        (('status', 'steam-core-complete'), ('steam_core', 'loaded'), ('create_interface', 'present')),
        report_kind='packaged-steam-core', require_run_id=True, terminal_status='steam-core-complete',
    ),
    'big-picture': ProbeCase(
        BIG_PICTURE_NAME, 'Steam Big Picture',
        'Check that the native Steam launcher is still running at the observation deadline.',
        (('status', 'client-running'),), filename=BIG_PICTURE_REPORT,
        report_kind='native-steam-big-picture', require_run_id=True,
    ),
}

PACKAGE_CASES = {case.package: key for key, case in CASES.items() if key not in ('named-pipe', 'steam-loader')}
