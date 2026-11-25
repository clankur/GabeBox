import base64
import contextlib
import hashlib
import io
import struct
import tempfile
import unittest
from pathlib import Path

from tools import xbox_remote_input as remote


class FakeSocket:
    def __init__(self, replies=(), certificate=b'xbox-cert'):
        self.replies = list(replies)
        self.sent = b''
        self.closed = False
        self.certificate = certificate

    def recv(self, size):
        if not self.replies:
            return b''
        chunk = self.replies.pop(0)
        if len(chunk) > size:
            self.replies.insert(0, chunk[size:])
            chunk = chunk[:size]
        return chunk

    def sendall(self, data):
        self.sent += data

    def settimeout(self, value):
        pass

    def getpeercert(self, binary_form=False):
        return self.certificate

    def close(self):
        self.closed = True


class FakeContext:
    def __init__(self, sock):
        self.sock = sock

    def wrap_socket(self, raw):
        return self.sock


class FakePortal:
    host, port = 'xbox.local', 11443
    authorization = None

    def __init__(self, sock, fingerprint=None, csrf_token='token123'):
        self.context = FakeContext(sock)
        self.fingerprint = fingerprint or hashlib.sha256(sock.certificate).hexdigest()
        self.csrf_token = csrf_token
        self.requests = []

    def request(self, method, path):
        self.requests.append((method, path))
        return 200, b'{"ConnectedControllerCount":1}'


def upgrade_reply(nonce, accept=None, status='HTTP/1.1 101 Switching Protocols'):
    accept = remote.accept_key(nonce) if accept is None else accept
    return (f'{status}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
            f'Sec-WebSocket-Accept: {accept}\r\n\r\n').encode()


def unmask(frame):
    length = frame[1] & 0x7F
    assert frame[1] & 0x80 and length < 126
    mask = frame[2:6]
    return frame[0], bytes(value ^ mask[index % 4] for index, value in enumerate(frame[6:6 + length]))


def frames(data):
    out = []
    while data:
        length = data[1] & 0x7F
        out.append(unmask(data[:6 + length]))
        data = data[6 + length:]
    return out


class MessageEncodingTests(unittest.TestCase):
    def test_gamepad_tap_uses_key_code_messages(self):
        self.assertEqual(remote.parse_step('a'), [(0, b'\x01\xc3\x01'), (remote.TAP_SECONDS, b'\x01\xc3\x00')])
        self.assertEqual(remote.parse_step('VIEW')[0][1], b'\x01\xd0\x01')
        self.assertEqual(remote.parse_step('ls_left')[0][1], b'\x01\xd6\x01')

    def test_hold_and_repeat(self):
        self.assertEqual(remote.parse_step('hold:RT:1.5'), [(0, b'\x01\xca\x01'), (1.5, b'\x01\xca\x00')])
        self.assertEqual(len(remote.parse_step('DOWN*3')), 6)
        self.assertEqual(remote.parse_step('chord:menu+VIEW'), [
            (0, b'\x01\xcf\x01'), (0, b'\x01\xd0\x01'),
            (remote.CHORD_SECONDS, b'\x01\xcf\x00'), (0, b'\x01\xd0\x00')])
        self.assertEqual(remote.parse_step('chord:MENU+VIEW:1')[2], (1.0, b'\x01\xcf\x00'))
        self.assertEqual(remote.parse_step('DOWN*3')[4][1], b'\x01\xcc\x01')

    def test_keyboard_uses_scancodes_except_navigation_keys(self):
        self.assertEqual(remote.parse_step('key:enter')[0][1], b'\x02\x1c\x01')
        self.assertEqual(remote.parse_step('key:ESC')[1][1], b'\x02\x01\x00')
        self.assertEqual(remote.parse_step('key:Q')[0][1], b'\x02\x10\x01')
        self.assertEqual(remote.parse_step('key:M')[0][1], b'\x02\x32\x01')
        self.assertEqual(remote.parse_step('key:0')[0][1], b'\x02\x0b\x01')
        self.assertEqual(remote.parse_step('key:F10')[0][1], b'\x02\x44\x01')
        self.assertEqual(remote.parse_step('key:F11')[0][1], b'\x02\x57\x01')
        # Arrows need the 0xE0 prefix, which a scancode message cannot carry.
        self.assertEqual(remote.parse_step('key:up')[0][1], b'\x01\x26\x01')
        self.assertEqual(remote.parse_step('scan:48')[0][1], b'\x02\x48\x01')
        self.assertEqual(remote.parse_step('vk:0d')[0][1], b'\x01\x0d\x01')

    def test_mouse_messages_are_network_order_absolute_positions(self):
        self.assertEqual(remote.mouse_message(remote.MOUSE_MOVE, 65535, 1),
                         b'\x03\x00\x01\x00\x00\xff\xff\x00\x00\x00\x01')
        move, down, up = (message for _, message in remote.parse_step('click:1919,0'))
        self.assertEqual(struct.unpack('>BHII', move), (3, 1, 65535, 0))
        self.assertEqual(struct.unpack('>BHII', down), (3, 2, 65535, 0))
        self.assertEqual(struct.unpack('>BHII', up), (3, 4, 65535, 0))
        self.assertEqual(struct.unpack('>BHII', remote.parse_step('move:960,540')[0][1])[2:], (32785, 32798))

    def test_invalid_steps(self):
        for step in ('chord:MENU', 'chord:MENU+MENU', 'chord:MENU+Q', 'chord:MENU+VIEW:-1', 'Z', 'key:NOPE', 'shot:a.jpg', 'hold:Q:1', 'click:1920,0', 'click:1,2,3', 'scan:100', 'wait:-1',
                     'A*0', 'vk:zz'):
            with self.subTest(step=step), self.assertRaises(ValueError):
                remote.parse_step(step)

    def test_frames_are_masked_binary(self):
        frame = remote.encode_frame(b'\x01\xc3\x01', mask=b'\x01\x02\x03\x04')
        self.assertEqual(frame[:2], b'\x82\x83')
        self.assertEqual(unmask(frame), (0x82, b'\x01\xc3\x01'))
        long_frame = remote.encode_frame(bytes(300), mask=b'\0\0\0\0')
        self.assertEqual(long_frame[:4], b'\x82\xfe\x01\x2c')
        self.assertEqual(remote.encode_frame(b'\x03\xe8', opcode=8, mask=b'\0\0\0\0')[:2], b'\x88\x82')

    def test_run_sends_messages_in_order(self):
        sent, slept = [], []

        class Client:
            def send(self, message):
                sent.append(message)

            def screenshot(self, path):
                sent.append(path)

        log = io.StringIO()
        remote.run(Client(), ['A', 'wait:2', 'key:ENTER', 'shot:out/a.png'], 0.5, log, sleep=slept.append)
        self.assertEqual(sent, [b'\x01\xc3\x01', b'\x01\xc3\x00', b'\x02\x1c\x01', b'\x02\x1c\x00',
                                Path('out/a.png')])
        self.assertIn(2, slept)
        self.assertEqual(len(log.getvalue().splitlines()), 4)


class HandshakeTests(unittest.TestCase):
    def test_accept_key_matches_rfc_6455_example(self):
        self.assertEqual(remote.accept_key('dGhlIHNhbXBsZSBub25jZQ=='), 's3pPLMBiTxaQ9kYGzzhZRbK+xOo=')

    def test_request_carries_csrf_and_authorization(self):
        request = remote.handshake_request('xbox:11443', 'abc=', 'Basic dTpw', 'tok').decode()
        self.assertTrue(request.startswith('GET /ext/remoteinput HTTP/1.1\r\nHost: xbox:11443\r\n'))
        for header in ('Upgrade: websocket', 'Sec-WebSocket-Key: abc=', 'Sec-WebSocket-Version: 13',
                       'Authorization: Basic dTpw', 'Cookie: CSRF-Token=tok', 'X-CSRF-Token: tok'):
            self.assertIn(header + '\r\n', request)
        self.assertTrue(request.endswith('\r\n\r\n'))
        self.assertNotIn('Cookie', remote.handshake_request('xbox:11443', 'abc=').decode())

    def test_read_handshake_accepts_split_response_and_returns_trailing_bytes(self):
        nonce = base64.b64encode(b'0123456789abcdef').decode()
        reply = upgrade_reply(nonce).replace(b'Sec-WebSocket-Accept', b'sec-websocket-accept')
        sock = FakeSocket([reply[:20], reply[20:] + b'\x88\x00'])
        self.assertEqual(remote.read_handshake(sock, nonce), b'\x88\x00')

    def test_read_handshake_rejects_bad_responses(self):
        nonce = base64.b64encode(b'0123456789abcdef').decode()
        cases = {
            'wrong accept': upgrade_reply(nonce, accept='bogus='),
            'http error': upgrade_reply(nonce, status='HTTP/1.1 403 Forbidden'),
            'missing upgrade': upgrade_reply(nonce).replace(b'Upgrade: websocket\r\n', b''),
            'closed': b'HTTP/1.1 101 Switching',
        }
        for name, reply in cases.items():
            with self.subTest(name), self.assertRaises(ConnectionError):
                remote.read_handshake(FakeSocket([reply]), nonce)

    def test_client_handshake_sends_close_only_when_no_input(self):
        sock = FakeSocket()
        portal = FakePortal(sock)
        original = remote.read_handshake
        captured = {}

        def fake_read(s, nonce):
            captured['nonce'] = nonce
            return original(FakeSocket([upgrade_reply(nonce)]), nonce)

        remote.read_handshake = fake_read
        try:
            client = remote.RemoteInput(portal, connect=lambda address, timeout: sock)
        finally:
            remote.read_handshake = original
        self.assertEqual(portal.requests, [('GET', remote.PREFLIGHT)])
        request = sock.sent.decode()
        self.assertIn(f'Sec-WebSocket-Key: {captured["nonce"]}\r\n', request)
        self.assertIn('Cookie: CSRF-Token=token123\r\n', request)
        sock.sent = b''
        client.close()
        self.assertEqual(frames(sock.sent), [(0x88, b'\x03\xe8')])
        self.assertTrue(sock.closed)

    def test_client_clears_held_input_before_closing(self):
        sock = FakeSocket()
        original = remote.read_handshake
        remote.read_handshake = lambda s, nonce: b''
        try:
            client = remote.RemoteInput(FakePortal(sock), connect=lambda address, timeout: sock)
        finally:
            remote.read_handshake = original
        sock.sent = b''
        client.send(b'\x01\xc3\x01')
        client.close()
        self.assertEqual(frames(sock.sent), [(0x82, b'\x01\xc3\x01'), (0x82, b'\x04'), (0x88, b'\x03\xe8')])

    def test_client_rejects_changed_certificate(self):
        sock = FakeSocket()
        with self.assertRaises(ValueError):
            remote.RemoteInput(FakePortal(sock, fingerprint='0' * 64), connect=lambda address, timeout: sock)
        self.assertTrue(sock.closed)
        self.assertEqual(sock.sent, b'')


class CommandLineTests(unittest.TestCase):
    def test_dry_run_prints_messages(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            remote.main(['--dry-run', 'A', 'key:ENTER'])
        self.assertIn('A\t+0s\t01c301', out.getvalue())
        self.assertIn('key:ENTER\t+0.08s\t021c00', out.getvalue())

    def test_script_file(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'steps.txt'
            path.write_text('# comment\nA wait:1  # trailing\nDOWN*2\n')
            self.assertEqual(remote.read_script(path), ['A', 'wait:1', 'DOWN*2'])


if __name__ == '__main__':
    unittest.main()
