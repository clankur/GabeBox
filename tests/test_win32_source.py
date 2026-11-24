import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


VERIFY = Path(__file__).resolve().parents[1] / "probe" / "verify-win32-source.py"


class Win32SourceTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        work = Path(temp.name)
        self.profile = work / "source.json"
        self.install = work / "install"
        self.source = self.install / "asset_archive" / "toc"
        self.source.parent.mkdir(parents=True)
        original = bytes(range(256)) * 5000
        self.source.write_bytes(original)
        self.record = {"path": "asset_archive/toc", "size": len(original),
                       "sha256": hashlib.sha256(original).hexdigest()}

    def verify(self, build="123", *rows):
        self.profile.write_text(json.dumps({"123": rows or [self.record]}))
        return subprocess.run([sys.executable, str(VERIFY), str(self.profile), str(self.install), "--build", build],
                              capture_output=True, text=True, check=False)

    def test_build_and_hash_are_checked_independently(self):
        self.assertIn("matched 1 source fingerprints", self.verify().stdout)
        self.assertIn("Unsupported Steam build 124", self.verify("124").stderr)
        original = self.source.read_bytes()
        self.source.write_bytes(b"x" + original[1:])
        self.assertIn("SHA-256 mismatch", self.verify().stderr)
        self.source.write_bytes(original[:-1])
        self.assertIn("Size mismatch", self.verify().stderr)

    def test_profile_cannot_read_outside_install_or_duplicate_paths(self):
        for name in ("../Game.exe", "assets\\game.exe", "NUL.exe", "assets//game.exe", "asset_archive/TOC"):
            with self.subTest(name=name):
                invalid = self.verify("123", self.record, dict(self.record, path=name))
                self.assertEqual(invalid.returncode, 1)
                self.assertIn("Invalid source fingerprint", invalid.stderr)
        elsewhere = self.install.parent / "toc"
        self.source.rename(elsewhere)
        self.source.symlink_to(elsewhere)
        self.assertIn("Unexpected file type", self.verify().stderr)
