import hashlib
import json
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile


ROOT = Path(__file__).resolve().parents[1]
builder = runpy.run_path(str(ROOT / "probe" / "prepare-big-picture.py"))
prepare = builder["prepare_layout"]
stage = builder["stage_client"]
release_layout = runpy.run_path(str(ROOT / "probe" / "prepare-big-picture-library.py"))["release_layout"]
MANIFEST = b"""<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10">
<Identity Name="XboxPortabilityProbe.BigPicture" ProcessorArchitecture="x64" Publisher="CN=Xbox Portability Probe"/>
<Applications><Application Id="App" StartPage="big-picture.html"/></Applications>
<Extensions><Extension Category="windows.activatableClass.inProcessServer"><InProcessServer>
<Path>BigPicture.Native.dll</Path><ActivatableClass ActivatableClassId="BigPicture.Native.Launcher"/>
</InProcessServer></Extension></Extensions>
<Capabilities><Capability Name="internetClient"/></Capabilities>
</Package>"""


class BigPicturePackageTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.work = Path(temp.name)
        self.layout = self.work / "layout"
        self.cache = self.work / "cache"
        self.cache.mkdir()
        self.template = self.work / "BigPictureTemplate-x64.appx"
        with ZipFile(self.template, "w") as package:
            for name in ("big-picture.html", "big-picture.js", "BigPicture.Native.dll", "BigPicture.Native.winmd",
                         "resources.pri", "Assets/Logo.png", "Assets/SmallLogo.png", "Assets/Splash.png",
                         "Assets/StoreLogo.png", "AppxSignature.p7x",
                         "[Content_Types].xml", "AppxBlockMap.xml", "steam-signin.html"):
                package.writestr(name, name)
            package.writestr("AppxManifest.xml", MANIFEST)

        self.archive = self.cache / ("steam_win64_steamrow.zip." + "a" * 40)
        with ZipFile(self.archive, "w") as archive:
            archive.writestr("steam.exe", b"original")
            archive.writestr("steamui\\localization/", b"")
            archive.writestr("steamui/localization/english.js", b"strings")
        data = self.archive.read_bytes()
        self.manifest = self.work / "steam_client_win64.manifest"
        self.manifest.write_text(f'"file" "{self.archive.name}" "size" "{len(data)}" '
                                 f'"sha2" "{hashlib.sha256(data).hexdigest()}"')
        release = self.work / "valve-release.json"
        release.write_text(json.dumps({"version": "fixture", "manifest_url": "https://unused.invalid/release",
                                       "manifest_sha256": hashlib.sha256(self.manifest.read_bytes()).hexdigest()}))
        pin = patch.dict(stage.__globals__, RELEASE=release, MANIFEST=self.manifest)
        pin.start()
        self.addCleanup(pin.stop)

    def test_native_entry_and_original_payload(self):
        prepare(self.template, self.layout, self.cache)
        expected = {"big-picture.html", "big-picture.js", "BigPicture.Native.dll", "BigPicture.Native.winmd",
                    "AppxManifest.xml", "resources.pri", "Assets/Logo.png", "Assets/SmallLogo.png",
                    "Assets/Splash.png", "Assets/StoreLogo.png",
                    "ValveClient/steam.exe", "ValveClient/steamui/localization/english.js",
                    "ValveClient/package/steam_client_win64.manifest"}
        self.assertEqual({p.relative_to(self.layout).as_posix() for p in self.layout.rglob("*") if p.is_file()},
                         expected)
        self.assertEqual((self.layout / "AppxManifest.xml").read_bytes(), MANIFEST)
        self.assertEqual((self.layout / "ValveClient" / "steam.exe").read_bytes(), b"original")
        with self.assertRaises(FileExistsError):
            prepare(self.template, self.layout, self.cache)

    def test_changed_cached_archive_and_manifest_leave_no_partial_layout(self):
        self.archive.write_bytes(b"x" * self.archive.stat().st_size)
        with self.assertRaisesRegex(ValueError, "Archive did not match"):
            prepare(self.template, self.layout, self.cache)
        self.assertFalse(self.layout.exists())
        self.manifest.write_text("changed")
        with self.assertRaisesRegex(ValueError, "manifest differs"):
            prepare(self.template, self.layout, self.cache)
        self.assertFalse(self.layout.exists())

    def test_legacy_template_cannot_select_native_mode(self):
        with ZipFile(self.template, "w") as package:
            package.writestr("AppxManifest.xml", MANIFEST.replace(b'StartPage="big-picture.html"',
                                                                   b'StartPage="index.html"'))
        with self.assertRaisesRegex(ValueError, "native launcher"):
            prepare(self.template, self.layout, self.cache)
        self.assertFalse(self.layout.exists())

    def test_webview_stages_only_the_complete_ui_set(self):
        entries = []
        for name, member in (("steamui_websrc_all", "steamui/index.html"),
                             ("steamui_websrc_sounds_all", "steamui/sounds/open.wav"),
                             ("steamui_websrc_movies_all", "steamui/movies/start.webm")):
            path = self.cache / (name + ".zip." + "b" * 40)
            with ZipFile(path, "w") as archive:
                archive.writestr(member, member.encode())
            data = path.read_bytes()
            entries.append(f'"file" "{path.name}" "size" "{len(data)}" '
                           f'"sha2" "{hashlib.sha256(data).hexdigest()}"')
        native = self.manifest.read_text()
        self.archive.unlink()  # The web layout must build without any native client archives.
        release = stage.__globals__["RELEASE"]
        pin = json.loads(release.read_text())
        for count in (2, 3):
            self.manifest.write_text("\n".join([native] + entries[:count]))
            pin["manifest_sha256"] = hashlib.sha256(self.manifest.read_bytes()).hexdigest()
            release.write_text(json.dumps(pin))
            if count == 2:
                with self.assertRaisesRegex(ValueError, "steamui archive set"):
                    stage(self.layout, self.cache, ui_only=True)
                self.assertFalse(self.layout.exists())
            else:
                stage(self.layout, self.cache, ui_only=True)
        self.assertEqual([p.name for p in self.layout.iterdir()], ["steamui"])
        for path in self.layout.rglob("*"):
            if path.is_file():
                self.assertEqual(path.read_text(), path.relative_to(self.layout).as_posix())

    def test_fhd_upgrade_keeps_package_identity_and_decodes_opc_file_names(self):
        base = self.work / "FHD.appx"
        manifest = MANIFEST.replace(b'Name="XboxPortabilityProbe.BigPicture"',
                                    b'Name="XboxPortabilityProbe.ChildProcess.SteamCore" Version="1.0.230.0"')
        with ZipFile(base, "w") as package:
            package.writestr("AppxManifest.xml", manifest)
            package.writestr("SlayConsoleDiagnostic%241.class", b"class bytes")
            package.writestr("AppxBlockMap.xml", b"old footprint")
        self.layout.mkdir()
        with self.assertRaisesRegex(ValueError, "signed Steam Core"):
            release_layout(base, self.layout, "1.0.230.1")
        with patch.dict(release_layout.__globals__, BASE_SHA256=hashlib.sha256(base.read_bytes()).hexdigest()):
            release_layout(base, self.layout, "1.0.230.1")
        self.assertEqual((self.layout / "AppxManifest.xml").read_bytes(),
                         manifest.replace(b'Version="1.0.230.0"', b'Version="1.0.230.1"'))
        self.assertEqual((self.layout / "SlayConsoleDiagnostic$1.class").read_bytes(), b"class bytes")
        self.assertFalse((self.layout / "AppxBlockMap.xml").exists())


if __name__ == "__main__":
    unittest.main()
