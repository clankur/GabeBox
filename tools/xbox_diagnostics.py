"""Observe existing Steam reports and collect run-scoped Xbox artifacts."""

import json
from pathlib import PureWindowsPath
import re
import struct
import time
from urllib.parse import urlencode

from probe.catalog import PROBE_REPORT, WEBVIEW_REPORT
from probe.runner import report_fields as fields

from .device_portal import CertificateChanged, PortalRetry, PortalUnavailable, utc_text
from .flash_probe import HEADER as FLASH_HEADER, MAGIC as FLASH_MAGIC, RECORD as FLASH_RECORD
from .measure import FILETIME_EPOCH, HEADER, PRESENT


REPORT_LIMIT = 64 * 1024
TRACE_LIMIT = 32 * 1024
FRAME_LIMIT = 8 * 1024 * 1024
# fhd-flash-probe.bin from the opt-in white-flash probe: about 6 KB per second of play.
FLASH_PROBE_LIMIT = 16 * 1024 * 1024
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
# The Big Picture host's WebView2 environment arguments and process changes
# (ui/big-picture-webview/renderer.cpp). Each host process starts the file again.
WEBVIEW_PROCESS_LOG = 'big-picture-webview-processes.txt'
WEBVIEW_PROCESS_LIMIT = 256 * 1024
_WEB_PROCESSES = r'(?:-|[a-z-]{1,16}:[0-9]{1,10}(?:,[a-z-]{1,16}:[0-9]{1,10}){0,63})'
WEBVIEW_PROCESS = re.compile(
    r'webview2 (?:environment|processes) env=[0-9]{1,10} '
    r'utc=[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z tick_ms=[0-9]{1,20}'
    rf'(?: arguments=[a-z-]{{1,16}}(?: switches=[a-z0-9,-]{{1,256}})?| count=[0-9]{{1,4}} added={_WEB_PROCESSES} '
    rf'removed={_WEB_PROCESSES}| error=0x[0-9a-f]{{8}})')
COUNT = re.compile(r'[0-9]{1,20}')
CHECKPOINT = re.compile(r'[a-z][a-z0-9_.-]{0,62}(?::(?:0x[0-9a-f]{1,8}|[a-z][a-z0-9-]{0,40}))?', re.I)
TIMINGS = re.compile(r'[a-z][a-z0-9_.-]{0,63}:[0-9]{1,12}(?:,[a-z][a-z0-9_.-]{0,63}:[0-9]{1,12}){0,63}')
BROWSER_RUN = re.compile(r'[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}', re.I)
WINDOW_TRACE = re.compile(r'(?:win32|sdl)_[a-z0-9_]{1,72}=[a-z0-9_.=-]{1,72}', re.I | re.ASCII)
# Measurement records from runtime/win32: the endpoint peak meter, first-use pipeline timing
# (XBOX_WIN32_PSO_TRACE=1) and D3D12 device-removed data. They use characters the general
# record filter rejects, so each has an exact form.
AUDIO_METER = re.compile(r'audio meter=(ready|unavailable stage=[a-z]{1,16} result=0x[0-9A-F]{8})')
AUDIO_WINDOW = re.compile(r'audio window=([0-9]{1,5}) peak-milli=([0-9]{1,6}) audible=([0-9]{1,3})/([0-9]{1,3})')
PSO_TIMING = re.compile(r'pso timing result=0x([0-9A-F]{8})')
# A cache or replay mode appends its outcome for the creation (hit, miss, early, ...).
PSO_EVENT = re.compile(r'pso kind=([GCS]) utc=[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} us=([0-9]{1,10}) result=0x([0-9A-F]{8})'
                       r'(?: cache=([a-z][a-z-]{0,23}))?')
# Debug object names keep the general record character set, so a name with a path is dropped.
DRED_NAME = r'[a-zA-Z0-9._?@$#=-]{1,96}'
DRED_RECORD = re.compile(rf'dxgi dred-node queue={DRED_NAME} list={DRED_NAME} completed=0x[0-9A-F]{{4}}/0x[0-9A-F]{{4}} '
                         r'ops=[0-9A-F\[\] ]{0,64}|'
                         rf'dxgi dred-fault va=0x[0-9A-F]{{16}}(?: (?:existing|freed)={DRED_NAME}:0x[0-9A-F]{{2}}){{0,8}}')
# SteamAPI_RunCallbacks calls of 2 ms or more (XBOX_WIN32_STEAM_CALLBACK_TRACE=1), stamped when the call returned.
STEAM_CALLBACK = re.compile(r'asset steam-run-callbacks us=([0-9]{1,10}) utc=[0-9]{8}T[0-9]{6}\.[0-9]{3}Z')
# Steam Datagram Relay status changes from GetRelayNetworkStatus (XBOX_WIN32_STEAM_RELAY_STATUS=1): overall,
# network config and any-relay availability (ESteamNetworkingAvailability, 100 is Current), whether ping measurement
# is in progress, milliseconds since InitRelayNetworkAccess, and Steam's debug text in the record character set.
STEAM_RELAY_STATUS = re.compile(r'asset steam-relay-status avail=(-?[0-9]{1,10}) config=(-?[0-9]{1,10}) '
                                r'relay=(-?[0-9]{1,10}) ping=([01]) init_ms=([0-9]{1,10}) '
                                r'utc=([0-9]{8}T[0-9]{6}\.[0-9]{3}Z) msg=[a-zA-Z0-9._?@$#-]{0,48}')
# GetRelayNetworkStatus polls of 2 ms or more: another thread held the networking lock.
STEAM_RELAY_SLOW = re.compile(r'asset steam-relay-status-slow us=([0-9]{1,10}) utc=[0-9]{8}T[0-9]{6}\.[0-9]{3}Z')
DIAGNOSTIC_TRACE = (AUDIO_METER, AUDIO_WINDOW, PSO_TIMING, PSO_EVENT, DRED_RECORD, STEAM_CALLBACK,
                    STEAM_RELAY_STATUS, STEAM_RELAY_SLOW)
DIAGNOSTIC_LIMIT = 4096
LOADER_TRACE = re.compile('|'.join([r'(?:load|export|fault|asset|dxgi|input) [a-zA-Z0-9 ._?@$#=-]{1,174}',
                                    *(pattern.pattern for pattern in DIAGNOSTIC_TRACE)]))
LIVE_LOADER_EVENTS = {
    'graphics-pso-before': re.compile(r'asset graphics-pso-before(?: |$)'),
    'boot-wait': re.compile(r'asset boot-wait(?:[ =-]|$)'),
    'shell-request': re.compile(r'(?:asset|input) (?:boot-)?shell-request(?:[ =-]|$)'),
    # The DXGI adapter's video memory budget install and its periodic usage samples.
    'gpu-memory': re.compile(r'dxgi gpu-memory(?:[ -]|$)'),
    # Direct3D 12 residency: the selected mode, the hook install, per-second Evict/MakeResident counts and
    # the 60-second totals (runtime/win32/d3d12-residency.c).
    'residency': re.compile(r'asset residency(?:[ -]|$)'),
    # Direct3D 12 fence waits (xbox-d3d12-fence-trace.txt): the selected mode, the hook install and self-test,
    # per-second NULL-event SetEventOnCompletion and ExecuteCommandLists counts, the first early return and the
    # 60-second totals (runtime/win32/d3d12-fence-trace.c).
    'd3d12-fence': re.compile(r'asset d3d12-fence(?:[ -]|$)'),
    # Steam title overrides: the relay start-up stub or worker, relay status changes and slow status polls,
    # SteamAPI_RunCallbacks calls of 2 ms or more, and the delayed Steam API init (runtime/win32/loader-trace.c).
    'steam': re.compile(r'asset steam-(?:relay|run-callbacks|init)(?:[ =-]|$)'),
}
# Diagnostic shift tests (XBOX_WIN32_STEAM_RELAY_DELAY_MS, XBOX_WIN32_STEAM_INIT_DELAY_MS): when the delayed
# call was scheduled, when it started, and how the real call returned.
UTC_STAMP = r'([0-9]{8}T[0-9]{6}\.[0-9]{3}Z)'
STEAM_RELAY_DELAY = re.compile(rf'asset steam-relay=worker-delay call=1 delay_ms=([0-9]{{1,6}}) utc={UTC_STAMP}')
STEAM_RELAY_START = re.compile(rf'asset steam-relay=worker-start call=1 waited_ms=([0-9]{{1,10}}) utc={UTC_STAMP}')
STEAM_RELAY_DONE = re.compile(rf'asset steam-relay=(worker-done|worker-skipped) call=1(?: us=([0-9]{{1,10}}))? '
                              rf'utc={UTC_STAMP}')
STEAM_INIT_DELAY = re.compile(rf'asset steam-init=delayed export=([A-Za-z_]{{1,40}}) call=1 delay_ms=([0-9]{{1,6}}) '
                              rf'utc={UTC_STAMP}')
STEAM_INIT_CALL = re.compile(rf'asset steam-init=called export=([A-Za-z_]{{1,40}}) call=1 slept_ms=([0-9]{{1,10}}) '
                             rf'us=([0-9]{{1,10}}) result=(-?[0-9]{{1,10}}) utc={UTC_STAMP}')
PIPELINE_MARKER = re.compile(r'asset (graphics|compute)-pso-(before|after) bytes=0x[0-9a-fA-F]{16} '
                             r'flags=0x([0-9a-fA-F]{8}) available-commit-kib=0x([0-9a-fA-F]{8})(?: .+)?')
PIPELINE_SAMPLE = re.compile(r'asset live-pipeline-(graphics|compute) bytes=0x([0-9a-fA-F]{16}) '
                             r'flags=0x([0-9a-fA-F]{8}) available-commit-kib=0x([0-9a-fA-F]{8})')
UNITY_TRACE = re.compile(r'(?:Forcing GfxDevice: (?:Direct3D 11|Direct3D 12|OpenGL Core|Vulkan)|'
                         r'Direct3D:|Version: +Direct3D [0-9.]{1,8} \[level [0-9.]{1,8}\]|'
                         r'Begin MonoManager ReloadAssembly|Could not allocate memory: System out of memory!|'
                         r'Trying to allocate: [0-9]{1,20}B with [0-9]{1,10} alignment\. '
                         r'MemoryLabel: [a-zA-Z][a-zA-Z0-9_]{0,47})')
LIFECYCLE = re.compile(r'pid=[0-9]{1,10} stage=[a-z-]{1,48} hresult=0x[0-9a-f]{8}'
                       r'(?: (?:tick_ms|old_bytes|new_bytes|usage_bytes|view_id|activation_mode|activation_state|'
                       r'visible|user_initiated|app_initiated)=-?[0-9]{1,20}){0,12}', re.I)
GAME_KEYS = {
    'steam_launch', 'steam_launch_phase', 'steam_launch_backend', 'steam_launch_client_reason',
    'steam_shadow_stage', 'steam_shadow_child', 'steam_shadow_renderer', 'steam_shadow_godot_shader_cache',
    'steam_sdl_host', 'steam_sdl_host_frame', 'steam_sdl_opengl_gpu_mailbox', 'steam_dos',
    'steam_source_check', 'steam_shadow_documents', 'steam_shadow_companion_hashes', 'steam_shadow_keyboard',
    'steam_shadow_roaming_folders', 'steam_shadow_sdl_directinput',
    'steam_cloud_sync', 'steam_cloud_sync_upload_policy', 'steam_cloud_sync_exit_watch',
}
GAME_CODES = {'steam_shadow_stage_hresult', 'steam_shadow_win32', 'steam_shadow_exit',
              'steam_launch_process_exit', 'steam_shadow_child_exit_u32', 'steam_shadow_child_exit_error_u32'}
FRAME_FIELDS = {
    'width', 'height', 'visible', 'presented', 'gpu_presented', 'cpu_presented', 'primary_panel',
    'source_sequence', 'gpu_sequence', 'cpu_sequence', 'sample_us', 'sample_frames', 'sample_gpu_sequence',
    'sample_render_callbacks', 'sample_source_empty_callbacks', 'sample_present_busy_callbacks',
    'slots_busy', 'pool_waits', 'source_empty_callbacks', 'presentation_busy_callbacks',
    'native_input', 'native_menu_requests', 'native_menu_chords', 'native_menu_chords_ignored',
    'native_menu_chords_expired', 'native_input_active', 'native_input_paused', 'native_key_events',
    'native_focus_restores',
}
# Big Picture's numbered record of how it kept a running game when its renderer failed.
WEBVIEW_RECOVERY = re.compile(r'webview_recovery_([1-9][0-9]?)')
WEBVIEW_RECOVERY_ENTRY = re.compile(r'[a-z][a-z-]{0,47} appid=[1-9][0-9]{0,7}'
                                    r'(?: covered=[01] cause=[A-Za-z0-9:._-]{1,96}| reason=[a-z][a-z-]{0,31})?')


def numeric(report, key):
    value = report.get(key, '')
    return int(value) if COUNT.fullmatch(value) else None


def select_fields(report, match, value_pattern):
    return {key: value for key, value in report.items()
            if match(key) and value_pattern.fullmatch(value)}


def startup_snapshot(probe, browser):
    native = select_fields(probe, lambda key: re.fullmatch(r'steam_(?:native_startup_[a-z_]{1,48}_ms|'
                            r'library_(?:named|visible|unnamed_[a-z_]{1,40}|count|pending_metadata))', key), COUNT)
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
    recovery = sorted((int(match[1]), value) for key, value in browser.items()
                      if (match := WEBVIEW_RECOVERY.fullmatch(key)) and WEBVIEW_RECOVERY_ENTRY.fullmatch(value))
    if recovery:
        web['webview_recovery'] = [value for _, value in recovery]
    return {'probe': native, 'webview': web}


def recent_trace(raw, launch_id):
    if len(raw) <= HEADER.size or (len(raw) - HEADER.size) % PRESENT.size:
        return False
    magic, version, frequency, utc, _ = HEADER.unpack_from(raw)
    return (magic == 0x31444846 and version in (1, 2) and frequency > 0
            and utc >= FILETIME_EPOCH + launch_id * 10_000)


def recent_flash_probe(raw, launch_id):
    """The whole records of a flash probe file opened for this launch, or None."""
    if len(raw) < FLASH_HEADER.size:
        return None
    magic, version, frequency, utc, *_ = FLASH_HEADER.unpack_from(raw)
    if magic != FLASH_MAGIC or version != 1 or not frequency or utc < FILETIME_EPOCH + launch_id * 10_000:
        return None
    # The presenter may be appending a batch while the file is read.
    return raw[:FLASH_HEADER.size + (len(raw) - FLASH_HEADER.size) // FLASH_RECORD.size * FLASH_RECORD.size]


def filtered_lines(report, pattern, count=128):
    return [line for line in (report or '').splitlines() if pattern.fullmatch(line)][:count]


def diagnostic_record(line):
    return line.startswith(('audio ', 'pso ', 'dxgi dred-', 'asset steam-run-callbacks us=',
                            'asset steam-relay-status ', 'asset steam-relay-status-slow ')) and any(
        pattern.fullmatch(line) for pattern in DIAGNOSTIC_TRACE)


def diagnostic_lines(lines):
    """Measurement and device-removed records in trace order. Each is distinct evidence, so none are merged."""
    return [line for line in lines if diagnostic_record(line)
            or (line.startswith('dxgi dred-') and LOADER_TRACE.fullmatch(line))][:DIAGNOSTIC_LIMIT]


def unique_loader_lines(lines):
    latest = {}
    for line in lines:
        if LOADER_TRACE.fullmatch(line) and not diagnostic_record(line):
            latest.pop(line, None)
            latest[line] = None
    return list(latest)[-128:]


def diagnostic_summary(lines):
    """Summarize audio meter windows, pipeline timing, Steam callback and relay records, and device-removed records."""
    summary = {}
    meter = next((match[1] for match in map(AUDIO_METER.fullmatch, lines) if match), None)
    windows = [tuple(map(int, match.groups())) for match in map(AUDIO_WINDOW.fullmatch, lines) if match]
    if meter or windows:
        read = [window for window in windows if window[3]]
        summary['audio'] = {
            'meter': 'ready' if meter == 'ready' else meter.replace('unavailable ', 'unavailable: ') if meter else 'not-reported',
            # Each window is 100 endpoint peak samples taken 100 ms apart. A sample above 0.001 counts as audible.
            'windows': len(windows), 'window_seconds': 10,
            'first_window': windows[0][0] if windows else None, 'last_window': windows[-1][0] if windows else None,
            'audible_windows': sum(audible * 2 >= samples for _, _, audible, samples in read),
            'silent_windows': sum(audible == 0 for _, _, audible, _ in read),
            'unread_windows': len(windows) - len(read),
            'peak_milli_max': max((peak for _, peak, _, _ in windows), default=None),
            'first': [line for line in lines if AUDIO_WINDOW.fullmatch(line)][:3],
            'latest': [line for line in lines if AUDIO_WINDOW.fullmatch(line)][-3:],
        }
    timing = next((match[1] for match in map(PSO_TIMING.fullmatch, lines) if match), None)
    events = [(line, *match.groups()) for line, match in ((line, PSO_EVENT.fullmatch(line)) for line in lines) if match]
    if timing or events:
        names = {'G': 'graphics', 'C': 'compute', 'S': 'stream'}
        kinds = {}
        outcomes = {}
        for _, kind, _, _, outcome in events:
            kinds[names[kind]] = kinds.get(names[kind], 0) + 1
            if outcome:
                outcomes[outcome] = outcomes.get(outcome, 0) + 1
        slowest = sorted(events, key=lambda event: int(event[2]), reverse=True)[:5]
        summary['pipelines'] = {
            'timing': 'unreported' if timing is None else 'ready' if not int(timing, 16) & 0x80000000 else f'0x{timing}',
            'events_over_2ms': len(events), 'kinds': kinds,
            # Present only for cache and replay modes: events over 2 ms by outcome.
            **({'outcomes': outcomes} if outcomes else {}),
            'total_ms': round(sum(int(event[2]) for event in events) / 1000, 3),
            'max_ms': round(int(slowest[0][2]) / 1000, 3) if slowest else None,
            'failures': sum(int(event[3], 16) & 0x80000000 != 0 for event in events),
            'slowest': [event[0] for event in slowest],
            # The runtime stops recording after 2000 events.
            'event_limit_reached': len(events) >= 2000,
        }
    callbacks = [(line, int(match[1])) for line, match in ((line, STEAM_CALLBACK.fullmatch(line)) for line in lines)
                 if match]
    if callbacks:
        slowest = sorted(callbacks, key=lambda call: call[1], reverse=True)[:5]
        summary['steam_callbacks'] = {
            'calls_over_2ms': len(callbacks),
            'total_ms': round(sum(us for _, us in callbacks) / 1000, 3),
            'max_ms': round(slowest[0][1] / 1000, 3),
            'slowest': [line for line, _ in slowest],
            # The runtime stops recording after 4096 calls.
            'event_limit_reached': len(callbacks) >= 4096,
        }
    relay = [(line, match) for line, match in ((line, STEAM_RELAY_STATUS.fullmatch(line)) for line in lines) if match]
    slow_polls = [(line, int(match[1])) for line, match in ((line, STEAM_RELAY_SLOW.fullmatch(line)) for line in lines)
                  if match]
    if relay or slow_polls:
        current = next((match for _, match in relay if int(match[1]) == 100), None)
        slowest = sorted(slow_polls, key=lambda poll: poll[1], reverse=True)[:5]
        summary['steam_relay'] = {
            'changes': len(relay),
            # k_ESteamNetworkingAvailability_Current: relay initialization has completed.
            'reached_current': current is not None,
            'current_init_ms': int(current[5]) if current else None,
            'current_utc': current[6] if current else None,
            'latest_avail': int(relay[-1][1][1]) if relay else None,
            'records': [line for line, _ in relay][:16],
            'slow_polls': len(slow_polls),
            'slow_max_ms': round(slowest[0][1] / 1000, 3) if slowest else None,
            'slowest': [line for line, _ in slowest],
            # The runtime stops recording after 256 changes and 1024 slow polls.
            'event_limit_reached': len(relay) >= 256 or len(slow_polls) >= 1024,
        }
    dred = [line for line in lines if line.startswith('dxgi dred-')]
    if dred:
        summary['device_removed'] = {
            'incomplete_command_lists': sum(line.startswith('dxgi dred-node ') for line in dred),
            'page_fault': any(line.startswith('dxgi dred-fault ') for line in dred),
            'records': dred[:16],
        }
    return summary


def embedded_loader_lines(report, *, ordered=False):
    prefix = 'steam_shadow_loader='
    lines = (line[len(prefix):] for line in (report or '').splitlines() if line.startswith(prefix))
    if ordered:
        return [line for line in lines if LOADER_TRACE.fullmatch(line)]
    return unique_loader_lines(lines)


def checkpoint_matches(record, prefix):
    if '*' not in prefix:
        return record == prefix or record.startswith(prefix + ' ')
    expected = prefix.split()
    actual = record.split()
    return len(actual) >= len(expected) and all(
        token.startswith(pattern[:-1]) and len(token) >= len(pattern)
        if pattern.endswith('=*') else token == pattern
        for token, pattern in zip(actual, expected))


def valid_checkpoint_pattern(pattern):
    return not pattern.endswith(' ') and bool(LOADER_TRACE.fullmatch(re.sub(r'=\*(?= |$)', '=0', pattern)))


def checkpoint_progress(records, required, failures):
    reached = []
    for record in records:
        failure = next((prefix for prefix in failures if checkpoint_matches(record, prefix)), None)
        if failure:
            return reached, record
        if len(reached) < len(required) and checkpoint_matches(record, required[len(reached)]):
            reached.append(record)
    return reached, None


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


def stages_for_launch(stages, launch_id):
    created = FILETIME_EPOCH + launch_id * 10_000
    latest = created + TRIAL_CREATION_WINDOW_MS * 10_000
    return {stage for stage in stages if created <= stage[1] <= latest}


def complete_loader_lines(data):
    # The writer can be appending. Keep complete ASCII records in order, including repeated events.
    complete, _, _ = data.rpartition(b'\n')
    rows = complete.decode('ascii', errors='replace').split('\n')
    records = (row.removesuffix('\r') for row in rows)
    return [record for record in records if LOADER_TRACE.fullmatch(record)]


def live_loader_snapshot(raw):
    data = (raw or b'')[:LIVE_LOADER_LIMIT]
    lines = complete_loader_lines(data)
    return loader_summary(lines, len(data), bool(raw and len(raw) > LIVE_LOADER_LIMIT)), lines


def first_match(pattern, lines):
    return next((match for match in map(pattern.fullmatch, lines) if match), None)


FENCE_UTC = r'([0-9]{8}T[0-9]{6}\.[0-9]{3}Z)'
FENCE_MODE = re.compile(r'asset d3d12-fence mode=(trace|wait|off) source=(package-file(?: override=invalid)?) '
                        r'hooks=([a-z-]{1,16})')
FENCE_INSTALL = re.compile(rf'asset d3d12-fence install result=0x([0-9A-F]{{8}}) tables=([0-9]{{1,3}}) '
                           rf'mode=(trace|wait|off) utc={FENCE_UTC}')
FENCE_SELFTEST = re.compile(rf'asset d3d12-fence selftest result=0x([0-9A-F]{{8}}) wait_us=([0-9]{{1,20}}) '
                            rf'utc={FENCE_UTC}')
FENCE_WINDOW = re.compile(r'asset d3d12-fence null_waits=([0-9]{1,20}) early=([0-9]{1,20}) '
                          r'max_wait_us=([0-9]{1,20}) exec=([0-9]{1,20})((?: [a-z_]{1,16}=[0-9]{1,20}){0,8}) '
                          rf'utc={FENCE_UTC}')
FENCE_EVENT = re.compile(r'asset d3d12-fence (first-early|timeout) mode=(trace|wait) value=([0-9]{1,20}) '
                         r'completed=([0-9]{1,20}) wait_us=([0-9]{1,20})(?: settle_us=[0-9]{1,20} '
                         rf'settle_completed=[0-9]{{1,20}})? utc={FENCE_UTC}')


def fence_summary(lines):
    """Summarize the fence-wait trace: whether a blocking SetEventOnCompletion(value, NULL) ever returned
    before the fence reached value, and the batch rate."""
    mode = first_match(FENCE_MODE, lines)
    install = first_match(FENCE_INSTALL, lines)
    selftest = first_match(FENCE_SELFTEST, lines)
    windows = [match for match in map(FENCE_WINDOW.fullmatch, lines) if match]
    events = [(line, match) for line, match in ((line, FENCE_EVENT.fullmatch(line)) for line in lines) if match]
    if not (mode or install or windows or events):
        return {}
    totals = {'null_waits': 0, 'early': 0, 'exec': 0, 'event_waits': 0, 'settled': 0, 'timeouts': 0, 'failed': 0}
    max_wait_us = max_settle_us = max_exec = 0
    early_seconds = []
    for match in windows:
        null_waits, early, wait_us, executed = (int(value) for value in match.groups()[:4])
        extra = dict(field.split('=') for field in match[5].split())
        totals['null_waits'] += null_waits
        totals['early'] += early
        totals['exec'] += executed
        for name in ('event_waits', 'settled', 'timeouts', 'failed'):
            totals[name] += int(extra.get(name, 0))
        max_wait_us = max(max_wait_us, wait_us)
        max_settle_us = max(max_settle_us, int(extra.get('max_settle_us', 0)))
        max_exec = max(max_exec, executed)
        if early:
            early_seconds.append(match[6])
    first_early = next((line for line, match in events if match[1] == 'first-early'), None)
    # early-returns supports the in-flight descriptor-heap reset; no-early-returns refutes it for the waits the
    # hooks saw; no-null-waits with batches means the hooks did not reach the waiting fence.
    verdict = ('early-returns' if totals['early'] or first_early else
               'no-early-returns' if totals['null_waits'] else
               'no-null-waits' if totals['exec'] else 'no-calls')
    return {
        'mode': mode[1] if mode else install[3] if install else None,
        'source': mode[2] if mode else None,
        'hooks': mode[3] if mode else None,
        'install_result': f'0x{install[1]}' if install else None,
        'tables': int(install[2]) if install else None,
        'selftest_result': f'0x{selftest[1]}' if selftest else None,
        'selftest_wait_us': int(selftest[2]) if selftest else None,
        'seconds': len(windows),
        **totals,
        'max_wait_us': max_wait_us,
        'max_settle_us': max_settle_us,
        'max_exec_per_second': max_exec,
        'verdict': verdict,
        'first_early': first_early,
        'early_seconds': early_seconds[:32],
        'timeouts_logged': [line for line, match in events if match[1] == 'timeout'][:16],
    }


def steam_shift_summary(lines):
    """Summarize the delayed relay start-up and the delayed Steam API init, when either ran."""
    summary = {}
    scheduled = first_match(STEAM_RELAY_DELAY, lines)
    if scheduled:
        started = first_match(STEAM_RELAY_START, lines)
        done = first_match(STEAM_RELAY_DONE, lines)
        summary['relay'] = {
            'delay_ms': int(scheduled[1]), 'scheduled_utc': scheduled[2],
            'waited_ms': int(started[1]) if started else None, 'started_utc': started[2] if started else None,
            # worker-skipped: SteamAPI_Shutdown began during the delay, so the real call did not run.
            'outcome': done[1] if done else None, 'call_us': int(done[2]) if done and done[2] else None,
        }
    delayed = first_match(STEAM_INIT_DELAY, lines)
    if delayed:
        called = first_match(STEAM_INIT_CALL, lines)
        summary['init'] = {
            'export': delayed[1], 'delay_ms': int(delayed[2]), 'delayed_utc': delayed[3],
            'slept_ms': int(called[2]) if called else None, 'call_us': int(called[3]) if called else None,
            # ESteamAPIInitResult (0 is OK), or 1 for a successful legacy SteamAPI_InitSafe.
            'result': int(called[4]) if called else None, 'called_utc': called[5] if called else None,
        }
    return summary


def loader_summary(lines, bytes_read, truncated=False):
    summary = {'state': 'observed' if lines else 'no-complete-records' if bytes_read else 'unavailable',
               'bytes_read': bytes_read, 'truncated': truncated,
               'records': len(lines), 'recent': [line for line in lines if not diagnostic_record(line)][-12:],
               'events': {}}
    measured = diagnostic_summary(diagnostic_lines(lines))
    if measured:
        summary['diagnostics'] = measured
    shift = steam_shift_summary(lines)
    if shift:
        summary['steam_shift'] = shift
    fence = fence_summary(lines)
    if fence:
        summary['d3d12_fence'] = fence
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
    return summary


def collect_live_loader(args, portal, app, read_file, decode_report, portal_error):
    blob, launch_report = diagnostic_reader(portal, app, read_file, decode_report, portal_error)
    def candidates():
        try:
            stages = temporary_stages(portal, app)
        except portal_error as error:
            if error.status not in (404, 409):
                raise
            raise ValueError('Xbox Device Portal could not list the staging directories.') from None
        return stages_for_launch(stages, args.run_id)

    before = launch_report()
    if not matching_probe(before, args.run_id, args.app_id) or not numeric(before, 'run_id'):
        raise ValueError('The current Steam launch does not match the requested AppID and run ID.')
    if before.get('steam_shadow_stage') != 'verified-local-copy':
        raise ValueError('The current Steam launch does not report a verified compatibility copy.')
    matches = candidates()
    filename = named_exit_report(before, args.app_id)
    hosted_run = numeric(before, 'steam_launch_sdl_run')
    exit_code = None
    if len(matches) == 1:
        (name, _), = matches
        raw = blob('loader-trace.txt', LIVE_LOADER_LIMIT, TEMP + '\\' + name)
        current = candidates()
        summary, lines = live_loader_snapshot(raw)
        summary['source'] = 'live-stage'
    elif not matches and filename:
        if hosted_run is not None and filename != f'steam-win32-shadow-{args.app_id}-exit-{hosted_run}.txt':
            raise ValueError('The child exit report does not match the hosted launch.')
        raw = blob(filename, REPORT_LIMIT)
        report = decode_report(raw) if raw and len(raw) <= REPORT_LIMIT else ''
        report_fields = fields(report)
        terminal = report.rstrip('\r\n').rpartition('\n')[2]
        if (not report.endswith('\n') or report_fields.get('steam_shadow_child_status') != 'exited'
                or not re.fullmatch(r'steam_shadow_loader_trace=(?:collected|no-output|no-complete-records|read-error|too-large)',
                                    terminal)):
            raise ValueError('The child exit report is unavailable or its loader trace is still pending.')
        lines = embedded_loader_lines(report, ordered=True)
        summary = loader_summary(lines, len(raw))
        summary['source'] = 'exit-report'
        exit_code = numeric(report_fields, 'steam_shadow_child_exit_u32')
        current = candidates()
    else:
        raise ValueError(f'Expected one staging directory created for the requested launch. Found {len(matches)}.')
    after = launch_report()
    if (not matching_probe(after, args.run_id, args.app_id)
            or any(before.get(key) != after.get(key) for key in
                   ('run_id', 'steam_shadow_stage', 'steam_shadow_exit_report', 'steam_launch_sdl_run'))):
        raise ValueError('The Steam launch changed during capture. No loader trace was saved.')
    if current != matches:
        raise ValueError('The staging directory changed during capture. No loader trace was saved.')

    result = {'version': args.version, 'mode': 'loader', 'app_id': args.app_id, 'run_id': args.run_id,
              'loader_trace': summary}
    if exit_code is not None and 0 <= exit_code <= 0xffffffff:
        result['process_exit'] = f'0x{exit_code:08X}'
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
    web_processes = filtered_lines(report(WEBVIEW_PROCESS_LOG, WEBVIEW_PROCESS_LIMIT), WEBVIEW_PROCESS, 4096)
    if web_processes:
        artifacts['webview2-processes.txt'] = ('\n'.join(web_processes) + '\n').encode()
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
    flashes = recent_flash_probe(blob('fhd-flash-probe.bin', FLASH_PROBE_LIMIT) or b'', launch_id)
    if flashes is not None:
        artifacts['fhd-flash-probe.bin'] = flashes

    current_probe = fields(report(PROBE_REPORT) or '')
    if not matching_probe(current_probe, launch_id, app_id):
        # The fixed frame filenames are shared with a later launch and could have been reset.
        artifacts.pop('fhd-present-times.bin', None)
        artifacts.pop('fhd-flash-probe.bin', None)
    if loader_lines:
        artifacts['loader-trace.txt'] = ('\n'.join(unique_loader_lines(loader_lines)) + '\n').encode()
    unity_log = last_probe.get('steam_shadow_unity_log', '')
    if re.fullmatch(rf'steam-unity-{app_id}-[0-9]{{1,20}}\.log', unity_log):
        text = report(unity_log, TRACE_LIMIT)
        lines = (line.strip() for line in (text or '').splitlines())
        checkpoints = [line for line in lines if UNITY_TRACE.fullmatch(line)][:128]
        if checkpoints:
            artifacts['unity-startup.txt'] = ('\n'.join(checkpoints) + '\n').encode()
    return artifacts


def observe(args, portal, app, read_file, decode_report, portal_error, *,
            clock=time.monotonic, wall_clock=time.time, pause=time.sleep, progress=None):
    deadline = clock() + args.timeout
    # Transient network failures skip one read and are recorded; the poll loop keeps going.
    retry = PortalRetry(clock=clock, wall_clock=wall_clock, pause=pause, deadline=deadline, progress=progress)

    def blob(filename, limit, folder=LOCAL, *, allow_prefix=False):
        try:
            raw = retry.call(lambda: read_file(portal, app, folder, filename, limit + 1)) or b''
        except PortalUnavailable:
            return None
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
            return retry.call(lambda: temporary_stages(portal, app))
        except PortalUnavailable:
            return None
        except portal_error as error:
            if error.status not in (404, 409):
                raise
            return None

    def terminal_loader_lines(filename, text, *, ordered=False):
        # The child exit watcher publishes process status before appending its trace.
        cutoff = min(deadline, clock() + EXIT_TRACE_WAIT_SECONDS)
        while True:
            lines = embedded_loader_lines(text, ordered=ordered)
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
    # The staging folder is deleted when the game exits normally, so a saved observation keeps
    # copying its loader trace while the game runs. That copy is the only durable record of
    # the measurement lines that the exit report does not carry.
    live_trace = bool(args.output and args.app_id)
    baseline_stages = stages() if live_trace and not args.run_id else None

    def stage_candidates(run=None):
        candidates = stages() or set()
        if run is not None:
            candidates = stages_for_launch(candidates, run)
        return candidates - baseline_stages if baseline_stages is not None else candidates

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
    required = args.require_checkpoint
    failures = args.fail_on_checkpoint
    watched = args.watch_checkpoint
    checkpoint_timeouts = args.checkpoint_timeouts or [args.checkpoint_timeout] * len(required)
    checkpoint_deadline = None
    checkpoint_records = []
    failed_checkpoint = None
    failed_checkpoint_source = None
    checkpoint_source = 'unavailable'
    checkpoint_trace_truncated = False
    watched_records = {}
    watch_priority = {'exit-report': 0, 'live-loader': 1, 'launch-report': 2}
    if progress and clock() < deadline:
        progress('waiting-for-startup' if args.startup else 'attaching-to-launch' if args.run_id else 'waiting-for-launch')

    def fresh(run):
        return run is not None and (run > prior_run if prior_run is not None else run >= earliest_id)

    fatal = None
    measured_lines = []
    live_copy = {}
    try:
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
                        try:
                            names = retry.call(lambda: process_image_names(portal))
                        except PortalUnavailable:
                            names = None  # Unknown, not missing: keep the previous process state.
                        if names is not None:
                            active = args.image_name.casefold() in names
                            process_seen |= active
                            process_missing = None if active or not process_seen else (
                                process_missing if process_missing is not None else clock())
                        snapshot['game_process_listed'] = (active if names is not None else
                                                           bool(last_snapshot and last_snapshot.get('game_process_listed')))
                    live_records = []
                    if (required or live_trace) and probe.get('steam_shadow_stage') == 'verified-local-copy':
                        # Without a baseline listing, select the staging folder created for this launch.
                        new_stages = stage_candidates(game_run if required or baseline_stages is None else None)
                        if trial_stage is None and len(new_stages) == 1:
                            trial_stage = next(iter(new_stages))
                        if trial_stage in new_stages and (not required or len(new_stages) == 1):
                            folder = TEMP + '\\' + trial_stage[0]
                            raw = blob('loader-trace.txt', LIVE_LOADER_LIMIT, folder, allow_prefix=True)
                            unchanged = True
                            if required:
                                current_stages = stage_candidates(game_run)
                                after = fields(report(PROBE_REPORT) or '')
                                unchanged = (current_stages == new_stages and matching_probe(after, game_run, args.app_id)
                                             and after.get('run_id') == probe.get('run_id')
                                             and after.get('steam_shadow_stage') == probe.get('steam_shadow_stage'))
                            if unchanged:
                                live_records = complete_loader_lines(raw or b'')
                                if raw:
                                    live_copy = {'stage': trial_stage[0], 'reads': live_copy.get('reads', 0) + 1,
                                                 'last_read_utc': utc_text(wall_clock()), 'last_read_bytes': len(raw)}
                            loader_lines = unique_loader_lines(live_records) or loader_lines
                            # The trace only grows, so the latest complete read holds every earlier record.
                            measured_lines = diagnostic_lines(live_records) or measured_lines
                    if args.output:
                        primary_loader_lines = embedded_loader_lines(probe_text) or primary_loader_lines
                    if (args.output or required) and exit_report.get('steam_shadow_child_status') == 'exited':
                        exit_loader_lines = terminal_loader_lines(exit_name, exit_text, ordered=bool(required))
                    if required:
                        # A Portal poll can return after its observation deadline.
                        poll_deadline = min(deadline, checkpoint_deadline or deadline)
                        if clock() > poll_deadline:
                            if poll_deadline == checkpoint_deadline:
                                outcome = 'checkpoint-stalled'
                            break
                    last_snapshot = snapshot
                    finished = finish_reason(probe, exit_report)
                    if process_missing is not None and clock() - process_missing >= 3:
                        finished = finished or 'game-process-gone'
                    if probe.get('steam_sdl_host_frame') == 'presented' and first_frame is None:
                        first_frame = clock()
                    if required:
                        previous_count = len(checkpoint_records)
                        if live_records:
                            checkpoint_trace_truncated = len(raw) == LIVE_LOADER_LIMIT
                        sources = (('live-loader', live_records), ('exit-report', exit_loader_lines),
                                   ('launch-report', embedded_loader_lines(probe_text, ordered=True)))
                        # The exit report can contain records written after the last live read.
                        for pattern in watched:
                            previous = watched_records.get(pattern, {}).get('source', 'launch-report')
                            for source, records in (sources[1], sources[0], sources[2]):
                                if watch_priority[source] > watch_priority[previous]:
                                    break
                                latest = next((record for record in reversed(records)
                                               if checkpoint_matches(record, pattern)), None)
                                if latest:
                                    watched_records[pattern] = {'record': latest, 'source': source}
                                    break
                        for source, records in sources:
                            if not records:
                                continue
                            if checkpoint_source == 'unavailable':
                                checkpoint_source = source
                            reached, failure = checkpoint_progress(records, required, failures)
                            if len(reached) > len(checkpoint_records):
                                checkpoint_records = reached
                                checkpoint_source = source
                                if progress:
                                    progress(f'checkpoint-reached {len(reached)}/{len(required)}')
                            if failure:
                                failed_checkpoint = failure
                                failed_checkpoint_source = source
                                outcome = 'checkpoint-failed'
                                break
                        if failed_checkpoint is not None:
                            break
                        if finished and finished != 'window-ready':
                            outcome = finished
                            break
                        if len(checkpoint_records) == len(required):
                            outcome = 'checkpoints-observed'
                            break
                        game_started = (probe.get('steam_shadow_child') == 'created' or first_frame is not None
                                        or process_seen or finished == 'window-ready')
                        completed = len(checkpoint_records)
                        if completed > previous_count or (checkpoint_deadline is None and game_started):
                            budget = checkpoint_timeouts[completed]
                            checkpoint_deadline = clock() + budget
                            if progress:
                                progress(f'waiting-for-checkpoint {completed + 1}/{len(required)} ({budget}s) {required[completed]}')
                    elif finished:
                        outcome = finished
                        break
                    elif first_frame is not None and clock() - first_frame >= args.frame_seconds:
                        outcome = 'frame-observed'
                        break
                elif game_run and seen_run is not None and seen_run > game_run:
                    outcome = 'launch-superseded'
                    break
            if checkpoint_deadline is not None and clock() >= checkpoint_deadline:
                outcome = 'checkpoint-stalled'
                break
            if last_snapshot is not None and progress:
                state = last_snapshot['webview'].get('status') or last_snapshot['probe'].get('steam_launch_phase')
                if state and state != last_state:
                    progress(state)
                    last_state = state
            cutoff = min(deadline, checkpoint_deadline) if checkpoint_deadline is not None else deadline
            pause(min(1, max(0, cutoff - clock())))
    except (CertificateChanged, KeyboardInterrupt) as error:
        # Save what was already copied, then stop. A changed certificate is never retried.
        fatal = error
        outcome = 'portal-certificate-changed' if isinstance(error, CertificateChanged) else 'interrupted'

    result = {'version': args.version, 'mode': 'startup' if args.startup else 'game', 'outcome': outcome}
    if last_snapshot is not None:
        result.update(last_snapshot)
        run_id = numeric(last_probe, 'run_id') if args.startup else game_run
        result['run_id'] = run_id
    if not args.startup:
        result['app_id'] = args.app_id
    if args.run_id:
        result['requested_run_id'] = args.run_id
    if required:
        completed = len(checkpoint_records)
        result['checkpoints'] = {'reached': required[:completed], 'records': checkpoint_records,
                                 'source': checkpoint_source}
        if checkpoint_trace_truncated:
            result['checkpoints']['trace_at_read_limit'] = True
        if completed < len(required):
            result['checkpoints']['waiting_for'] = required[completed]
            result['checkpoints']['waiting_for_timeout_seconds'] = checkpoint_timeouts[completed]
        if watched:
            result['checkpoints']['watched'] = {pattern: watched_records.get(pattern) for pattern in watched}
        if failed_checkpoint is not None:
            result['checkpoints']['failure'] = failed_checkpoint
            result['checkpoints']['failure_source'] = failed_checkpoint_source
    if live_copy:
        result['live_loader'] = live_copy
    if measured_lines:
        result['loader_diagnostics'] = diagnostic_summary(measured_lines)
    if args.output:
        if outcome == 'game-exited' and ('steam_launch_process_exit' in last_probe or 'steam_shadow_exit' in last_probe):
            # Before the exit watcher starts, the primary launch report carries the terminal trace.
            loader_lines += primary_loader_lines + exit_loader_lines
        else:
            loader_lines = primary_loader_lines + loader_lines + exit_loader_lines
        artifacts = {}
        if last_probe and fatal is None:
            artifacts = collect_artifacts(report, blob, last_probe, game_run if not args.startup else None,
                                          args.app_id, args.bootstrap_frame, baseline_primary, loader_lines)
        elif last_probe and loader_lines:
            artifacts['loader-trace.txt'] = ('\n'.join(unique_loader_lines(loader_lines)) + '\n').encode()
        if measured_lines:
            artifacts['loader-diagnostics.txt'] = ('\n'.join(measured_lines) + '\n').encode('ascii')
        result['artifacts'] = {name: len(data) for name, data in artifacts.items()}
    portal_summary = retry.summary()
    if portal_summary:
        result['portal'] = portal_summary
    if args.output:
        save_observation(args.output, result, artifacts)
    if fatal is not None:
        raise fatal
    return result
