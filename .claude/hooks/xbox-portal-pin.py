#!/usr/bin/env python3
"""Check the pinned Xbox Device Portal TLS fingerprint against the live console.

The portal uses a self-signed certificate, and the tools verify it against
XBOX_PORTAL_CERT_SHA256 in the local .env. The console regenerates that
certificate (a reboot, a Developer Mode toggle, an OS update), and from then on
every deploy, probe and observation fails with a mismatch until someone re-pins
it by hand. This reads the live fingerprint and says so up front.

It never blocks: a console that is off, asleep or on another network makes the
TLS connect fail inside a short timeout, and the hook reports "unreachable" and
exits 0.

The pin is a real control — it is what stops something else on the network from
impersonating the console and collecting the portal password in the .env. So the
default is to report a change, not to accept it. Compare the fingerprint with the
one Dev Home shows, then apply it:

    python3 .claude/hooks/xbox-portal-pin.py --write

Set XBOX_PORTAL_PIN_AUTO=1 to re-pin automatically when it changes, which trades
that protection for not being interrupted. Every change is appended to
.claude/xbox-portal-pin.log either way.

    --hook       emit Claude Code hook JSON instead of plain text
    --env-file   .env to read (default: $XBOX_ENV_FILE, else ./.env)
    --timeout    seconds to wait for the console (default 3)
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import socket
import ssl
import sys
from pathlib import Path
from urllib.parse import urlsplit

FINGERPRINT = re.compile(r'^[0-9a-f]{64}$')


def read_env(path: Path) -> dict:
    settings = {}
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        line = line.strip()
        if not line or line.startswith('#') or '=' not in line:
            continue
        name, _, value = line.partition('=')
        settings[name.strip()] = value.strip().strip('"').strip("'")
    return settings


def live_fingerprint(url: str, timeout: float) -> str:
    address = urlsplit(url)
    if address.scheme != 'https' or not address.hostname:
        raise ValueError('XBOX_PORTAL_URL must be the HTTPS address shown in Dev Home.')
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    with socket.create_connection((address.hostname, address.port or 443), timeout) as raw:
        with context.wrap_socket(raw) as secure:
            certificate = secure.getpeercert(binary_form=True)
    return hashlib.sha256(certificate).hexdigest()


def repin(path: Path, fingerprint: str) -> bool:
    lines = path.read_text(encoding='utf-8').splitlines(keepends=True)
    replaced = False
    for index, line in enumerate(lines):
        if line.split('=', 1)[0].strip() == 'XBOX_PORTAL_CERT_SHA256':
            ending = '\r\n' if line.endswith('\r\n') else '\n' if line.endswith('\n') else ''
            lines[index] = f'XBOX_PORTAL_CERT_SHA256={fingerprint}{ending}'
            replaced = True
            break
    if not replaced:
        if lines and not lines[-1].endswith(('\n', '\r\n')):
            lines[-1] += '\n'
        lines.append(f'XBOX_PORTAL_CERT_SHA256={fingerprint}\n')
    path.write_text(''.join(lines), encoding='utf-8')
    return replaced


def log(root: Path, message: str) -> None:
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
    try:
        (root / '.claude').mkdir(exist_ok=True)
        with (root / '.claude' / 'xbox-portal-pin.log').open('a', encoding='utf-8') as file:
            file.write(f'{stamp} {message}\n')
    except OSError:
        pass


def main() -> int:
    parser = argparse.ArgumentParser(description='Check the Xbox Device Portal certificate pin.')
    parser.add_argument('--env-file')
    parser.add_argument('--timeout', type=float, default=3.0)
    parser.add_argument('--write', action='store_true', help='re-pin the .env to the live fingerprint')
    parser.add_argument('--hook', action='store_true', help='emit Claude Code hook JSON')
    args = parser.parse_args()

    root = Path(os.environ.get('CLAUDE_PROJECT_DIR') or '.').resolve()
    candidate = args.env_file or os.environ.get('XBOX_ENV_FILE') or str(root / '.env')
    path = Path(candidate).expanduser()

    def report(state: str, message: str, context: str = '', notice: str = '') -> int:
        if args.hook:
            payload: dict = {'suppressOutput': True}
            if context:
                payload['hookSpecificOutput'] = {'hookEventName': 'SessionStart',
                                                 'additionalContext': context}
            if notice:
                payload['systemMessage'] = notice
            print(json.dumps(payload))
        else:
            print(f'{state}: {message}')
        return 0

    if not path.is_file():
        # No portal configured here: stay silent rather than nagging contributors.
        return report('no-env', f'no Device Portal .env at {path}')

    try:
        settings = read_env(path)
    except OSError as error:
        return report('unreadable', f'{path}: {error}')

    url = settings.get('XBOX_PORTAL_URL', '')
    pinned = settings.get('XBOX_PORTAL_CERT_SHA256', '').lower().replace(':', '')
    if not url:
        return report('no-url', f'{path} sets no XBOX_PORTAL_URL')

    try:
        actual = live_fingerprint(url, args.timeout)
    except (OSError, ValueError, ssl.SSLError) as error:
        reason = error.strerror if isinstance(error, OSError) and error.strerror else str(error)
        return report('unreachable', f'{url} did not answer ({reason})',
                      context=f'The Xbox Device Portal at {url} did not answer within '
                              f'{args.timeout:g}s, so its certificate pin could not be checked. '
                              'Console commands will fail until it is reachable; off-console work '
                              'is unaffected.')

    if FINGERPRINT.fullmatch(pinned) and actual == pinned:
        return report('current', f'pin matches the live certificate ({actual[:12]}…)')

    auto = args.write or os.environ.get('XBOX_PORTAL_PIN_AUTO') == '1'
    previous = pinned or 'unset'
    if auto:
        try:
            repin(path, actual)
        except OSError as error:
            return report('write-failed', f'could not update {path}: {error}')
        log(root, f'repinned {path} {previous} -> {actual} (auto)')
        return report('repinned', f'{previous} -> {actual}',
                      context=f'The Xbox Device Portal certificate changed and '
                              f'XBOX_PORTAL_CERT_SHA256 in {path} was updated automatically to '
                              f'{actual}. Verify it against the fingerprint Dev Home shows before '
                              'trusting a console run.',
                      notice=f'Xbox portal pin updated to {actual[:12]}… (was {previous[:12]}…)')

    log(root, f'pin mismatch {path} pinned={previous} live={actual}')
    return report('mismatch', f'pinned {previous}, live {actual}',
                  context=f'The Xbox Device Portal certificate no longer matches the pin in '
                          f'{path}: pinned {previous}, live {actual}. Every Device Portal command '
                          'will fail until the pin is updated. Compare the live fingerprint with '
                          'the one Dev Home shows, then run '
                          '`python3 .claude/hooks/xbox-portal-pin.py --write`. Do not re-pin '
                          'blindly: the pin is what stops another host on the network from '
                          'impersonating the console and collecting the portal password.',
                  notice=f'Xbox portal certificate changed (live {actual[:12]}…) — the pin in '
                         'your .env is stale.')


if __name__ == '__main__':
    sys.exit(main())
