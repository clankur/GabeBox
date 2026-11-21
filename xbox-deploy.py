#!/usr/bin/env python3
"""Deploy a signed app package through Xbox Device Portal."""

import argparse
import base64
import getpass
import hashlib
import hmac
import http.client
from http.cookies import SimpleCookie
import json
import os
from pathlib import Path
import re
import ssl
from struct import unpack_from
import sys
import time
from urllib.parse import urlencode, urlsplit
from uuid import uuid4
from xml.etree import ElementTree
from zipfile import BadZipFile, ZipFile


PACKAGE_API = '/api/app/packagemanager'
PROBE_NAME = 'XboxPortabilityProbe.ChildProcess'
PROBE_CONTROL_NAME = PROBE_NAME + '.Control'
PROBE_STEAM_CORE_NAME = PROBE_NAME + '.SteamCore'
PROBE_LABELS = {PROBE_NAME: 'Packaged process test',
                PROBE_CONTROL_NAME: 'Packaged process startup control',
                PROBE_STEAM_CORE_NAME: 'Steam Core Loader Probe'}
PROBE_REPORT = 'probe-report.txt'
STEAMAPPS_PATH = r'\LocalState\SteamLibrary\steamapps'
SDL_TRIALS = {
    975370: ('Dwarf Fortress', '2.26.2', '9603fdfd2b4bd75b3574bb993e25332bae820a66f18e7ffb6f8c82cb4f6c004a'),
    413150: ('Stardew Valley', '2.30.9', '58f0a2af6653445b48e58ace6890fbe237bf76452c9743e800358d6b935ded64'),
}
SDL_LIMIT = 8 * 1024 * 1024
SETTING_NAMES = ('XBOX_PORTAL_URL', 'XBOX_PORTAL_USERNAME', 'XBOX_PORTAL_CERT_SHA256',
                 'XBOX_PORTAL_PASSWORD')


def load_settings(config_path=None):
    settings = {}
    if config_path is None:
        config_path = Path(__file__).with_name('.env')
    elif not config_path.is_file():
        raise ValueError(f'Environment file not found: {config_path}')
    if config_path.is_file():
        for line in config_path.read_text().splitlines():
            key, _, value = line.strip().partition('=')
            if key.strip() in SETTING_NAMES:
                settings[key.strip()] = value.strip()
    settings.update({key: os.environ[key] for key in SETTING_NAMES if key in os.environ})
    return settings


class PortalError(ValueError):
    def __init__(self, status, details):
        self.status = status
        super().__init__(details)


class DevicePortal:
    def __init__(self, url, fingerprint):
        address = urlsplit(url)
        if (address.scheme != 'https' or not address.hostname or address.username is not None
                or address.path not in ('', '/') or address.query or address.fragment):
            raise ValueError('Set XBOX_PORTAL_URL to the HTTPS address shown in Dev Home.')
        self.host = address.hostname
        self.port = address.port or 443
        self.fingerprint = fingerprint
        # Xbox uses a private CA. Verify the configured certificate before sending HTTP.
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        self.context.check_hostname = False
        self.context.verify_mode = ssl.CERT_NONE
        self.authorization = None
        self.csrf_token = None

    def request(self, method=None, path=None, body=None, headers=None, read_limit=None):
        connection = http.client.HTTPSConnection(self.host, self.port, timeout=30,
                                                context=self.context)
        try:
            connection.connect()
            certificate = connection.sock.getpeercert(binary_form=True)
            actual = hashlib.sha256(certificate).hexdigest()
            if method is None:
                return actual
            if not hmac.compare_digest(actual, self.fingerprint):
                raise ValueError(f'Xbox portal certificate changed. Observed SHA-256: {actual}')

            request_headers = dict(headers or {}, Authorization=self.authorization)
            if self.csrf_token:
                request_headers['Cookie'] = f'CSRF-Token={self.csrf_token}'
            if method == 'GET':
                request_headers['CSRF-Token'] = self.csrf_token or 'Fetch'
            elif self.csrf_token:
                request_headers['X-CSRF-Token'] = self.csrf_token
            connection.request(method, path, body=body, headers=request_headers)
            response = connection.getresponse()
            for key, value in response.getheaders():
                if key.lower() == 'set-cookie':
                    token = SimpleCookie(value).get('CSRF-Token')
                    if token is not None:
                        self.csrf_token = token.value
            data = response.read(read_limit)
            if response.status == 401:
                raise ValueError('Xbox Device Portal rejected the username or password.')
            accepted_statuses = (200, 202, 204) if method == 'POST' else (200, 204)
            if response.status not in accepted_statuses:
                details = data.decode('utf-8', errors='replace')[:1000].strip()
                raise PortalError(response.status,
                                  f'{method} {path} returned HTTP {response.status}. {details}'.strip())
            return response.status, data
        finally:
            connection.close()

    def upload(self, files):
        boundary = uuid4().hex
        closing = f'--{boundary}--\r\n'.encode()
        parts = []
        for file in files:
            if any(char in file.name for char in '\r\n"\\'):
                raise ValueError(f'Unsupported upload file name: {file.name!r}')
            content_type = ('application/x-x509-ca-cert' if file.suffix.lower() == '.cer'
                            else 'application/octet-stream')
            header = (f'--{boundary}\r\n'
                      f'Content-Disposition: form-data; name="{file.name}"; filename="{file.name}"\r\n'
                      f'Content-Type: {content_type}\r\n\r\n').encode()
            parts.append((header, file))

        def body():
            for header, file in parts:
                yield header
                with file.open('rb') as source:
                    for chunk in iter(lambda: source.read(1024 * 1024), b''):
                        yield chunk
                yield b'\r\n'
            yield closing

        headers = {
            'Content-Type': f'multipart/form-data; boundary={boundary}',
            'Content-Length': str(len(closing) + sum(len(header) + file.stat().st_size + 2
                                                    for header, file in parts)),
        }
        query = urlencode({'package': files[0].name})
        self.request('GET', f'{PACKAGE_API}/packages')
        self.request('POST', f'{PACKAGE_API}/package?{query}', body(), headers)


def probe_identity(file):
    with ZipFile(file) as archive:
        manifest = ElementTree.fromstring(archive.read('AppxManifest.xml'))
    identity = manifest.find('{*}Identity')
    if (identity is None or identity.get('Name') not in PROBE_LABELS
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


def read_game_file(portal, package, folder, filename, limit):
    query = urlencode({'knownfolderid': 'LocalAppData', 'packagefullname': package['PackageFullName'],
                       'path': folder, 'filename': filename})
    try:
        _, data = portal.request('GET', '/api/filesystem/apps/file?' + query, read_limit=limit)
    except PortalError as error:
        if error.status == 404:
            return None
        raise
    return data


def sdl_frame_stem(report):
    run = re.search(r'^steam_(launch_sdl|stardew_managed)_run=([0-9]{1,20})\r?$', report, re.MULTILINE)
    if not run:
        raise ValueError('The current console report has no SDL frame trial.')
    return f'sdl-startup-{run[2]}' if run[1] == 'launch_sdl' else f'stardew-managed-{run[2]}.sdl'


def read_sdl_frame(portal, app, report, destination):
    stem = sdl_frame_stem(report)
    data = read_game_file(portal, app, r'\LocalState', stem + '.bmp', 4096 * 4096 * 4 + 1024)
    if not data or len(data) < 54 or data[:2] != b'BM':
        raise ValueError('The latest SDL trial has no captured BMP frame.')
    size, offset = unpack_from('<I4xI', data, 2)
    width, height, planes, bits = unpack_from('<iiHH', data, 18)
    stride = (width * bits + 31) // 32 * 4
    if (size != len(data) or not 54 <= offset < size or not 1 <= width <= 4096 or not 1 <= abs(height) <= 4096
            or planes != 1 or bits not in (24, 32) or size - offset < stride * abs(height)):
        raise ValueError('The SDL frame was incomplete or had an unexpected bitmap layout.')
    with destination.open('xb') as output:
        output.write(data)
    print(f'Saved SDL frame ({width} by {abs(height)}) to {destination}.')


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


def pe_summary(header):
    if len(header) < 64 or header[:2] != b'MZ':
        return {'pe_header': 'unavailable'}
    pe = unpack_from('<I', header, 0x3c)[0]
    if pe < 64 or pe + 26 > len(header) or header[pe:pe + 4] != b'PE\0\0':
        return {'pe_header': 'unavailable'}

    machine, sections = unpack_from('<HH', header, pe + 4)
    optional_size = unpack_from('<H', header, pe + 20)[0]
    optional = pe + 24
    magic = unpack_from('<H', header, optional)[0]
    directory = {0x10b: 96, 0x20b: 112}.get(magic)
    section_table = optional + optional_size
    if directory is None or optional_size < directory or section_table > len(header):
        return {'pe_header': 'unavailable'}

    summary = {'pe_machine': {0x14c: 'x86', 0x8664: 'x64', 0xaa64: 'arm64'}.get(machine, hex(machine)),
               'pe_format': 'PE32' if magic == 0x10b else 'PE32+'}
    clr_entry = directory + 14 * 8
    count = unpack_from('<I', header, optional + directory - 4)[0]
    if count < 15 or optional_size < clr_entry + 8:
        return summary
    clr_rva, clr_size = unpack_from('<II', header, optional + clr_entry)
    if not clr_rva or clr_size < 20:
        return summary

    summary['clr_flags'] = 'unavailable'
    for section in range(min(sections, (len(header) - section_table) // 40)):
        _, address, raw_size, offset = unpack_from('<IIII', header, section_table + 40 * section + 8)
        relative = clr_rva - address
        if 0 <= relative <= raw_size - 20 and offset + relative + 20 <= len(header):
            flags = unpack_from('<I', header, offset + relative + 16)[0]
            summary['clr_flags'] = f'0x{flags:08x}'
            summary['clr_requires_x86'] = 'yes' if flags & 2 and not flags & 0x20000 else 'no'
            break
    return summary


def inspect_game(portal, package, app_id, executable):
    raw = read_game_file(portal, package, STEAMAPPS_PATH, f'appmanifest_{app_id}.acf', 65536)
    if raw is None:
        raise ValueError('Steam has not created a local manifest for this AppID.')
    manifest = decode_report(raw)
    fields = ('StateFlags', 'UpdateResult', 'BytesToDownload', 'BytesDownloaded', 'SizeOnDisk')
    for field in fields:
        match = re.search(rf'^[ \t]*"{field}"[ \t]+"([0-9]+)"', manifest, re.MULTILINE | re.IGNORECASE)
        if match:
            print(f'{field}={match[1]}')
    if executable is None:
        return
    install = re.search(r'^[ \t]*"installdir"[ \t]+"([^"\r\n]+)"', manifest, re.MULTILINE | re.IGNORECASE)
    valid_component = r'[\w -][\w .()\-]*(?<![ .])'
    if not install or not re.fullmatch(valid_component, install[1]):
        raise ValueError('The Steam manifest has no usable local install folder.')
    parts = executable.replace('\\', '/').split('/')
    if not parts[-1].lower().endswith('.exe') or any(not re.fullmatch(valid_component, p) for p in parts):
        raise ValueError('Use an .exe path relative to the game folder, such as jre/bin/javaw.exe.')

    candidates = (('installed', ('common', install[1])), ('staging', ('downloading', str(app_id))))
    for source, folder in candidates:
        path = '\\'.join((STEAMAPPS_PATH, *folder, *parts[:-1]))
        header = read_game_file(portal, package, path, parts[-1], 131072)
        if header is not None:
            print(f'executable_source={source}')
            for key, value in pe_summary(header).items():
                print(f'{key}={value}')
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
    artifact = args.artifact / 'SDL2.dll'
    checksum = (args.artifact / 'SHA256SUMS').read_text(encoding='ascii').strip()
    expected = re.fullmatch(r'([0-9a-f]{64})  SDL2\.dll', checksum)
    if artifact.stat().st_size > SDL_LIMIT or not expected:
        raise ValueError('Use a checked SDL2 workflow artifact containing SHA256SUMS.')
    candidate = artifact.read_bytes()
    if hashlib.sha256(candidate).hexdigest() != expected[1] or pe_summary(candidate).get('pe_machine') != 'x64':
        raise ValueError('The SDL2 workflow artifact has an unexpected checksum or architecture.')
    version_marker = b'\xbd\x04\xef\xfe\x00\x00\x01\x00'
    version_offset = candidate.find(version_marker)
    if version_offset < 0 or version_offset + 16 > len(candidate):
        raise ValueError('The SDL2 workflow artifact has no Windows version resource.')
    major_minor, patch_build = unpack_from('<II', candidate, version_offset + 8)
    version = f'{major_minor >> 16}.{major_minor & 0xffff}.{patch_build >> 16}'
    if version != upstream:
        raise ValueError(f'{name} requires the version-matched SDL {upstream} workflow artifact.')
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
                wait_for_probe(portal, PROBE_STEAM_CORE_NAME, args.version, False, args.timeout,
                               require_app_id=args.app_id)
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


def wait_for_probe(portal, name, version, manual, timeout, require_ipc=False, restart=False, require_app_id=None):
    app = find_installed_probe(portal, name, version)
    print(f'Installed package: {app["PackageFullName"]}', flush=True)
    report_url = probe_report_path(app)

    def read_report():
        try:
            _, raw = portal.request('GET', report_url)
        except PortalError as error:
            if error.status != 404:
                raise
            return b''
        return raw

    baseline = read_report()
    launch_error = ''
    if manual:
        print(f'Launch {PROBE_LABELS[name]} once from Xbox Dev Home.', flush=True)
    else:
        if restart:
            package = base64.b64encode(app['PackageFullName'].encode()).decode('ascii')
            try:
                portal.request('DELETE', '/api/taskmanager/app?' + urlencode({'package': package}))
            except PortalError as error:
                if error.status != 404:
                    raise
        # Match Xbox Device Portal's App Launcher, which sends the AUMID only.
        launch = {'appid': base64.b64encode(app['PackageRelativeId'].encode()).decode('ascii')}
        try:
            portal.request('POST', '/api/taskmanager/app?' + urlencode(launch))
        except PortalError as error:
            launch_error = str(error)
            print(f'Remote launch returned an error. You can launch from Dev Home while the CLI waits.\n{error}',
                  flush=True)

    deadline = time.monotonic() + timeout
    last_report = ''
    while time.monotonic() < deadline:
        try:
            raw = read_report()
        except PortalError as error:
            if error.status != 409:
                raise
            raw = b''
        if raw and raw != baseline:
            report = decode_report(raw)
            if report.endswith('\n') and report != last_report:
                last_report = report
                print(report, end='', flush=True)
                if name == PROBE_CONTROL_NAME and re.search(r'^status=control-ready\s*$', report, re.MULTILINE):
                    return
                if name == PROBE_STEAM_CORE_NAME and re.search(
                        r'^status=steam-core-complete\s*$', report, re.MULTILINE):
                    if require_app_id is not None and not re.search(
                            rf'^steam_launch_(?:repeat_)?app_id={require_app_id}\r?$', report, re.MULTILINE):
                        raise ValueError('The library loaded without launching the requested game. '
                                         'Prepare the diagnostic with --repeat-launch.')
                    return
                if re.search(r'^exit_code_matches=yes\s*$', report, re.MULTILINE):
                    if not require_ipc or re.search(r'^ipc=success\s*$', report, re.MULTILINE):
                        return
                    if re.search(r'^ipc=failed\s*$', report, re.MULTILINE):
                        raise ValueError('The signed helper ran, but the named-pipe exchange did not succeed.')
                if re.search(r'^exit_code_matches=no\s*$', report, re.MULTILINE):
                    raise ValueError('The probe ran, but the helper did not return its expected exit code.')
                if re.search(r'^status=.*(?:error|failed)\s*$', report, re.MULTILINE):
                    raise ValueError('The probe reported a startup or test error above.')
        time.sleep(0.5)

    if last_report:
        if require_ipc and re.search(r'^exit_code_matches=yes\s*$', last_report, re.MULTILINE):
            raise ValueError('The signed helper ran, but no terminal named-pipe result appeared.')
        raise ValueError('The probe wrote a startup checkpoint but did not finish the test.')
    raise ValueError('No fresh probe-report.txt appeared in LocalState.' +
                     (' The remote launch also failed.' if launch_error else ''))


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
    variant = probe.add_mutually_exclusive_group()
    variant.add_argument('--control', dest='probe_name', action='store_const', const=PROBE_CONTROL_NAME,
                         help='watch the clean-registration control')
    variant.add_argument('--steam-core', dest='probe_name', action='store_const', const=PROBE_STEAM_CORE_NAME,
                         help='watch the Steam Core Loader Probe')
    probe.set_defaults(probe_name=PROBE_NAME)
    report_command = commands.add_parser('report', help='read the current Steam probe report without launching it')
    report_command.add_argument('--version', required=True, help='installed Steam package version')
    report_variant = report_command.add_mutually_exclusive_group()
    report_variant.add_argument('--download', dest='report_file', action='store_const',
                                const='steam-download-report.txt', help='read the Steam download checkpoint')
    report_variant.add_argument('--artwork', dest='report_file', action='store_const',
                                const='steam-library-art.txt', help='read the Steam library artwork counters')
    report_variant.add_argument('--inventory', dest='report_file', action='store_const',
                                const='steam-library-inventory.json', help='read the opted-in game name and AppID inventory')
    report_variant.add_argument('--sdl-frame', type=Path, metavar='BMP', help='save the current trial’s SDL frame')
    report_command.set_defaults(report_file=PROBE_REPORT)
    game = commands.add_parser('inspect-game', help='read one Steam install manifest and optional executable header')
    game.add_argument('--version', required=True, help='installed Steam package version')
    game.add_argument('--app-id', type=int, required=True, help='Steam AppID')
    game.add_argument('--executable', help='relative .exe path within the game folder')
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
    for command in (deploy, probe):
        command.add_argument('--manual-launch', action='store_true', help='wait for launch from Xbox Dev Home')
        command.add_argument('--timeout', type=int, default=20, help='seconds to wait for a fresh report')
        command.add_argument('--require-ipc', action='store_true', help='require a successful named-pipe exchange')
    args = parser.parse_args()
    if args.command == 'deploy' and args.require_ipc and not args.test_probe:
        deploy.error('--require-ipc requires --test-probe')
    if args.command == 'probe' and args.require_ipc and args.probe_name != PROBE_NAME:
        probe.error('--require-ipc applies to the main probe')
    if args.command == 'probe' and args.restart and args.manual_launch:
        probe.error('--restart uses a CLI launch and cannot be combined with --manual-launch')
    if args.command == 'inspect-game' and not 0 < args.app_id <= 0xFFFFFFFF:
        game.error('--app-id must be a positive Steam AppID')

    settings = load_settings(args.env_file)
    fingerprint = settings.get('XBOX_PORTAL_CERT_SHA256', '').lower().replace(':', '')
    portal = DevicePortal(settings.get('XBOX_PORTAL_URL', ''), fingerprint)
    if args.command == 'fingerprint':
        print(portal.request())
        return
    if not re.fullmatch(r'[0-9a-f]{64}', fingerprint):
        raise ValueError('Set XBOX_PORTAL_CERT_SHA256 to the verified portal TLS fingerprint.')
    username = settings.get('XBOX_PORTAL_USERNAME', '')
    if not username or ':' in username:
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
    if args.command in ('deploy', 'probe', 'sdl-trial') and args.timeout <= 0:
        raise ValueError('--timeout must be a positive number of seconds.')

    if not hmac.compare_digest(portal.request(), fingerprint):
        raise ValueError('Xbox portal certificate does not match XBOX_PORTAL_CERT_SHA256.')
    password = settings.get('XBOX_PORTAL_PASSWORD')
    if not password:
        if not sys.stdin.isatty():
            raise ValueError('Set XBOX_PORTAL_PASSWORD or run this command in a terminal.')
        password = getpass.getpass('Xbox Device Portal password: ')
    portal.authorization = 'Basic ' + base64.b64encode(f'{username}:{password}'.encode()).decode('ascii')
    if args.command == 'probe':
        wait_for_probe(portal, args.probe_name, args.version, args.manual_launch, args.timeout,
                       args.require_ipc, restart=args.restart)
        return
    if args.command == 'report':
        app = find_installed_probe(portal, PROBE_STEAM_CORE_NAME, args.version)
        _, raw = portal.request('GET', probe_report_path(app, args.report_file))
        report = decode_report(raw)
        if not report.endswith('\n'):
            raise ValueError('The app is writing its report. Try again.')
        if args.sdl_frame is not None:
            read_sdl_frame(portal, app, report, args.sdl_frame)
            return
        print(report, end='')
        return
    if args.command == 'inspect-game':
        app = find_installed_probe(portal, PROBE_STEAM_CORE_NAME, args.version)
        inspect_game(portal, app, args.app_id, args.executable)
        return
    if args.command == 'sdl-trial':
        if not 1 <= args.frame_timeout <= 120:
            raise ValueError('--frame-timeout must be between 1 and 120 seconds.')
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
