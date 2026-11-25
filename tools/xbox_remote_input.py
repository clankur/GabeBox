#!/usr/bin/env python3
"""Send controller, keyboard or mouse input to an Xbox through Device Portal.

Device Portal's /ext/remoteinput WebSocket accepts binary messages: a type byte
followed by its fields. Gamepad buttons travel as VK_GAMEPAD_* key codes and the
mouse as absolute 0-65535 positions in network byte order. The Steam Core
presenter merges the gamepad codes into the hosted controller, and hosted games
receive keyboard and pointer input through the hosted mailbox.
[Remote Input API](https://learn.microsoft.com/en-us/windows/uwp/xbox-apps/uwp-remoteinput-api)

Steps (names are case-insensitive; append *N to repeat a step N times):
  A, B, X, Y, LB, RB, LT, RT, UP, DOWN, LEFT, RIGHT, MENU, VIEW, LS, RS,
  LS_UP, LS_DOWN, LS_LEFT, LS_RIGHT, RS_UP, RS_DOWN, RS_LEFT, RS_RIGHT
                       tap a controller input
  hold:NAME:SECONDS    hold a controller input
  chord:NAME+NAME[:SECONDS]
                       press controller inputs together, hold them (default 0.25 s),
                       then release them; chord:MENU+VIEW opens Big Picture over a game
  key:NAME             tap a keyboard key: ENTER, ESC, SPACE, TAB, BACKSPACE, letters,
                       digits, F1-F12, UP, DOWN, LEFT, RIGHT, HOME, END, PGUP, PGDN,
                       INSERT, DELETE
  scan:HEX / vk:HEX    tap a raw scancode or virtual-key code
  move:X,Y             move the mouse to a 1920x1080 pixel position
  click:X,Y            left-click at a 1920x1080 pixel position
  wait:SECONDS         pause
  shot:FILE.png        save a Device Portal screenshot (read-only; not sent as input)

Keys with a one-byte set-1 scancode are sent as scancodes, which the API
recommends for games. Navigation keys need the 0xE0 prefix that the message
cannot carry, so they are sent as virtual-key codes.
"""

import argparse
import base64
import getpass
import hashlib
import hmac
import os
from pathlib import Path
import re
import socket
import struct
import sys
import time

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.device_portal import DevicePortal  # noqa: E402
from tools.xbox_deploy import load_settings  # noqa: E402

REMOTE_INPUT = '/ext/remoteinput'
SCREENSHOT = '/ext/screenshot'
# A read-only request on the same extension that returns Device Portal's CSRF cookie.
PREFLIGHT = '/ext/remoteinput/controllers'
WEBSOCKET_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'

KEY_CODES, SCAN_CODES, MOUSE, CLEAR_ALL = 1, 2, 3, 4
MOUSE_MOVE, MOUSE_LEFT_DOWN, MOUSE_LEFT_UP = 0x0001, 0x0002, 0x0004
SCREEN_WIDTH, SCREEN_HEIGHT = 1920, 1080
TAP_SECONDS, KEY_SECONDS, CHORD_SECONDS = 0.12, 0.08, 0.25

GAMEPAD = {
    'A': 0xC3, 'B': 0xC4, 'X': 0xC5, 'Y': 0xC6, 'RB': 0xC7, 'LB': 0xC8, 'LT': 0xC9, 'RT': 0xCA,
    'UP': 0xCB, 'DOWN': 0xCC, 'LEFT': 0xCD, 'RIGHT': 0xCE, 'MENU': 0xCF, 'VIEW': 0xD0,
    'LS': 0xD1, 'RS': 0xD2, 'LS_UP': 0xD3, 'LS_DOWN': 0xD4, 'LS_RIGHT': 0xD5, 'LS_LEFT': 0xD6,
    'RS_UP': 0xD7, 'RS_DOWN': 0xD8, 'RS_RIGHT': 0xD9, 'RS_LEFT': 0xDA,
}

SCANCODES = {'ESC': 0x01, 'ESCAPE': 0x01, 'BACKSPACE': 0x0E, 'TAB': 0x0F, 'ENTER': 0x1C, 'RETURN': 0x1C,
             'LCTRL': 0x1D, 'LSHIFT': 0x2A, 'LALT': 0x38, 'SPACE': 0x39, 'F11': 0x57, 'F12': 0x58}
SCANCODES.update({digit: code for digit, code in zip('1234567890', range(0x02, 0x0C))})
SCANCODES.update({letter: code for row, start in (('QWERTYUIOP', 0x10), ('ASDFGHJKL', 0x1E), ('ZXCVBNM', 0x2C))
                  for letter, code in zip(row, range(start, start + len(row)))})
SCANCODES.update({f'F{n}': 0x3A + n for n in range(1, 11)})

VIRTUAL_KEYS = {'PGUP': 0x21, 'PGDN': 0x22, 'END': 0x23, 'HOME': 0x24, 'LEFT': 0x25, 'UP': 0x26,
                'RIGHT': 0x27, 'DOWN': 0x28, 'INSERT': 0x2D, 'DELETE': 0x2E}


def key_message(kind, code, down):
    return bytes((kind, code, 1 if down else 0))


def mouse_message(action, x, y):
    return struct.pack('>BHII', MOUSE, action, x, y)


def screen_position(text):
    match = re.fullmatch(r'\s*(\d+)\s*,\s*(\d+)\s*', text)
    if not match:
        raise ValueError(f'Expected X,Y pixels: {text}')
    x, y = int(match[1]), int(match[2])
    if not (0 <= x < SCREEN_WIDTH and 0 <= y < SCREEN_HEIGHT):
        raise ValueError(f'Positions are {SCREEN_WIDTH}x{SCREEN_HEIGHT} pixels: {text}')
    return round(x * 65535 / (SCREEN_WIDTH - 1)), round(y * 65535 / (SCREEN_HEIGHT - 1))


def byte_value(text):
    value = int(text, 16)
    if not 0 < value < 256:
        raise ValueError(f'Expected a byte from 01 to FF: {text}')
    return value


def seconds_value(text):
    value = float(text)
    if not 0 <= value <= 600:
        raise ValueError(f'Expected seconds from 0 to 600: {text}')
    return value


def parse_step(step):
    """Return (seconds, action) pairs. Each action follows a pause of its seconds and is a
    message to send, None for a pause, or a Path to save a screenshot to."""
    step = step.strip()
    count = 1
    if (match := re.fullmatch(r'(.+)\*(\d+)', step)):
        step, count = match[1], int(match[2])
        if not 1 <= count <= 100:
            raise ValueError(f'Repeat count must be 1-100: {step}*{count}')
    kind, _, rest = step.partition(':')
    name = kind.upper()
    if name in GAMEPAD and not rest:
        actions = tap(KEY_CODES, GAMEPAD[name], TAP_SECONDS)
    elif name == 'HOLD':
        target, _, seconds = rest.partition(':')
        if target.upper() not in GAMEPAD:
            raise ValueError(f'Unknown controller input: {target}')
        actions = tap(KEY_CODES, GAMEPAD[target.upper()], seconds_value(seconds))
    elif name == 'CHORD':
        names, _, seconds = rest.partition(':')
        codes = [GAMEPAD.get(target.upper()) for target in names.split('+')]
        if len(codes) < 2 or None in codes or len(set(codes)) != len(codes):
            raise ValueError(f'Expected two or more distinct controller inputs joined by +: {rest}')
        held = seconds_value(seconds) if seconds else CHORD_SECONDS
        actions = ([(0, key_message(KEY_CODES, code, True)) for code in codes] +
                   [(held if index == 0 else 0, key_message(KEY_CODES, code, False))
                    for index, code in enumerate(codes)])
    elif name == 'KEY':
        key = rest.upper()
        if key in VIRTUAL_KEYS:
            actions = tap(KEY_CODES, VIRTUAL_KEYS[key], KEY_SECONDS)
        elif key in SCANCODES:
            actions = tap(SCAN_CODES, SCANCODES[key], KEY_SECONDS)
        else:
            raise ValueError(f'Unknown key: {rest}')
    elif name == 'SCAN':
        actions = tap(SCAN_CODES, byte_value(rest), KEY_SECONDS)
    elif name == 'VK':
        actions = tap(KEY_CODES, byte_value(rest), KEY_SECONDS)
    elif name == 'MOVE':
        actions = [(0, mouse_message(MOUSE_MOVE, *screen_position(rest)))]
    elif name == 'CLICK':
        x, y = screen_position(rest)
        actions = [(0, mouse_message(MOUSE_MOVE, x, y)), (0.05, mouse_message(MOUSE_LEFT_DOWN, x, y)),
                   (KEY_SECONDS, mouse_message(MOUSE_LEFT_UP, x, y))]
    elif name == 'WAIT':
        actions = [(seconds_value(rest), None)]
    elif name == 'SHOT':
        if not rest.lower().endswith('.png'):
            raise ValueError(f'Screenshots are saved as .png files: {rest}')
        actions = [(0, Path(rest))]
    else:
        raise ValueError(f'Unknown step: {step}')
    return actions * count


def tap(kind, code, seconds):
    return [(0, key_message(kind, code, True)), (seconds, key_message(kind, code, False))]


def accept_key(nonce):
    return base64.b64encode(hashlib.sha1((nonce + WEBSOCKET_GUID).encode('ascii')).digest()).decode('ascii')


def handshake_request(host, nonce, authorization=None, csrf_token=None):
    lines = [f'GET {REMOTE_INPUT} HTTP/1.1', f'Host: {host}', 'Upgrade: websocket', 'Connection: Upgrade',
             f'Sec-WebSocket-Key: {nonce}', 'Sec-WebSocket-Version: 13', f'Origin: https://{host}']
    if authorization:
        lines.append(f'Authorization: {authorization}')
    if csrf_token:
        # A browser sends the cookie. The header matches Device Portal's state-changing requests.
        lines += [f'Cookie: CSRF-Token={csrf_token}', f'X-CSRF-Token: {csrf_token}']
    return ('\r\n'.join(lines) + '\r\n\r\n').encode('ascii')


def read_handshake(sock, nonce, limit=65536):
    """Read the upgrade response and return any bytes the server sent after it."""
    response = b''
    while b'\r\n\r\n' not in response:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError('Device Portal closed the remote input connection during the handshake.')
        response += chunk
        if len(response) > limit:
            raise ConnectionError('Oversized WebSocket handshake response.')
    head, _, rest = response.partition(b'\r\n\r\n')
    status, *header_lines = head.decode('latin-1').split('\r\n')
    parts = status.split(' ', 2)
    if len(parts) < 2 or parts[1] != '101':
        raise ConnectionError(f'Remote input handshake failed: {status}')
    headers = {}
    for line in header_lines:
        key, _, value = line.partition(':')
        headers[key.strip().lower()] = value.strip()
    if headers.get('upgrade', '').lower() != 'websocket':
        raise ConnectionError('Remote input handshake did not upgrade to a WebSocket.')
    if not hmac.compare_digest(headers.get('sec-websocket-accept', ''), accept_key(nonce)):
        raise ConnectionError('Remote input handshake returned an unexpected Sec-WebSocket-Accept key.')
    return rest


def encode_frame(payload, opcode=0x2, mask=None):
    """Encode one final, masked client frame (RFC 6455 section 5.2)."""
    mask = os.urandom(4) if mask is None else mask
    length = len(payload)
    if length < 126:
        header = struct.pack('>BB', 0x80 | opcode, 0x80 | length)
    elif length < 65536:
        header = struct.pack('>BBH', 0x80 | opcode, 0xFE, length)
    else:
        header = struct.pack('>BBQ', 0x80 | opcode, 0xFF, length)
    return header + mask + bytes(value ^ mask[index % 4] for index, value in enumerate(payload))


class RemoteInput:
    """A client WebSocket to /ext/remoteinput over the pinned Device Portal TLS connection."""

    def __init__(self, portal, timeout=15, connect=socket.create_connection):
        self.portal = portal
        # The preflight checks authentication and collects the CSRF cookie before the upgrade.
        portal.request('GET', PREFLIGHT)
        raw = connect((portal.host, portal.port), timeout=timeout)
        try:
            self.sock = portal.context.wrap_socket(raw)
        except Exception:
            raw.close()
            raise
        try:
            actual = hashlib.sha256(self.sock.getpeercert(binary_form=True)).hexdigest()
            if not hmac.compare_digest(actual, portal.fingerprint):
                raise ValueError(f'Xbox portal certificate changed. Observed SHA-256: {actual}')
            nonce = base64.b64encode(os.urandom(16)).decode('ascii')
            host = f'{portal.host}:{portal.port}'
            self.sock.sendall(handshake_request(host, nonce, portal.authorization, portal.csrf_token))
            read_handshake(self.sock, nonce)
        except Exception:
            self.sock.close()
            raise
        self.sent_input = False

    def send(self, payload):
        self.sock.sendall(encode_frame(payload))
        self.sent_input = True

    def screenshot(self, path):
        _, data = self.portal.request('GET', SCREENSHOT, read_limit=32 * 1024 * 1024)
        if not data.startswith(b'\x89PNG\r\n\x1a\n'):
            raise ValueError('Device Portal did not return a PNG screenshot.')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def close(self):
        """Release anything still held, then close the WebSocket with status 1000."""
        try:
            if self.sent_input:
                self.send(bytes((CLEAR_ALL,)))
            self.sock.sendall(encode_frame(struct.pack('>H', 1000), opcode=0x8))
            self.sock.settimeout(2)
            try:
                self.sock.recv(128)  # the server's close frame, if it sends one
            except (OSError, socket.timeout):
                pass
        finally:
            self.sock.close()


def run(client, steps, gap, log, sleep=time.sleep):
    for step in steps:
        for seconds, action in parse_step(step):
            sleep(seconds)
            if isinstance(action, Path):
                client.screenshot(action)
            elif action is not None:
                client.send(action)
        print(f'{time.strftime("%H:%M:%S")} {step}', file=log, flush=True)
        sleep(gap)


def read_script(path):
    steps = []
    for line in path.read_text().splitlines():
        line = line.split('#', 1)[0]
        steps += line.split()
    return steps


def connect_portal(env_file):
    settings = load_settings(env_file)
    fingerprint = settings.get('XBOX_PORTAL_CERT_SHA256', '').lower().replace(':', '')
    if not re.fullmatch(r'[0-9a-f]{64}', fingerprint):
        raise ValueError('Set XBOX_PORTAL_CERT_SHA256 to the verified portal TLS fingerprint.')
    portal = DevicePortal(settings.get('XBOX_PORTAL_URL', ''), fingerprint)
    username = settings.get('XBOX_PORTAL_USERNAME', '')
    password = settings.get('XBOX_PORTAL_PASSWORD', '')
    if ':' in username or (password and not username):
        raise ValueError('Set XBOX_PORTAL_USERNAME to your Device Portal username.')
    if username:
        if not password:
            if not sys.stdin.isatty():
                raise ValueError('Set XBOX_PORTAL_PASSWORD or run this command in a terminal.')
            password = getpass.getpass('Xbox Device Portal password: ')
        portal.authorization = 'Basic ' + base64.b64encode(f'{username}:{password}'.encode()).decode('ascii')
    return portal


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('steps', nargs='*')
    parser.add_argument('--script', type=Path, help='read whitespace-separated steps from a file (# comments)')
    parser.add_argument('--env-file', type=Path)
    parser.add_argument('--gap', type=float, default=0.6, help='seconds between steps (default 0.6)')
    parser.add_argument('--check', action='store_true',
                        help='open the WebSocket and close it without sending input')
    parser.add_argument('--dry-run', action='store_true', help='print the encoded messages without connecting')
    args = parser.parse_args(argv)
    steps = args.steps + (read_script(args.script) if args.script else [])
    if args.check and steps:
        parser.error('--check sends no input; omit the steps')
    if not args.check and not steps:
        parser.error('provide steps, --script, or --check')
    if not 0 <= args.gap <= 60:
        parser.error('--gap must be between 0 and 60 seconds')
    for step in steps:
        try:
            parse_step(step)
        except ValueError as error:
            parser.error(str(error))
    if args.dry_run:
        for step in steps:
            for seconds, action in parse_step(step):
                text = action.hex() if isinstance(action, bytes) else 'screenshot' if action else 'wait'
                print(f'{step}\t+{seconds:g}s\t{text}')
        return
    client = RemoteInput(connect_portal(args.env_file))
    try:
        if args.check:
            print('remote input handshake=ok', flush=True)
        else:
            run(client, steps, args.gap, sys.stdout)
    finally:
        client.close()


if __name__ == '__main__':
    try:
        main()
    except (ValueError, ConnectionError, OSError) as error:
        sys.exit(f'xbox_remote_input: {error}')
