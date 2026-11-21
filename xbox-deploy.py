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
               r'stardew_graphics_sdl_driver=(?:windows|offscreen|none|other|unavailable)|'
               r'stardew_graphics_gl_context=(?:absent|present)|'
               r'stardew_graphics_system_opengl=(?:loaded|0x[0-9A-F]{8})|'
               r'stardew_graphics_opengl_origin=(?:bundled|system|other)|'
               r'stardew_graphics_packaged_opengl=(?:loaded|0x[0-9A-F]{8})|'
               r'stardew_graphics_d3d11_hresult=(?:unavailable|0x[0-9A-F]{8})|'
               r'stardew_graphics_d3d11_feature_level=0x[0-9A-F]{4}|'
               r'stardew_graphics_probe=incomplete')
    lines = (raw or b'').decode('ascii', errors='replace').splitlines()
    return '\n'.join(dict.fromkeys(line for line in lines if re.fullmatch(allowed, line))) or 'stardew_managed=no-output'


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


def wait_for_probe(portal, name, version, manual, timeout, require_ipc=False, restart=False):
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
                    return report
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


def wait_for_repeat_launch(portal, version, app_id, restart, timeout):
    previous_run = None
    for attempt in range(2):
        report = wait_for_probe(portal, PROBE_STEAM_CORE_NAME, version, False, timeout, restart=restart)
        fields = dict(line.split('=', 1) for line in report.splitlines() if '=' in line)
        run_id = fields.get('steam_launch_run_id', '')
        if fields.get('steam_launch_repeat_app_id') != str(app_id) or not run_id.isdecimal():
            raise ValueError(f'Expected a repeat launch report for Steam AppID {app_id}.')
        if run_id == previous_run:
            raise ValueError('The restarted probe returned the previous launch report.')
        if fields.get('steam_launch') != 'prerequisite-restart-required':
            return
        if attempt:
            raise ValueError('The game directory still needs a restart after one recovery attempt.')
        print('Shared prerequisite installed. Restarting the probe once to restore the game directory.', flush=True)
        previous_run = run_id
        restart = True


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
    probe.add_argument('--recover-prerequisites', type=int, metavar='APPID',
                       help='restart a repeat-launch Steam build once if a completed shared install changed the game directory')
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
    report_variant.add_argument('--stardew-host', dest='report_file', action='store_const',
                                const='stardew-host', help='summarize the current Stardew Valley .NET host trace')
    report_command.set_defaults(report_file=PROBE_REPORT)
    game = commands.add_parser('inspect-game', help='read one Steam install manifest and optional executable header')
    game.add_argument('--version', required=True, help='installed Steam package version')
    game.add_argument('--app-id', type=int, required=True, help='Steam AppID')
    game.add_argument('--executable', help='relative .exe path within the game folder')
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
    if args.command == 'probe' and args.recover_prerequisites is not None:
        if args.probe_name != PROBE_STEAM_CORE_NAME or not args.version or args.manual_launch:
            probe.error('--recover-prerequisites requires --steam-core, --version and a CLI launch')
        if not 0 < args.recover_prerequisites <= 0x00ffffff:
            probe.error('--recover-prerequisites requires a positive 24-bit Steam AppID')
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
    if args.command in ('deploy', 'probe') and args.timeout <= 0:
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
        if args.recover_prerequisites is not None:
            wait_for_repeat_launch(portal, args.version, args.recover_prerequisites, args.restart, args.timeout)
            return
        wait_for_probe(portal, args.probe_name, args.version, args.manual_launch, args.timeout,
                       args.require_ipc, restart=args.restart)
        return
    if args.command == 'report':
        app = find_installed_probe(portal, PROBE_STEAM_CORE_NAME, args.version)
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
                trace = read_game_file(portal, app, r'\LocalState', f'stardew-managed-{managed[1]}.sdl.txt', 4096)
                snapshots = re.findall(r'^sdl_(?:main_ready|working_directory|init|subsystem_init|video_init|window|renderer|'
                                       r'gl_library|gl_context|surfaceless_egl|surfaceless_context)'
                                       r'=(?:begin|ready|error)$', decode_report(trace or b''), re.MULTILINE)
                if snapshots:
                    print('\n'.join(snapshots))
            return
        _, raw = portal.request('GET', probe_report_path(app, args.report_file))
        report = decode_report(raw)
        if not report.endswith('\n'):
            raise ValueError('The app is writing its report. Try again.')
        print(report, end='')
        return
    if args.command == 'inspect-game':
        app = find_installed_probe(portal, PROBE_STEAM_CORE_NAME, args.version)
        inspect_game(portal, app, args.app_id, args.executable)
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
