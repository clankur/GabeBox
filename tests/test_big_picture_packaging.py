import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import Mock, patch
from xml.etree import ElementTree
from zipfile import ZipFile


from build_tools import bundle_big_picture, stage_glyphs, stage_session, stage_webview
from build_tools import prepare_big_picture_library as library_builder
from build_tools.stage_session import APPX as SESSION_APPX
from build_tools.prepare_big_picture import prepare_layout as prepare
from build_tools.stage_pinned_client import stage_client as stage

ROOT = Path(__file__).resolve().parents[1]
release_layout = library_builder.release_layout
MANIFEST = b"""<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10">
<Identity Name="XboxPortabilityProbe.BigPicture" ProcessorArchitecture="x64" Publisher="CN=Xbox Portability Probe"/>
<Applications><Application Id="App" StartPage="big-picture.html"/></Applications>
<Extensions><Extension Category="windows.activatableClass.inProcessServer"><InProcessServer>
<Path>BigPicture.Native.dll</Path><ActivatableClass ActivatableClassId="BigPicture.Native.Launcher"/>
</InProcessServer></Extension></Extensions>
<Capabilities><Capability Name="internetClient"/></Capabilities>
</Package>"""

PROBE_PROPERTIES = (b"<Properties><DisplayName>Steam Core Loader Probe</DisplayName>"
                    b"<PublisherDisplayName>Xbox Portability Probe</PublisherDisplayName></Properties>")
PROBE_TILE = (b'<uap:VisualElements DisplayName="Steam Core Loader Probe" Description="Measures launching one '
              b'fixed packaged child process" BackgroundColor="#202020" Square150x150Logo="Assets\\Logo.png"/>')


def stage_trials(layout, profiles, template, source_profiles=()):
    with ZipFile(template) as package:
        library_builder.stage_win32_trial_profiles(layout, profiles, package, source_profiles)


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
        release.write_text(json.dumps({"version": "1794536281", "manifest_url": "https://unused.invalid/release",
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

    def test_integrated_library_stages_components_and_rejects_invalid_pin(self):
        manifest = (MANIFEST.replace(b"XboxPortabilityProbe.BigPicture", b"XboxPortabilityProbe.ChildProcess.SteamCore")
                    .replace(b'ProcessorArchitecture=', b'Version="1.0.230.0" ProcessorArchitecture=')
                    .replace(b"BigPicture.Native.dll", b"ProbeNative.dll")
                    .replace(b"BigPicture.Native.Launcher", b"XboxProbe.Native.ProbeRunner")
                    .replace(b'<Package xmlns=', b'<Package xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10" xmlns=')
                    .replace(b'StartPage="big-picture.html"/>', b'StartPage="steam-signin.html">' + PROBE_TILE + b'</Application>')
                    .replace(b"<Applications>", PROBE_PROPERTIES + b"<Dependencies/><Applications>"))
        base = self.work / "fhd.appx"
        page = (b'<html lang="en"><script src="steam-signin.js"></script>'
                b'<script id="steam-sdl-profiles" data-apps="975370" src="steam-sdl-host.js"></script></html>')
        with ZipFile(base, "w") as package:
            package.writestr("AppxManifest.xml", manifest)
            package.writestr("ProbeNative.dll", b"MZold")
            package.writestr("XboxProbe.Native.winmd", b"MZold")
            package.writestr("steam-signin.html", page)
            package.writestr("steam-cloud.js", b"old Cloud helper")
            package.writestr("steam-sdl-host-975370.txt", b"")
            package.writestr("steam-sdl-host-413150.txt", b"")
            package.writestr("StardewStartupDiagnostic.dll", b"recovery hook")
            package.writestr("xcom-runtime/placeholder", b"dos runtime")
            package.writestr("AppxBlockMap.xml", b"stale footprint")
            for name in library_builder.BRAND_FILES:
                package.writestr(f"Assets/{name}", b"probe tile")
            for name in ("SDL2.dll", "opengl32.dll", "libEGL.dll", "dxil.dll"):
                package.writestr(name, b"runtime fixture")

        session = self.work / "session"
        session.mkdir()
        session_appx = session / SESSION_APPX
        xinput = "XboxWin32XInput910.dll"
        transactions = "ktmw32.dll"
        startup_hook = b"managed SDK hook fixture"
        metadata = (b"MZ\x00SteamSessionSteamId\x00RunSteamSignIn\x00ResumeSteamSignIn\x00LaunchSteamGame\x00"
                    b"LaunchSteamSdlGame\x00SteamLibraryCommand\x00")
        with ZipFile(session_appx, "w") as package:
            package.writestr("AppxManifest.xml", manifest)
            package.writestr("AppxSignature.p7x", b"signature fixture")
            package.writestr("ProbeNative.dll", b"MZsession")
            package.writestr("XboxProbe.Native.winmd", metadata)
            package.writestr("StardewStartupDiagnostic.dll", startup_hook)
            for name in ("XboxWin32Input.dll", "XboxWin32Desktop.dll", xinput, transactions):
                package.writestr(name, b"session artifact adapter")
        sums = f"{hashlib.sha256(session_appx.read_bytes()).hexdigest()}  {SESSION_APPX}\n".encode()
        (session / "SHA256SUMS").write_bytes(sums)

        host = self.work / "host"
        host.mkdir()
        host_pins = {}
        for name in (*stage_webview.HOST_FILES, *stage_webview.SDK_FILES):
            (host / name).write_bytes(name.encode())
            host_pins[name] = hashlib.sha256(name.encode()).hexdigest()
        (host / "SHA256SUMS").write_text("".join(f"{pin}  {name}\n" for name, pin in host_pins.items()))

        urls = b'SteamStore\n{\nurl "https://store.steampowered.com/"\n}\n'
        archives = {
            "steamui_websrc_all": {"steamui/index.html": b"Valve UI fixture"},
            "steamui_websrc_sounds_all": {"steamui/sounds/open.wav": b"sound fixture"},
            "steamui_websrc_movies_all": {"steamui/movies/bigpicture_startup.webm": b"movie fixture"},
            "public_all": {
                "public/url_list.txt": urls,
                "controller_base/images/api/knockout/shared_button_a.svg": b"<svg/>",
                "controller_base/images/api/knockout/shared_button_b.svg": b"<svg/>",
            },
        }
        entries = []
        pins = {}
        for archive_name, members in archives.items():
            path = self.cache / (archive_name + ".zip." + "c" * 40)
            with ZipFile(path, "w") as archive:
                for member, data in members.items():
                    archive.writestr(member, data)
            pins[archive_name] = hashlib.sha256(path.read_bytes()).hexdigest()
            entries.append(f'"file" "{path.name}" "size" "{path.stat().st_size}" "sha2" "{pins[archive_name]}"')
        self.manifest.write_text("\n".join(entries))
        release = stage.__globals__["RELEASE"]
        pin = json.loads(release.read_text())
        pin["manifest_sha256"] = hashlib.sha256(self.manifest.read_bytes()).hexdigest()
        release.write_text(json.dumps(pin))

        sdl_artifact = self.work / "dwarf-sdl"
        sdl_artifact.mkdir()
        sdl = b"SDL fixture"
        license_text = b"SDL license fixture"
        (sdl_artifact / "LICENSE.txt").write_bytes(license_text)

        profile = self.work / "profile"
        profile.mkdir()
        app = "1145360"
        (profile / f"steam-win32-shadow-{app}.txt").write_text("10929685\nHades.exe\n")
        (profile / f"steam-win32-shadow-{app}.xwp").write_bytes(
            struct.pack("<8sQII32s", b"XWPEV1\r\n", 512, 64, 32, bytes(32)) + bytes(96))
        adapter_manifest = f"steam-win32-adapters-{app}.json"
        declaration = {"build_id": "10929685", "required_adapters": [xinput, transactions]}
        manifest_path = profile / adapter_manifest
        trial = [f"{app}={profile}"]
        args = (base, host, self.layout, "1.0.230.1", self.cache, session)
        with patch.object(library_builder, "VERIFIED_BASES", {hashlib.sha256(base.read_bytes()).hexdigest(): "1.0.230.0"}), \
                patch.object(stage_webview, "SDK_FILES", {name: host_pins[name] for name in stage_webview.SDK_FILES}), \
                patch.object(stage_glyphs, "PUBLIC_SHA256", pins["public_all"]), \
                patch.object(bundle_big_picture, "URL_LIST_SHA256", hashlib.sha256(urls).hexdigest()), \
                patch.object(stage_session, "pe_summary", return_value={"clr_requires_x86": "no"}), \
                patch.object(library_builder, "load_sdl_artifact", return_value=sdl) as load_sdl:
            before = set(self.work.iterdir())
            with self.assertRaisesRegex(ValueError, "artifact checksums"):
                library_builder.prepare_layout(*args, "0" * 64)
            self.assertEqual(set(self.work.iterdir()), before)
            checksum = hashlib.sha256(sums).hexdigest()
            manifest_path.write_text(json.dumps({**declaration, "required_adapters": ["XboxWin32Timer.dll"]}))
            with self.assertRaisesRegex(ValueError, r"missing required Win32 adapters: XboxWin32Timer\.dll"):
                library_builder.prepare_layout(*args, checksum, win32_trial_profiles=trial)
            self.assertEqual(set(self.work.iterdir()), before)
            manifest_path.write_text(json.dumps({**declaration, "build_id": "1"}))
            with self.assertRaisesRegex(ValueError, "must match build 10929685"):
                library_builder.prepare_layout(*args, checksum, win32_trial_profiles=trial)
            self.assertEqual(set(self.work.iterdir()), before)
            manifest_path.write_text(json.dumps({**declaration, "required_adapters": ["ProbeNative.dll"]}))
            with self.assertRaisesRegex(ValueError, "invalid Win32 adapter declaration"):
                library_builder.prepare_layout(*args, checksum, win32_trial_profiles=trial)
            self.assertEqual(set(self.work.iterdir()), before)
            manifest_path.write_text(json.dumps(declaration))
            library_builder.prepare_layout(*args, checksum, library_inventory=True, win32_trial_profiles=trial,
                                           dwarf_sdl=(sdl_artifact, hashlib.sha256(sdl).hexdigest()),
                                           surfaceless_profiles=["1145360=10929685"])

        load_sdl.assert_called_once_with(sdl_artifact, 975370)
        self.assertEqual((self.layout / "steam-cloud.js").read_bytes(), (library_builder.SIGNIN / "steam-cloud.js").read_bytes())
        self.assertEqual((self.layout / "DwarfRuntime/SDL2.dll").read_bytes(), sdl)
        self.assertEqual((self.layout / "DwarfRuntime/LICENSE.SDL2").read_bytes(), license_text)
        result = ElementTree.parse(self.layout / "AppxManifest.xml")
        identity = result.find("{*}Identity")
        self.assertEqual(identity.get("Name"), "XboxPortabilityProbe.ChildProcess.SteamCore")
        self.assertEqual(identity.get("Version"), "1.0.230.1")
        self.assertEqual(result.findtext("{*}Properties/{*}DisplayName"), "GabeBox")
        self.assertEqual(result.findtext("{*}Properties/{*}PublisherDisplayName"), "Xbox Portability Probe")
        tile = next(item for item in result.iter() if item.tag.endswith("}VisualElements"))
        self.assertEqual((tile.get("DisplayName"), tile.get("BackgroundColor")), ("GabeBox", "#0B0B0B"))
        for name in library_builder.BRAND_FILES:
            self.assertEqual((self.layout / "Assets" / name).read_bytes(),
                             (ROOT / "ui/big-picture/Assets" / name).read_bytes())
        classes = {item.get("ActivatableClassId") for item in result.iter() if item.tag.endswith("}ActivatableClass")}
        self.assertTrue({"XboxProbe.Native.ProbeRunner", "BigPictureView.Native.Renderer",
                         "Microsoft.Web.WebView2.Core.CoreWebView2Environment"} <= classes)
        self.assertEqual((self.layout / "XboxProbe.Native.winmd").read_bytes(), metadata)
        self.assertEqual((self.layout / "ProbeNative.dll").read_bytes(), b"MZsession")
        self.assertEqual((self.layout / xinput).read_bytes(), b"session artifact adapter")
        self.assertEqual((self.layout / transactions).read_bytes(), b"session artifact adapter")
        self.assertEqual((self.layout / "StardewStartupDiagnostic.dll").read_bytes(), startup_hook)
        self.assertEqual(json.loads((self.layout / adapter_manifest).read_bytes()), declaration)
        self.assertEqual((self.layout / "steam-sdl-surfaceless-1145360.txt").read_text(), "10929685")
        self.assertTrue((self.layout / "steam-library-inventory.txt").is_file())
        self.assertFalse((self.layout / "AppxBlockMap.xml").exists())
        page = (self.layout / "steam-signin.html").read_text()
        self.assertIn('data-steam-big-picture="true"', page)
        self.assertIn('data-apps="975370 1145360" data-pointer-apps="975370"', page)
        self.assertIn("xboxLocalCloudCacheReads = true",
                      (self.layout / "ValveClient/steamui/xbox/bridge.js").read_text())
        self.assertTrue((self.layout / "ValveClient/steamui/steaminputglyphs/shared_button_a.svg").is_file())
        self.assertEqual((self.layout / "xcom-runtime/xcom-dos.js").read_bytes(),
                         (ROOT / "runtime/dos/xcom-dos.js").read_bytes())

    def test_branding_keeps_identity_and_rejects_unexpected_manifests(self):
        uap = b' xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"'
        manifest = (b'<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"' + uap + b'>'
                    b'<Identity Name="XboxPortabilityProbe.ChildProcess.SteamCore" Version="1.0.232.61"/>'
                    + PROBE_PROPERTIES + b'<Applications><Application Id="App">' + PROBE_TILE
                    + b'</Application></Applications></Package>')
        self.layout.mkdir()
        (self.layout / "Assets").mkdir()
        (self.layout / "AppxManifest.xml").write_bytes(manifest)
        for name in library_builder.BRAND_FILES[:-1]:
            (self.layout / "Assets" / name).write_bytes(b"probe tile")
        with self.assertRaisesRegex(ValueError, "StoreLogo.png"):
            library_builder.stage_branding(self.layout)
        self.assertEqual((self.layout / "AppxManifest.xml").read_bytes(), manifest)
        (self.layout / "Assets/StoreLogo.png").write_bytes(b"probe tile")
        library_builder.stage_branding(self.layout)
        branded = (self.layout / "AppxManifest.xml").read_bytes()
        self.assertEqual(branded, manifest.replace(b"Steam Core Loader Probe", b"GabeBox")
                         .replace(b"Measures launching one fixed packaged child process", b"Steam Big Picture on Xbox")
                         .replace(b"#202020", b"#0B0B0B"))
        for broken in (manifest.replace(PROBE_PROPERTIES, b""), manifest.replace(b' Description="', b' Summary="')):
            (self.layout / "AppxManifest.xml").write_bytes(broken)
            with self.subTest(broken=broken), self.assertRaisesRegex(ValueError, "one package display name"):
                library_builder.stage_branding(self.layout)

    def test_default_library_bridge_preserves_partial_layouts_and_enables_cache_reads(self):
        stage_bridge = library_builder.stage_bridge
        for has_movie in (False, True):
            with self.subTest(has_movie=has_movie):
                layout = self.work / f"library-{has_movie}"
                steamui = layout / "ValveClient/steamui"
                (steamui / "movies").mkdir(parents=True)
                if has_movie:
                    (steamui / "movies/bigpicture_startup.webm").touch()
                bundle = Mock(return_value="signed bridge")
                with patch.object(library_builder, "bundle", bundle):
                    stage_bridge(layout)
                bundle.assert_called_once_with(layout / "ValveClient/public/url_list.txt",
                    movie_probe_assets=steamui if has_movie else None, local_cloud_cache_reads=True)
                self.assertEqual((steamui / "xbox/bridge.js").read_text(), "signed bridge")

    def test_surfaceless_profile_preserves_existing_presenters_and_pins_steam_build(self):
        self.layout.mkdir()
        for dll in ("SDL2.dll", "opengl32.dll", "libEGL.dll", "dxil.dll"):
            (self.layout / dll).touch()
        page = self.layout / "steam-signin.html"
        page.write_text('<script id="steam-sdl-profiles" data-apps="975370 413150 646570" src="steam-sdl-host.js"></script>')
        stage_profiles = library_builder.stage_surfaceless_profiles
        stage_profiles(self.layout, ["1145360=10929685"])
        self.assertIn('data-apps="975370 413150 646570 1145360"', page.read_text())
        self.assertEqual((self.layout / "steam-sdl-surfaceless-1145360.txt").read_text(), "10929685")
        with self.assertRaisesRegex(ValueError, "cannot replace"):
            stage_profiles(self.layout, ["413150=123"])

    def test_win32_legacy_steam_sdk_requires_a_matching_unmodified_staged_companion(self):
        self.layout.mkdir()
        profile = self.work / "legacy-sdk-profile"
        profile.mkdir()
        app = "1817070"
        build = "23986256"
        (profile / f"steam-win32-shadow-{app}.txt").write_text(f"{build}\nGame.exe\n\n\n\n\nloader-trace\n")
        (profile / f"steam-win32-shadow-{app}.xwp").write_bytes(
            struct.pack("<8sQQII32s", b"XWPEV2\r\n", 512, 256, 64, 32, bytes(32)) + bytes(96))
        (profile / f"steam-win32-source-{app}.json").write_text(json.dumps({build: [
            {"path": "Game.exe", "size": 512, "sha256": "1" * 64}]}))
        marker = f"steam-win32-steam-legacy-sdk-{app}.txt"
        digest = "2" * 64
        (profile / marker).write_text(digest + "\n")
        companions = profile / f"steam-win32-files-{app}.txt"
        companions.write_text("steam_api64.dll\n")
        (profile / "steam_api64.dll").write_bytes(b"owned fixture must stay local")
        template = self.work / "legacy-sdk-runtime.appx"
        feature = "XBOX_STEAM_HANDOFF_LEGACY\0".encode("utf-16le")
        with ZipFile(template, "w") as archive:
            archive.writestr("AppxManifest.xml", MANIFEST.replace(
                b'Name="XboxPortabilityProbe.BigPicture"', b'Name="XboxPortabilityProbe.ChildProcess.SteamCore"'))
            archive.writestr("ProbeNative.dll", feature + "steam_source_check=steam_shadow_companion_hashes=verified".encode("utf-16le"))
            archive.writestr("XboxWin32SteamDiscovery.dll", feature)
            for name in ("XboxProbe.Native.winmd", "XboxWin32Input.dll", "XboxWin32Desktop.dll", "XboxWin32LoaderTrace.dll"):
                archive.writestr(name, b"runtime fixture")
        with self.assertRaisesRegex(ValueError, "matching hash-pinned companion"):
            stage_trials(self.layout, [f"{app}={profile}"], template)
        companions.write_text(f"steam_api64.dll\tsha256:{digest}\n")
        stage_trials(self.layout, [f"{app}={profile}"], template)
        self.assertEqual((self.layout / marker).read_text(), digest)
        self.assertEqual((self.layout / "XboxWin32SteamDiscovery.dll").read_bytes(), feature)
        self.assertFalse((self.layout / "steam_api64.dll").exists())

        adapter = "XboxWin32SteamDiscovery.dll"
        required = ["SteamAPI_RestartAppIfNecessary", "SteamInternal_SteamAPI_Init"]
        declaration = profile / f"steam-win32-adapters-{app}.json"
        declaration.write_text(json.dumps({"build_id": build, "required_adapters": [],
                                           "required_exports": {adapter: required}}))
        with self.assertRaisesRegex(ValueError, "Invalid Win32 adapter export requirements"):
            stage_trials(self.layout, [f"{app}={profile}"], template)
        declaration.write_text(json.dumps({"build_id": build, "required_adapters": [adapter],
                                           "required_exports": {adapter: required}}))

        def adapter_image(names):
            image = bytearray(0x600)
            image[:2], image[0x80:0x84] = b"MZ", b"PE\0\0"
            struct.pack_into("<I", image, 0x3c, 0x80)
            struct.pack_into("<HH", image, 0x84, 0x8664, 1)
            struct.pack_into("<HH", image, 0x94, 240, 0x2000)
            optional = 0x98
            struct.pack_into("<H", image, optional, 0x20b)
            struct.pack_into("<II", image, optional + 56, 0x2000, 0x200)
            struct.pack_into("<III", image, optional + 108, 16, 0x1000, 0x200)
            struct.pack_into("<8sIIIIIIHHI", image, optional + 240, b".rdata", 0x400, 0x1000,
                             0x400, 0x200, 0, 0, 0, 0, 0x40000040)
            struct.pack_into("<5I", image, 0x214, len(names), len(names), 0x1040, 0x1050, 0x1060)
            for index, name in enumerate(names):
                offset = 0x280 + 64 * index
                symbol = name.encode("ascii") + b"\0"
                image[offset:offset + len(symbol)] = symbol
                struct.pack_into("<I", image, 0x240 + 4 * index, 0x1300 + 16 * index)
                struct.pack_into("<I", image, 0x250 + 4 * index, 0x1000 + offset - 0x200)
                struct.pack_into("<H", image, 0x260 + 2 * index, index)
            image[0x400:0x400 + len(feature)] = feature
            return image

        def package_adapter(image):
            runtime = self.work / "sdk-adapter.appx"
            with ZipFile(template) as baseline, ZipFile(runtime, "w") as package:
                for member in baseline.namelist():
                    package.writestr(member, image if member == adapter else baseline.read(member))
            return runtime

        stale = package_adapter(adapter_image(required[:1]))
        with self.assertRaisesRegex(ValueError, "missing required Win32 exports: SteamInternal_SteamAPI_Init"):
            stage_trials(self.layout, [f"{app}={profile}"], stale)

        valid = adapter_image(required)
        stage_trials(self.layout, [f"{app}={profile}"], package_adapter(valid))
        self.assertEqual((self.layout / adapter).read_bytes(), valid)

        for offset, value, error in (
            (0x10c, 0x500, "outside the file-backed sections"),
            (0x244, 0x4000, "invalid pe named export target"),
            (0x244, 0x11ff, "invalid pe export forwarder"),
            (0x254, 0x1080, "invalid pe export name ordering"),
        ):
            with self.subTest(error=error):
                malformed = bytearray(valid)
                struct.pack_into("<I", malformed, offset, value)
                with self.assertRaisesRegex(ValueError, "(?i)" + error):
                    stage_trials(self.layout, [f"{app}={profile}"], package_adapter(malformed))
                self.assertEqual((self.layout / adapter).read_bytes(), valid)

        forwarded = bytearray(valid)
        struct.pack_into("<I", forwarded, 0x244, 0x11c0)
        forwarder = b"AnotherAdapter.#5\0"
        forwarded[0x3c0:0x3c0 + len(forwarder)] = forwarder
        stage_trials(self.layout, [f"{app}={profile}"], package_adapter(forwarded))
        self.assertEqual((self.layout / adapter).read_bytes(), forwarded)

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
            struct.pack("<8sQQII32s", b"XWPEV2\r\n", 512, 256, 64, 32, bytes(32)) + bytes(96))
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
                         "XboxWin32Window.dll", "XboxWin32SteamDiscovery.dll", "XboxWin32CrashReport.dll",
                         "XboxWin32Dxgi.dll", "XboxWin32StorageTrace.dll", "XboxWin32Unity.dll", "XboxWin32Timer.dll",
                         "XboxWin32DisplayConfig.dll"):
                archive.writestr(name, b"runtime fixture")
        stage_trials(self.layout, [f"1145360={profile}"], template)
        self.assertEqual({path.name for path in self.layout.iterdir()},
                         {"XboxProbe.Native.winmd", "XboxWin32Input.dll",
                          "XboxWin32Desktop.dll", "XboxWin32LoaderTrace.dll", "XboxWin32Timer.dll",
                          marker, recipe, extra, companions})
        self.assertEqual((self.layout / recipe).read_bytes(), (profile / recipe).read_bytes())
        self.assertEqual((self.layout / "XboxProbe.Native.winmd").read_bytes(),
                         b"older metadata from the base release")
        with self.assertRaisesRegex(ValueError, "one APPID"):
            stage_trials(self.layout, [f"1145360={profile}"] * 2, template)
        with self.assertRaisesRegex(ValueError, "missing required Win32 adapters"):
            stage_trials(self.layout, [f"1145360={profile}"], self.template)

        documents_marker = "steam-win32-documents-1145360.txt"
        documents = profile / documents_marker
        documents.write_text("C:\\Users\\Someone\\Documents")
        with self.assertRaisesRegex(ValueError, "empty Documents marker"):
            stage_trials(self.layout, [f"1145360={profile}"], template)
        documents.write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "artifact with profile Documents support"):
            stage_trials(self.layout, [f"1145360={profile}"], template)
        current_template = self.work / "documents-runtime.appx"
        with ZipFile(template) as original, ZipFile(current_template, "w") as updated:
            for name in original.namelist():
                data = original.read(name)
                if name in ("ProbeNative.dll", "XboxWin32Desktop.dll"):
                    data += "XBOX_WIN32_DOCUMENTS\0".encode("utf-16le")
                updated.writestr(name, data)
        stage_trials(self.layout, [f"1145360={profile}"], current_template)
        self.assertEqual((self.layout / documents_marker).read_bytes(), b"")
        documents.unlink()

        (profile / marker).write_text("10929685\nGame.exe\nopengl3\ngodot\n")
        stage_trials(self.layout, [f"1145360={profile}"], template)
        self.assertEqual((self.layout / "XboxWin32Window.dll").read_bytes(), b"runtime fixture")
        adapted = "10929685\nGame.exe\nopengl3\ngodot\n\n\n\naccessibility=disabled\n"
        (profile / marker).write_text(adapted)
        with self.assertRaisesRegex(ValueError, "matching hosted OpenGL profile"):
            stage_trials(self.layout, [f"1145360={profile}"], template)
        (self.layout / "steam-sdl-surfaceless-1145360.txt").write_text("10929685")
        stage_trials(self.layout, [f"1145360={profile}"], template)
        self.assertEqual((self.layout / marker).read_text(), adapted)
        storage_marker = "steam-win32-storage-trace-1145360.txt"
        (profile / storage_marker).touch()
        with self.assertRaisesRegex(ValueError, "requires an empty file and a loader trace"):
            stage_trials(self.layout, [f"1145360={profile}"], template)
        with_storage = adapted.replace("\n\n\naccessibility=", "\n\nloader-trace\naccessibility=")
        (profile / marker).write_text(with_storage)
        stage_trials(self.layout, [f"1145360={profile}"], template)
        self.assertEqual((self.layout / marker).read_text(), with_storage)
        self.assertEqual((self.layout / storage_marker).read_bytes(), b"")
        self.assertEqual((self.layout / "XboxWin32StorageTrace.dll").read_bytes(), b"runtime fixture")
        (profile / storage_marker).unlink()
        shader_cache = "steam-win32-godot-shader-cache-1145360.txt"
        for option in ("control", "disabled"):
            (profile / shader_cache).write_text(option + "\n")
            stage_trials(self.layout, [f"1145360={profile}"], template)
            self.assertEqual((self.layout / shader_cache).read_text(), option)
        (profile / shader_cache).write_text("disabled\nother-setting")
        with self.assertRaisesRegex(ValueError, "shader cache trial"):
            stage_trials(self.layout, [f"1145360={profile}"], template)
        (profile / shader_cache).unlink()
        (profile / marker).write_text(adapted)
        sdk_marker = "steam-win32-steam-sdk-1145360.txt"
        (profile / sdk_marker).write_text("data/steam_api64.dll\n")
        with self.assertRaisesRegex(ValueError, "unchanged companion"):
            stage_trials(self.layout, [f"1145360={profile}"], template)
        (profile / companions).write_text("Hades.exe\tlauncher\ndata/steam_api64.dll\n")
        crash_marker = "steam-win32-crash-report-1145360.txt"
        (profile / crash_marker).touch()
        stage_trials(self.layout, [f"1145360={profile}"], template)
        self.assertEqual((self.layout / sdk_marker).read_text(), "data/steam_api64.dll")
        self.assertEqual((self.layout / "XboxWin32SteamDiscovery.dll").read_bytes(), b"runtime fixture")
        self.assertEqual((self.layout / "XboxWin32CrashReport.dll").read_bytes(), b"runtime fixture")
        dxgi_marker = "steam-win32-dxgi-1145360.txt"
        (profile / dxgi_marker).touch()
        with self.assertRaisesRegex(ValueError, "default native renderer"):
            stage_trials(self.layout, [f"1145360={profile}"], template)
        (profile / sdk_marker).unlink()
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

        (profile / dxgi_marker).unlink()
        (profile / marker).write_text("10929685\nGame.exe\nopengl3\nunity\n\n\nloader-trace\n")
        with self.assertRaisesRegex(ValueError, "UnityPlayer.dll must appear once as a companion"):
            stage_trials(self.layout, [f"1145360={profile}"], template)
        (profile / companions).write_text("UnityPlayer.dll\n")
        (profile / "UnityPlayer.dll").write_bytes(b"signed engine fixture")
        stage_trials(self.layout, [f"1145360={profile}"], template)
        self.assertEqual((self.layout / "XboxWin32Unity.dll").read_bytes(), b"runtime fixture")
        self.assertEqual((self.layout / "XboxWin32DisplayConfig.dll").read_bytes(), b"runtime fixture")
        self.assertFalse((self.layout / "UnityPlayer.dll").exists())
        engine_recipe = "steam-win32-shadow-1145360-engine.xwp"
        (profile / companions).write_text("UnityPlayer.dll\tengine\n")
        (profile / engine_recipe).write_bytes((profile / recipe).read_bytes())
        stage_trials(self.layout, [f"1145360={profile}"], template)
        self.assertEqual((self.layout / engine_recipe).read_bytes(), (profile / engine_recipe).read_bytes())
        self.assertFalse((self.layout / "UnityPlayer.dll").exists())
        dynamic_companions = "steam-win32-dynamic-companions-1145360.txt"
        (profile / dynamic_companions).touch()
        (profile / original_executable_marker).unlink()
        with self.assertRaisesRegex(ValueError, "Dynamic companions require the original executable path"):
            stage_trials(self.layout, [f"1145360={profile}"], template)
        (profile / original_executable_marker).touch()
        nested_runtime = "MonoBleedingEdge/EmbedRuntime/mono-2.0-bdwgc.dll"
        nested_recipe = "steam-win32-shadow-1145360-mono.xwp"
        (profile / companions).write_text(f"UnityPlayer.dll\tengine\n{nested_runtime}\tmono\n")
        (profile / nested_recipe).write_bytes((profile / recipe).read_bytes())
        stage_trials(self.layout, [f"1145360={profile}"], template)
        self.assertEqual((self.layout / dynamic_companions).read_bytes(), b"")
        self.assertEqual((self.layout / nested_recipe).read_bytes(), (profile / nested_recipe).read_bytes())
        self.assertFalse((self.layout / nested_runtime).exists())

    def test_win32_source_profile_requires_verifier_and_stages_only_fingerprints(self):
        self.layout.mkdir()
        profile = ROOT / "profiles/1817070/source.json"
        template = self.work / "runtime.appx"
        entry = f"1817070={profile}"
        for native in (b"old", "steam_source_check=".encode("utf-16le")):
            with ZipFile(template, "w") as archive:
                archive.writestr("AppxManifest.xml", MANIFEST.replace(
                    b'Name="XboxPortabilityProbe.BigPicture"', b'Name="XboxPortabilityProbe.ChildProcess.SteamCore"'))
                archive.writestr("XboxProbe.Native.winmd", b"metadata")
                archive.writestr("ProbeNative.dll", native)
            if native == b"old":
                with self.assertRaisesRegex(ValueError, "source-profile verification"):
                    stage_trials(self.layout, [], template, [entry])
                self.assertEqual(list(self.layout.iterdir()), [])
            else:
                stage_trials(self.layout, [], template, [entry])
        staged_name = "steam-win32-source-1817070.json"
        self.assertEqual({path.name for path in self.layout.iterdir()},
                         {staged_name})
        self.assertEqual(json.loads((self.layout / staged_name).read_bytes()), json.loads(profile.read_bytes()))
        with self.assertRaisesRegex(ValueError, "one APPID"):
            stage_trials(self.layout, [], template, [entry, entry])

    def test_win32_trial_packages_embedded_source_and_companion_fingerprints(self):
        self.layout.mkdir()
        profile = self.work / "profile"
        profile.mkdir()
        source = ROOT / "profiles/1817070/source.json"
        (profile / "steam-win32-source-1817070.json").write_bytes(source.read_bytes())
        marker = profile / "steam-win32-shadow-1817070.txt"
        marker.write_text("14752622\nSpider-Man.exe\n")
        (profile / "steam-win32-shadow-1817070.xwp").write_bytes(
            struct.pack("<8sQII32s", b"XWPEV1\r\n", 512, 64, 32, bytes(32)) + bytes(96))
        files = profile / "steam-win32-files-1817070.txt"
        pin = "D3D12/D3D12Core.dll\tsha256:" + "a" * 64 + "\n"
        files.write_text(pin)
        template = self.work / "runtime.appx"
        base = "steam_source_check=".encode("utf-16le")
        for binary in (base, base + "steam_shadow_companion_hashes=verified".encode("utf-16le")):
            with ZipFile(template, "w") as archive:
                archive.writestr("AppxManifest.xml", MANIFEST.replace(
                    b'Name="XboxPortabilityProbe.BigPicture"', b'Name="XboxPortabilityProbe.ChildProcess.SteamCore"'))
                for name in ("XboxProbe.Native.winmd", "XboxWin32Input.dll", "XboxWin32Desktop.dll"):
                    archive.writestr(name, b"runtime fixture")
                archive.writestr("ProbeNative.dll", binary)
            if binary == base:
                with self.assertRaisesRegex(ValueError, "companion fingerprint verification"):
                    stage_trials(self.layout, [f"1817070={profile}"], template)
                self.assertEqual(list(self.layout.iterdir()), [])
            else:
                stage_trials(self.layout, [f"1817070={profile}"], template)
        self.assertEqual((self.layout / files.name).read_text(), pin)
        self.assertFalse((self.layout / "D3D12").exists())
        self.assertEqual(json.loads((self.layout / "steam-win32-source-1817070.json").read_text()),
                         json.loads(source.read_text()))
        release = profile / "steam-win32-release-renderer-1817070.txt"
        release.touch()
        with self.assertRaisesRegex(ValueError, "matching hosted build and source fingerprints"):
            stage_trials(self.layout, [f"1817070={profile}"], template)
        (self.layout / "steam-sdl-surfaceless-1817070.txt").write_text("14752622")
        stage_trials(self.layout, [f"1817070={profile}"], template)
        self.assertEqual((self.layout / release.name).read_bytes(), b"")
        files.write_text(pin + pin.lower())
        with self.assertRaisesRegex(ValueError, "duplicate companion path"):
            stage_trials(self.layout, [f"1817070={profile}"], template)
        marker.write_text("3\nSpider-Man.exe\n")
        with self.assertRaisesRegex(ValueError, "does not include the trial build"):
            stage_trials(self.layout, [f"1817070={profile}"], template)

    def test_hosted_xinput_profile_requires_matching_build_and_runtime(self):
        self.layout.mkdir()
        profile = self.work / "profile"
        profile.mkdir()
        app, build = "427520", "42"
        (profile / f"steam-win32-shadow-{app}.txt").write_text(f"{build}\nGame.exe\n\n\n\n\nloader-trace\n")
        (profile / f"steam-win32-shadow-{app}.xwp").write_bytes(
            struct.pack("<8sQII32s", b"XWPEV1\r\n", 512, 64, 32, bytes(32)) + bytes(96))
        marker = f"steam-win32-hosted-xinput-{app}.txt"
        (profile / marker).touch()
        (profile / f"steam-win32-source-{app}.json").write_text(json.dumps({
            build: [{"path": "Game.exe", "size": 512, "sha256": "a" * 64}]}))
        declaration = profile / f"steam-win32-adapters-{app}.json"
        declaration.write_text(json.dumps({"build_id": build, "required_adapters": ["XboxWin32XInput910.dll"]}))
        hosted = self.layout / f"steam-sdl-surfaceless-{app}.txt"
        hosted.write_text("41")
        runtime = self.work / "xinput-runtime.appx"
        variable = "XBOX_WIN32_HOSTED_XINPUT\0".encode("utf-16le")
        trial = [f"{app}={profile}"]
        for loader in (b"older loader", variable):
            with ZipFile(runtime, "w") as package:
                for name in ("XboxWin32Input.dll", "XboxWin32Desktop.dll", "XboxWin32XInput910.dll"):
                    package.writestr(name, b"runtime fixture")
                package.writestr("ProbeNative.dll", "steam_source_check=".encode("utf-16le") + variable)
                package.writestr("XboxWin32LoaderTrace.dll", loader)
            if loader != variable:
                with self.assertRaisesRegex(ValueError, "matching hosted build"):
                    stage_trials(self.layout, trial, runtime)
                hosted.write_text(build)
                with self.assertRaisesRegex(ValueError, "launch-scoped XInput support"):
                    stage_trials(self.layout, trial, runtime)
                self.assertFalse((self.layout / marker).exists())
            else:
                stage_trials(self.layout, trial, runtime)
        self.assertEqual((self.layout / marker).read_bytes(), b"")
        self.assertEqual((self.layout / "XboxWin32XInput910.dll").read_bytes(), b"runtime fixture")

        notify = f"steam-win32-hosted-controller-notify-{app}.txt"
        (profile / notify).touch()
        with self.assertRaisesRegex(ValueError, "a declared notification adapter"):
            stage_trials(self.layout, trial, runtime)
        declaration.write_text(json.dumps({"build_id": build, "required_adapters": [
            "XboxWin32XInput910.dll", "XboxWin32ControllerNotify.dll"]}))
        with self.assertRaisesRegex(ValueError, "missing required Win32 adapters: XboxWin32ControllerNotify.dll"):
            stage_trials(self.layout, trial, runtime)
        notify_variable = "XBOX_WIN32_HOSTED_CONTROLLER_NOTIFY\0".encode("utf-16le")
        notify_runtime = self.work / "controller-notification-runtime.appx"
        with ZipFile(runtime) as source, ZipFile(notify_runtime, "w") as target:
            for name in source.namelist():
                data = source.read(name)
                if name in ("ProbeNative.dll", "XboxWin32LoaderTrace.dll"):
                    data += notify_variable
                target.writestr(name, data)
            target.writestr("XboxWin32ControllerNotify.dll", b"notification fixture")
        (profile / marker).unlink()
        with self.assertRaisesRegex(ValueError, "need hosted XInput"):
            stage_trials(self.layout, trial, notify_runtime)
        (profile / marker).touch()
        stage_trials(self.layout, trial, notify_runtime)
        self.assertEqual((self.layout / notify).read_bytes(), b"")

        directinput = f"steam-win32-disable-sdl-directinput-{app}.txt"
        (profile / directinput).write_text("unexpected")
        with self.assertRaisesRegex(ValueError, "an empty marker and hosted XInput"):
            stage_trials(self.layout, trial, notify_runtime)
        (profile / directinput).write_bytes(b"")
        (profile / marker).unlink()
        with self.assertRaisesRegex(ValueError, "an empty marker and hosted XInput"):
            stage_trials(self.layout, trial, notify_runtime)
        (profile / marker).touch()
        with self.assertRaisesRegex(ValueError, "launch-scoped SDL DirectInput selection"):
            stage_trials(self.layout, trial, notify_runtime)
        directinput_runtime = self.work / "sdl-directinput-runtime.appx"
        with ZipFile(notify_runtime) as source, ZipFile(directinput_runtime, "w") as target:
            for name in source.namelist():
                data = source.read(name)
                if name == "ProbeNative.dll":
                    data += "SDL_JOYSTICK_DIRECTINPUT\0".encode("utf-16le")
                target.writestr(name, data)
        stage_trials(self.layout, trial, directinput_runtime)
        self.assertEqual((self.layout / directinput).read_bytes(), b"")

    def test_default_audio_profile_requires_verified_launch_components(self):
        self.layout.mkdir()
        profile = self.work / "profile"
        profile.mkdir()
        app, build = "427520", "42"
        (profile / f"steam-win32-shadow-{app}.txt").write_text(f"{build}\nGame.exe\n\n\n\n\nloader-trace\n")
        (profile / f"steam-win32-shadow-{app}.xwp").write_bytes(
            struct.pack("<8sQII32s", b"XWPEV1\r\n", 512, 64, 32, bytes(32)) + bytes(96))
        (profile / f"steam-win32-source-{app}.json").write_text(json.dumps({
            build: [{"path": "Game.exe", "size": 512, "sha256": "a" * 64}]}))
        marker = f"steam-win32-default-audio-{app}.txt"
        (profile / marker).touch()
        declaration = profile / f"steam-win32-adapters-{app}.json"
        audio = "XboxWin32DefaultAudio.dll"
        runtime = self.work / "audio-runtime.appx"
        variable = "XBOX_WIN32_DEFAULT_AUDIO\0".encode("utf-16le")
        trial = [f"{app}={profile}"]
        with ZipFile(runtime, "w"):
            pass
        with self.assertRaisesRegex(ValueError, "the declared audio adapter"):
            stage_trials(self.layout, trial, runtime)
        declaration.write_text(json.dumps({"build_id": build, "required_adapters": [audio]}))
        for loader in (b"older loader", variable):
            with ZipFile(runtime, "w") as package:
                for name in ("XboxWin32Input.dll", "XboxWin32Desktop.dll", audio):
                    package.writestr(name, b"runtime fixture")
                package.writestr("ProbeNative.dll", "steam_source_check=".encode("utf-16le") + variable)
                package.writestr("XboxWin32LoaderTrace.dll", loader)
            if loader != variable:
                with self.assertRaisesRegex(ValueError, "launch-scoped default audio support"):
                    stage_trials(self.layout, trial, runtime)
                self.assertFalse((self.layout / marker).exists())
            else:
                stage_trials(self.layout, trial, runtime)
        self.assertEqual((self.layout / marker).read_bytes(), b"")
        self.assertEqual((self.layout / audio).read_bytes(), b"runtime fixture")
        (profile / marker).write_text("1")
        with self.assertRaisesRegex(ValueError, "an empty marker"):
            stage_trials(self.layout, trial, runtime)

    def test_hosted_dxgi_output_requires_matching_profile_and_runtime(self):
        self.layout.mkdir()
        profile = self.work / "profile"
        profile.mkdir()
        app, build = "427520", "42"
        (profile / f"steam-win32-shadow-{app}.txt").write_text(f"{build}\nGame.exe\n\n\n\n\nloader-trace\n")
        (profile / f"steam-win32-shadow-{app}.xwp").write_bytes(
            struct.pack("<8sQII32s", b"XWPEV1\r\n", 512, 64, 32, bytes(32)) + bytes(96))
        (profile / f"steam-win32-source-{app}.json").write_text(json.dumps({
            build: [{"path": "Game.exe", "size": 512, "sha256": "a" * 64}]}))
        window, dxgi = "XboxWin32Window.dll", "XboxWin32Dxgi.dll"
        (profile / f"steam-win32-adapters-{app}.json").write_text(json.dumps({
            "build_id": build, "required_adapters": [window, dxgi]}))
        marker = f"steam-win32-dxgi-output-{app}.txt"
        (profile / marker).touch()
        hosted = self.layout / f"steam-sdl-surfaceless-{app}.txt"
        hosted.write_text("41")
        runtime = self.work / "dxgi-runtime.appx"
        variable = "XBOX_WIN32_HOSTED_DXGI_OUTPUT\0".encode("utf-16le")
        trial = [f"{app}={profile}"]
        for adapter in (b"older dxgi", variable):
            with ZipFile(runtime, "w") as package:
                for name in ("XboxWin32Input.dll", "XboxWin32Desktop.dll", "XboxWin32LoaderTrace.dll", window):
                    package.writestr(name, b"runtime fixture")
                package.writestr("ProbeNative.dll", "steam_source_check=".encode("utf-16le") + variable)
                package.writestr(dxgi, adapter)
            if adapter != variable:
                with self.assertRaisesRegex(ValueError, "matching hosted build"):
                    stage_trials(self.layout, trial, runtime)
                (profile / f"steam-win32-dxgi-{app}.txt").touch()
                with self.assertRaisesRegex(ValueError, "matching hosted build"):
                    stage_trials(self.layout, trial, runtime)
                hosted.write_text(build)
                with self.assertRaisesRegex(ValueError, "launch-scoped DXGI output support"):
                    stage_trials(self.layout, trial, runtime)
                self.assertFalse((self.layout / marker).exists())
            else:
                stage_trials(self.layout, trial, runtime)
        self.assertEqual((self.layout / marker).read_bytes(), b"")
        (profile / marker).write_text("1")
        with self.assertRaisesRegex(ValueError, "matching hosted build"):
            stage_trials(self.layout, trial, runtime)

    def test_dxgi_adapter_budget_requires_dxgi_profile_and_runtime(self):
        self.layout.mkdir()
        profile = self.work / "profile"
        profile.mkdir()
        app, build = "427520", "42"
        (profile / f"steam-win32-shadow-{app}.txt").write_text(f"{build}\nGame.exe\n")
        (profile / f"steam-win32-shadow-{app}.xwp").write_bytes(
            struct.pack("<8sQII32s", b"XWPEV1\r\n", 512, 64, 32, bytes(32)) + bytes(96))
        dxgi = "XboxWin32Dxgi.dll"
        (profile / f"steam-win32-adapters-{app}.json").write_text(json.dumps({
            "build_id": build, "required_adapters": [dxgi]}))
        marker = f"steam-win32-dxgi-adapter-budget-{app}.txt"
        (profile / marker).touch()
        runtime = self.work / "dxgi-runtime.appx"
        variable = "XBOX_WIN32_DXGI_ADAPTER_BUDGET\0".encode("utf-16le")
        trial = [f"{app}={profile}"]
        for adapter in (b"older dxgi", variable):
            with ZipFile(runtime, "w") as package:
                for name in ("XboxWin32Input.dll", "XboxWin32Desktop.dll", "XboxWin32LoaderTrace.dll",
                             "XboxWin32Window.dll"):
                    package.writestr(name, b"runtime fixture")
                package.writestr("ProbeNative.dll", "steam_source_check=".encode("utf-16le") + variable)
                package.writestr(dxgi, adapter)
            if adapter != variable:
                with self.assertRaisesRegex(ValueError, "needs the DXGI marker"):
                    stage_trials(self.layout, trial, runtime)
                (profile / f"steam-win32-dxgi-{app}.txt").touch()
                with self.assertRaisesRegex(ValueError, "DXGI adapter budget support"):
                    stage_trials(self.layout, trial, runtime)
                self.assertFalse((self.layout / marker).exists())
            else:
                stage_trials(self.layout, trial, runtime)
        self.assertEqual((self.layout / marker).read_bytes(), b"")
        (profile / marker).write_text("1")
        with self.assertRaisesRegex(ValueError, "must be empty"):
            stage_trials(self.layout, trial, runtime)

    def test_hosted_window_frame_requires_launch_support_and_window_exports(self):
        self.layout.mkdir()
        profile = self.work / "profile"
        profile.mkdir()
        app, build = "427520", "42"
        (profile / f"steam-win32-shadow-{app}.txt").write_text(f"{build}\nGame.exe\n\n\n\n\nloader-trace\n")
        (profile / f"steam-win32-shadow-{app}.xwp").write_bytes(
            struct.pack("<8sQII32s", b"XWPEV1\r\n", 512, 64, 32, bytes(32)) + bytes(96))
        (profile / f"steam-win32-source-{app}.json").write_text(json.dumps({
            build: [{"path": "Game.exe", "size": 512, "sha256": "a" * 64}]}))
        window, loader = "XboxWin32Window.dll", "XboxWin32LoaderTrace.dll"
        (profile / f"steam-win32-adapters-{app}.json").write_text(json.dumps({
            "build_id": build, "required_adapters": [window, loader]}))
        (self.layout / f"steam-sdl-surfaceless-{app}.txt").write_text(build)
        marker = f"steam-win32-window-frame-{app}.txt"
        (profile / marker).touch()
        runtime = self.work / "window-frame-runtime.appx"
        variable = "XBOX_WIN32_HOSTED_WINDOW_FRAME\0".encode("utf-16le")
        trial = [f"{app}={profile}"]
        exports = b"AdjustWindowRectEx\0AdjustWindowRectExForDpi\0GetDpiForWindow\0"
        for window_binary in (variable, variable + exports):
            with ZipFile(runtime, "w") as package:
                package.writestr(window, window_binary)
                package.writestr(loader, variable)
                package.writestr("ProbeNative.dll", "steam_source_check=".encode("utf-16le") + variable)
                for name in ("XboxWin32Input.dll", "XboxWin32Desktop.dll", "XboxWin32Dxgi.dll"):
                    package.writestr(name, b"runtime fixture")
            if window_binary == variable:
                with self.assertRaisesRegex(ValueError, "matching hosted build"):
                    stage_trials(self.layout, trial, runtime)
                (profile / f"steam-win32-dxgi-{app}.txt").touch()
                with self.assertRaisesRegex(ValueError, "launch-scoped window frame support"):
                    stage_trials(self.layout, trial, runtime)
                self.assertFalse((self.layout / marker).exists())
            else:
                stage_trials(self.layout, trial, runtime)
        self.assertEqual((self.layout / marker).read_bytes(), b"")

    def test_publisher_opengl_requires_a_matching_hosted_profile_and_runtime(self):
        self.layout.mkdir()
        profile = self.work / "profile"
        profile.mkdir()
        app, build = "427520", "42"
        (profile / f"steam-win32-shadow-{app}.txt").write_text(f"{build}\nGame.exe\nopengl3\n\n\n\nloader-trace\n")
        (profile / f"steam-win32-shadow-{app}.xwp").write_bytes(
            struct.pack("<8sQII32s", b"XWPEV1\r\n", 512, 64, 32, bytes(32)) + bytes(96))
        (profile / f"steam-win32-source-{app}.json").write_text(json.dumps({
            build: [{"path": "Game.exe", "size": 512, "sha256": "a" * 64}]}))
        window, loader = "XboxWin32Window.dll", "XboxWin32LoaderTrace.dll"
        (profile / f"steam-win32-adapters-{app}.json").write_text(json.dumps({
            "build_id": build, "required_adapters": [window, loader]}))
        trial = [f"{app}={profile}"]
        runtime = self.work / "wgl-runtime.appx"
        with ZipFile(runtime, "w"):
            pass
        with self.assertRaisesRegex(ValueError, "Publisher OpenGL needs a matching hosted build"):
            stage_trials(self.layout, trial, runtime)
        (self.layout / f"steam-sdl-surfaceless-{app}.txt").write_text(build)
        variable = "XBOX_WIN32_HOSTED_WGL\0".encode("utf-16le")
        for window_binary in (b"older window runtime", variable):
            with ZipFile(runtime, "w") as package:
                package.writestr(window, window_binary)
                package.writestr(loader, variable)
                package.writestr("ProbeNative.dll", "steam_source_check=".encode("utf-16le") + variable)
                for adapter in ("XboxWin32Input.dll", "XboxWin32Desktop.dll"):
                    package.writestr(adapter, b"baseline adapter")
            if window_binary != variable:
                with self.assertRaisesRegex(ValueError, "launch-scoped WGL support"):
                    stage_trials(self.layout, trial, runtime)
            else:
                stage_trials(self.layout, trial, runtime)
        self.assertEqual((self.layout / window).read_bytes(), variable)
        (profile / f"steam-win32-dxgi-{app}.txt").touch()
        with self.assertRaisesRegex(ValueError, "default native renderer"):
            stage_trials(self.layout, trial, runtime)

    def test_roaming_appdata_requires_source_pin_and_both_runtime_components(self):
        self.layout.mkdir()
        profile = self.work / "profile"
        profile.mkdir()
        app, build = "427520", "42"
        (profile / f"steam-win32-shadow-{app}.txt").write_text(f"{build}\nGame.exe\n")
        (profile / f"steam-win32-shadow-{app}.xwp").write_bytes(
            struct.pack("<8sQII32s", b"XWPEV1\r\n", 512, 64, 32, bytes(32)) + bytes(96))
        desktop = "XboxWin32Desktop.dll"
        (profile / f"steam-win32-adapters-{app}.json").write_text(
            json.dumps({"build_id": build, "required_adapters": [desktop]}))
        marker = f"steam-win32-roaming-appdata-{app}.txt"
        (profile / marker).touch()
        runtime = self.work / "roaming-runtime.appx"
        trial = [f"{app}={profile}"]
        with ZipFile(runtime, "w"):
            pass
        with self.assertRaisesRegex(ValueError, "a source profile"):
            stage_trials(self.layout, trial, runtime)
        (profile / f"steam-win32-source-{app}.json").write_text(json.dumps({
            build: [{"path": "Game.exe", "size": 512, "sha256": "a" * 64}]}))
        variable = "XBOX_WIN32_ROAMING_APPDATA\0".encode("utf-16le")
        source_check = "steam_source_check=".encode("utf-16le")
        for host_current, adapter_current in ((False, True), (True, False), (True, True)):
            with ZipFile(runtime, "w") as package:
                package.writestr("XboxWin32Input.dll", b"input")
                package.writestr(desktop, variable if adapter_current else b"older adapter")
                package.writestr("ProbeNative.dll", source_check + (variable if host_current else b""))
            if host_current and adapter_current:
                stage_trials(self.layout, trial, runtime)
            else:
                with self.assertRaisesRegex(ValueError, "profile Roaming AppData support"):
                    stage_trials(self.layout, trial, runtime)
                self.assertFalse((self.layout / marker).exists())
        self.assertEqual((self.layout / marker).read_bytes(), b"")
        self.assertEqual((self.layout / desktop).read_bytes(), variable)
        (profile / marker).write_text("unscoped-path")
        with self.assertRaisesRegex(ValueError, "an empty marker"):
            stage_trials(self.layout, trial, runtime)

        seed_name = f"steam-win32-roaming-default-{app}.txt"
        seed = profile / seed_name
        content = b"[input]\ninput-method=GameController\n"
        seed.write_bytes(b"Game/config/config.ini\n" + content)
        (profile / marker).unlink()
        with self.assertRaisesRegex(ValueError, "a matching Roaming profile"):
            stage_trials(self.layout, trial, runtime)
        (profile / marker).touch()
        for path in (b"../config.ini", b"Game/AUX/config.ini", b"Game/config.bin", b"/config.ini"):
            seed.write_bytes(path + b"\n" + content)
            with self.assertRaisesRegex(ValueError, "a relative INI path"):
                stage_trials(self.layout, trial, runtime)
        seed.write_bytes(b"Game/config/config.ini\n" + content)
        with self.assertRaisesRegex(ValueError, "first-run Roaming defaults support"):
            stage_trials(self.layout, trial, runtime)
        self.assertFalse((self.layout / seed_name).exists())
        with ZipFile(runtime, "w") as package:
            package.writestr("XboxWin32Input.dll", b"input")
            package.writestr(desktop, variable)
            package.writestr("ProbeNative.dll", source_check + variable + "steam_shadow_roaming_default=".encode("utf-16le"))
        stage_trials(self.layout, trial, runtime)
        self.assertEqual((self.layout / seed_name).read_bytes(), seed.read_bytes())

        folders_name = f"steam-win32-roaming-folders-{app}.txt"
        folders = profile / folders_name
        for path in (b"../saves", b"Game/AUX", b"/saves", b"Game//saves"):
            folders.write_bytes(path + b"\n")
            with self.assertRaisesRegex(ValueError, "relative paths"):
                stage_trials(self.layout, trial, runtime)
        folders.write_bytes(b"Game/saves\r\nGame/scenarios\r\n")
        with self.assertRaisesRegex(ValueError, "first-run Roaming folder support"):
            stage_trials(self.layout, trial, runtime)
        with ZipFile(runtime) as package:
            native = package.read("ProbeNative.dll")
        with ZipFile(runtime, "w") as package:
            package.writestr("XboxWin32Input.dll", b"input")
            package.writestr(desktop, variable)
            package.writestr("ProbeNative.dll", native + "steam_shadow_roaming_folders=".encode("utf-16le"))
        stage_trials(self.layout, trial, runtime)
        self.assertEqual((self.layout / folders_name).read_bytes(), b"Game/saves\nGame/scenarios\n")

    def test_win32_system_companions_require_a_matching_runtime_and_top_level_dll(self):
        self.layout.mkdir()
        profile = self.work / "profile"
        profile.mkdir()
        (profile / "steam-win32-shadow-1817070.txt").write_text(
            "14752622\nSpider-Man.exe\n\n\n\n\nloader-trace\n")
        (profile / "steam-win32-shadow-1817070.xwp").write_bytes(
            struct.pack("<8sQII32s", b"XWPEV1\r\n", 512, 64, 32, bytes(32)) + bytes(96))
        files = profile / "steam-win32-files-1817070.txt"
        pin = "XAudio2_7.dll\tsystem-sha256:" + "a" * 64 + "\n"
        files.write_text(pin)
        template = self.work / "runtime.appx"
        profiles = [f"1817070={profile}"]
        variable = "XBOX_WIN32_SYSTEM_COMPANIONS\0".encode("utf-16le")
        hash_check = "steam_shadow_companion_hashes=verified".encode("utf-16le")
        for loader in (b"old loader", variable):
            with ZipFile(template, "w") as archive:
                archive.writestr("AppxManifest.xml", MANIFEST.replace(
                    b'Name="XboxPortabilityProbe.BigPicture"', b'Name="XboxPortabilityProbe.ChildProcess.SteamCore"'))
                for name in ("XboxProbe.Native.winmd", "XboxWin32Input.dll", "XboxWin32Desktop.dll"):
                    archive.writestr(name, b"runtime fixture")
                archive.writestr("ProbeNative.dll", hash_check + variable)
                archive.writestr("XboxWin32LoaderTrace.dll", loader)
            if loader != variable:
                with self.assertRaisesRegex(ValueError, "verified system companion support"):
                    stage_trials(self.layout, profiles, template)
                self.assertEqual(list(self.layout.iterdir()), [])
        files.write_text("nested/" + pin)
        with self.assertRaisesRegex(ValueError, "top-level DLL"):
            stage_trials(self.layout, profiles, template)
        files.write_text("COM¹.dll\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Invalid or duplicate companion path"):
            stage_trials(self.layout, profiles, template)
        files.write_text(pin)
        stage_trials(self.layout, profiles, template)
        self.assertEqual((self.layout / files.name).read_text(), pin)

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
