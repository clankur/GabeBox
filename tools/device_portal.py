"""Pinned Xbox Device Portal HTTP transport and package upload."""

from datetime import datetime, timezone
import errno
import hashlib
import hmac
import http.client
from http.cookies import SimpleCookie
import socket
import ssl
import time
from urllib.parse import urlencode, urlsplit
from uuid import uuid4


PACKAGE_API = '/api/app/packagemanager'


class PortalError(ValueError):
    def __init__(self, status, details, response=''):
        self.status = status
        self.response = response
        super().__init__(details)


class CertificateChanged(ValueError):
    """The console presented a TLS certificate other than the pinned one. Never retried or trusted."""

    def __init__(self, observed, expected=''):
        self.observed = observed
        self.expected = expected
        pinned = f' instead of the pinned {expected}' if expected else ''
        super().__init__(
            f'Xbox Device Portal presented a different TLS certificate (SHA-256 {observed}){pinned}. '
            'The console creates a new portal certificate after some reboots and Dev Home resets. '
            'Nothing was trusted automatically. Confirm that this fingerprint belongs to your console, '
            'then set XBOX_PORTAL_CERT_SHA256 to it and rerun.')


class PortalUnavailable(OSError):
    """A Device Portal read kept failing with transient network errors after bounded retries."""


TRANSIENT_ERRNOS = {getattr(errno, name) for name in (
    'ETIMEDOUT', 'ECONNRESET', 'ECONNABORTED', 'ECONNREFUSED', 'EHOSTUNREACH', 'EHOSTDOWN',
    'ENETUNREACH', 'ENETDOWN', 'ENETRESET', 'EPIPE') if hasattr(errno, name)}


def transient_error(error):
    """Network failures that can clear on their own: timeouts, resets, refused or dropped connections."""
    if isinstance(error, (CertificateChanged, PortalUnavailable)):
        return False
    if isinstance(error, (TimeoutError, socket.timeout, ConnectionError, http.client.RemoteDisconnected,
                          http.client.IncompleteRead, http.client.BadStatusLine, ssl.SSLEOFError,
                          ssl.SSLZeroReturnError)):
        return True
    return isinstance(error, OSError) and error.errno in TRANSIENT_ERRNOS


def describe_error(error):
    code = getattr(error, 'errno', None)
    return f'{type(error).__name__} errno={code}' if isinstance(code, int) else type(error).__name__


def utc_text(seconds):
    return datetime.fromtimestamp(seconds, timezone.utc).isoformat(timespec='milliseconds').replace('+00:00', 'Z')


class PortalRetry:
    """Retry idempotent Device Portal reads with bounded backoff and record when the portal was unreachable.

    A gap opens at the first transient failure and closes at the next successful read. An
    operation is retried after each delay in turn. When the delays are exhausted, or the next
    attempt would start after the deadline, PortalUnavailable is raised so the caller can skip
    that read and keep going. Certificate changes and HTTP errors are never retried.
    """

    MAX_GAPS = 64

    def __init__(self, *, delays=(0.5, 1, 2, 4), clock=time.monotonic, wall_clock=time.time, pause=time.sleep,
                 deadline=None, progress=None):
        self.delays = tuple(delays)
        self.clock = clock
        self.wall_clock = wall_clock
        self.pause = pause
        self.deadline = deadline
        self.progress = progress
        self.retries = 0
        self.unavailable = 0
        self.gaps = []
        self.dropped_gaps = 0
        self.open_gap = None

    def call(self, operation):
        # While a gap is already open after exhausted retries, try each read once so a poll
        # loop does not spend the full backoff on every file during an outage.
        delays = () if self.open_gap is not None and self.open_gap['skipped_reads'] else self.delays
        attempt = 0
        while True:
            try:
                value = operation()
            except Exception as error:
                if not transient_error(error):
                    raise
                self._fail(error)
                delay = delays[attempt] if attempt < len(delays) else None
                if delay is None or (self.deadline is not None and self.clock() + delay > self.deadline):
                    self.unavailable += 1
                    self.open_gap['skipped_reads'] += 1
                    raise PortalUnavailable(f'Xbox Device Portal is unreachable ({describe_error(error)}).') from error
                attempt += 1
                self.retries += 1
                self.pause(delay)
                continue
            self._recover()
            return value

    def request(self, portal, method, path, **kwargs):
        """Retry a GET through this policy. Other methods are not idempotent and pass straight through."""
        if method != 'GET':
            return portal.request(method, path, **kwargs)
        return self.call(lambda: portal.request(method, path, **kwargs))

    def _fail(self, error):
        if self.open_gap is None:
            self.open_gap = {'start_utc': utc_text(self.wall_clock()), 'started': self.clock(),
                             'error': describe_error(error), 'failed_attempts': 0, 'skipped_reads': 0}
            if self.progress:
                self.progress(f'portal-unreachable {self.open_gap["error"]}; retrying')
        self.open_gap['failed_attempts'] += 1

    def _recover(self):
        if self.open_gap is None:
            return
        gap = self.open_gap
        self.open_gap = None
        gap['end_utc'] = utc_text(self.wall_clock())
        gap['seconds'] = round(self.clock() - gap.pop('started'), 3)
        if self.progress:
            self.progress(f'portal-recovered after {gap["seconds"]}s')
        if len(self.gaps) < self.MAX_GAPS:
            self.gaps.append(gap)
        else:
            self.dropped_gaps += 1

    def summary(self):
        """None when every read succeeded first time; otherwise the retries and gaps for a result."""
        if not self.retries and not self.unavailable and not self.gaps and self.open_gap is None:
            return None
        result = {'retries': self.retries, 'skipped_reads': self.unavailable, 'gaps': list(self.gaps)}
        if self.dropped_gaps:
            result['gaps_not_listed'] = self.dropped_gaps
        if self.open_gap is not None:
            gap = dict(self.open_gap)
            gap['seconds_so_far'] = round(self.clock() - gap.pop('started'), 3)
            result['unrecovered_gap'] = gap
        return result


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

    def request(self, method=None, path=None, body=None, headers=None, read_limit=None, receive=None):
        connection = http.client.HTTPSConnection(self.host, self.port, timeout=30,
                                                context=self.context)
        try:
            connection.connect()
            certificate = connection.sock.getpeercert(binary_form=True)
            actual = hashlib.sha256(certificate).hexdigest()
            if method is None:
                return actual
            if not hmac.compare_digest(actual, self.fingerprint):
                raise CertificateChanged(actual, self.fingerprint)

            request_headers = dict(headers or {})
            if self.authorization:
                request_headers['Authorization'] = self.authorization
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
            if response.status == 401:
                if self.authorization:
                    raise ValueError('Xbox Device Portal rejected the username or password.')
                raise ValueError('Xbox Device Portal requires authentication. Set XBOX_PORTAL_USERNAME.')
            accepted_statuses = (200, 202, 204) if method == 'POST' else (200, 204)
            if method == 'GET' and 'Range' in request_headers:
                accepted_statuses += (206,)
            if response.status not in accepted_statuses:
                data = response.read(min(read_limit, 4096) if read_limit is not None else 4096)
                details = data.decode('utf-8', errors='replace')[:1000].strip()
                raise PortalError(response.status,
                                  f'{method} {path} returned HTTP {response.status}. {details}'.strip(), details)
            return response.status, receive(response) if receive is not None else response.read(read_limit)
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
