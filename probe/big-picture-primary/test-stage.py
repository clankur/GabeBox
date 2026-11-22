import hashlib
from pathlib import Path
import runpy
import tempfile
import unittest
from xml.etree import ElementTree as ET

stage = runpy.run_path(str(Path(__file__).with_name("stage.py")))["stage_primary"]
MANIFEST = b"""<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
 xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"><Applications>
 <Application Id="App" StartPage="steam-signin.html"><uap:VisualElements></uap:VisualElements></Application>
 </Applications></Package>"""
PAGE = b'<html data-steam-big-picture="true"><meta charset="utf-8"><script src="steam-signin.js"></script></html>'


class Primary(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.layout = Path(temp.name)
        self.artifact = self.layout / "build"
        self.artifact.mkdir()
        payload = b"MZ primary host test fixture"
        (self.artifact / "SteamPrimary.exe").write_bytes(payload)
        sums = (hashlib.sha256(payload).hexdigest() + "  SteamPrimary.exe\r\n").encode()
        (self.artifact / "SHA256SUMS").write_bytes(sums)
        self.pin = hashlib.sha256(sums).hexdigest()
        self.manifest = self.layout / "AppxManifest.xml"
        self.manifest.write_bytes(MANIFEST)
        self.html = self.layout / "steam-signin.html"
        self.html.write_bytes(PAGE)

    def test_only_the_fixed_packaged_document_gets_projection(self):
        stage(self.layout, self.artifact, self.pin)
        app = ET.parse(self.manifest).find("./{*}Applications/{*}Application")
        self.assertEqual(app.attrib, {"Id": "App", "Executable": "SteamPrimary.exe", "EntryPoint": "BigPicture.Primary.App"})
        rules = app.findall("{*}ApplicationContentUriRules/{*}Rule")
        self.assertEqual([rule.attrib for rule in rules], [
            {"Match": "ms-appx-web:///steam-signin.html", "WindowsRuntimeAccess": "all", "Type": "include"}])
        html = self.html.read_bytes()
        self.assertIn(b'script-src \'self\'', html)
        self.assertLess(html.index(b"Content-Security-Policy"), html.index(b"<script"))
        self.assertIn(b'<script src="steam-signin.js"></script>', html)
        self.assertEqual((self.layout / "SteamPrimary.exe").read_bytes(), (self.artifact / "SteamPrimary.exe").read_bytes())

    def test_tampered_payload_is_rejected_before_staging(self):
        (self.artifact / "SteamPrimary.exe").write_bytes(b"changed")
        with self.assertRaises(ValueError):
            stage(self.layout, self.artifact, self.pin)
        self.assertEqual(self.manifest.read_bytes(), MANIFEST)
        self.assertFalse((self.layout / "SteamPrimary.exe").exists())

    def test_reject_existing_privileges_in_another_content_rule(self):
        self.manifest.write_bytes(MANIFEST.replace(b"</Application>", b"<uap:ApplicationContentUriRules/></Application>"))
        with self.assertRaises(ValueError):
            stage(self.layout, self.artifact, self.pin)


if __name__ == "__main__":
    unittest.main()
