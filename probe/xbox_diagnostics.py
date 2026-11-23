"""Observe existing Steam reports and collect run-scoped Xbox artifacts."""

import json
from pathlib import PureWindowsPath
import re
import struct
import time
from urllib.parse import urlencode

from .fhd.measure import FILETIME_EPOCH, HEADER, PRESENT


REPORT_LIMIT = 64 * 1024
TRACE_LIMIT = 32 * 1024
FRAME_LIMIT = 8 * 1024 * 1024
LIVE_LOADER_LIMIT = 1024 * 1024
EXIT_TRACE_WAIT_SECONDS = 3
TRIAL_CREATION_WINDOW_MS = 5 * 60 * 1000
BMP_MAX_SIDE = 4096
BMP_LIMIT = BMP_MAX_SIDE * BMP_MAX_SIDE * 4 + 1024
GPU_MAGIC = 0x31475053
# See SdlGpuMailbox. The cursor storage is part of the file but is omitted
# from diagnostic output, along with the adapter LUID and shared handle names.
GPU_HEADER = struct.Struct('<Ii8xIIII')
GPU_STATUS = struct.Struct('<QQi8xI')
GPU_STATUS_OFFSET = GPU_HEADER.size + 2 * 3 * 64 * 2
GPU_MAILBOX_SIZE = GPU_STATUS_OFFSET + GPU_STATUS.size + 2 * (20 + 128 * 128 * 4)
GPU_MAILBOX_NAME = re.compile(r'sdl-host-[0-9]{1,10}-[0-9]{1,20}\.bin')
LOCAL = r'\LocalState'
TEMP = r'\TempState'
PRIMARY_REPORT = 'big-picture-primary-startup.txt'
PROBE_REPORT = 'probe-report.txt'
WEBVIEW_REPORT = 'big-picture-webview-report.txt'
COUNT = re.compile(r'[0-9]{1,20}')
CHECKPOINT = re.compile(r'[a-z][a-z0-9_.-]{0,62}(?::(?:0x[0-9a-f]{1,8}|[a-z][a-z0-9-]{0,40}))?', re.I)
TIMINGS = re.compile(r'[a-z][a-z0-9_.-]{0,63}:[0-9]{1,12}(?:,[a-z][a-z0-9_.-]{0,63}:[0-9]{1,12}){0,63}')
BROWSER_RUN = re.compile(r'[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}', re.I)
WINDOW_TRACE = re.compile(r'(?:win32|sdl)_[a-z0-9_]{1,72}=[a-z0-9_.=-]{1,72}', re.I | re.ASCII)
LOADER_TRACE = re.compile(r'(?:load|export|fault|asset|dxgi|input) [a-zA-Z0-9 ._?@$#=-]{1,174}')
LIVE_LOADER_EVENTS = {
    'graphics-pso-before': re.compile(r'asset graphics-pso-before(?: |$)'),
    'boot-wait': re.compile(r'asset boot-wait(?:[ =-]|$)'),
    'shell-request': re.compile(r'(?:asset|input) (?:boot-)?shell-request(?:[ =-]|$)'),
}
PIPELINE_MARKER = re.compile(r'asset (graphics|compute)-pso-(before|after) bytes=0x[0-9a-fA-F]{16} '
                             r'flags=0x([0-9a-fA-F]{8}) available-commit-kib=0x([0-9a-fA-F]{8})(?: .+)?')
PIPELINE_SAMPLE = re.compile(r'asset live-pipeline-(graphics|compute) bytes=0x([0-9a-fA-F]{16}) '
                             r'flags=0x([0-9a-fA-F]{8}) available-commit-kib=0x([0-9a-fA-F]{8})')
LIFECYCLE = re.compile(r'pid=[0-9]{1,10} stage=[a-z-]{1,48} hresult=0x[0-9a-f]{8}'
                       r'(?: (?:tick_ms|old_bytes|new_bytes|usage_bytes|view_id|activation_mode|activation_state|'
                       r'visible|user_initiated|app_initiated)=-?[0-9]{1,20}){0,12}', re.I)
GAME_KEYS = {
    'steam_launch', 'steam_launch_phase', 'steam_launch_backend', 'steam_launch_client_reason',
    'steam_shadow_stage', 'steam_shadow_child', 'steam_shadow_renderer', 'steam_shadow_godot_shader_cache',
    'steam_sdl_host', 'steam_sdl_host_frame', 'steam_sdl_opengl_gpu_mailbox', 'steam_dos',
    'steam_source_check', 'steam_shadow_documents', 'steam_shadow_companion_hashes',
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
    native.update(select_fields(probe, {'steam_source_build'}.__contains__, re.compile(r'[1-9][0-9]{0,19}')))
    source_file = probe.get('steam_source_file', '')
    if (len(source_file) <= 260 and all(re.fullmatch(r'[a-zA-Z0-9_. -]{1,255}', part) and not part.endswith(('.', ' '))
                                       for part in source_file.split('/'))):
        native['steam_source_file'] = source_file
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


def unique_loader_lines(lines):
    latest = {}
    for line in lines:
        if LOADER_TRACE.fullmatch(line):
            latest.pop(line, None)
            latest[line] = None
    return list(latest)[-128:]


def embedded_loader_lines(report):
    prefix = 'steam_shadow_loader='
    lines = (line[len(prefix):] for line in (report or '').splitlines() if line.startswith(prefix))
    return unique_loader_lines(lines)


def named_exit_report(probe, app_id):
    filename = probe.get('steam_shadow_exit_report', '')
    return filename if re.fullmatch(rf'steam-win32-shadow-{app_id}-exit-[0-9]{{1,20}}\.txt', filename) else None


def matching_probe(probe, launch_id, app_id):
    return numeric(probe, 'steam_launch_run_id') == launch_id and numeric(probe, 'steam_launch_app_id') == app_id


def bmp_frame_dimensions(data):
    if not 54 <= len(data) <= BMP_LIMIT or data[:2] != b'BM':
        return None
    size, offset = struct.unpack_from('<I4xI', data, 2)
    header, width, height, planes, bits, compression, image_size = struct.unpack_from('<IiiHHII', data, 14)
    if (size != len(data) or header not in (40, 52, 56, 108, 124) or not 14 + header <= offset < size
            or not 1 <= width <= BMP_MAX_SIDE or not 1 <= abs(height) <= BMP_MAX_SIDE or planes != 1
            or bits not in (24, 32) or compression not in (0, 3)):
        return None
    if compression == 3:
        if bits != 32 or offset < 66:
            return None
        red, green, blue = struct.unpack_from('<III', data, 54)
        if not all((red, green, blue)) or red & green or red & blue or green & blue:
            return None
    pixel_size = ((width * bits + 31) // 32 * 4) * abs(height)
    if size - offset != pixel_size or image_size not in (0, pixel_size):
        return None
    return width, abs(height)


def gpu_mailbox_snapshot(raw):
    if not raw:
        return {'state': 'unavailable'}
    if len(raw) != GPU_MAILBOX_SIZE:
        return {'state': 'invalid'}
    magic, generation, width, height, format_code, flip = GPU_HEADER.unpack_from(raw)
    if magic != GPU_MAGIC or generation < 0:
        return {'state': 'invalid'}
    if generation == 0:
        return {'state': 'inactive'}
    if generation % 2:
        return {'state': 'updating'}
    sequence_a, sequence_b, accepted, active = GPU_STATUS.unpack_from(raw, GPU_STATUS_OFFSET)
    if accepted not in (0, generation):
        return {'state': 'updating'}
    if not (1 <= width <= 16384 and 1 <= height <= 16384 and format_code in (28, 87)
            and flip in (0, 1) and active in (0, 1)):
        return {'state': 'invalid'}
    return {'state': 'published', 'width': width, 'height': height, 'dxgi_format': format_code,
            'flip_y': bool(flip), 'generation': generation, 'accepted_generation': accepted,
            'gpu_exchange_active': bool(active), 'producer_sequence': max(sequence_a, sequence_b)}


def diagnostic_reader(portal, app, read_file, decode_report, portal_error):
    def blob(filename, limit, folder=LOCAL):
        try:
            raw = read_file(portal, app, folder, filename, limit + 1)
        except portal_error as error:
            if error.status not in (404, 409):
                raise
            return None
        return raw

    def launch_report():
        raw = blob(PROBE_REPORT, REPORT_LIMIT)
        report = decode_report(raw) if raw and len(raw) <= REPORT_LIMIT else ''
        return fields(report) if report.endswith('\n') else {}

    return blob, launch_report


def collect_render_state(args, portal, app, read_file, decode_report, portal_error):
    blob, launch_report = diagnostic_reader(portal, app, read_file, decode_report, portal_error)
    before = launch_report()
    if not matching_probe(before, args.run_id, args.app_id):
        raise ValueError('The current Steam launch does not match the requested AppID and run ID.')
    mailbox = before.get('steam_sdl_mailbox', '')
    trace_id = numeric(before, 'steam_launch_sdl_run')
    result = {'version': args.version, 'mode': 'render', 'app_id': args.app_id, 'run_id': args.run_id}
    artifacts = {}
    if args.output:
        screenshots = {}
        for suffix, filename in (('', 'bootstrap.bmp'), ('-late', 'bootstrap-late.bmp'),
                                 ('-requested', 'requested.bmp')):
            data = blob(f'sdl-startup-{trace_id}{suffix}.bmp', BMP_LIMIT) if trace_id else None
            dimensions = bmp_frame_dimensions(data or b'')
            if dimensions is not None:
                artifacts[filename] = data
                screenshots[filename] = {'state': 'captured', 'width': dimensions[0], 'height': dimensions[1]}
            else:
                screenshots[filename] = {'state': 'invalid' if data else 'unavailable'}
        result['screenshots'] = screenshots
        if trace_id:
            trace = blob(f'sdl-startup-{trace_id}.txt', TRACE_LIMIT)
            if trace and len(trace) <= TRACE_LIMIT:
                complete, _, _ = decode_report(trace).rpartition('\n')
                checkpoints = filtered_lines(complete, WINDOW_TRACE)
                if checkpoints:
                    artifacts['sdl-startup.txt'] = ('\n'.join(checkpoints) + '\n').encode('ascii')
    if GPU_MAILBOX_NAME.fullmatch(mailbox):
        filename = mailbox + '.gpu'
        first = gpu_mailbox_snapshot(blob(filename, GPU_MAILBOX_SIZE))
        gpu = gpu_mailbox_snapshot(blob(filename, GPU_MAILBOX_SIZE))
        keys = ('state', 'generation', 'width', 'height', 'dxgi_format', 'flip_y')
        if any(first.get(key) != gpu.get(key) for key in keys):
            gpu = {'state': 'updating'}
    else:
        gpu = {'state': 'invalid' if mailbox else 'unavailable'}

    after = launch_report()
    if (not matching_probe(after, args.run_id, args.app_id)
            or any(before.get(key) != after.get(key)
                   for key in ('run_id', 'steam_sdl_mailbox', 'steam_launch_sdl_run'))):
        raise ValueError('The Steam launch changed during capture. No render state or screenshots were saved.')
    result['probe'] = game_snapshot(after, {}, {}, args.app_id)['probe']
    result['gpu_mailbox'] = gpu
    if args.output:
        result['artifacts'] = {name: len(data) for name, data in artifacts.items()}
        save_observation(args.output, result, artifacts)
    return result


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


def complete_loader_lines(data):
    # The writer can be appending. Keep complete ASCII records in order, including repeated events.
    complete, _, _ = data.rpartition(b'\n')
    rows = complete.decode('ascii', errors='replace').split('\n')
    records = (row.removesuffix('\r') for row in rows)
    return [record for record in records if LOADER_TRACE.fullmatch(record)]


def live_loader_snapshot(raw):
    data = (raw or b'')[:LIVE_LOADER_LIMIT]
    lines = complete_loader_lines(data)
    summary = {'state': 'observed' if lines else 'no-complete-records' if data else 'unavailable',
               'bytes_read': len(data), 'truncated': bool(raw and len(raw) > LIVE_LOADER_LIMIT),
               'records': len(lines), 'recent': lines[-12:], 'events': {}}
    for label, pattern in LIVE_LOADER_EVENTS.items():
        matching = [line for line in lines if pattern.match(line)]
        summary['events'][label] = {'count': len(matching), 'latest': matching[-4:]}
    pipelines = {}
    for line in lines:
        sample = PIPELINE_SAMPLE.fullmatch(line)
        if sample:
            kind, requests, owner, commit = sample.groups()
            pipelines.setdefault(kind, {})['latest_counter_sample'] = {
                'requests': int(requests, 16), 'compiler_active': bool(int(owner, 16)),
                'available_commit_kib': int(commit, 16),
            }
            continue
        match = PIPELINE_MARKER.fullmatch(line)
        if not match:
            continue
        kind, phase, flags, commit = match.groups()
        value = int(flags, 16)
        stats = pipelines.setdefault(kind, {})
        stats['latest_sampled_commit_kib'] = int(commit, 16)
        if phase == 'before':
            stats['requests_at_least'] = max(stats.get('requests_at_least', 0), value)
        elif value & 0x80000000:
            stats['latest_failure_hresult'] = f'0x{value:08X}'
    if pipelines:
        summary['pipelines'] = pipelines
    return summary, lines


def collect_live_loader(args, portal, app, read_file, decode_report, portal_error):
    blob, launch_report = diagnostic_reader(portal, app, read_file, decode_report, portal_error)
    launch_filetime = FILETIME_EPOCH + args.run_id * 10_000
    latest_creation = launch_filetime + TRIAL_CREATION_WINDOW_MS * 10_000

    def candidates():
        try:
            stages = temporary_stages(portal, app)
        except portal_error as error:
            if error.status not in (404, 409):
                raise
            raise ValueError('Xbox Device Portal could not list the staging directories.') from None
        return {(name, created) for name, created in stages if launch_filetime <= created <= latest_creation}

    before = launch_report()
    if not matching_probe(before, args.run_id, args.app_id) or not numeric(before, 'run_id'):
        raise ValueError('The current Steam launch does not match the requested AppID and run ID.')
    if before.get('steam_shadow_stage') != 'verified-local-copy':
        raise ValueError('The current Steam launch does not report a verified compatibility copy.')
    matches = candidates()
    if len(matches) != 1:
        raise ValueError(f'Expected one staging directory created for the requested launch. Found {len(matches)}.')
    (name, _), = matches
    raw = blob('loader-trace.txt', LIVE_LOADER_LIMIT, TEMP + '\\' + name)
    current = candidates()
    after = launch_report()
    if (not matching_probe(after, args.run_id, args.app_id)
            or any(before.get(key) != after.get(key) for key in ('run_id', 'steam_shadow_stage'))):
        raise ValueError('The Steam launch changed during capture. No loader trace was saved.')
    if current != matches:
        raise ValueError('The staging directory changed during capture. No loader trace was saved.')

    summary, lines = live_loader_snapshot(raw)
    result = {'version': args.version, 'mode': 'loader', 'app_id': args.app_id, 'run_id': args.run_id,
              'loader_trace': summary}
    if args.output:
        artifacts = {'loader-trace.txt': ('\n'.join(lines) + '\n').encode('ascii')} if lines else {}
        result['artifacts'] = {filename: len(data) for filename, data in artifacts.items()}
        save_observation(args.output, result, artifacts)
    return result


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
            if bmp_frame_dimensions(image) is not None:
                artifacts['bootstrap.bmp'] = image
    trace = blob('fhd-present-times.bin', FRAME_LIMIT) or b''
    if recent_trace(trace, launch_id):
        artifacts['fhd-present-times.bin'] = trace

    current_probe = fields(report(PROBE_REPORT) or '')
    if not matching_probe(current_probe, launch_id, app_id):
        # The fixed frame filename is shared with a later launch and could have been reset.
        artifacts.pop('fhd-present-times.bin', None)
    if loader_lines:
        artifacts['loader-trace.txt'] = ('\n'.join(unique_loader_lines(loader_lines)) + '\n').encode()
    return artifacts


def observe(args, portal, app, read_file, decode_report, portal_error, *,
            clock=time.monotonic, wall_clock=time.time, pause=time.sleep, progress=None):
    def blob(filename, limit, folder=LOCAL, *, allow_prefix=False):
        try:
            raw = read_file(portal, app, folder, filename, limit + 1) or b''
        except portal_error as error:
            if error.status == 409:
                return None
            if error.status == 404:
                return b''
            raise
        if allow_prefix:
            return raw[:limit]
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

    def terminal_loader_lines(filename, text):
        # The child exit watcher publishes process status before appending its trace.
        cutoff = min(deadline, clock() + EXIT_TRACE_WAIT_SECONDS)
        while True:
            lines = embedded_loader_lines(text)
            if lines or fields(text or '').get('steam_shadow_loader_trace'):
                return lines
            remaining = cutoff - clock()
            if remaining <= 0:
                return []
            pause(min(1, remaining))
            text = report(filename)

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
    primary_loader_lines = []
    exit_loader_lines = []
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
                        raw = blob('loader-trace.txt', LIVE_LOADER_LIMIT, folder, allow_prefix=True)
                        lines = unique_loader_lines(complete_loader_lines(raw or b''))
                        loader_lines = lines or loader_lines
                if args.output:
                    primary_loader_lines = embedded_loader_lines(probe_text) or primary_loader_lines
                    if exit_report.get('steam_shadow_child_status') == 'exited':
                        exit_loader_lines = terminal_loader_lines(exit_name, exit_text)
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
        if outcome == 'game-exited' and ('steam_launch_process_exit' in last_probe or 'steam_shadow_exit' in last_probe):
            # Before the exit watcher starts, the primary launch report carries the terminal trace.
            loader_lines += primary_loader_lines + exit_loader_lines
        else:
            loader_lines = primary_loader_lines + loader_lines + exit_loader_lines
        artifacts = (collect_artifacts(report, blob, last_probe, game_run if last_probe and not args.startup else None,
                                      args.app_id, args.bootstrap_frame, baseline_primary, loader_lines)
                     if last_probe else {})
        result['artifacts'] = {name: len(data) for name, data in artifacts.items()}
        save_observation(args.output, result, artifacts)
    return result
