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

    def request(self, method=None, path=None, body=None, headers=None):
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
            data = response.read()
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


def wait_for_probe(portal, name, version, manual, timeout, require_ipc=False):
    _, data = portal.request('GET', f'{PACKAGE_API}/packages')
    prefix = f'{name}_{version}_x64__' if version else f'{name}_'
    candidates = [package for package in json.loads(data)['InstalledPackages']
                  if package['PackageFullName'].startswith(prefix)]
    if len(candidates) != 1:
        raise ValueError(f'Expected exactly one installed probe matching {prefix}. Found {len(candidates)}.')
    app = candidates[0]
    package_name = app['PackageFullName']
    print(f'Installed package: {package_name}', flush=True)
    query = urlencode({'knownfolderid': 'LocalAppData', 'packagefullname': package_name,
                       'path': r'\LocalState', 'filename': PROBE_REPORT})
    report_url = f'/api/filesystem/apps/file?{query}'

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
            encoding = 'utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8'
            try:
                report = raw.decode(encoding)
            except UnicodeDecodeError:
                report = ''  # The report can be read while the app is replacing it.
            if report.endswith('\n') and report != last_report:
                last_report = report
                print(report, end='', flush=True)
                if name == PROBE_CONTROL_NAME and re.search(r'^status=control-ready\s*$', report, re.MULTILINE):
                    return
                if name == PROBE_STEAM_CORE_NAME and re.search(
                        r'^status=steam-core-complete\s*$', report, re.MULTILINE):
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
    variant = probe.add_mutually_exclusive_group()
    variant.add_argument('--control', dest='probe_name', action='store_const', const=PROBE_CONTROL_NAME,
                         help='watch the clean-registration control')
    variant.add_argument('--steam-core', dest='probe_name', action='store_const', const=PROBE_STEAM_CORE_NAME,
                         help='watch the Steam Core Loader Probe')
    probe.set_defaults(probe_name=PROBE_NAME)
    for command in (deploy, probe):
        command.add_argument('--manual-launch', action='store_true', help='wait for launch from Xbox Dev Home')
        command.add_argument('--timeout', type=int, default=20, help='seconds to wait for a fresh report')
        command.add_argument('--require-ipc', action='store_true', help='require a successful named-pipe exchange')
    args = parser.parse_args()
    if args.command == 'deploy' and args.require_ipc and not args.test_probe:
        deploy.error('--require-ipc requires --test-probe')
    if args.command == 'probe' and args.require_ipc and args.probe_name != PROBE_NAME:
        probe.error('--require-ipc applies to the main probe')

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
        wait_for_probe(portal, args.probe_name, args.version, args.manual_launch, args.timeout, args.require_ipc)
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
