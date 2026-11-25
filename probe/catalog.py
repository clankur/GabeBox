"""Named Xbox probe cases composed from console report checks."""

from dataclasses import dataclass


PROBE_NAME = 'XboxPortabilityProbe.ChildProcess'
PROBE_CONTROL_NAME = PROBE_NAME + '.Control'
GRAPHICS_CONTROL_NAME = PROBE_NAME + '.GraphicsControl'
PROBE_STEAM_CORE_NAME = PROBE_NAME + '.SteamCore'
BIG_PICTURE_NAME = 'XboxPortabilityProbe.BigPicture'
NATIVE_MOUSE_PROBE_NAME = 'XboxPortabilityProbe.NativeMouseSource'
PROBE_REPORT = 'probe-report.txt'
BIG_PICTURE_REPORT = 'big-picture-report.txt'
WEBVIEW_REPORT = 'big-picture-webview-report.txt'
NATIVE_MOUSE_CAPTURE_REPORT = 'mouse-capture.txt'
NATIVE_MOUSE_DEVICE_REPORT = 'mouse-gameinput.txt'
NATIVE_MOUSE_INDEPENDENT_REPORT = 'mouse-independent.txt'
NATIVE_MOUSE_ROUTE_REPORT = 'mouse-routes.txt'
NATIVE_MOUSE_STARTUP_REPORT = 'mouse-startup.txt'

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
    'mouse-source': ProbeCase(
        PROBE_NAME + '.MouseSource', 'UWP mouse source control',
        'Detect nonzero relative mouse motion in the foreground CoreWindow.',
        (('relative_motion', 'observed'),), report_kind='uwp-mouse-source',
        require_run_id=True, terminal_status='mouse-sample-complete',
    ),
    'native-mouse-ready': ProbeCase(
        NATIVE_MOUSE_PROBE_NAME, 'Native Xbox Mouse Probe',
        'Verify the standalone XAML app registered native mouse events and reached the foreground.',
        (('status', 'mouse-capture-ready'), ('visible', '1'), ('registered', '0x3f')),
        filename=NATIVE_MOUSE_STARTUP_REPORT, report_kind='native-mouse-capture', require_run_id=True,
        terminal_status='mouse-capture-ready',
    ),
    'native-independent-mouse-ready': ProbeCase(
        NATIVE_MOUSE_PROBE_NAME, 'Independent Xbox Mouse Probe',
        'Verify a presented SwapChainPanel and its independent mouse event subscriptions.',
        (('status', 'independent-mouse-ready'), ('visible', '1'), ('presented', '1'), ('registered', '0xf')),
        filename=NATIVE_MOUSE_INDEPENDENT_REPORT, report_kind='native-independent-mouse', require_run_id=True,
        terminal_status='independent-mouse-ready',
    ),
    'native-mouse-routes': ProbeCase(
        NATIVE_MOUSE_PROBE_NAME, 'Xbox mouse route matrix',
        'Verify the standalone app enumerated every physical mouse delivery route in the foreground.',
        (('stage', 'routes-ready'),), filename=NATIVE_MOUSE_ROUTE_REPORT, report_kind='native-mouse-routes',
        require_run_id=True, terminal_status='routes-ready',
    ),
    'graphics-control': ProbeCase(
        GRAPHICS_CONTROL_NAME, 'Graphics runtime control',
        'Check the result of the graphics helper in the selected package build.',
        CHILD_EXIT, report_kind='packaged-child-process', require_run_id=True,
    ),
    'wasapi-hosted': ProbeCase(
        GRAPHICS_CONTROL_NAME, 'Hosted WASAPI render control',
        'Check default render discovery, activation, and stream progress.',
        CHILD_EXIT, report_kind='packaged-child-process', require_run_id=True,
    ),
    'profile-folders': ProbeCase(
        GRAPHICS_CONTROL_NAME, 'Win32 profile folder control',
        'Check scoped Documents and Roaming AppData paths and file round trips.',
        CHILD_EXIT, report_kind='packaged-child-process', require_run_id=True,
    ),
    'steam-sdk-versioned': ProbeCase(
        GRAPHICS_CONTROL_NAME, 'Versioned Steam SDK control',
        'Check caller-selected Steamworks versions, SDK results, and account validation.',
        CHILD_EXIT, report_kind='packaged-child-process', require_run_id=True,
    ),
    'dxgi-present-commit': ProbeCase(
        GRAPHICS_CONTROL_NAME, 'DXGI accepted-frame control',
        'Check that only successful presents publish a shared GPU frame.',
        CHILD_EXIT, report_kind='packaged-child-process', require_run_id=True,
    ),
    'dxgi-output-native': ProbeCase(
        GRAPHICS_CONTROL_NAME, 'Native DXGI output and mode control',
        'Check native adapter output enumeration and ANSI display mode lookup.',
        CHILD_EXIT, report_kind='packaged-child-process', require_run_id=True,
    ),
    'dxgi-output-hosted': ProbeCase(
        GRAPHICS_CONTROL_NAME, 'Hosted ANSI display mode control',
        'Check hosted ANSI display settings with a virtual monitor and a native DXGI output.',
        CHILD_EXIT, report_kind='packaged-child-process', require_run_id=True,
    ),
    'd3d11-hosted-output': ProbeCase(
        GRAPHICS_CONTROL_NAME, 'Hosted D3D11 explicit adapter control',
        'Check native or hosted output selection and explicit hardware device creation.',
        CHILD_EXIT, report_kind='packaged-child-process', require_run_id=True,
    ),
    'dxgi-d3d11-resize': ProbeCase(
        GRAPHICS_CONTROL_NAME, 'Hosted D3D11 swap-chain resize control',
        'Check frame continuity after a rejected resize and a new GPU generation after a successful resize.',
        CHILD_EXIT, report_kind='packaged-child-process', require_run_id=True,
    ),
    'd3d11-nonblocking-present': ProbeCase(
        GRAPHICS_CONTROL_NAME, 'Hosted D3D11 nonblocking present control',
        'Check Present and Present1 rejection while a resize holds the hosted chain lock.',
        CHILD_EXIT, report_kind='packaged-child-process', require_run_id=True,
    ),
    'window-geometry': ProbeCase(
        GRAPHICS_CONTROL_NAME, 'Hosted window geometry and style control',
        'Check committed client geometry, position messages, and window style transitions.',
        CHILD_EXIT, report_kind='packaged-child-process', require_run_id=True,
    ),
    'window-frame': ProbeCase(
        GRAPHICS_CONTROL_NAME, 'Hosted window frame and DPI control',
        'Check scoped DPI lookup and exact client dimensions when switching to a windowed style.',
        CHILD_EXIT, report_kind='packaged-child-process', require_run_id=True,
    ),
    'window-keyboard': ProbeCase(
        GRAPHICS_CONTROL_NAME, 'Hosted window keyboard control',
        'Check mailbox key and Unicode text delivery, key state and focus reset.',
        CHILD_EXIT, report_kind='packaged-child-process', require_run_id=True,
    ),
    'wgl-loader': ProbeCase(
        GRAPHICS_CONTROL_NAME, 'Dynamic hosted WGL control',
        'Check hosted pixel formats, OpenGL export selection, and core-context rendering.',
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

PACKAGE_CASES = {
    case.package: key for key, case in CASES.items()
    if key not in ('named-pipe', 'native-independent-mouse-ready', 'native-mouse-routes', 'steam-loader',
                   'wasapi-hosted', 'profile-folders', 'steam-sdk-versioned',
                   'dxgi-present-commit', 'dxgi-output-native', 'dxgi-output-hosted', 'd3d11-hosted-output',
                   'dxgi-d3d11-resize', 'd3d11-nonblocking-present', 'window-geometry', 'window-frame',
                   'window-keyboard', 'wgl-loader')
}
