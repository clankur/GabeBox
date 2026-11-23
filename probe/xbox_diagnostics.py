"""Observe existing Steam reports and collect run-scoped Xbox artifacts."""

import json
from pathlib import PureWindowsPath
import re
import time
from urllib.parse import urlencode

from .fhd.measure import FILETIME_EPOCH, HEADER, PRESENT


REPORT_LIMIT = 64 * 1024
TRACE_LIMIT = 32 * 1024
FRAME_LIMIT = 8 * 1024 * 1024
BMP_LIMIT = 16 * 1024 * 1024
LOCAL = r'\LocalState'
TEMP = r'\TempState'
PRIMARY_REPORT = 'big-picture-primary-startup.txt'
PROBE_REPORT = 'probe-report.txt'
WEBVIEW_REPORT = 'big-picture-webview-report.txt'
COUNT = re.compile(r'[0-9]{1,20}')
CHECKPOINT = re.compile(r'[a-z][a-z0-9_.-]{0,62}(?::(?:0x[0-9a-f]{1,8}|[a-z][a-z0-9-]{0,40}))?', re.I)
TIMINGS = re.compile(r'[a-z][a-z0-9_.-]{0,63}:[0-9]{1,12}(?:,[a-z][a-z0-9_.-]{0,63}:[0-9]{1,12}){0,63}')
BROWSER_RUN = re.compile(r'[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}', re.I)
WINDOW_TRACE = re.compile(r'(?:win32|sdl)_[a-z0-9_]{1,72}=[a-z0-9_.=-]{1,72}', re.I)
LOADER_TRACE = re.compile(r'(?:load|export|fault|asset|dxgi|input) [a-zA-Z0-9 ._?@$#=-]{1,174}')
LIFECYCLE = re.compile(r'pid=[0-9]{1,10} stage=[a-z-]{1,48} hresult=0x[0-9a-f]{8}'
                       r'(?: (?:tick_ms|old_bytes|new_bytes|usage_bytes|view_id|activation_mode|activation_state|'
                       r'visible|user_initiated|app_initiated)=-?[0-9]{1,20}){0,12}', re.I)
GAME_KEYS = {
    'steam_launch', 'steam_launch_phase', 'steam_launch_backend', 'steam_launch_client_reason',
    'steam_shadow_stage', 'steam_shadow_child', 'steam_shadow_renderer', 'steam_shadow_godot_shader_cache',
    'steam_sdl_host', 'steam_sdl_host_frame', 'steam_sdl_opengl_gpu_mailbox', 'steam_dos',
}
GAME_CODES = {'steam_shadow_stage_hresult', 'steam_shadow_win32', 'steam_shadow_exit',
              'steam_launch_process_exit', 'steam_shadow_child_exit_u32', 'steam_shadow_child_exit_error_u32'}
FRAME_FIELDS = {
    'width', 'height', 'visible', 'presented', 'gpu_presented', 'cpu_presented', 'primary_panel',
    'source_sequence', 'gpu_sequence', 'cpu_sequence', 'sample_us', 'sample_frames', 'sample_gpu_sequence',
    'sample_render_callbacks', 'sample_source_empty_callbacks', 'sample_present_busy_callbacks',
    'slots_busy', 'pool_waits', 'source_empty_callbacks', 'presentation_busy_callbacks',
}


def fields(report):
    return dict(line.split('=', 1) for line in report.splitlines() if '=' in line)


def numeric(report, key):
    value = report.get(key, '')
    return int(value) if COUNT.fullmatch(value) else None


def select_fields(report, match, value_pattern):
    return {key: value for key, value in report.items()
            if match(key) and value_pattern.fullmatch(value)}


def startup_snapshot(probe, browser):
    native = select_fields(probe, lambda key: re.fullmatch(r'steam_(?:native_startup_[a-z_]{1,48}_ms|'
                            r'library_(?:named|visible|unnamed_[a-z_]{1,40}|count))', key), COUNT)
    web = select_fields(browser, {'status', 'valve_startup', 'valve_surface',
                                  'startup_movie_override_js'}.__contains__, CHECKPOINT)
    web.update(select_fields(browser, {'owned_games_served'}.__contains__, COUNT))
    web.update(select_fields(browser, {'startup_elapsed_ms', 'startup_service_elapsed_ms',
                                       'startup_cloud_cache_reads'}.__contains__, TIMINGS))
    warmup = browser.get('warmup', '')
    if re.fullmatch(r'[a-z-]{1,64} elapsed_ms=[0-9]{1,12}', warmup):
        web['warmup'] = warmup
    return {'probe': native, 'webview': web}


def game_snapshot(probe, browser, exit_report, app_id):
    native = select_fields(probe, GAME_KEYS.__contains__, CHECKPOINT)
    native.update(select_fields(probe, GAME_CODES.__contains__, re.compile(r'(?:0x[0-9a-f]{1,8}|[0-9]{1,10})', re.I)))
    native.update(select_fields(exit_report, {'steam_shadow_child_status'}.__contains__, CHECKPOINT))
    native.update(select_fields(exit_report, GAME_CODES.__contains__, re.compile(r'[0-9]{1,10}')))
    web = select_fields(browser, lambda key: re.fullmatch(r'memory_(?:handoff|first_frame)_(?:usage|limit)_bytes', key), COUNT)
    web.update(select_fields(browser, {'status'}.__contains__, CHECKPOINT))
    for name in ('first', 'sample', 'latest_sample', 'fallback'):
        frame = browser.get('game_frame_' + name, '')
        if frame.startswith(f'appid={app_id} '):
            frame_fields = fields(frame.replace(' ', '\n'))
            web['game_frame_' + name] = select_fields(frame_fields, FRAME_FIELDS.__contains__, COUNT)
    return {'probe': native, 'webview': web}


def recent_trace(raw, launch_id):
    if len(raw) <= HEADER.size or (len(raw) - HEADER.size) % PRESENT.size:
        return False
    magic, version, frequency, utc, _ = HEADER.unpack_from(raw)
    return (magic == 0x31444846 and version in (1, 2) and frequency > 0
            and utc >= FILETIME_EPOCH + launch_id * 10_000)


def filtered_lines(report, pattern, count=128):
    return [line for line in (report or '').splitlines() if pattern.fullmatch(line)][:count]


def embedded_loader_lines(report):
    prefix = 'steam_shadow_loader='
    lines = (line[len(prefix):] for line in (report or '').splitlines() if line.startswith(prefix))
    return [line for line in lines if LOADER_TRACE.fullmatch(line)][:128]


def named_exit_report(probe, app_id):
    filename = probe.get('steam_shadow_exit_report', '')
    return filename if re.fullmatch(rf'steam-win32-shadow-{app_id}-exit-[0-9]{{1,20}}\.txt', filename) else None


def matching_probe(probe, launch_id, app_id):
    return numeric(probe, 'steam_launch_run_id') == launch_id and numeric(probe, 'steam_launch_app_id') == app_id


def process_image_names(portal):
    _, raw = portal.request('GET', '/api/resourcemanager/processes', read_limit=1024 * 1024)
    items = json.loads(raw)['Processes']
    return {PureWindowsPath(item['ImageName']).name.casefold() for item in items}


def temporary_stages(portal, app):
    query = urlencode({'knownfolderid': 'LocalAppData', 'packagefullname': app['PackageFullName'], 'path': TEMP})
    _, raw = portal.request('GET', '/api/filesystem/apps/files?' + query, read_limit=1024 * 1024)
    return {(item['Name'], item['DateCreated']) for item in json.loads(raw)['Items']
            if re.fullmatch(r'SteamWin32Trial(?: \([0-9]{1,9}\))?', item.get('Name', ''))
            and type(item.get('DateCreated')) is int}


def finish_reason(probe, exit_report):
    if exit_report.get('steam_shadow_child_status') == 'exited' or 'steam_shadow_exit' in probe:
        return 'game-exited'
    if 'steam_launch_process_exit' in probe:
        return 'game-exited'
    if 'steam_shadow_stage_hresult' in probe or 'steam_shadow_win32' in probe:
        return 'launch-failed'
    if probe.get('status') != 'steam-core-complete':
        return None
    if probe.get('steam_launch') == 'window-ready' or probe.get('steam_dos') == 'guest-started':
        return 'window-ready'
    if probe.get('steam_sdl_host_frame') == 'presented':
        return None
    if probe.get('steam_launch') or probe.get('steam_sdl_host'):
        return 'launch-ended'
    return None


def save_observation(output, result, artifacts):
    if output is None:
        return
    output.mkdir(mode=0o700)
    with (output / 'observation.json').open('x', encoding='utf-8') as file:
        json.dump(result, file, indent=2)
        file.write('\n')
    for name, data in artifacts.items():
        with (output / name).open('xb') as file:
            file.write(data)


def collect_artifacts(report, blob, last_probe, launch_id, app_id, include_frame, baseline_primary, loader_lines):
    artifacts = {}
    primary = report(PRIMARY_REPORT)
    if primary and primary != baseline_primary:
        lines = filtered_lines(primary, LIFECYCLE)
        if lines:
            artifacts['primary-lifecycle.txt'] = ('\n'.join(lines) + '\n').encode()
    if launch_id is None:
        return artifacts

    sdl_run = numeric(last_probe, 'steam_launch_sdl_run')
    if sdl_run:
        checkpoints = filtered_lines(report(f'sdl-startup-{sdl_run}.txt'), WINDOW_TRACE)
        if checkpoints:
            artifacts['sdl-startup.txt'] = ('\n'.join(checkpoints) + '\n').encode()
        if include_frame:
            image = blob(f'sdl-startup-{sdl_run}.bmp', BMP_LIMIT) or b''
            if image.startswith(b'BM'):
                artifacts['bootstrap.bmp'] = image
    trace = blob('fhd-present-times.bin', FRAME_LIMIT) or b''
    if recent_trace(trace, launch_id):
        artifacts['fhd-present-times.bin'] = trace

    current_probe = fields(report(PROBE_REPORT) or '')
    if not matching_probe(current_probe, launch_id, app_id):
        # The fixed frame filename is shared with a later launch and could have been reset.
        artifacts.pop('fhd-present-times.bin', None)
    if loader_lines:
        artifacts['loader-trace.txt'] = ('\n'.join(list(dict.fromkeys(loader_lines))[:128]) + '\n').encode()
    return artifacts


def observe(args, portal, app, read_file, decode_report, portal_error, *,
            clock=time.monotonic, wall_clock=time.time, pause=time.sleep, progress=None):
    def blob(filename, limit, folder=LOCAL):
        try:
            raw = read_file(portal, app, folder, filename, limit + 1) or b''
        except portal_error as error:
            if error.status == 409:
                return None
            if error.status == 404:
                return b''
            raise
        return raw if len(raw) <= limit else b''

    def report(filename, limit=REPORT_LIMIT, folder=LOCAL):
        raw = blob(filename, limit, folder)
        if raw is None:
            return None
        text = decode_report(raw)
        return text if text.endswith('\n') else ''

    def stages():
        try:
            return temporary_stages(portal, app)
        except portal_error as error:
            if error.status not in (404, 409):
                raise
            return None

    deadline = clock() + args.timeout
    earliest_id = int(wall_clock() * 1000) - 5000
    initial_probe = initial_browser = None
    while clock() < deadline and (initial_probe is None or initial_browser is None):
        if initial_probe is None:
            initial_probe = report(PROBE_REPORT)
        if initial_browser is None:
            initial_browser = report(WEBVIEW_REPORT)
        if initial_probe is None or initial_browser is None:
            pause(min(1, max(0, deadline - clock())))
    initial_probe = fields(initial_probe or '')
    initial_browser = fields(initial_browser or '')
    prior_run = numeric(initial_probe, 'run_id' if args.startup else 'steam_launch_run_id')
    baseline_primary = report(PRIMARY_REPORT) if args.output else ''
    baseline_stages = stages() if args.output and args.app_id and not args.run_id else None
    game_run = args.run_id
    last_probe = {}
    last_snapshot = None
    loader_lines = []
    direct_loader_lines = []
    trial_stage = None
    last_browser = {}
    first_frame = None
    process_seen = False
    process_missing = None
    outcome = 'observation-timeout'
    last_state = None
    if progress and clock() < deadline:
        progress('waiting-for-startup' if args.startup else 'attaching-to-launch' if args.run_id else 'waiting-for-launch')

    def fresh(run):
        return run is not None and (run > prior_run if prior_run is not None else run >= earliest_id)

    while clock() < deadline:
        probe_text = report(PROBE_REPORT)
        probe = fields(probe_text or '')
        browser_text = report(WEBVIEW_REPORT)
        if browser_text:
            last_browser = fields(browser_text)
        browser = last_browser
        if args.startup:
            native_run = numeric(probe, 'run_id')
            web_run = browser.get('run_id', '')
            if (fresh(native_run)
                    and BROWSER_RUN.fullmatch(web_run) and web_run != initial_browser.get('run_id')):
                snapshot = startup_snapshot(probe, browser)
                last_probe = probe
                last_snapshot = snapshot
                if browser.get('status') in ('gamepad-ready', 'setup-ready'):
                    outcome = browser['status']
                    break
        else:
            seen_run = numeric(probe, 'steam_launch_run_id')
            if (not game_run and fresh(seen_run)
                    and matching_probe(probe, seen_run, args.app_id)):
                game_run = seen_run
            if game_run and matching_probe(probe, game_run, args.app_id):
                last_probe = probe
                exit_name = named_exit_report(probe, args.app_id)
                exit_text = report(exit_name) if exit_name else ''
                exit_report = fields(exit_text or '')
                new_browser = {key: value for key, value in browser.items() if initial_browser.get(key) != value}
                snapshot = game_snapshot(probe, new_browser, exit_report, args.app_id)
                if args.image_name:
                    active = args.image_name.casefold() in process_image_names(portal)
                    snapshot['game_process_listed'] = active
                    process_seen |= active
                    process_missing = None if active or not process_seen else (
                        process_missing if process_missing is not None else clock())
                if args.output and baseline_stages is not None and probe.get('steam_shadow_stage') == 'verified-local-copy':
                    current = stages()
                    new_stages = current - baseline_stages if current is not None else set()
                    if trial_stage is None and len(new_stages) == 1:
                        trial_stage = next(iter(new_stages))
                    if trial_stage in new_stages:
                        folder = TEMP + '\\' + trial_stage[0]
                        lines = filtered_lines(report('loader-trace.txt', TRACE_LIMIT, folder), LOADER_TRACE)
                        loader_lines = lines or loader_lines
                if args.output:
                    lines = embedded_loader_lines(probe_text) + embedded_loader_lines(exit_text)
                    direct_loader_lines = lines or direct_loader_lines
                last_snapshot = snapshot
                finished = finish_reason(probe, exit_report)
                if finished:
                    outcome = finished
                    break
                if process_missing is not None and clock() - process_missing >= 3:
                    outcome = 'game-process-gone'
                    break
                if probe.get('steam_sdl_host_frame') == 'presented' and first_frame is None:
                    first_frame = clock()
                if first_frame is not None and clock() - first_frame >= args.frame_seconds:
                    outcome = 'frame-observed'
                    break
            elif game_run and seen_run is not None and seen_run > game_run:
                outcome = 'launch-superseded'
                break
        if last_snapshot is not None and progress:
            state = last_snapshot['webview'].get('status') or last_snapshot['probe'].get('steam_launch_phase')
            if state and state != last_state:
                progress(state)
                last_state = state
        pause(min(1, max(0, deadline - clock())))

    result = {'version': args.version, 'mode': 'startup' if args.startup else 'game', 'outcome': outcome}
    if last_snapshot is not None:
        result.update(last_snapshot)
        run_id = numeric(last_probe, 'run_id') if args.startup else game_run
        result['run_id'] = run_id
    if not args.startup:
        result['app_id'] = args.app_id
    if args.run_id:
        result['requested_run_id'] = args.run_id
    if args.output:
        artifacts = (collect_artifacts(report, blob, last_probe, game_run if last_probe and not args.startup else None,
                                      args.app_id, args.bootstrap_frame, baseline_primary, loader_lines + direct_loader_lines)
                     if last_probe else {})
        result['artifacts'] = {name: len(data) for name, data in artifacts.items()}
        save_observation(args.output, result, artifacts)
    return result
