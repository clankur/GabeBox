#!/usr/bin/env python3
"""Deploy and observe signed app packages through Xbox Device Portal."""

import argparse
import base64
import getpass
import hashlib
import hmac
import http.client
import json
import os
from pathlib import Path
import re
from struct import Struct, unpack_from
import sys
from tempfile import TemporaryDirectory
import time
from urllib.parse import urlencode
from uuid import uuid4
from xml.etree import ElementTree
from zipfile import BadZipFile, ZipFile


from probe.catalog import (
    BIG_PICTURE_NAME, BIG_PICTURE_REPORT, CASES, GRAPHICS_CONTROL_NAME, PACKAGE_CASES,
    PROBE_CONTROL_NAME, PROBE_NAME, PROBE_REPORT, PROBE_STEAM_CORE_NAME, WEBVIEW_REPORT,
)
from probe.runner import report_fields, run_case
from tools.artifacts import SDL_LIMIT, SDL_TRIALS, load_sdl_artifact, pe_summary
from tools.device_portal import PACKAGE_API, CertificateChanged, DevicePortal, PortalError, utc_text


PROBE_LIFETIME_NAME = PROBE_NAME + '.Lifetime'
ACTIVATION_REPORT = 'big-picture-activation.txt'
BIG_PICTURE_REPORTS = {BIG_PICTURE_REPORT, WEBVIEW_REPORT, ACTIVATION_REPORT}
STEAMAPPS_PATH = r'\LocalState\SteamLibrary\steamapps'
DEVELOPER_STEAMAPPS_PATH = r'\SteamLibrary\steamapps'
LIFETIME_REPORT = 'lifetime-report.txt'
OBSERVE_LIMIT = 3600
PROTOCOL_REPORT = 'protocol-return.txt'
SETTING_NAMES = ('XBOX_PORTAL_URL', 'XBOX_PORTAL_USERNAME', 'XBOX_PORTAL_CERT_SHA256',
                 'XBOX_PORTAL_PASSWORD')


def load_settings(config_path=None):
    settings = {}
    if config_path is None:
        config_path = Path(__file__).resolve().parents[1] / '.env'
    elif not config_path.is_file():
        raise ValueError(f'Environment file not found: {config_path}')
    if config_path.is_file():
        for line in config_path.read_text().splitlines():
            key, _, value = line.strip().partition('=')
            if key.strip() in SETTING_NAMES:
                settings[key.strip()] = value.strip()
    settings.update({key: os.environ[key] for key in SETTING_NAMES if key in os.environ})
    return settings


def probe_identity(file):
    with ZipFile(file) as archive:
        manifest = ElementTree.fromstring(archive.read('AppxManifest.xml'))
    identity = manifest.find('{*}Identity')
    if (identity is None or identity.get('Name') not in PACKAGE_CASES
            or identity.get('ProcessorArchitecture') != 'x64'):
        raise ValueError('--test-probe requires the x64 packaged process probe APPX.')
    return identity.attrib['Name'], identity.attrib['Version']


def find_installed_probe(portal, name, version):
    _, data = portal.request('GET', f'{PACKAGE_API}/packages')
    prefix = f'{name}_{version}_x64__' if version else f'{name}_'
    candidates = [package for package in json.loads(data)['InstalledPackages']
                  if package['PackageFullName'].startswith(prefix)]
    if len(candidates) != 1:
        raise ValueError(f'Expected exactly one installed probe matching {prefix}. Found {len(candidates)}.')
    return candidates[0]


def probe_report_path(app, filename=PROBE_REPORT):
    package_name = app['PackageFullName']
    query = urlencode({'knownfolderid': 'LocalAppData', 'packagefullname': package_name,
                       'path': r'\LocalState', 'filename': filename})
    return f'/api/filesystem/apps/file?{query}'


def decode_report(raw):
    encoding = 'utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8'
    try:
        return raw.decode(encoding)
    except UnicodeDecodeError:
        return ''  # The report can be read while the app is replacing it.


def slay_exception_types(text):
    errors = re.findall(r'\b(?:[a-zA-Z_$][\w$]*\.)+(?:[A-Z][\w$]*)?(?:Error|Exception)\b', text)
    return ['slay_logged_exception=' + error for error in dict.fromkeys(errors)]


def slay_console_summary(raw):
    if not raw:
        return 'slay_console=no-output'
    text = raw.decode('utf-8', errors='replace').replace('\r\n', '\n')
    result = re.findall(r'^slay_console=(?:helper-started|create-error|running-at-timeout|exit--?\d+)\r?$', text, re.M)
    if not result or result[0] != 'slay_console=helper-started':
        return 'slay_console=unrecognized-output'
    result.extend(re.findall(r'^slay_console_jar=(?:readable|unavailable)\r?$', text, re.M))
    result.extend(re.findall(r'^slay_console_capture=(?:truncated|read-error|incomplete)\r?$', text, re.M))
    result.extend(re.findall(r'^slay_console_cwd=(?:game-directory|unexpected-main-class|native-load-error)\r?$', text, re.M))
    result.extend(re.findall(r'^slay_console_cwd_error=0x[0-9A-F]{8}\r?$', text, re.M))
    result.extend(re.findall(r'^slay_console_lwjgl_sha256=[0-9a-f]{64}$', text, re.M))
    result.extend(re.findall(r'^slay_console_display=windowed-fallback$', text, re.M))
    result.extend(re.findall(r'^slay_sdl_display=(?:ready|window|context|presented|cursor)$', text, re.M))
    result.extend(re.findall(r'^slay_sdl_gamepad=connected$', text, re.M))
    result.extend(re.findall(r'^slay_font_atlas_(?:initial|limit)=(?:-1|[0-9]{1,5}|unavailable)$', text, re.M))
    result.extend(re.findall(r'^slay_jvm_(?:profile=low-memory|max_heap_bytes=[0-9]{1,16})$', text, re.M))
    result.extend(re.findall(r'^slay_mesa_child=(?:loading|entered|load-error)$|^slay_mesa_child_(?:stage|checkpoint)=[0-8]$|'
                             r'^slay_mesa_child_(?:win32|egl|gl|fbo)=0x[0-9A-F]{8}$', text, re.M))
    result.extend(re.findall(r'^slay_lwjgl_egl_binding=(?:begin|natives-ready|no-version|gl-version|shader-created|no-shader|'
                             r'native-search-path|native-dependency|release-error)$|'
                             r'^slay_lwjgl_egl_binding_fault=[a-zA-Z0-9_.$]{1,128}$', text, re.M))
    stages = [('Unable to access jarfile', 'jar-unavailable'), ('Could not find or load main class', 'main-class'),
              ('Could not create the Java Virtual Machine', 'vm-creation'),
              ('Could not reserve enough space', 'vm-reservation'), ('DesktopLauncher', 'desktop-launcher'),
              ('Access is denied', 'file-access-denied'), ('DisplayConfig.writeDisplayConfigFile', 'display-config-write'),
              ('lwjgl64.dll', 'lwjgl-native-load'), ("Can\'t find dependent libraries", 'native-dependency-missing'),
              ('Pixel format not accelerated', 'pixel-format'), ('Failed to create display', 'display-creation'),
              ('Error loading pixmap.', 'pixmap-allocation'),
              ('Native memory allocation (malloc) failed', 'native-allocation'),
              ('java.lang.OutOfMemoryError: Java heap space', 'heap-allocation')]
    for marker, stage in stages:
        if marker in text:
            result.append('slay_console_observed=' + stage)
    return '\n'.join(result + slay_exception_types(text))


def game_file_url(package, folder, filename, knownfolder):
    parameters = {'knownfolderid': knownfolder, 'path': folder, 'filename': filename}
    if knownfolder == 'LocalAppData':
        parameters['packagefullname'] = package['PackageFullName']
    return '/api/filesystem/apps/file?' + urlencode(parameters)


def read_game_file(portal, package, folder, filename, limit, *, knownfolder='LocalAppData'):
    try:
        _, data = portal.request('GET', game_file_url(package, folder, filename, knownfolder), read_limit=limit)
    except PortalError as error:
        if error.status == 404:
            return None
        raise
    return data


# SdlHostMailbox contains two input headers separated by the CPU frame pixels.
SDL_INPUT_MAGIC = 0x53444C31
SDL_INPUT_HEADER = Struct('<9I')
SDL_INPUT_GAMEPAD = Struct('<II6iII')
SDL_INPUT_GAMEPAD_OFFSET = SDL_INPUT_HEADER.size + 5 * 4 + 1920 * 1080 * 4
# The presenter labels each held chord with its source, pad (Windows.Gaming.Input) or keys
# (VK_GAMEPAD events, as Device Portal remote input sends), and records rejected and expired chords.
# Older presenters write a bare menu_chord=held.
SDL_INPUT_PANEL_MARKERS = {
    'panel_ready': (b'panel_input=ready',),
    'chord_held': (b'menu_chord=held', b'menu_chord=held source=pad', b'menu_chord=held source=keys'),
    'chord_from_keys': (b'menu_chord=held source=keys',),
    'chord_ignored': (b'menu_chord=ignored reason=unpaired',),
    'chord_expired': (b'menu_chord=expired',),
    'menu_request': (b'menu_request=ready',),
}
SDL_INPUT_PANEL_LINES = frozenset(line for lines in SDL_INPUT_PANEL_MARKERS.values() for line in lines)


def inspect_host_input(args, portal, app, *, clock=time.monotonic, pause=time.sleep, progress=None):
    from tools.xbox_diagnostics import (GPU_MAILBOX_NAME, LOCAL, diagnostic_reader, matching_probe,
                                        save_observation)

    blob, launch_report = diagnostic_reader(portal, app, read_game_file, decode_report, PortalError)
    first = launch_report()
    if not matching_probe(first, args.run_id, args.app_id):
        raise ValueError('The current Steam launch does not match the requested AppID and run ID.')
    if first.get('steam_sdl_host') == 'exited' or first.get('steam_launch_process_exit'):
        raise ValueError('The selected game process exited and its input mailbox is no longer available.')
    mailbox = first.get('steam_sdl_mailbox', '')
    if not GPU_MAILBOX_NAME.fullmatch(mailbox):
        raise ValueError('The current launch does not have a valid hosted SDL mailbox.')
    url = game_file_url(app, LOCAL, mailbox, 'LocalAppData')

    def read_range(offset, length):
        end = offset + length - 1

        def receive(response):
            return response.getheader('Content-Range', ''), response.read(length + 1)

        status, (returned_range, raw) = portal.request('GET', url, headers={'Range': f'bytes={offset}-{end}'},
                                                       receive=receive)
        if status != 206 or len(raw) != length or not re.fullmatch(rf'bytes {offset}-{end}/(?:\d+|\*)', returned_range):
            raise ValueError('Xbox Device Portal did not return the exact requested SDL mailbox range.')
        return raw

    def channel(serial, ack, confirmed):
        if serial & 1 or serial != confirmed:
            return {'state': 'updating', 'serial': serial, 'ack': ack}
        state = 'host-idle' if not serial and not ack else 'acknowledged' if serial == ack else 'unacknowledged'
        return {'state': state, 'serial': serial, 'ack': ack}

    def panel_trace():
        raw = blob('fhd-panel-input.txt', 4096)
        if raw is None or len(raw) > 4096:
            return None
        complete, _, _ = raw.rpartition(b'\n')
        return tuple(line for line in complete.splitlines() if line in SDL_INPUT_PANEL_LINES)

    panel_before = panel_trace() if args.panel_menu else None
    samples = []
    if args.seconds and progress:
        progress('sampling-host-input')
    start = clock()
    while True:
        head = SDL_INPUT_HEADER.unpack(read_range(0, SDL_INPUT_HEADER.size))
        pad = SDL_INPUT_GAMEPAD.unpack(read_range(SDL_INPUT_GAMEPAD_OFFSET, SDL_INPUT_GAMEPAD.size))
        pointer_confirmed, = unpack_from('<I', read_range(5 * 4, 4))
        pad_confirmed, = unpack_from('<I', read_range(SDL_INPUT_GAMEPAD_OFFSET, 4))
        if head[0] != SDL_INPUT_MAGIC or head[8] not in (0, 1) or pad[9] not in (0, 1):
            raise ValueError('The hosted SDL input mailbox does not match the expected protocol.')
        pointer = channel(head[5], head[6], pointer_confirmed)
        gamepad = channel(pad[0], pad[8], pad_confirmed)
        if gamepad['state'] != 'updating':
            if pad[1] & ~0x17FFF or any(not -32768 <= axis <= 32767 for axis in pad[2:8]):
                raise ValueError('The hosted SDL gamepad state does not match the expected protocol.')
            gamepad.update(connected=bool(pad[1] & 0x10000), buttons=f'0x{pad[1]:08x}', axes=pad[2:8])
        elapsed = clock() - start
        samples.append({'elapsed_seconds': round(elapsed, 3), 'gamepad': gamepad, 'pointer': pointer,
                        'keyboard_ready': bool(pad[9]), 'closing': bool(head[8])})
        if elapsed >= args.seconds or head[8]:
            break
        pause(min(0.5, args.seconds - elapsed))

    panel_after = panel_trace() if args.panel_menu else None
    last = launch_report()
    if (not matching_probe(last, args.run_id, args.app_id)
            or any(first.get(key) != last.get(key) for key in ('run_id', 'steam_sdl_mailbox'))):
        raise ValueError('The Steam launch changed during input sampling. No samples were saved.')

    result = {'version': args.version, 'mode': 'input', 'app_id': args.app_id, 'run_id': args.run_id,
              'activity': {}, 'samples': samples}
    for name in ('gamepad', 'pointer'):
        stable = [sample[name]['serial'] for sample in samples if sample[name]['state'] != 'updating']
        result['activity'][name] = ('insufficient-samples' if len(stable) < 2 else
                                    'host-activity' if len(set(stable)) > 1 else 'host-idle')
    if args.panel_menu:
        menu = {'scope': 'package', 'state': 'unavailable'}
        if panel_after is not None:
            def count(trace):
                return {name: sum(map(trace.count, lines)) for name, lines in SDL_INPUT_PANEL_MARKERS.items()}
            menu.update(state='observed', counts=count(panel_after))
            if panel_before is not None and args.seconds and panel_after[:len(panel_before)] == panel_before:
                menu['added'] = count(panel_after[len(panel_before):])
        result['panel_menu'] = menu
    if args.output:
        save_observation(args.output, result, {})
    return result


def stardew_host_summary(raw):
    if not raw:
        return 'stardew_host_trace=no-output'
    encoding = 'utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig'
    text = raw.decode(encoding, errors='replace')
    if not re.search(r'Stardew Valley\.(?:dll|exe)\b', text, re.I):
        return 'stardew_host_trace=target-unconfirmed'
    fields = ['stardew_host_trace=observed', f'stardew_host_bytes_read={len(raw)}']
    markers = [('apphost', 'Invoked apphost'), ('hostfxr', 'Invoked hostfxr'),
               ('hostpolicy', 'Invoked hostpolicy'), ('coreclr-path', 'CoreCLR path ='),
               ('managed-launch', 'Launch host:')]
    for phase, marker in markers:
        if marker in text:
            fields.append(f'stardew_host_phase={phase}')
    for line in text.splitlines():
        if re.search(r'Failed to load the dll|(?:required library|library.*required to execute).*'
                     r'(?:could not be found|was not found)|Could not resolve CoreCLR path|'
                     r'Failed to create CoreCLR|Failed to execute managed app', line, re.I):
            library = re.search(r'\b(hostfxr|hostpolicy|coreclr)\.dll\b', line, re.I)
            operation = re.search(r'Could not resolve CoreCLR path|Failed to create CoreCLR|'
                                  r'Failed to execute managed app', line, re.I)
            failure = 'runtime-library-load'
            if library:
                failure = library[0].lower()
            elif operation:
                failure = operation[0].lower().replace(' ', '-')
            fields.append('stardew_host_error=' + failure)
            break
    for label, pattern in [('hresult', r'HRESULT:\s*(0x[0-9a-f]+)'),
                           ('exit', r'Execute managed assembly exit code:\s*(0x[0-9a-f]+)')]:
        for code in dict.fromkeys(re.findall(pattern, text, re.I)):
            fields.append(f'stardew_host_{label}={code}')
    return '\n'.join(fields)


def stardew_managed_summary(raw):
    allowed = (r'stardew_managed=(?:entered|unhandled-exception|process-exit)|'
               r'stardew_managed_exception=(?:dll-not-found|entry-point-not-found|bad-image-format|platform-not-supported)|'
               r'stardew_managed_fault=[a-zA-Z0-9_.+]{1,128}|'
               r'stardew_managed_detail=(?:opengl|sdl|access-denied)|'
               r'stardew_managed_library=(?:opengl32|sdl2)|'
               r'stardew_graphics_frame=(?:MonoGame\.OpenGL\.|Microsoft\.Xna\.Framework\.Graphics\.)[a-zA-Z0-9_.+]{1,140}|'
               r'stardew_graphics_sdl_error=(?:opengl|wgl)|'
               r'stardew_graphics_sdl_driver=(?:windows|none|other|unavailable)|'
               r'stardew_graphics_gl_context=(?:absent|present)|'
               r'stardew_graphics_system_opengl=(?:loaded|0x[0-9A-F]{8})|'
               r'stardew_graphics_d3d11_hresult=(?:unavailable|0x[0-9A-F]{8})|'
               r'stardew_graphics_d3d11_feature_level=0x[0-9A-F]{4}|'
               r'stardew_graphics_probe=incomplete')
    lines = (raw or b'').decode('ascii', errors='replace').splitlines()
    return '\n'.join(dict.fromkeys(line for line in lines if re.fullmatch(allowed, line))) or 'stardew_managed=no-output'


def sdl_frame_stem(report):
    run = re.search(r'^steam_(launch_sdl|stardew_managed|slay_console)_run=([0-9]{1,20})\r?$', report, re.MULTILINE)
    if not run:
        raise ValueError('The current console report has no SDL frame trial.')
    template = {
        'launch_sdl': 'sdl-startup-{}',
        'stardew_managed': 'stardew-managed-{}.sdl',
        'slay_console': 'slay-console-{}.sdl',
    }
    return template[run[1]].format(run[2])


def read_sdl_frame(portal, app, report, destination):
    from tools.xbox_diagnostics import BMP_LIMIT, bmp_frame_dimensions

    stem = sdl_frame_stem(report)
    data = read_game_file(portal, app, r'\LocalState', stem + '.bmp', BMP_LIMIT + 1)
    if not data:
        raise ValueError('The latest SDL trial has no captured BMP frame.')
    dimensions = bmp_frame_dimensions(data)
    if dimensions is None:
        raise ValueError('The SDL frame was incomplete or had an unexpected bitmap layout.')
    width, height = dimensions
    with destination.open('xb') as output:
        output.write(data)
    print(f'Saved SDL frame ({width} by {height}) to {destination}.')


def wait_for_sdl_frame(portal, package, destination, timeout):
    _, raw = portal.request('GET', probe_report_path(package))
    report = decode_report(raw)
    stem = sdl_frame_stem(report)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        trace = read_game_file(portal, package, r'\LocalState', stem + '.txt', 4096)
        if re.search(r'^sdl_(?:software|offscreen)_frame=ready\r?$', decode_report(trace or b''), re.MULTILINE):
            read_sdl_frame(portal, package, report, destination)
            return
        time.sleep(0.5)
    raise ValueError('No frame was captured after the game’s launch report.')


def xbox_executable_requirement(summary):
    """Identify executable architectures outside the x64 Xbox app process model."""
    if summary.get('pe_machine') == 'x86':
        flags = summary.get('clr_flags')
        if flags == 'unavailable':
            return 'unknown'
        if flags is None:
            return 'requires-x86'
        clr = int(flags, 16)
        if (clr & 0x20002) == 0x20000:
            return 'unknown'
        if summary['clr_requires_x86'] == 'yes' or not clr & 1:
            return 'requires-x86'
        return 'managed-il'
    if summary.get('pe_machine') == 'x64':
        return 'x64'
    return 'unknown'


GAME_PATH_COMPONENT = re.compile(r"[\w -][\w .()'’\-]*(?<![ .])")


def export_game_inputs(portal, package, app_id, knownfolder, steamapps, raw, profile, output):
    from build_tools.verify_win32_source import read_profile
    from tools.steam_compatibility import parse_installed_manifest
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise ValueError('Use a new export directory under an existing parent.')
    build, install, state = parse_installed_manifest(raw, app_id)
    if state != '4':
        raise ValueError('Steam must show a completed install before exporting game inputs.')
    if not GAME_PATH_COMPONENT.fullmatch(install):
        raise ValueError('The Steam manifest has no usable local install folder.')
    entries = read_profile(profile).get(build)
    if entries is None:
        raise ValueError(f'The source profile does not support installed Steam build {build}.')

    with TemporaryDirectory(dir=output.parent) as temporary:
        stage = Path(temporary) / 'inputs'
        stage.mkdir(mode=0o700)
        for row in entries:
            parts = row['path'].split('/')
            folder = '\\'.join((steamapps, 'common', install, *parts[:-1]))
            target = stage.joinpath(*parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            digest = hashlib.sha256()
            size = row['size']

            def receive(response):
                remaining = size
                with target.open('xb') as destination:
                    while remaining:
                        chunk = response.read(min(1024 * 1024, remaining))
                        if not chunk:
                            raise ValueError(f'Incomplete Xbox file: {row["path"]}.')
                        remaining -= len(chunk)
                        digest.update(chunk)
                        destination.write(chunk)
                    if response.read(1) or digest.hexdigest() != row['sha256']:
                        raise ValueError(f'Xbox source fingerprint mismatch: {row["path"]}.')

            portal.request('GET', game_file_url(package, folder, parts[-1], knownfolder), receive=receive)
        fresh = read_game_file(portal, package, steamapps, f'appmanifest_{app_id}.acf',
                               1024 * 1024 + 1, knownfolder=knownfolder)
        if fresh is None or parse_installed_manifest(fresh, app_id) != (build, install, state):
            raise ValueError('The Xbox Steam installation changed during the export.')
        if output.exists() or output.is_symlink():
            raise FileExistsError(f'Export directory already exists: {output}.')
        os.rename(stage, output)
    print(f'Exported {len(entries)} verified inputs for Steam build {build} to {output}.')


def inspect_game(portal, package, app_id, executable, source_profile=None, export_inputs=None):
    libraries = (('D', 'DevelopmentFiles', DEVELOPER_STEAMAPPS_PATH), ('local', 'LocalAppData', STEAMAPPS_PATH))
    manifest_limit = 1024 * 1024 + 1 if export_inputs is not None else 65536
    for drive, knownfolder, steamapps in libraries:
        raw = read_game_file(portal, package, steamapps, f'appmanifest_{app_id}.acf', manifest_limit, knownfolder=knownfolder)
        if raw is not None:
            break
    else:
        raise ValueError('Steam has no manifest for this AppID on Device Portal’s internal libraries.')
    print(f'steam_library_drive={drive}')
    manifest = decode_report(raw)
    fields = ('StateFlags', 'UpdateResult', 'buildid', 'BytesToDownload', 'BytesDownloaded', 'SizeOnDisk')
    for field in fields:
        match = re.search(rf'^[ \t]*"{field}"[ \t]+"([0-9]+)"', manifest, re.MULTILINE | re.IGNORECASE)
        if match:
            print(f'{field}={match[1]}')
    if export_inputs is not None:
        export_game_inputs(portal, package, app_id, knownfolder, steamapps, raw, source_profile, export_inputs)
    if executable is None:
        return
    install = re.search(r'^[ \t]*"installdir"[ \t]+"([^"\r\n]+)"', manifest, re.MULTILINE | re.IGNORECASE)
    if not install or not GAME_PATH_COMPONENT.fullmatch(install[1]):
        raise ValueError('The Steam manifest has no usable local install folder.')
    parts = executable.replace('\\', '/').split('/')
    if not parts[-1].lower().endswith('.exe') or any(not GAME_PATH_COMPONENT.fullmatch(p) for p in parts):
        raise ValueError('Use an .exe path relative to the game folder, such as jre/bin/javaw.exe.')

    candidates = (('installed', ('common', install[1])), ('staging', ('downloading', str(app_id))))
    for source, folder in candidates:
        path = '\\'.join((steamapps, *folder, *parts[:-1]))
        header = read_game_file(portal, package, path, parts[-1], 131072, knownfolder=knownfolder)
        if header is not None:
            print(f'executable_source={source}')
            summary = pe_summary(header)
            for key, value in summary.items():
                print(f'{key}={value}')
            print(f'xbox_executable_requirement={xbox_executable_requirement(summary)}')
            return
    print('executable_source=missing')


def stop_probe_for_sdl_trial(portal, package):
    encoded = base64.b64encode(package['PackageFullName'].encode()).decode('ascii')
    try:
        portal.request('DELETE', '/api/taskmanager/app?' + urlencode({'package': encoded}))
    except PortalError as error:
        if error.status != 404 and not (error.status == 400 and re.search(
                r'"ErrorCode"\s*:\s*-2147483635\b', str(error))):
            raise


def trial_sdl(portal, args):
    name, upstream, original_hash = SDL_TRIALS[args.app_id]
    package = find_installed_probe(portal, PROBE_STEAM_CORE_NAME, args.version)
    folder = f'{STEAMAPPS_PATH}\\common\\{name}'
    candidate = load_sdl_artifact(args.artifact, args.app_id)
    if args.install_only:
        raw = read_game_file(portal, package, STEAMAPPS_PATH, f'appmanifest_{args.app_id}.acf', 65536)
        manifest = decode_report(raw or b'')
        flags = re.MULTILINE | re.IGNORECASE
        if not (re.search(rf'^\s*"appid"\s+"{args.app_id}"\s*$', manifest, flags)
                and re.search(rf'^\s*"installdir"\s+"{re.escape(name)}"\s*$', manifest, flags)
                and re.search(r'^\s*"StateFlags"\s+"4"\s*$', manifest, flags)):
            raise ValueError('Steam has not verified a complete installation for this SDL profile.')

    def read_sdl():
        data = read_game_file(portal, package, folder, 'SDL2.dll', SDL_LIMIT + 1)
        if not data or len(data) > SDL_LIMIT:
            raise ValueError('The Xbox install does not contain a readable SDL2.dll of the expected size.')
        return data

    def replace_sdl(data):
        boundary = uuid4().hex
        body = (f'--{boundary}\r\nContent-Disposition: form-data; name="SDL2.dll"; filename="SDL2.dll"\r\n'
                'Content-Type: application/octet-stream\r\n\r\n').encode() + data + f'\r\n--{boundary}--\r\n'.encode()
        query = urlencode({'knownfolderid': 'LocalAppData', 'packagefullname': package['PackageFullName'],
                           'path': folder, 'extract': 'false'})
        portal.request('POST', '/api/filesystem/apps/file?' + query, body,
                       {'Content-Type': f'multipart/form-data; boundary={boundary}'})
        if read_sdl() != data:
            raise ValueError('The uploaded SDL2.dll did not match the bytes read back from the Xbox.')

    args.backup_dir.mkdir(parents=True, exist_ok=True)
    backup = args.backup_dir / f'{args.app_id}-SDL2-{original_hash[:12]}.dll'
    lock = backup.with_suffix('.lock')
    fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(fd)
    try:
        current = read_sdl()
        if args.restore_only and hashlib.sha256(current).hexdigest() == original_hash:
            print(f'Verified the original SDL2 for {name}.', flush=True)
            return
        if (args.install_only and current == candidate and backup.exists()
                and hashlib.sha256(backup.read_bytes()).hexdigest() == original_hash):
            print(f'The verified SDL {upstream} runtime is already installed for {name}.', flush=True)
            return
        if not args.restore_only:
            if hashlib.sha256(current).hexdigest() != original_hash:
                raise ValueError('The installed SDL2 differs from the known original. Check the game version or restore a previous trial.')
            try:
                with open(backup, 'xb', opener=lambda path, flags: os.open(path, flags, 0o600)) as output:
                    output.write(current)
            except FileExistsError:
                pass
        original = backup.read_bytes()
        if hashlib.sha256(original).hexdigest() != original_hash:
            raise ValueError('The SDL2 backup checksum differs from the known installed original.')
        if args.restore_only:
            if not candidate.startswith(current):
                raise ValueError('The installed SDL2 matches neither the original nor this trial artifact.')
            stop_probe_for_sdl_trial(portal, package)
            replace_sdl(original)
            print(f'Verified the original SDL2 for {name}.', flush=True)
        else:
            if not args.install_only:
                _, raw = portal.request('GET', probe_report_path(package))
                if not re.search(rf'^steam_launch_(?:repeat_)?app_id={args.app_id}\r?$', decode_report(raw), re.MULTILINE):
                    raise ValueError('First run the signed Xbox diagnostic for the selected AppID to establish its stock baseline.')
            stop_probe_for_sdl_trial(portal, package)
            applied = False
            retain = False
            try:
                replace_sdl(candidate)
                applied = True
                if args.install_only:
                    retain = True
                    print(f'Installed and verified SDL {upstream} for {name}. Original backup: {backup}', flush=True)
                    return
                print(f'Testing the SDL {upstream} rebuild for {name}.', flush=True)
                report = wait_for_probe(portal, PROBE_STEAM_CORE_NAME, args.version, False, args.timeout)
                fields = report_fields(report)
                app_id = fields.get('steam_launch_app_id') or fields.get('steam_launch_repeat_app_id')
                if app_id != str(args.app_id) or fields.get('steam_launch_submitted') != 'yes':
                    raise ValueError(f'The fresh report did not record Steam Play for AppID {args.app_id}. '
                                     'Prepare the diagnostic with --repeat-launch.')
                if args.frame is not None:
                    wait_for_sdl_frame(portal, package, args.frame, args.frame_timeout)
            finally:
                if not retain:
                    try:
                        installed = read_sdl()
                        if installed != original:
                            if installed != candidate and (applied or not candidate.startswith(installed)):
                                raise ValueError('SDL2 changed during the trial. The backup was kept without overwriting the change.')
                            stop_probe_for_sdl_trial(portal, package)
                            replace_sdl(original)
                        print(f'Verified the original SDL2 for {name}.', flush=True)
                    except (OSError, ValueError):
                        print(f'SDL restore needs a retry with --restore-only. Backup: {backup}', file=sys.stderr)
                        raise
    finally:
        lock.unlink()


def local_state_query(app, **parameters):
    return urlencode({'knownfolderid': 'LocalAppData', 'packagefullname': app['PackageFullName'],
                      'path': r'\\LocalState', **parameters})


def read_probe_report(portal, app, filename=PROBE_REPORT):
    query = local_state_query(app, filename=filename)
    try:
        _, raw = portal.request('GET', f'/api/filesystem/apps/file?{query}')
    except PortalError as error:
        if error.status not in (404, 409):
            raise
        return ''
    encoding = 'utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8'
    try:
        report = raw.decode(encoding)
    except UnicodeDecodeError:
        return ''  # The report can be read while the app is replacing it.
    return report if report.endswith('\n') else ''


def activate_probe(portal, app):
    # Match Xbox Device Portal's App Launcher, which sends the AUMID only.
    launch = {'appid': base64.b64encode(app['PackageRelativeId'].encode()).decode('ascii')}
    portal.request('POST', '/api/taskmanager/app?' + urlencode(launch))


def stop_probe(portal, app, allow_inactive=False):
    package = base64.b64encode(app['PackageFullName'].encode()).decode('ascii')
    try:
        portal.request('DELETE', '/api/taskmanager/app?' + urlencode({'package': package}))
    except PortalError as error:
        if error.status == 404:
            return
        if allow_inactive and error.status == 400:
            try:
                response = json.loads(error.response)
            except json.JSONDecodeError:
                response = {}
            if isinstance(response, dict) and response.get('ErrorCode') == -2147483635:
                return  # Xbox returns E_ILLEGAL_STATE_CHANGE for a freshly installed, inactive app.
        raise


def package_processes(portal, app):
    _, raw = portal.request('GET', '/api/resourcemanager/processes', read_limit=4 * 1024 * 1024)
    return sorted((item['ImageName'], item['ProcessId']) for item in json.loads(raw)['Processes']
                  if item.get('PackageFullName') == app['PackageFullName'])


def relaunch_package(portal, app, timeout, *, clock=time.monotonic, pause=time.sleep):
    """Stop every process of an installed package, wait until none remain, then activate it again."""
    before = package_processes(portal, app)
    stop_probe(portal, app, allow_inactive=True)
    started = clock()
    while True:
        remaining = package_processes(portal, app)
        if not remaining:
            break
        if clock() - started >= timeout:
            names = ', '.join(sorted({image for image, _ in remaining}))
            raise ValueError(f'{app["PackageFullName"]} still has running processes after {timeout}s ({names}). '
                             'It was not activated again.')
        pause(0.5)
    stopped_seconds = round(clock() - started, 3)
    activate_probe(portal, app)
    return {'package': app['PackageFullName'], 'stopped_processes': [image for image, _ in before],
            'stop_seconds': stopped_seconds, 'activated_utc': utc_text(time.time())}


def collect_probe(portal, key, version, manual, timeout, restart=False, json_output=False):
    case = CASES[key]
    app = find_installed_probe(portal, case.package, version)
    package = app['PackageFullName']
    stream = sys.stderr if json_output else sys.stdout
    print(f'Installed package: {package}', file=stream, flush=True)
    report_url = probe_report_path(app, case.filename)

    def read_report(*, polling=False):
        try:
            _, raw = portal.request('GET', report_url)
        except PortalError as error:
            if error.status != 404 and not (polling and error.status == 409):
                raise
            return ''
        return decode_report(raw)

    def launch():
        if manual:
            print(f'Launch {case.label} once from Xbox Dev Home.', file=stream, flush=True)
            return False
        if restart:
            stop_probe(portal, app, allow_inactive=True)
        try:
            activate_probe(portal, app)
        except PortalError as error:
            print('Remote launch returned an error. You can launch from Dev Home while the CLI waits.',
                  file=stream, flush=True)
            print(error, file=stream, flush=True)
            return True
        return False

    def progress(report):
        if not json_output:
            print(report, end='', flush=True)

    result = run_case(case, read_report, launch, timeout, progress)
    if json_output:
        print(json.dumps({
            'schema_version': 1, 'case': key, 'package': package, 'report': case.filename,
            'outcome': result.outcome, 'message': result.message,
            'checks': dict(case.checks), 'observed': result.evidence(),
        }, indent=2))
        if result.outcome != 'passed':
            raise SystemExit(1)
    elif result.outcome != 'passed':
        raise ValueError(result.message)
    elif key == 'big-picture':
        print('Steam was running at the observation deadline. Check the Xbox display for Big Picture.')
    return result.report


def wait_for_probe(portal, name, version, manual, timeout, require_ipc=False, restart=False):
    key = 'named-pipe' if require_ipc else PACKAGE_CASES[name]
    report = collect_probe(portal, key, version, manual, timeout, restart)
    if name in (PROBE_STEAM_CORE_NAME, BIG_PICTURE_NAME):
        return report


def wait_for_checkpoint(portal, app, filename, baseline, timeout, checkpoint):
    deadline = time.monotonic() + timeout
    last = baseline
    while time.monotonic() < deadline:
        report = read_probe_report(portal, app, filename)
        if report and report not in (baseline, last):
            last = report
            print(report, end='', flush=True)
            fields = report_fields(report)
            if fields.get('lifetime') == 'failed':
                raise ValueError('The lifetime probe reported a failure above.')
            if checkpoint(fields):
                return report, fields
        time.sleep(0.5)
    raise ValueError(f'Timed out waiting for the next checkpoint in {filename}.')


def check_lifetime_continuity(ready, returned):
    keys = ('run_id', 'parent_pid', 'channel_id', 'helper_pid', 'helper_token')
    if any(not ready.get(key) or ready[key] != returned.get(key) for key in keys):
        raise ValueError('The original app, helper, or connection identity changed.')
    if all(returned.get(event) != 'yes' for event in ('observed_hidden', 'observed_background')):
        raise ValueError('The app did not observe leaving the foreground.')
    if 'return_challenge' not in returned:
        raise ValueError('The return report is missing its challenge.')
    challenge = int(returned['return_challenge'], 16)
    token = int(ready['helper_token'], 16)
    expected = 0x51000000 | ((challenge ^ token) & 0xFFFFFF)
    if int(returned.get('lifetime_child_exit', '0'), 16) != expected:
        raise ValueError('The original helper did not acknowledge the return challenge.')


def observe_probe_processes(portal, ready, phase):
    _, data = portal.request('GET', '/api/resourcemanager/processes')
    pids = {entry['ProcessId'] for entry in json.loads(data)['Processes']}
    for role in ('parent', 'helper'):
        pid = ready[f'{role}_pid']
        print(f'{phase} {role}_pid={pid} listed={int(pid, 16) in pids}', flush=True)


def request_protocol_return(portal, control, run):
    boundary = uuid4().hex
    filename = 'lifetime-return.txt'
    body = (f'--{boundary}\r\n'
            f'Content-Disposition: form-data; name="{filename}"; filename="{filename}"\r\n'
            f'Content-Type: text/plain\r\n\r\n{run}\r\n'
            f'--{boundary}--\r\n').encode()
    query = local_state_query(control, extract='false')
    portal.request('POST', f'/api/filesystem/apps/file?{query}', body,
                   {'Content-Type': f'multipart/form-data; boundary={boundary}'})


STEAM_CLOUD_SYNC_REPORT = 'steam-cloud-sync-report.txt'
CLOUD_CHOICES = ('cloud', 'local')


def cloud_resolve_upload(app_id, keep, boundary):
    """Build the one-shot conflict choice the next launch of app_id consumes."""
    if not 0 < app_id <= 0xFFFFFFFF or keep not in CLOUD_CHOICES or not re.fullmatch(r'[0-9a-f]{32}', boundary):
        raise ValueError('Use a positive AppID and keep cloud or local.')
    filename = f'steam-cloud-resolve-{app_id}.txt'
    body = (f'--{boundary}\r\n'
            f'Content-Disposition: form-data; name="{filename}"; filename="{filename}"\r\n'
            # The file ends in a newline: the CRLF before the boundary belongs to the delimiter,
            # and read_probe_report treats a file without a final newline as still being written.
            f'Content-Type: text/plain\r\n\r\n{keep}\n\r\n'
            f'--{boundary}--\r\n').encode()
    return filename, body


def write_cloud_resolve(portal, app, app_id, keep):
    boundary = uuid4().hex
    filename, body = cloud_resolve_upload(app_id, keep, boundary)
    query = local_state_query(app, extract='false')
    portal.request('POST', f'/api/filesystem/apps/file?{query}', body,
                   {'Content-Type': f'multipart/form-data; boundary={boundary}'})
    if read_probe_report(portal, app, filename).strip() != keep:
        raise ValueError(f'{filename} did not read back as {keep}.')
    print(f'{filename}={keep}. The next launch of {app_id} applies it once if Steam Cloud reports a conflict.')


def run_lifetime(portal, version, dwell, timeout, portal_return):
    app = find_installed_probe(portal, PROBE_LIFETIME_NAME, version)
    control = find_installed_probe(portal, PROBE_CONTROL_NAME, version)
    stop_probe(portal, app, allow_inactive=True)
    baseline = read_probe_report(portal, app, LIFETIME_REPORT)
    try:
        print('Starting the lifetime probe.', flush=True)
        activate_probe(portal, app)
        ready_report, ready = wait_for_checkpoint(
            portal, app, LIFETIME_REPORT, baseline, timeout,
            lambda fields: fields.get('lifetime') == 'ready')

        observe_probe_processes(portal, ready, 'before-switch')

        print('Switching to the control app.', flush=True)
        control_baseline = read_probe_report(portal, control)
        activate_probe(portal, control)
        _, foreground = wait_for_checkpoint(
            portal, control, PROBE_REPORT, control_baseline, timeout,
            lambda fields: fields.get('status') == 'control-ready' and fields.get('control_visible') == 'yes')

        observe_probe_processes(portal, ready, 'control-foreground')

        print(f'Keeping the control app foreground for {dwell} seconds.', flush=True)
        time.sleep(dwell)
        if read_probe_report(portal, app, LIFETIME_REPORT) != ready_report:
            raise ValueError('The lifetime report changed before the requested return.')

        observe_probe_processes(portal, ready, 'before-return')
        if portal_return:
            print('Returning through the Device Portal app launcher.', flush=True)
            activate_probe(portal, app)
        else:
            print('Requesting a protocol return from the control app.', flush=True)
            request_protocol_return(portal, control, foreground['run_id'])
        try:
            _, returned = wait_for_return(portal, app, ready_report, ready, timeout)
        except ValueError:
            if not portal_return:
                report = read_probe_report(portal, control, PROTOCOL_REPORT)
                if report_fields(report).get('run_id') == foreground['run_id']:
                    print(report, end='', flush=True)
            raise
        check_lifetime_continuity(ready, returned)
        print('Verified the original helper transaction across the foreground switch.', flush=True)
    finally:
        try:
            stop_probe(portal, app)
        except (ValueError, OSError, http.client.HTTPException) as error:
            print(f'Could not stop the dedicated lifetime probe: {error}', file=sys.stderr)


def wait_for_return(portal, app, baseline, ready, timeout):
    def completed(fields):
        if fields.get('run_id') and fields['run_id'] != ready.get('run_id'):
            raise ValueError('Returning activated a new app instance. The original connection is unverified.')
        return fields.get('lifetime') == 'complete'

    return wait_for_checkpoint(portal, app, LIFETIME_REPORT, baseline, timeout, completed)


def print_install_result(data):
    result = json.loads(data) if data.strip() else {}
    if isinstance(result, dict) and result.get('Success') is True:
        print('Xbox Device Portal reports a successful install.')
    else:
        raise ValueError(f'Xbox Device Portal did not report a successful install: {result}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--env-file', type=Path, help='use a local .env from another workspace')
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('fingerprint', help='show the certificate fingerprint without signing in')
    commands.add_parser('status', help='check the latest installation status')
    deploy = commands.add_parser('deploy', help='upload a signed package and wait for installation')
    deploy.add_argument('package', type=Path)
    deploy.add_argument('--dependency', action='append', type=Path, default=[])
    deploy.add_argument('--certificate', type=Path, help='optional package-signing .cer')
    deploy.add_argument('--test-probe', action='store_true', help='launch this probe and wait for its report')
    probe = commands.add_parser('probe', help='launch the installed probe and wait for a fresh report')
    probe.add_argument('--version', help='require a specific installed package version')
    probe.add_argument('--restart', action='store_true', help='stop the selected probe before launching it')
    probe.add_argument('--json', action='store_true', help='emit one structured probe result')
    variant = probe.add_mutually_exclusive_group()
    variant.add_argument('--list', action='store_true', help='list registered probe cases without connecting')
    variant.add_argument('--case', choices=CASES, help='run a named probe case')
    variant.add_argument('--control', dest='probe_name', action='store_const', const=PROBE_CONTROL_NAME,
                         help='watch the clean-registration control')
    variant.add_argument('--graphics-control', dest='probe_name', action='store_const', const=GRAPHICS_CONTROL_NAME,
                         help='watch the static OpenGL import and EGL render control')
    variant.add_argument('--steam-core', dest='probe_name', action='store_const', const=PROBE_STEAM_CORE_NAME,
                         help='watch the Steam Core Loader Probe')
    variant.add_argument('--big-picture', dest='probe_name', action='store_const', const=BIG_PICTURE_NAME,
                         help='watch the native Steam gamepad-mode launcher')
    probe.set_defaults(probe_name=PROBE_NAME)
    report_command = commands.add_parser('report', help='read the current Steam probe report without launching it')
    report_command.add_argument('--version', required=True, help='installed Steam package version')
    report_variant = report_command.add_mutually_exclusive_group()
    report_variant.add_argument('--big-picture', dest='report_file', action='store_const',
                                const=BIG_PICTURE_REPORT, help='read the native Steam gamepad-mode launcher report')
    report_variant.add_argument('--big-picture-webview', dest='report_file', action='store_const',
                                const=WEBVIEW_REPORT, help='read the Xbox WebView2 device-test stages')
    report_variant.add_argument('--steam-core-webview', dest='report_file', action='store_const',
                                const='steam-core-webview', help='read WebView2 stages after Steam Core sign-in')
    report_variant.add_argument('--big-picture-activation', dest='report_file', action='store_const',
                                const=ACTIVATION_REPORT, help='read the WebView2 and renderer activation checks')
    report_variant.add_argument('--download', dest='report_file', action='store_const',
                                const='steam-download-report.txt', help='read the Steam download checkpoint')
    report_variant.add_argument('--artwork', dest='report_file', action='store_const',
                                const='steam-library-art.txt', help='read the Steam library artwork counters')
    report_variant.add_argument('--inventory', dest='report_file', action='store_const',
                                const='steam-library-inventory.json', help='read the opted-in game name and AppID inventory')
    report_variant.add_argument('--storage-migration', dest='report_file', action='store_const',
                                const='steam-library-migration.json', help='read the Steam library migration result')
    report_variant.add_argument('--steam-cloud', dest='report_file', action='store_const',
                                const='steam-cloud-report.txt', help='read the Steam Cloud restore checkpoint')
    report_variant.add_argument('--steam-cloud-sync', dest='report_file', action='store_const',
                                const=STEAM_CLOUD_SYNC_REPORT,
                                help='read the Win32 trial launch download and exit upload of Steam Cloud files')
    report_variant.add_argument('--sdl-startup', dest='report_file', action='store_const',
                                const='sdl-startup', help='read the SDL startup checkpoints for the current launch')
    report_variant.add_argument('--slay-console', dest='report_file', action='store_const',
                                const='slay-console', help='summarize the publisher console diagnostic for this launch')
    report_variant.add_argument('--stardew-host', dest='report_file', action='store_const',
                                const='stardew-host', help='summarize the current Stardew Valley .NET host trace')
    report_variant.add_argument('--sdl-frame', type=Path, metavar='BMP', help='save the current trial’s SDL frame')
    report_command.set_defaults(report_file=PROBE_REPORT)
    observe_command = commands.add_parser('observe', help='watch Steam startup or a game launch without controlling the Xbox')
    observe_command.add_argument('--version', required=True, help='installed Steam package version')
    observation = observe_command.add_mutually_exclusive_group(required=True)
    observation.add_argument('--startup', action='store_true', help='wait for a new Big Picture startup')
    observation.add_argument('--app-id', type=int, help='wait for a new launch of this Steam AppID')
    observe_command.add_argument('--run-id', type=int, help='collect an existing game launch with this steam_launch_run_id')
    observe_command.add_argument('--image-name', help='optionally observe this game process by executable basename')
    observe_command.add_argument('--timeout', type=int, default=150,
                                 help=f'observation deadline in seconds (1–{OBSERVE_LIMIT}, default 150)')
    observe_command.add_argument('--frame-seconds', type=int, default=30,
                                 help=f'seconds to observe after the first hosted frame (0–{OBSERVE_LIMIT}, default 30)')
    observe_command.add_argument('--require-checkpoint', action='append', default=[], metavar='[SECONDS:]TRACE',
                                 help='require this loader record or prefix in sequence, optionally with its own deadline')
    observe_command.add_argument('--fail-on-checkpoint', action='append', default=[], metavar='TRACE',
                                 help='fail immediately when this loader record or prefix appears')
    observe_command.add_argument('--watch-checkpoint', action='append', default=[], metavar='TRACE',
                                 help='include the last matching record in the result without extending the deadline')
    observe_command.add_argument('--checkpoint-timeout', type=int, metavar='SECONDS',
                                 help='seconds to wait per required checkpoint after the game starts (default 20)')
    observe_command.add_argument('--output', type=Path, help='save filtered observations and available run artifacts to a new directory')
    observe_command.add_argument('--bootstrap-frame', action='store_true', help='save the startup BMP when available; requires --output')
    inspections = {
        'inspect-render': commands.add_parser('inspect-render', help='read GPU mailbox state and optional screenshots for an existing launch'),
        'inspect-loader': commands.add_parser('inspect-loader', help='read the filtered live loader trace for an existing launch'),
        'inspect-input': commands.add_parser('inspect-input', help='sample hosted game input without controlling the Xbox'),
    }
    for inspection in inspections.values():
        inspection.add_argument('--version', required=True, help='installed Steam package version')
        inspection.add_argument('--app-id', type=int, required=True, help='Steam AppID of the existing launch')
        inspection.add_argument('--run-id', type=int, required=True, help='exact steam_launch_run_id of the existing launch')
        inspection.add_argument('--output', type=Path, help='save inspection metadata and available artifacts to a new directory')
    inspections['inspect-input'].add_argument('--seconds', type=int, default=0,
                                             help='sample duration, 0 for one snapshot (default 0, maximum 30)')
    inspections['inspect-input'].add_argument('--panel-menu', action='store_true',
                                             help='read optional package-level panel and menu checkpoints')
    game = commands.add_parser('inspect-game', help='read one Steam install manifest and optional executable header')
    game.add_argument('--version', required=True, help='installed Steam package version')
    game.add_argument('--app-id', type=int, required=True, help='Steam AppID')
    game.add_argument('--executable', help='relative .exe path within the game folder')
    game.add_argument('--source-profile', type=Path, help='trusted Win32 fingerprint file for exporting owned inputs')
    game.add_argument('--export-inputs', type=Path, help='save the build-matched profile files to a new local directory')
    resolve = commands.add_parser('cloud-resolve',
                                  help='override which side wins a Steam Cloud conflict at the next Win32 trial launch (default: cloud)')
    resolve.add_argument('--version', required=True, help='installed Steam package version')
    resolve.add_argument('--app-id', type=int, required=True, help='Steam AppID')
    resolve.add_argument('--keep', required=True, choices=CLOUD_CHOICES,
                         help='cloud downloads the cloud files, local uploads this console\'s files; '
                              'local files are backed up under LocalState\\SteamCloudSync first')
    sdl = commands.add_parser('sdl-trial', help='test a version-matched SDL2 workflow artifact and restore the original')
    sdl.add_argument('--version', required=True, help='installed Steam diagnostic package version')
    sdl.add_argument('--app-id', required=True, type=int, choices=tuple(SDL_TRIALS))
    sdl.add_argument('--artifact', required=True, type=Path, help='workflow artifact directory with SDL2.dll and SHA256SUMS')
    sdl.add_argument('--frame', type=Path, metavar='BMP', help='wait for and save the trial’s frame before restoring SDL')
    sdl.add_argument('--frame-timeout', type=int, default=20, help='seconds to wait for a frame after the launch report')
    sdl.add_argument('--backup-dir', required=True, type=Path, help='directory for the verified original DLL and trial lock')
    sdl_mode = sdl.add_mutually_exclusive_group()
    sdl_mode.add_argument('--restore-only', action='store_true', help='restore a previously tested artifact without launching')
    sdl_mode.add_argument('--install-only', action='store_true', help='leave the verified SDL runtime in the selected installed title')
    sdl.add_argument('--timeout', type=int, default=100, help='seconds to wait for a fresh launch report')
    lifetime = commands.add_parser('lifetime', help='verify a helper pipe across a foreground switch')
    lifetime.add_argument('--version', required=True, help='installed version of both lifetime and control')
    lifetime.add_argument('--portal-return', action='store_true',
                          help='reproduce return through Device Portal’s app launcher')
    lifetime.add_argument('--dwell', type=int, default=15, help='seconds to keep the control foreground')
    relaunch = commands.add_parser(
        'relaunch', help='stop the installed Steam Core package and its game, then activate it again',
        description='Stop the installed Steam Core package and any game it started, wait until none of its '
                    'processes remain, then activate it again. This restarts Steam only. A configured '
                    '--play-installed-app-id Play runs once per installed package version and is not repeated; '
                    'select Play on the console or install a new package version to launch the game again.')
    relaunch.add_argument('--version', help='require a specific installed package version')
    relaunch.add_argument('--stop-timeout', type=int, default=30, help='seconds to wait for every package process to exit')
    sampler = commands.add_parser('sample-processes',
                                  help='record memory and CPU of the Steam Core package and named processes without controlling the Xbox')
    sampler.add_argument('--version', help='require a specific installed package version')
    sampler.add_argument('--image-name', action='append', default=[], metavar='EXE',
                         help='also sample this executable basename, for example a game outside the package')
    sampler.add_argument('--seconds', type=int, required=True, help='sampling duration (1–7200)')
    sampler.add_argument('--interval', type=float, default=1.0, help='seconds between samples (0.5–60, default 1)')
    sampler.add_argument('--output', type=Path, required=True, help='new JSON Lines file for samples and process events')
    shots = commands.add_parser('screenshots', help='save Device Portal screenshots on a schedule without controlling the Xbox')
    shots.add_argument('--output', type=Path, required=True, help='new directory for PNG files and screenshots.jsonl')
    shots.add_argument('--delay', type=float, default=0, help='seconds before the first screenshot (default 0)')
    shots.add_argument('--interval', type=float, default=30, help='seconds between screenshots (default 30)')
    shots.add_argument('--count', type=int, default=1, help='number of screenshots (1–240, default 1)')
    list_command = commands.add_parser('list-files', help='list a Steam Core package folder such as \\TempState')
    list_command.add_argument('folder', help='package folder beginning with \\LocalState, \\TempState, \\LocalCache, \\RoamingState or \\AC')
    list_command.add_argument('--version', help='require a specific installed package version')
    list_command.add_argument('--recursive', action='store_true', help='include subfolders up to six levels deep')
    list_command.add_argument('--since-utc', help='keep entries created or modified at or after this UTC time')
    get_command = commands.add_parser('get-file', help='copy one file from a Steam Core package folder')
    get_command.add_argument('folder', help='package folder beginning with \\LocalState, \\TempState, \\LocalCache, \\RoamingState or \\AC')
    get_command.add_argument('name', help='file name within the folder')
    get_command.add_argument('--version', help='require a specific installed package version')
    get_command.add_argument('--output', type=Path, required=True, help='new local file')
    for command in (deploy, probe, lifetime):
        command.add_argument('--timeout', type=int, default=20, help='seconds to wait per checkpoint')
    for command in (deploy, probe):
        command.add_argument('--manual-launch', action='store_true', help='wait for launch from Xbox Dev Home')
        command.add_argument('--require-ipc', action='store_true', help='require a successful named-pipe exchange')
    args = parser.parse_args()
    if args.command == 'probe' and args.list:
        if args.json:
            print(json.dumps({key: case.description for key, case in CASES.items()}, indent=2))
        else:
            for key, case in CASES.items():
                print(f'{key:16} {case.description}')
        return
    if args.command == 'deploy' and args.require_ipc and not args.test_probe:
        deploy.error('--require-ipc requires --test-probe')
    if args.command == 'probe' and args.case and args.require_ipc:
        probe.error('Use --case named-pipe to require a named-pipe exchange.')
    if args.command == 'probe' and args.require_ipc and args.probe_name != PROBE_NAME:
        probe.error('--require-ipc applies to the main probe')
    if args.command == 'probe' and args.restart and args.manual_launch:
        probe.error('--restart uses a CLI launch and cannot be combined with --manual-launch')
    if args.command == 'report' and args.report_file in BIG_PICTURE_REPORTS and args.sdl_frame:
        report_command.error('--sdl-frame applies to a Steam Core game trial')
    if args.command == 'observe' or args.command in inspections:
        diagnostic_command = observe_command if args.command == 'observe' else inspections[args.command]
        if not re.fullmatch(r'[0-9]{1,5}(?:\.[0-9]{1,5}){3}', args.version) or any(
                int(part) > 65535 for part in args.version.split('.')):
            diagnostic_command.error('--version must be a four-part package version')
        if args.app_id is not None and not 1 <= args.app_id <= 0xFFFFFFFF:
            diagnostic_command.error('--app-id must be a positive Steam AppID')
        if args.run_id is not None and not (args.app_id and 1 <= args.run_id < 10 ** 20):
            diagnostic_command.error('--run-id requires --app-id and a positive launch ID')
        if args.output and (args.output.exists() or args.output.is_symlink() or not args.output.parent.is_dir()):
            diagnostic_command.error('--output must name a new directory under an existing parent')
    if args.command == 'inspect-input' and not 0 <= args.seconds <= 30:
        inspections[args.command].error('--seconds must be between 0 and 30')
    if args.command == 'observe':
        if args.image_name and not (args.app_id and re.fullmatch(r'[a-z0-9_. -]{1,100}\.exe', args.image_name, re.I)):
            observe_command.error('--image-name requires --app-id and an executable basename')
        if args.bootstrap_frame and not (args.output and args.app_id):
            observe_command.error('--bootstrap-frame requires --output and --app-id')
        # The bound only catches typos. The poll loop holds no per-second state, so an hour-long
        # gameplay session can be observed in one run.
        if not 1 <= args.timeout <= OBSERVE_LIMIT or not 0 <= args.frame_seconds <= OBSERVE_LIMIT:
            observe_command.error(f'Use a 1–{OBSERVE_LIMIT} second timeout and 0–{OBSERVE_LIMIT} seconds after the first frame')
        if (args.fail_on_checkpoint or args.watch_checkpoint or args.checkpoint_timeout is not None) and not args.require_checkpoint:
            observe_command.error('--fail-on-checkpoint, --watch-checkpoint and --checkpoint-timeout require --require-checkpoint')
        args.checkpoint_timeouts = []
        if args.require_checkpoint:
            from tools.xbox_diagnostics import valid_checkpoint_pattern

            if not args.app_id:
                observe_command.error('--require-checkpoint requires --app-id')
            if args.checkpoint_timeout is None:
                args.checkpoint_timeout = 20
            if not 1 <= args.checkpoint_timeout <= 900:
                observe_command.error('--checkpoint-timeout must be between 1 and 900 seconds')
            for index, checkpoint in enumerate(args.require_checkpoint):
                seconds, separator, trace = checkpoint.partition(':')
                if separator:
                    if not re.fullmatch(r'[0-9]{1,3}', seconds) or not 1 <= int(seconds) <= 900:
                        observe_command.error('A required checkpoint deadline must be between 1 and 900 seconds')
                    args.require_checkpoint[index] = trace
                args.checkpoint_timeouts.append(int(seconds) if separator else args.checkpoint_timeout)
            for checkpoints in (args.require_checkpoint, args.fail_on_checkpoint, args.watch_checkpoint):
                if (len(checkpoints) > 8 or len(set(checkpoints)) != len(checkpoints)
                        or not all(map(valid_checkpoint_pattern, checkpoints))):
                    observe_command.error('Use up to eight distinct loader record prefixes per checkpoint option. Use key=* to match a field value.')
    if args.command == 'inspect-game':
        if not 0 < args.app_id <= 0xFFFFFFFF:
            game.error('--app-id must be a positive Steam AppID')
        if (args.source_profile is None) != (args.export_inputs is None):
            game.error('Use --source-profile and --export-inputs together')

    if args.command == 'sdl-trial' and not 1 <= args.frame_timeout <= 120:
        sdl.error('--frame-timeout must be between 1 and 120 seconds')
    if args.command == 'relaunch' and not 1 <= args.stop_timeout <= 300:
        relaunch.error('--stop-timeout must be between 1 and 300 seconds')
    if args.command == 'sample-processes':
        if not 1 <= args.seconds <= 7200 or not 0.5 <= args.interval <= 60:
            sampler.error('Use 1–7200 seconds of sampling and an interval of 0.5–60 seconds')
        if any(not re.fullmatch(r'[a-z0-9_. -]{1,100}\.exe', name, re.I) for name in args.image_name):
            sampler.error('--image-name takes an executable basename')
    if args.command == 'screenshots' and not (1 <= args.count <= 240 and 0 <= args.delay <= 3600
                                              and 1 <= args.interval <= 3600):
        shots.error('Use 1–240 screenshots, a 0–3600 second delay and a 1–3600 second interval')
    if args.command in ('sample-processes', 'screenshots', 'get-file'):
        if args.output.exists() or args.output.is_symlink() or not args.output.parent.is_dir():
            parser.error('--output must name a new path under an existing directory')
    if args.command in ('list-files', 'get-file'):
        from tools.xbox_capture import file_name, package_folder, utc_filetime

        try:
            package_folder(args.folder)
            if args.command == 'get-file':
                file_name(args.name)
            args.since = utc_filetime(args.since_utc) if args.command == 'list-files' and args.since_utc else None
        except ValueError as error:
            (list_command if args.command == 'list-files' else get_command).error(str(error))

    settings = load_settings(args.env_file)
    fingerprint = settings.get('XBOX_PORTAL_CERT_SHA256', '').lower().replace(':', '')
    portal = DevicePortal(settings.get('XBOX_PORTAL_URL', ''), fingerprint)
    if args.command == 'fingerprint':
        print(portal.request())
        return
    if not re.fullmatch(r'[0-9a-f]{64}', fingerprint):
        raise ValueError('Set XBOX_PORTAL_CERT_SHA256 to the verified portal TLS fingerprint.')
    username = settings.get('XBOX_PORTAL_USERNAME', '')
    password = settings.get('XBOX_PORTAL_PASSWORD', '')
    if ':' in username or (password and not username):
        raise ValueError('Set XBOX_PORTAL_USERNAME to your Device Portal username.')

    if args.command == 'deploy':
        if args.package.suffix.lower() not in ('.appx', '.appxbundle', '.msix', '.msixbundle'):
            raise ValueError('Provide a signed APPX, APPXBUNDLE, MSIX, or MSIXBUNDLE file.')
        files = [args.package, *args.dependency]
        if args.certificate:
            files.append(args.certificate)
        for file in files:
            if not file.is_file():
                raise ValueError(f'File not found: {file}')
        if args.test_probe:
            name, version = probe_identity(args.package)
            if args.require_ipc and name != PROBE_NAME:
                deploy.error('--require-ipc applies to the main probe')
    if args.command in ('deploy', 'probe', 'lifetime', 'sdl-trial') and args.timeout <= 0:
        raise ValueError('--timeout must be a positive number of seconds.')

    if args.command == 'lifetime':
        if args.dwell <= 0:
            lifetime.error('--dwell must be a positive number of seconds')
        if not args.portal_return and args.dwell >= 120:
            lifetime.error('protocol return requires --dwell below the control app’s 120-second request window')

    observed = portal.request()
    if not hmac.compare_digest(observed, fingerprint):
        raise CertificateChanged(observed, fingerprint)
    if username:
        if not password:
            if not sys.stdin.isatty():
                raise ValueError('Set XBOX_PORTAL_PASSWORD or run this command in a terminal.')
            password = getpass.getpass('Xbox Device Portal password: ')
        portal.authorization = 'Basic ' + base64.b64encode(f'{username}:{password}'.encode()).decode('ascii')
    if args.command in ('relaunch', 'sample-processes', 'screenshots', 'list-files', 'get-file'):
        from tools import xbox_capture

        def progress(stage):
            print(stage, file=sys.stderr, flush=True)

        if args.command == 'screenshots':
            result = xbox_capture.capture_screenshots(portal, args.output, args.delay, args.interval, args.count,
                                                      progress=progress)
        else:
            app = find_installed_probe(portal, PROBE_STEAM_CORE_NAME, args.version)
            if args.command == 'relaunch':
                result = relaunch_package(portal, app, args.stop_timeout)
            elif args.command == 'sample-processes':
                images = list(dict.fromkeys([*args.image_name, *xbox_capture.CRASH_IMAGES]))
                progress(f'sampling {app["PackageFullName"]} and {", ".join(images)}')
                result = xbox_capture.sample_processes(portal, app['PackageFullName'], images, args.seconds,
                                                       args.interval, args.output, progress=progress)
            elif args.command == 'list-files':
                result = xbox_capture.list_files(portal, app, args.folder, recursive=args.recursive, since=args.since)
            else:
                result = xbox_capture.get_file(portal, app, args.folder, args.name, args.output)
        print(json.dumps(result, indent=2))
        return
    if args.command == 'lifetime':
        run_lifetime(portal, args.version, args.dwell, args.timeout, args.portal_return)
        return
    if args.command == 'probe':
        key = args.case or ('named-pipe' if args.require_ipc else PACKAGE_CASES[args.probe_name])
        collect_probe(portal, key, args.version, args.manual_launch, args.timeout,
                      restart=args.restart, json_output=args.json)
        return
    if args.command == 'observe':
        from tools.xbox_diagnostics import observe

        try:
            app = find_installed_probe(portal, PROBE_STEAM_CORE_NAME, args.version)
            result = observe(args, portal, app, read_game_file, decode_report, PortalError,
                             progress=lambda stage: print(stage, file=sys.stderr, flush=True))
        except PortalError as error:
            raise ValueError(f'Xbox Device Portal returned HTTP {error.status} while observing.') from None
        except (json.JSONDecodeError, TypeError, KeyError, AttributeError):
            raise ValueError('Xbox Device Portal returned an invalid observation response.') from None
        print(json.dumps(result, indent=2))
        if (result['outcome'] == 'observation-timeout'
                or (args.require_checkpoint and result['outcome'] != 'checkpoints-observed')):
            raise SystemExit(1)
        return
    if args.command in inspections:
        from tools.xbox_diagnostics import collect_live_loader, collect_render_state

        target = {'inspect-render': 'render state', 'inspect-loader': 'the live loader trace',
                  'inspect-input': 'hosted input'}[args.command]
        try:
            app = find_installed_probe(portal, PROBE_STEAM_CORE_NAME, args.version)
            if args.command == 'inspect-input':
                result = inspect_host_input(args, portal, app,
                                            progress=lambda stage: print(stage, file=sys.stderr, flush=True))
            else:
                collector = collect_render_state if args.command == 'inspect-render' else collect_live_loader
                result = collector(args, portal, app, read_game_file, decode_report, PortalError)
        except PortalError as error:
            raise ValueError(f'Xbox Device Portal returned HTTP {error.status} while inspecting {target}.') from None
        except (json.JSONDecodeError, TypeError, KeyError, AttributeError):
            raise ValueError(f'Xbox Device Portal returned an invalid response while inspecting {target}.') from None
        print(json.dumps(result, indent=2))
        return
    if args.command == 'report':
        name = BIG_PICTURE_NAME if args.report_file in BIG_PICTURE_REPORTS else PROBE_STEAM_CORE_NAME
        if args.report_file == 'steam-core-webview':
            args.report_file = WEBVIEW_REPORT
        app = find_installed_probe(portal, name, args.version)
        if args.report_file == 'sdl-startup':
            _, raw = portal.request('GET', probe_report_path(app))
            run = re.search(r'^steam_launch_sdl_run=([0-9]{1,20})\r?$', decode_report(raw), re.MULTILINE)
            if not run:
                print('sdl_startup=no-trace-request')
                return
            trace = read_game_file(portal, app, r'\LocalState', f'sdl-startup-{run[1]}.txt', 4096)
            snapshots = re.findall(r'^(?:sdl_(?:(?:main_ready|working_directory|init|subsystem_init|video_init|window|renderer|'
                                   r'audio_init|audio_device|base_path|pref_path|pref_path_local|message_box|gl_library|gl_context|'
                                   r'gl_swap_interval|surfaceless_egl|surfaceless_context|surfaceless_framebuffer)'
                                   r'=(?:begin|ready|error)|software_frame=(?:ready|error|uniform)|'
                                   r'gpu_interop=published|gpu_first_copy=submitted|gpu_failure=[a-z-]{1,32}:0x[0-9a-fA-F]{8}|'
                                   r'gpu_followup=host_bound=[01] accepted=[01] heartbeat_fresh=[01] cursor_supported=[01]|'
                                   r'gpu_(?:mesa_flush|gl_flush|mesa_export)=(?:begin|returned)|'
                                   r'init_flags=0x[0-9A-F]{8})|win32_(?:window_class=registered|window=created|'
                                   r'window_create=(?:begin|nccreate|create|thread-unsupported|class-missing|parent-or-menu-unsupported|'
                                   r'limit-reached|invalid-size|nccreate-refused|create-refused)|'
                                   r'host_exit=[a-z-]{1,32}|host_exit_(?:error|code)=0x[0-9a-fA-F]{8}|'
                                   r'wgl_context=created|wgl_swap=submitted|steam_handoff=(?:ready|error|unavailable)))\r?$',
                                   decode_report(trace or b''), re.MULTILINE)
            print('\n'.join(snapshots) or 'sdl_startup=no-checkpoints')
            return
        if args.report_file == 'slay-console':
            _, launch = portal.request('GET', probe_report_path(app))
            run = re.search(r'^steam_slay_console_run=([0-9]{1,20})\r?$', decode_report(launch), re.MULTILINE)
            if not run:
                print('slay_console=not-requested-for-this-launch')
                return
            raw = read_game_file(portal, app, r'\LocalState', f'slay-console-{run[1]}.log', 1024 * 1024)
            print(slay_console_summary(raw))
            return
        if args.report_file == 'stardew-host':
            _, report = portal.request('GET', probe_report_path(app))
            run = re.search(r'^steam_stardew_host_run=([0-9]{1,20})\r?$', decode_report(report), re.MULTILINE)
            if not run:
                print('stardew_host_trace=no-trace-request-in-report')
                return
            raw = read_game_file(portal, app, r'\LocalState', f'stardew-host-{run[1]}.log', 4 * 1024 * 1024)
            print(stardew_host_summary(raw))
            managed = re.search(r'^steam_stardew_managed_run=([0-9]{1,20})\r?$', decode_report(report), re.MULTILINE)
            if managed:
                raw = read_game_file(portal, app, r'\LocalState', f'stardew-managed-{managed[1]}.log', 8192)
                print(stardew_managed_summary(raw))
            return
        _, raw = portal.request('GET', probe_report_path(app, args.report_file))
        report = decode_report(raw)
        if not report.endswith('\n'):
            raise ValueError('The app is writing its report. Try again.')
        if args.sdl_frame is not None:
            read_sdl_frame(portal, app, report, args.sdl_frame)
        print(report, end='')
        return
    if args.command == 'inspect-game':
        app = find_installed_probe(portal, PROBE_STEAM_CORE_NAME, args.version)
        inspect_game(portal, app, args.app_id, args.executable, args.source_profile, args.export_inputs)
        return
    if args.command == 'cloud-resolve':
        app = find_installed_probe(portal, PROBE_STEAM_CORE_NAME, args.version)
        write_cloud_resolve(portal, app, args.app_id, args.keep)
        return
    if args.command == 'sdl-trial':
        trial_sdl(portal, args)
        return
    if args.command == 'deploy':
        portal.upload(files)
        print(f'Uploaded {args.package.name}. Waiting for Xbox Device Portal...', flush=True)
        time.sleep(0.5)

    deadline = time.monotonic() + 300
    while True:
        status, data = portal.request('GET', f'{PACKAGE_API}/state')
        if status == 200:
            print_install_result(data)
            if args.command == 'deploy' and args.test_probe:
                wait_for_probe(portal, name, version, args.manual_launch, args.timeout, args.require_ipc)
            return
        if args.command == 'status':
            print('Xbox Device Portal reports an install in progress.')
            return
        if time.monotonic() >= deadline:
            raise ValueError('Installation is still in progress. Check again with the status command.')
        time.sleep(1)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, BadZipFile, ElementTree.ParseError, OSError, http.client.HTTPException) as error:
        sys.exit(f'Error: {error}')
