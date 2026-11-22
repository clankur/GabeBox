import hashlib
import json
from pathlib import Path
import runpy
import struct
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile


ROOT = Path(__file__).resolve().parents[1]
builder = runpy.run_path(str(ROOT / "probe" / "prepare-big-picture.py"))
prepare = builder["prepare_layout"]
stage = builder["stage_client"]
library_builder = runpy.run_path(str(ROOT / "probe" / "prepare-big-picture-library.py"))
release_layout = library_builder["release_layout"]
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

    def test_surfaceless_profile_preserves_existing_presenters_and_pins_steam_build(self):
        self.layout.mkdir()
        for dll in ("SDL2.dll", "opengl32.dll", "libEGL.dll", "dxil.dll"):
            (self.layout / dll).touch()
        page = self.layout / "steam-signin.html"
        page.write_text('<script id="steam-sdl-profiles" data-apps="975370 413150 646570" src="steam-sdl-host.js"></script>')
        stage_profiles = library_builder["stage_surfaceless_profiles"]
        stage_profiles(self.layout, ["1145360=10929685"])
        self.assertIn('data-apps="975370 413150 646570 1145360"', page.read_text())
        self.assertEqual((self.layout / "steam-sdl-surfaceless-1145360.txt").read_text(), "10929685")
        with self.assertRaisesRegex(ValueError, "cannot replace"):
            stage_profiles(self.layout, ["413150=123"])

    def test_win32_trial_packages_only_the_runtime_and_a_recipe_for_installed_files(self):
        self.layout.mkdir()
        (self.layout / "XboxProbe.Native.winmd").write_bytes(b"older metadata from the base release")
        profile = self.work / "profile"
        profile.mkdir()
        marker = "steam-win32-shadow-1145360.txt"
        recipe = "steam-win32-shadow-1145360.xwp"
        companions = "steam-win32-files-1145360.txt"
        (profile / marker).write_text("10929685\nHades.exe\n\n\nEngineWin64s.dll\nx64\nloader-trace\n")
        (profile / recipe).write_bytes(
            struct.pack("<8sQII32s", b"XWPEV1\r\n", 512, 64, 32, bytes(32)) + bytes(96))
        extra = "steam-win32-shadow-1145360-launcher.xwp"
        (profile / extra).write_bytes(
            struct.pack("<8sQII32s", b"XWPEV1\r\n", 512, 64, 32, bytes(32)) + bytes(96))
        (profile / companions).write_text("Hades.exe\tlauncher\n")
        (profile / "Hades.exe").write_bytes(b"fixture game payload that must not be packaged")
        template = self.work / "runtime.appx"
        with ZipFile(template, "w") as archive:
            archive.writestr("AppxManifest.xml", MANIFEST.replace(
                b'Name="XboxPortabilityProbe.BigPicture"', b'Name="XboxPortabilityProbe.ChildProcess.SteamCore"'))
            archive.writestr("XboxProbe.Native.winmd", b"metadata")
            for name in ("ProbeNative.dll", "XboxWin32Input.dll", "XboxWin32Desktop.dll", "XboxWin32LoaderTrace.dll",
                         "XboxWin32Window.dll", "XboxWin32CrashReport.dll", "XboxWin32Dxgi.dll"):
                archive.writestr(name, b"runtime fixture")
        stage_trials = library_builder["stage_win32_trial_profiles"]
        stage_trials(self.layout, [f"1145360={profile}"], template)
        self.assertEqual({path.name for path in self.layout.iterdir()},
                         {"XboxProbe.Native.winmd", "ProbeNative.dll", "XboxWin32Input.dll",
                          "XboxWin32Desktop.dll", "XboxWin32LoaderTrace.dll", marker, recipe, extra, companions})
        self.assertEqual((self.layout / recipe).read_bytes(), (profile / recipe).read_bytes())
        self.assertEqual((self.layout / "XboxProbe.Native.winmd").read_bytes(), b"metadata")
        with self.assertRaisesRegex(ValueError, "one APPID"):
            stage_trials(self.layout, [f"1145360={profile}"] * 2, template)
        with self.assertRaisesRegex(ValueError, "x64 Steam Core"):
            stage_trials(self.layout, [f"1145360={profile}"], self.template)

        (profile / marker).write_text("10929685\nGame.exe\nopengl3\ngodot\n")
        stage_trials(self.layout, [f"1145360={profile}"], template)
        self.assertEqual((self.layout / "XboxWin32Window.dll").read_bytes(), b"runtime fixture")
        crash_marker = "steam-win32-crash-report-1145360.txt"
        (profile / crash_marker).touch()
        dxgi_marker = "steam-win32-dxgi-1145360.txt"
        (profile / dxgi_marker).touch()
        with self.assertRaisesRegex(ValueError, "default native renderer"):
            stage_trials(self.layout, [f"1145360={profile}"], template)
        (profile / marker).write_text("10929685\nHades.exe\n\n\nEngineWin64s.dll\nx64\n")
        original_executable_marker = "steam-win32-original-executable-1145360.txt"
        arguments_marker = "steam-win32-arguments-1145360.txt"
        (profile / arguments_marker).write_bytes(b"/Option=true\n/Other=false")
        with self.assertRaisesRegex(ValueError, "single printable launch argument line"):
            stage_trials(self.layout, [f"1145360={profile}"], template)
        (profile / arguments_marker).write_bytes(b"/Option=true /Other=false\r\n")
        (profile / original_executable_marker).write_text("unexpected path")
        with self.assertRaisesRegex(ValueError, "empty original-executable marker"):
            stage_trials(self.layout, [f"1145360={profile}"], template)
        (profile / original_executable_marker).write_bytes(b"")
        (self.layout / "XboxWin32LoaderTrace.dll").unlink()
        stage_trials(self.layout, [f"1145360={profile}"], template)
        self.assertEqual((self.layout / dxgi_marker).read_bytes(), b"")
        self.assertEqual((self.layout / crash_marker).read_bytes(), b"")
        self.assertEqual((self.layout / original_executable_marker).read_bytes(), b"")
        self.assertEqual((self.layout / arguments_marker).read_bytes(), b"/Option=true /Other=false")
        self.assertEqual((self.layout / "XboxWin32LoaderTrace.dll").read_bytes(), b"runtime fixture")
        self.assertEqual((self.layout / "XboxWin32Dxgi.dll").read_bytes(), b"runtime fixture")

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
        with self.assertRaisesRegex(ValueError, "verified Steam Core"):
            release_layout(base, self.layout, "1.0.230.1")
        pinned = {hashlib.sha256(base.read_bytes()).hexdigest(): "1.0.230.0"}
        with patch.dict(release_layout.__globals__, VERIFIED_BASES=pinned):
            release_layout(base, self.layout, "1.0.230.1")
        self.assertEqual((self.layout / "AppxManifest.xml").read_bytes(),
                         manifest.replace(b'Version="1.0.230.0"', b'Version="1.0.230.1"'))
        self.assertEqual((self.layout / "SlayConsoleDiagnostic$1.class").read_bytes(), b"class bytes")
        self.assertFalse((self.layout / "AppxBlockMap.xml").exists())


if __name__ == "__main__":
    unittest.main()
