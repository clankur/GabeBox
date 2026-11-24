import hashlib
from pathlib import Path
import runpy
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
from zipfile import ZipFile

STAGE = runpy.run_path(str(Path(__file__).resolve().parents[1] /
                           "build_tools/stage_glyphs.py"))["stage_glyphs"]


class GlyphStageTests(unittest.TestCase):
    def setUp(self):
        temp = TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.package = Path(temp.name) / "public.zip"
        self.steamui = Path(temp.name) / "steamui"

    def archive(self, buttons):
        with ZipFile(self.package, "w") as archive:
            for button in buttons:
                for style in ("knockout", "light"):
                    archive.writestr(f"controller_base/images/api/{style}/shared_button_{button}.svg",
                                     f"{style}-{button}")
            archive.writestr("controller_base/images/api/knockout/shared_button_a_sm.png", b"png")
            archive.writestr("clientui/fonts/clientui.uifont", b"font container")
            archive.writestr("steamui/movies/bigpicture_startup.webm", b"stock movie")
        return hashlib.sha256(self.package.read_bytes()).hexdigest()

    def test_only_native_glyphs_are_mapped_unchanged(self):
        with patch.dict(STAGE.__globals__, PUBLIC_SHA256=self.archive("abx")):
            STAGE(self.steamui, self.package)
            with self.assertRaises(FileExistsError):
                STAGE(self.steamui, self.package)
        files = {f.relative_to(self.steamui).as_posix(): f.read_bytes()
                 for f in self.steamui.rglob("*") if f.is_file()}
        self.assertEqual(files, {f"steaminputglyphs/shared_button_{key}.svg": f"knockout-{key}".encode()
                                 for key in "abx"})

    def test_tampered_or_incomplete_archive_publishes_no_glyphs(self):
        pin = self.archive("a")
        with self.assertRaisesRegex(ValueError, "Unexpected public_all"):
            STAGE(self.steamui, self.package)
        with patch.dict(STAGE.__globals__, PUBLIC_SHA256=pin), self.assertRaisesRegex(ValueError, "Missing"):
            STAGE(self.steamui, self.package)
        self.assertFalse((self.steamui / "steaminputglyphs").exists())


if __name__ == "__main__":
    unittest.main()
