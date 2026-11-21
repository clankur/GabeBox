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


PACKAGE_API = '/api/app/packagemanager'
SETTING_NAMES = ('XBOX_PORTAL_URL', 'XBOX_PORTAL_USERNAME', 'XBOX_PORTAL_CERT_SHA256')


def load_settings():
    settings = {}
    config_path = Path(__file__).with_name('.env')
    if config_path.is_file():
        for line in config_path.read_text().splitlines():
            key, _, value = line.strip().partition('=')
            if key.strip() in SETTING_NAMES:
                settings[key.strip()] = value.strip()
    settings.update({key: os.environ[key] for key in SETTING_NAMES if key in os.environ})
    return settings


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
            if response.status not in (200, 204):
                details = data.decode('utf-8', errors='replace')[:1000].strip()
                raise ValueError(f'{method} {path} returned HTTP {response.status}. {details}'.strip())
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


def print_install_result(data):
    result = json.loads(data) if data.strip() else {}
    if isinstance(result, dict) and result.get('Success') is True:
        print('Xbox Device Portal reports a successful install.')
    else:
        raise ValueError(f'Xbox Device Portal did not report a successful install: {result}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('fingerprint', help='show the certificate fingerprint without signing in')
    commands.add_parser('status', help='check the latest installation status')
    deploy = commands.add_parser('deploy', help='upload a signed package and wait for installation')
    deploy.add_argument('package', type=Path)
    deploy.add_argument('--dependency', action='append', type=Path, default=[])
    deploy.add_argument('--certificate', type=Path, help='optional package-signing .cer')
    args = parser.parse_args()

    settings = load_settings()
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

    if not hmac.compare_digest(portal.request(), fingerprint):
        raise ValueError('Xbox portal certificate does not match XBOX_PORTAL_CERT_SHA256.')
    if not sys.stdin.isatty():
        raise ValueError('Run this command in a terminal to enter your Device Portal password.')
    password = getpass.getpass('Xbox Device Portal password: ')
    portal.authorization = 'Basic ' + base64.b64encode(f'{username}:{password}'.encode()).decode('ascii')
    if args.command == 'deploy':
        portal.upload(files)
        print(f'Uploaded {args.package.name}. Waiting for Xbox Device Portal...', flush=True)
        time.sleep(0.5)

    deadline = time.monotonic() + 300
    while True:
        status, data = portal.request('GET', f'{PACKAGE_API}/state')
        if status == 200:
            print_install_result(data)
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
    except (ValueError, OSError, http.client.HTTPException) as error:
        sys.exit(f'Error: {error}')
