import contextlib
import hashlib
import io
from pathlib import Path
import tempfile
import unittest

from tools import artifacts


def sums(*entries, newline=b'\n'):
    return b''.join(f'{hashlib.sha256(data).hexdigest()}  {name}'.encode() + newline for name, data in entries)


class ChecksumTests(unittest.TestCase):
    def test_accepts_lf_crlf_and_binary_markers(self):
        listed = (('Probe-x64.appx', b'appx'), ('Probe.cer', b'cer'))
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            for name, data in listed:
                (directory / name).write_bytes(data)
            for manifest in (sums(*listed), sums(*listed, newline=b'\r\n'), sums(*listed).replace(b'  P', b' *P', 1)):
                with self.subTest(manifest=manifest):
                    (directory / 'SHA256SUMS').write_bytes(manifest)
                    pin = hashlib.sha256(manifest).hexdigest()
                    self.assertEqual(list(artifacts.verify_checksums(directory, pin.upper())), ['Probe-x64.appx', 'Probe.cer'])
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                artifacts.main(['verify', folder])
            self.assertEqual(output.getvalue(), 'Probe-x64.appx: OK\nProbe.cer: OK\n')

    def test_rejects_mismatches_pins_and_unsafe_names(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            (directory / 'a.dll').write_bytes(b'a')
            (directory / 'SHA256SUMS').write_bytes(sums(('a.dll', b'changed'), newline=b'\r\n'))
            with self.assertRaisesRegex(ValueError, 'a.dll does not match'):
                artifacts.verify_checksums(directory)
            (directory / 'SHA256SUMS').write_bytes(sums(('a.dll', b'a')))
            with self.assertRaisesRegex(ValueError, 'pin'):
                artifacts.verify_checksums(directory, '0' * 64)
        for manifest in (b'', sums(('../a.dll', b'a')), sums(('a', b'a'), ('a', b'a')), b'abc  a.dll\n'):
            with self.subTest(manifest=manifest), self.assertRaises(ValueError):
                artifacts.read_checksums(manifest)


if __name__ == '__main__':
    unittest.main()
