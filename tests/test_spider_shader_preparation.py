import hashlib
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


PROFILE = Path(__file__).resolve().parents[1] / "profiles/1817070/23986256"


class SpiderShaderPreparationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.shader = self.directory / "original.dxil"
        self.shader.write_bytes(b"pinned source")
        self.output = self.directory / "output.dxil"
        with patch.object(sys, "path", [str(PROFILE), *sys.path]):
            self.main = runpy.run_path(str(PROFILE / "prepare-compression-shader.py"))["main"]

    def prepare(self, *, validator_fails=False):
        def dump(argv, **kwargs):
            self.shader.write_bytes(b"changed after verification")
            source = Path(argv[2])
            self.assertNotEqual(source, self.shader)
            self.assertEqual(source.read_bytes(), b"pinned source")
            return "verified IR"

        def run(argv, **kwargs):
            if argv[0] == "dxa":
                self.assertEqual(Path(argv[2]).read_text(encoding="utf-8"), "prepared verified IR")
                Path(argv[1].removeprefix("-o=")).write_bytes(b"assembled")
            else:
                self.assertEqual(argv[0], "dxv")
                self.assertEqual(Path(argv[3]).read_bytes(), b"assembled")
                if validator_fails:
                    raise subprocess.CalledProcessError(1, argv)
                Path(argv[2]).write_bytes(b"validated")

        digest = hashlib.sha256(self.shader.read_bytes()).hexdigest()
        with (patch.dict(self.main.__globals__, SHADERS={digest: object()},
                        scalarize=lambda text, profile: "prepared " + text),
              patch.object(sys, "argv", ["prepare-compression-shader.py", str(self.shader), str(self.output)]),
              patch.object(subprocess, "check_output", side_effect=dump),
              patch.object(subprocess, "run", side_effect=run)):
            self.main()

    def test_uses_private_copy_of_the_verified_shader(self):
        self.prepare()
        self.assertEqual(self.output.read_bytes(), b"validated")

    def test_failed_validation_does_not_publish_a_container(self):
        with patch("sys.stderr"), self.assertRaises(SystemExit):
            self.prepare(validator_fails=True)
        self.assertFalse(self.output.exists())
