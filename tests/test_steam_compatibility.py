import hashlib
import json
from pathlib import Path
import runpy
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch
from zipfile import ZipFile


compat = runpy.run_path(str(Path(__file__).resolve().parents[1] / "tools/steam_compatibility.py"))
measure = runpy.run_path(str(Path(__file__).resolve().parents[1] / "tools/measure.py"))


class SteamCompatibilityTests(unittest.TestCase):
    def test_readobj_preserves_delayed_symbols_and_ordinals(self):
        ordinary = """fixture.exe: file format coff-x86-64
Magic 020b
Entry d 00001000 00000080 Delay Import Directory
The Import Tables:
    DLL Name: KERNEL32.dll
      1 GetLastError
"""
        delayed = """Format: COFF-x86-64
Import {
  Name: KERNEL32.dll
  Symbol: GetLastError (1)
}
DelayImport {
  Name: Version.dll
  Import {
    Symbol: GetFileVersionInfoW (2)
  }
  Import {
    Symbol:  (2521)
  }
}
"""
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "fixture.exe"
            binary.write_bytes(b"mocked PE")
            with patch.object(subprocess, "run", side_effect=[Mock(stdout=ordinary), Mock(stdout=delayed)]):
                machine, _, imports, delay_imports = compat["pe_inspector"]("objdump", None, "readobj")(binary)
        self.assertEqual(machine, "amd64")
        self.assertEqual(imports, {"kernel32.dll": {"GetLastError"}})
        self.assertEqual(delay_imports, {"version.dll": {"GetFileVersionInfoW", "#2521"}})

    def test_missing_module_uses_registry_only_when_all_imports_have_bindings(self):
        registry = {"uxtheme.dll": {"EnableThemeDialogTexture": "XboxWin32Desktop.dll"}}
        audit = {"observed_missing_system_imports": ["uxtheme.dll"], "modules": {"fixture.exe": {
            "architecture": "amd64", "imports": {"uxtheme.dll": ["EnableThemeDialogTexture"]},
        }}}
        self.assertEqual(compat["plan_bindings"](audit, "fixture.exe", registry), registry)
        audit["modules"]["fixture.exe"]["imports"]["uxtheme.dll"].append("OpenThemeData")
        self.assertEqual(compat["plan_bindings"](audit, "fixture.exe", registry), {})

    def test_library_scan_isolates_incomplete_ambiguous_and_escaped_installs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            apps = root / "steamapps"
            common = apps / "common"
            common.mkdir(parents=True)

            def manifest(identifier, state="4"):
                path = apps / f"appmanifest_{identifier}.acf"
                path.write_text(f'''// Steam data
                    "AppState" {{
                        "appid" "{identifier}" "buildid" "42" "StateFlags" "{state}"
                        "installdir" "Game {identifier}"
                        "InstalledDepots" {{ "42" {{ "manifest" "900" "buildid" "99" }} }}
                    }}''', encoding="utf-8-sig")
                return path

            for identifier in range(1, 6):
                manifest(identifier, state="1026" if identifier == 3 else "4")
                (common / f"Game {identifier}").mkdir()
            (common / "Game 1" / "Win64").mkdir()
            (common / "Game 1" / "Win64" / "game.exe").touch()
            for name in ("launcher.exe", "game.exe"):
                (common / "Game 2" / name).touch()
            escaped = manifest(4).rename(root / "external.acf")
            (apps / "appmanifest_4.acf").symlink_to(escaped)
            outside = root / "Game 5"
            outside.mkdir()
            (outside / "game.exe").touch()
            (common / "Game 5").rmdir()
            (common / "Game 5").symlink_to(outside, target_is_directory=True)
            inspect = Mock(return_value=("amd64", "digest", {"dxgi.dll": {"CreateDXGIFactory1"}}, {}))
            games = {index: str(index) for index in range(1, 7)}
            result = compat["scan_library"](games, [apps], {}, inspect)
            self.assertEqual([entry["status"] for entry in result["games"]],
                             ["audited", "entrypoints-needed", "incomplete-install",
                              "scan-error", "scan-error", "no-manifest"])
            self.assertEqual(result["games"][1]["candidates"], ["game.exe", "launcher.exe"])
            self.assertEqual(result["audits"][0]["build_id"], "42")
            self.assertEqual(result["audits"][0]["capabilities"], ["dxgi"])
            inspect.assert_called_once_with((common / "Game 1" / "Win64" / "game.exe").resolve())
            self.assertEqual(compat["binary_paths"]([r"Win64\game.exe"]), ["Win64/game.exe"])
            with self.assertRaises(ValueError):
                compat["binary_paths"]([r"..\outside.exe"])

    def test_library_manifest_build_takes_precedence_over_historical_audit(self):
        games = {1: "Game"}
        checks = dict.fromkeys(compat["CHECKS"], True)
        audit = {"id": 1, "build_id": "41", "capabilities": ["dxgi"]}
        trial = {"id": 1, "build_id": "41", "target": "Xbox", "checks": checks, "evidence": "trial"}
        known = {1: {"42"}}
        reporter = compat["coverage"]
        self.assertEqual(reporter(games, "Xbox", [audit], [trial], installed_builds=known)["local_verified_count"], 0)
        trial["build_id"] = "42"
        self.assertEqual(reporter(games, "Xbox", [audit], [trial], installed_builds=known)["local_verified_count"], 1)

    def test_batch_ranks_shared_console_import_gaps_for_matching_architecture(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            apps = root / "steamapps"
            for identifier in range(1, 4):
                game = apps / "common" / f"Game {identifier}"
                game.mkdir(parents=True)
                (game / "game.exe").touch()
                (apps / f"appmanifest_{identifier}.acf").write_text(
                    f'"AppState" {{ "appid" "{identifier}" "buildid" "{40 + identifier}" '
                    f'"StateFlags" "4" "installdir" "Game {identifier}" }}')
            (apps / "common" / "Game 2" / "Optional.dll").touch()

            first = root / "first.txt"
            first.write_text("missing_import=USER32.DLL!GetRawInputBuffer\n")
            second = root / "second.txt"
            second.write_text("optional.dll_system_load_error=0x0000007E\n"
                              "missing_import=user32.dll!GetRawInputBuffer\n")
            observations = compat["xbox_import_results"]([first, second])

            def inspect(path):
                machine = "i386" if path.parent.name == "Game 3" else "amd64"
                if path.suffix == ".dll":
                    return machine, "hash", {}, {}
                return machine, "hash", {"user32.dll": {"GetRawInputBuffer"}}, {"optional.dll": {"Probe"}}

            result = compat["scan_library"]({1: "First", 2: "Second", 3: "Third"}, [apps], {}, inspect, observations)
            self.assertEqual(result["observed_blocker_groups"], [
                {"import": "user32.dll!GetRawInputBuffer", "load_time": [{"id": 1, "build_id": "41"},
                                                                       {"id": 2, "build_id": "42"}],
                 "delay_load": []},
                {"import": "optional.dll", "load_time": [], "delay_load": [{"id": 1, "build_id": "41"}]},
            ])
            self.assertEqual(result["audits"][2]["observed_missing_system_imports"], [])

    def test_library_scan_finds_unity_player_and_nested_mono_next_to_its_launcher(self):
        with tempfile.TemporaryDirectory() as temporary:
            apps = Path(temporary) / "steamapps"
            game = apps / "common" / "Game"
            (game / "Game_Data").mkdir(parents=True)
            mono = "MonoBleedingEdge/EmbedRuntime/mono-2.0-bdwgc.dll"
            (game / mono).parent.mkdir(parents=True)
            for filename in ("Game.exe", "UnityCrashHandler64.exe", "UnityPlayer.dll", mono):
                (game / filename).touch()
            (apps / "appmanifest_1.acf").write_text(
                '"AppState" { "appid" "1" "buildid" "42" "StateFlags" "4" "installdir" "Game" }')
            inspect = Mock(return_value=("amd64", "digest", {}, {}))
            result = compat["scan_library"]({1: "Game"}, [apps], {}, inspect)
            self.assertEqual(result["games"][0]["status"], "audited")
            self.assertEqual(set(result["audits"][0]["modules"]), {"Game.exe", "UnityPlayer.dll", mono})
            self.assertEqual(result["audits"][0]["capabilities"], ["unity-player", "mono-runtime"])

            (game / "Other.exe").touch()
            (game / "Other_Data").mkdir()
            result = compat["scan_library"]({1: "Game"}, [apps], {}, inspect)
            self.assertEqual(result["games"][0]["status"], "entrypoints-needed")

    def test_dxgi_trial_stages_metadata_for_a_pinned_executable_and_companions(self):
        fixture = runpy.run_path(str(Path(__file__).with_name("test_pe_import_redirect.py")))["fixture"]
        image = fixture()
        # Two load-time import descriptors within the fixture's existing section.
        struct.pack_into("<II", image, 0x98 + 120, 0x1000, 60)
        for index, (module, symbol) in enumerate((("dxgi.dll", "CreateDXGIFactory2"),
                                                   ("user32.dll", "CreateWindowExW"))):
            start = 0x300 + 0x80 * index
            rva = 0x1000 + start - 0x200
            struct.pack_into("<IIIII", image, 0x200 + index * 20, rva, 0, 0, rva + 0x20, rva + 0x10)
            for offset in (start, start + 0x10):
                struct.pack_into("<QQ", image, offset, rva + 0x40, 0)
            name = module.encode() + b"\0"
            image[start + 0x20:start + 0x20 + len(name)] = name
            function = b"\0\0" + symbol.encode() + b"\0"
            image[start + 0x40:start + 0x40 + len(function)] = function
        struct.pack_into("<IIIII", image, 0x228, 0, 0, 0, 0, 0)
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            root = work / "install"
            core = root / "D3D12" / "D3D12Core.dll"
            core.parent.mkdir(parents=True)
            core.write_bytes(b"fixture graphics runtime")
            game = root / "Example.exe"
            game.write_bytes(image)
            fingerprints = [{"path": file.relative_to(root).as_posix(), "size": file.stat().st_size,
                             "sha256": hashlib.sha256(file.read_bytes()).hexdigest()} for file in (game, core)]
            profile = work / "source.json"
            profile.write_text(json.dumps({"123": fingerprints}))
            audit = {"id": 1817070, "build_id": "123", "observed_missing_system_imports": [],
                     "modules": {game.name: {"architecture": "amd64", "sha256": fingerprints[0]["sha256"],
                                              "bundled": {}, "imports": {"dxgi.dll": ["CreateDXGIFactory2"],
                                                                         "user32.dll": ["CreateWindowExW"]}}}}
            output = work / "trial"
            result = compat["prepare_dxgi_trial"](audit, game.name, root, profile, output)
            self.assertEqual(result["bindings"]["dxgi.dll"], {"CreateDXGIFactory2": "XboxWin32Dxgi.dll"})
            self.assertEqual(result["companions"], ["D3D12/D3D12Core.dll"])
            self.assertEqual((output / "steam-win32-files-1817070.txt").read_text(),
                             f"D3D12/D3D12Core.dll\tsha256:{fingerprints[1]['sha256']}\n")
            self.assertEqual((output / "steam-win32-shadow-1817070.txt").read_text().splitlines(),
                             ["123", "Example.exe", "", "", "", "", "loader-trace"])
            self.assertEqual(json.loads((output / "steam-win32-adapters-1817070.json").read_bytes()), {
                "build_id": "123",
                "required_adapters": [
                    "XboxWin32Desktop.dll", "XboxWin32Dxgi.dll", "XboxWin32Input.dll",
                    "XboxWin32LoaderTrace.dll", "XboxWin32Window.dll",
                ],
            })
            self.assertFalse((output / "Example.exe").exists())
            self.assertEqual(game.read_bytes(), image)

            packager = runpy.run_path(str(Path(__file__).resolve().parents[1] / "build_tools/prepare_big_picture_library.py"))
            layout = work / "layout"
            layout.mkdir()
            (layout / "steam-signin.html").write_text(
                '<script id="steam-sdl-profiles" data-apps="975370" src="steam-sdl-host.js"></script>')
            for name in ("SDL2.dll", "opengl32.dll", "libEGL.dll", "dxil.dll", "Fhd.Native.winmd"):
                (layout / name).touch()
            template = work / "runtime.appx"
            with ZipFile(template, "w") as archive:
                archive.writestr("AppxManifest.xml",
                                 '<Package><Identity Name="XboxPortabilityProbe.ChildProcess.SteamCore" '
                                 'ProcessorArchitecture="x64"/></Package>')
                archive.writestr("ProbeNative.dll", "steam_source_check= steam_shadow_companion_hashes=verified".encode("utf-16le"))
                for name in ("XboxProbe.Native.winmd", "XboxWin32Input.dll", "XboxWin32Desktop.dll",
                             "XboxWin32Window.dll", "XboxWin32Dxgi.dll", "XboxWin32LoaderTrace.dll"):
                    archive.writestr(name, b"runtime fixture")
            packager["stage_surfaceless_profiles"](layout, ["1817070=123"])
            with ZipFile(template) as archive:
                packager["stage_win32_trial_profiles"](layout, [f"1817070={output}"], archive)
            self.assertIn('data-apps="975370 1817070"', (layout / "steam-signin.html").read_text())
            self.assertEqual((layout / "steam-sdl-surfaceless-1817070.txt").read_text(), "123")
            self.assertTrue((layout / "XboxWin32Dxgi.dll").is_file())
            self.assertTrue((layout / "steam-win32-dxgi-1817070.txt").is_file())
            self.assertEqual((layout / "steam-win32-shadow-1817070.xwp").read_bytes(),
                             (output / "steam-win32-shadow-1817070.xwp").read_bytes())
            self.assertFalse((layout / "Example.exe").exists())
            self.assertFalse((layout / "D3D12").exists())

            core.write_bytes(b"x" + core.read_bytes()[1:])
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                compat["prepare_dxgi_trial"](audit, game.name, root, profile, work / "bad-trial")
            self.assertFalse((work / "bad-trial").exists())

    def test_bindings_include_only_observed_missing_imports_on_the_selected_image(self):
        audit = {"modules": {
            "game.exe": {"architecture": "amd64", "imports": {"ole32.dll": ["SetErrorInfo", "GetErrorInfo"]}},
            "engine.dll": {"architecture": "amd64", "imports": {"user32.dll": ["MapVirtualKeyExA"]}},
        }, "observed_missing_system_imports": ["ole32.dll!GetErrorInfo", "user32.dll!MapVirtualKeyExA"]}
        registry = {"ole32.dll": {"SetErrorInfo": "oleaut32.dll", "GetErrorInfo": "oleaut32.dll"},
                    "user32.dll": {"MapVirtualKeyExA": "XboxWin32Input.dll"}}
        self.assertEqual(compat["plan_bindings"](audit, "game.exe", registry),
                         {"ole32.dll": {"GetErrorInfo": "oleaut32.dll"}})

    def test_unchanged_signed_companion_is_identified_before_binding(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / "engine.dll"
            image = bytearray(512)
            image[:2] = b"MZ"
            struct.pack_into("<I", image, 60, 128)
            image[128:132] = b"PE\0\0"
            struct.pack_into("<H", image, 148, 240)
            struct.pack_into("<H", image, 152, 0x20B)
            struct.pack_into("<I", image, 152 + 108, 16)
            binary.write_bytes(image)
            self.assertIs(compat["pe_certificate_table"](binary), False)
            struct.pack_into("<II", image, 152 + 112 + 4 * 8, 480, 32)
            binary.write_bytes(image)
            inspect = Mock(return_value=("amd64", "a" * 64, {}, {}))
            result = compat["audit_install"](1, "42", root, [binary.name], inspect, None)
            self.assertIs(result["modules"][binary.name]["authenticode_table"], True)
            with self.assertRaisesRegex(ValueError, "declares an Authenticode table"):
                compat["plan_bindings"](result, binary.name, {})
            self.assertEqual(compat["plan_bindings"](result, binary.name, {}, signed_source_sha256="a" * 64), {})
            with self.assertRaisesRegex(ValueError, "audited signed-source SHA-256"):
                compat["plan_bindings"](result, binary.name, {}, signed_source_sha256="b" * 64)

    def test_rea_falls_back_to_llvm_only_for_file_size(self):
        binary = Path("game.exe")
        llvm = Mock(return_value=("amd64", "hash", {}, None))
        failure = {"code": "resource_constraint", "details": {"resource": "file-size"}}
        size_error = subprocess.CalledProcessError(1, "rea", json.dumps(failure), "")
        with patch("subprocess.run", side_effect=size_error):
            self.assertEqual(compat["rea_imports"]("rea", binary, llvm), llvm.return_value)
        llvm.assert_called_once_with(binary)

        failure["details"]["resource"] = "time"
        time_error = subprocess.CalledProcessError(1, "rea", json.dumps(failure), "")
        with patch("subprocess.run", side_effect=time_error):
            with self.assertRaises(subprocess.CalledProcessError):
                compat["rea_imports"]("rea", binary, llvm)
        llvm.assert_called_once_with(binary)

    def test_runtime_alias_requires_an_observed_missing_module_and_all_requested_exports(self):
        imports = {"desktop.dll": ["Present", "#189"], "system.dll": ["Present"]}
        audit = {"modules": {"game.exe": {"architecture": "amd64", "imports": imports}},
                 "observed_missing_system_imports": ["desktop.dll", "system.dll!Present"]}
        exports = {"Present", "#189"}
        aliases = dict.fromkeys(imports, ("Runtime.dll", "amd64", exports))
        self.assertEqual(compat["plan_bindings"](audit, "game.exe", {}, aliases),
                         {"desktop.dll": {"Present": "Runtime.dll", "#189": "Runtime.dll"}})

        aliases["desktop.dll"] = ("Runtime.dll", "amd64", {"Present"})
        with self.assertRaisesRegex(ValueError, "#189"):
            compat["plan_bindings"](audit, "game.exe", {}, aliases)
        aliases["desktop.dll"] = ("Runtime.dll", "i386", exports)
        with self.assertRaisesRegex(ValueError, "i386"):
            compat["plan_bindings"](audit, "game.exe", {}, aliases)

        audit["modules"]["game.exe"]["bundled"] = {"desktop.dll": "desktop.dll"}
        self.assertEqual(compat["plan_bindings"](audit, "game.exe", {}, aliases), {})

    def test_runtime_exports_require_a_present_address_for_named_and_ordinal_entries(self):
        output = """Export Table:
       1  0x1000  SDL_Init
       2  0x0000
       3  0x1200
       4  0x1400  SDL_Quit
"""
        self.assertEqual(compat["runtime_exports"](output), {"#1", "#3", "#4", "SDL_Init", "SDL_Quit"})

    def test_host_xinput_overrides_state_and_capabilities_in_a_hosted_profile(self):
        library = "xinput9_1_0.dll"
        symbols = ["XInputGetState", "XInputSetState", "XInputGetCapabilities"]
        image = {"architecture": "amd64", "imports": {library: symbols}}
        audit = {"modules": {"game.exe": image}, "observed_missing_system_imports": [library]}
        aliases = {library: ("xinput1_4.dll", "amd64", set(symbols))}
        self.assertEqual(compat["plan_bindings"](audit, "game.exe", {}, aliases),
                         {library: dict.fromkeys(symbols, "xinput1_4.dll")})
        self.assertEqual(compat["plan_bindings"](audit, "game.exe", {}, aliases, host_xinput=True),
                         {library: {"XInputGetState": "XboxWin32Input.dll", "XInputSetState": "xinput1_4.dll",
                                    "XInputGetCapabilities": "XboxWin32Input.dll"}})

    def test_import_graph_distinguishes_bundled_direct_and_delay_dependencies(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "game.exe").touch()
            (root / "Engine.dll").touch()
            (root / "steam_api64.dll").touch()
            report = root / "xbox.txt"
            report.write_text("opengl32.dll_system_load_error=0x0000007E\n"
                              "missing_import=user32.dll!GetDC\n")
            imports = {
                "game.exe": ({"engine.dll": {"Boot"}, "user32.dll": {"RegisterClassExW"}}, {"user32.dll": {"GetDC"}}),
                "Engine.dll": ({"opengl32.dll": {"glGetString"}, "d3d11.dll": {"D3D11CreateDevice"},
                                "mscoree.dll": {"_CorDllMain"}, "xinput9_1_0.dll": {"XInputGetState"},
                                "kernel32.dll": {"CreateNamedPipeW", "OpenFileMappingW"}},
                               {"user32.dll": {"CreateWindowExW", "RegisterRawInputDevices"},
                                "api-ms-win-core-synch-l1-2-0.dll": {"CreateEventExW"}}),
                "steam_api64.dll": ({}, {}),
            }

            def inspect(path):
                return "amd64", "digest", *imports[path.name]

            observations = compat["xbox_import_results"]([report])
            result = compat["audit_install"](1, "2", root, ["game.exe", "steam_api64.dll"], inspect, observations)
            self.assertEqual(result["modules"]["game.exe"]["bundled"], {"engine.dll": "Engine.dll"})
            self.assertEqual(result["observed_missing_system_imports"], ["opengl32.dll"])
            self.assertEqual(result["observed_missing_delay_imports"], ["user32.dll!GetDC"])
            self.assertIn("direct3d-11", result["capabilities"])
            self.assertTrue({"desktop-window-registration", "desktop-window-creation",
                             "legacy-clr-loader", "steamworks", "xinput", "raw-input"}.issubset(result["capabilities"]))
            self.assertTrue({"named-pipe-apis", "named-synchronization-apis",
                             "file-mapping-apis"}.issubset(result["capabilities"]))
            self.assertNotIn("engine.dll", result["external_imports"])

    def test_inventory_coverage_requires_matching_build_and_full_play_checks(self):
        with tempfile.TemporaryDirectory() as temporary:
            export = Path(temporary) / "inventory.json"
            export.write_text(json.dumps([
                {"id": 3, "name": "Third"}, {"id": 1, "name": "First"}, {"id": 2, "name": "Second"},
            ]))
            games = compat["library"](export)

        target = "series-x-1.2"
        checks = dict.fromkeys(compat["CHECKS"], True)
        good = {"id": 1, "target": target, "build_id": "7", "evidence": "test trial", "checks": checks}
        audits = [{"id": i, "build_id": "7", "capabilities": ["direct3d-11"]} for i in [1, 2, 3]]
        coverage = compat["coverage"](games, target, audits, [
            good, good,
            {**good, "id": 2, "build_id": "6"},
            {**good, "id": 3, "checks": {**checks, "controller": False}},
        ])
        self.assertEqual(coverage["library_count"], 3)
        self.assertEqual(coverage["goal_count"], 3)
        self.assertEqual(coverage["local_verified_count"], 1)
        self.assertEqual(coverage["capability_groups"], {"direct3d-11": [1, 2, 3]})

    def test_hardware_coverage_requires_sustained_distinct_gpu_presents(self):
        count = 30 * 60

        def measurement(version, mode):
            trace = measure["HEADER"].pack(0x31444846, version, 60000, measure["FILETIME_EPOCH"], 0)
            trace += b"".join(measure["PRESENT"].pack(i * 1000, i, i, mode) for i in range(count + 1))
            return measure["summarize"](trace, 0, None, 30, internal_resolution="1920x1080", output_resolution="1920x1080")

        checks = dict.fromkeys(compat["CHECKS"], True)
        trial = {"id": 1, "target": "series-x", "build_id": "7", "evidence": "test trial", "checks": checks,
                 "performance_scene": "gameplay"}
        for performance, verified in ((measurement(2, 2), 1), (measurement(2, 1), 0), (measurement(1, 2), 0),
                                      ({**measurement(2, 2), "min_rolling_1s_fps": 30}, 0),
                                      ({**measurement(2, 2), "internal_resolution": "1280x720"}, 0),
                                      ({**measurement(2, 2), "output_resolution": None}, 0)):
            with self.subTest(performance=performance):
                self.assertEqual(performance["presented"], count)
                result = compat["coverage"]({1: "Game"}, "series-x", [], [{**trial, "performance": performance}],
                                            require_gpu_60=True)
                self.assertEqual(result["local_verified_count"], verified)
        result = compat["coverage"]({1: "Game"}, "series-x", [],
                                    [{**trial, "performance_scene": "title", "performance": measurement(2, 2)}],
                                    require_gpu_60=True)
        self.assertEqual(result["local_verified_count"], 0)


if __name__ == "__main__":
    unittest.main()
