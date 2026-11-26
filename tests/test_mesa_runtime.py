import hashlib
import json
import re
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile

from build_tools import stage_mesa_uwp
from build_tools.stage_profiles import stage_win32_trial_profiles

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "runtime" / "mesa"
MANIFEST = (b'<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10">'
            b'<Identity Name="XboxPortabilityProbe.ChildProcess.SteamCore" ProcessorArchitecture="x64"/></Package>')
LICENSE = b"Mesa UWP license fixture\n"


def pe(marker=b""):
    return b"MZ" + bytes(62) + b"PE\0\0\x64\x86" + bytes(64) + marker


class MesaComponentPinTests(unittest.TestCase):
    def test_pin_matches_patches_and_packager(self):
        pin = json.loads((COMPONENT / "mesa-uwp.json").read_text())
        self.assertEqual(f"{pin['source']['repository']}/tree/{pin['source']['commit']}", stage_mesa_uwp.RUNTIME_SOURCE)
        self.assertEqual(pin["source"]["license_sha256"], stage_mesa_uwp.RUNTIME_LICENSE_SHA256)
        self.assertIn(pin["prebuilt"]["sha256"], stage_mesa_uwp.RELEASES)
        self.assertEqual(tuple(pin["libraries"]), stage_mesa_uwp.LIBRARIES)
        self.assertTrue((COMPONENT / pin["meson"]["machine_file"]).is_file())
        listed = []
        for entry in pin["patches"]:
            data = (COMPONENT / entry["file"]).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), entry["sha256"], entry["file"])
            self.assertTrue(set(entry["variants"]) <= set(pin["variants"]), entry["file"])
            listed.append(entry["file"])
        self.assertEqual(sorted(listed), sorted(f"patches/{path.name}" for path in (COMPONENT / "patches").iterdir()))
        patches = {entry["file"].split("/")[-1][:4]: (COMPONENT / entry["file"]).read_text() for entry in pin["patches"]}
        self.assertIn(stage_mesa_uwp.SHADER_CACHE_MARKER.decode(), patches["0003"])
        self.assertIn(stage_mesa_uwp.DESCRIPTOR_POOL_MARKER.decode(), patches["0004"])

    def test_descriptor_pool_patch_locks_every_cross_thread_free(self):
        pin = json.loads((COMPONENT / "mesa-uwp.json").read_text())
        entry = next(entry for entry in pin["patches"] if entry["file"].startswith("patches/0004-"))
        self.assertEqual(entry["variants"], ["shader-cache"])
        added = "\n".join(line[1:] for line in (COMPONENT / entry["file"]).read_text().splitlines()
                          if line.startswith("+") and not line.startswith("+++"))
        # The view-pool free, the sampler-pool allocations and the zombie sampler frees all take
        # the screen's descriptor_pool_mutex when the lock is on; the stats line is written per batch.
        self.assertEqual(added.count("mtx_lock(&screen->descriptor_pool_mutex);"), 4)
        self.assertEqual(added.count("mtx_unlock(&screen->descriptor_pool_mutex);"), 4)
        for name in ("D3D12_DESCRIPTOR_POOL_LOCK", "D3D12_DESCRIPTOR_STATS", "dup_allocs=", "double_frees=",
                     "overlaps=", "cross_thread_frees=", "threaded=", "d3d12_descriptor_pool_write_stats(ctx->threaded_context"):
            self.assertIn(name, added)
        # MSVC's std::call_once and std::mutex live in msvcp140_app.dll, which the pinned release does not import.
        self.assertNotIn("std::call_once", added)
        self.assertNotIn("<mutex>", added)

    def test_windows_cache_backend_uses_long_paths_and_reports_failures(self):
        pin = json.loads((COMPONENT / "mesa-uwp.json").read_text())
        backend, driver = ((COMPONENT / entry["file"]).read_text() for entry in pin["patches"][1:3])
        added = "\n".join(line[1:] for line in backend.splitlines() if line.startswith("+"))
        windows = added[added.index("#else /* DETECT_OS_WINDOWS */"):]
        # Entry paths below a packaged app's LocalState pass MAX_PATH, so the Windows backend
        # must not fall back to the C runtime's narrow file calls.
        for call in ("_sopen_s", "_mkdir", "_stat64", "rename", "_findfirst64", "_chsize_s"):
            self.assertIsNone(re.search(rf"(?<!\w){call}\(", windows), call)
        for call in ("CreateFile2", "CreateDirectoryW", "MoveFileExW", "\\\\\\\\?\\\\"):
            self.assertIn(call, windows)
        self.assertIn("disk_cache_print_diagnostics", added)
        self.assertIn("disk_cache_first_failure=", added)
        self.assertIn("+         disk_cache_print_diagnostics(screen->disk_cache, file);", driver)

    def test_baseline_only_changes_the_build(self):
        pin = json.loads((COMPONENT / "mesa-uwp.json").read_text())
        for entry in pin["patches"]:
            touched = [line.split()[2][2:] for line in (COMPONENT / entry["file"]).read_text().splitlines()
                       if line.startswith("diff --git ")]
            if "baseline" in entry["variants"]:
                self.assertTrue(all(path.endswith("meson.build") for path in touched), touched)
            self.assertTrue(touched)


class MesaRuntimeArtifactTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.work = Path(temp.name)
        self.artifact = self.work / "artifact"
        self.artifact.mkdir()
        for name in stage_mesa_uwp.LIBRARIES:
            marker = stage_mesa_uwp.SHADER_CACHE_MARKER if name == "libgallium_wgl.dll" else b""
            (self.artifact / name).write_bytes(pe(marker))
        (self.artifact / "LICENSE.mesa-uwp").write_bytes(LICENSE)
        (self.artifact / "mesa-uwp-source.txt").write_text(
            stage_mesa_uwp.RUNTIME_SOURCE + "\nvariant shader-cache\n")
        self.license = patch.object(stage_mesa_uwp, "RUNTIME_LICENSE_SHA256", hashlib.sha256(LICENSE).hexdigest())
        self.license.start()
        self.addCleanup(self.license.stop)

    def seal(self):
        sums = "".join(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n"
                       for path in sorted(self.artifact.iterdir()) if path.name != "SHA256SUMS")
        (self.artifact / "SHA256SUMS").write_text(sums)
        return hashlib.sha256(sums.encode()).hexdigest()

    def test_stage_replaces_the_prebuilt_runtime(self):
        digest = self.seal()
        layout = self.work / "layout"
        layout.mkdir()
        with self.assertRaisesRegex(ValueError, "does not carry the Mesa UWP runtime"):
            stage_mesa_uwp.stage_mesa_runtime(layout, self.artifact, digest)
        for name in stage_mesa_uwp.LIBRARIES:
            (layout / name).write_bytes(b"prebuilt")
        self.assertTrue(stage_mesa_uwp.stage_mesa_runtime(layout, self.artifact, digest))
        for name in stage_mesa_uwp.RUNTIME_FILES:
            self.assertEqual((layout / name).read_bytes(), (self.artifact / name).read_bytes(), name)

    def test_rejects_unpinned_or_changed_artifacts(self):
        digest = self.seal()
        with self.assertRaisesRegex(ValueError, "pinned SHA256SUMS digest"):
            stage_mesa_uwp.load_mesa_runtime(self.artifact, "0" * 64)
        with self.assertRaisesRegex(ValueError, "Pin the Mesa UWP runtime"):
            stage_mesa_uwp.load_mesa_runtime(self.artifact, "not-a-digest")
        (self.artifact / "opengl32.dll").write_bytes(pe(b"changed"))
        with self.assertRaisesRegex(ValueError, "differs from SHA256SUMS"):
            stage_mesa_uwp.load_mesa_runtime(self.artifact, digest)
        (self.artifact / "opengl32.dll").write_bytes(b"MZ" + bytes(62) + b"PE\0\0\x4c\x01")
        with self.assertRaisesRegex(ValueError, "not an x64 PE image"):
            stage_mesa_uwp.load_mesa_runtime(self.artifact, self.seal())
        (self.artifact / "opengl32.dll").write_bytes(pe())
        (self.artifact / "mesa-uwp-source.txt").write_text("https://example.invalid/mesa\n")
        with self.assertRaisesRegex(ValueError, "pinned source revision"):
            stage_mesa_uwp.load_mesa_runtime(self.artifact, self.seal())
        (self.artifact / "mesa-uwp-source.txt").write_text(stage_mesa_uwp.RUNTIME_SOURCE + "\n")
        (self.artifact / "extra.dll").write_bytes(pe())
        with self.assertRaisesRegex(ValueError, "must list exactly"):
            stage_mesa_uwp.load_mesa_runtime(self.artifact, self.seal())


class MesaProfileMarkerTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.work = Path(temp.name)
        self.layout = self.work / "layout"
        self.layout.mkdir()
        self.app, self.build = "2868840", "26655341"
        self.profile = self.work / "profile"
        self.profile.mkdir()
        self.shadow = self.profile / f"steam-win32-shadow-{self.app}.txt"
        self.shadow.write_text(f"{self.build}\nSlayTheSpire2.exe\nopengl3\ngodot\n\n\nloader-trace\n")
        (self.profile / f"steam-win32-shadow-{self.app}.xwp").write_bytes(
            struct.pack("<8sQII32s", b"XWPEV1\r\n", 512, 64, 32, bytes(32)) + bytes(96))

    def runtime(self, native, window=b"runtime fixture"):
        template = self.work / f"runtime-{len(list(self.work.glob('runtime-*')))}.appx"
        with ZipFile(template, "w") as archive:
            archive.writestr("AppxManifest.xml", MANIFEST)
            archive.writestr("ProbeNative.dll", native)
            archive.writestr("XboxWin32Window.dll", window)
            for name in ("XboxProbe.Native.winmd", "XboxWin32Input.dll", "XboxWin32Desktop.dll",
                         "XboxWin32LoaderTrace.dll"):
                archive.writestr(name, b"runtime fixture")
        return template

    def stage(self, template):
        with ZipFile(template) as package:
            stage_win32_trial_profiles(self.layout, [f"{self.app}={self.profile}"], package)

    def test_mesa_shader_cache_needs_the_patched_runtime_and_host(self):
        marker = self.profile / f"steam-win32-mesa-shader-cache-{self.app}.txt"
        marker.write_text("on")
        host = self.runtime("MESA_SHADER_CACHE_DIR\0".encode("utf-16le"))
        with self.assertRaisesRegex(ValueError, "empty marker and the OpenGL renderer"):
            self.stage(host)
        marker.write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "sets the Mesa shader cache directory"):
            self.stage(self.runtime(b"native fixture"))
        (self.layout / "libgallium_wgl.dll").write_bytes(pe())
        with self.assertRaisesRegex(ValueError, "shader-cache Mesa UWP runtime"):
            self.stage(host)
        (self.layout / "libgallium_wgl.dll").write_bytes(pe(stage_mesa_uwp.SHADER_CACHE_MARKER))
        self.stage(host)
        self.assertEqual((self.layout / marker.name).read_bytes(), b"")
        self.shadow.write_text(f"{self.build}\nSlayTheSpire2.exe\nd3d12\ngodot\n\n\nloader-trace\n")
        with self.assertRaisesRegex(ValueError, "OpenGL renderer"):
            self.stage(host)

    def test_mesa_no_thread_needs_an_empty_marker_opengl_and_host(self):
        marker = self.profile / f"steam-win32-mesa-no-thread-{self.app}.txt"
        marker.write_text("0")
        host = self.runtime("GALLIUM_THREAD\0".encode("utf-16le"))
        with self.assertRaisesRegex(ValueError, "empty marker and the OpenGL renderer"):
            self.stage(host)
        marker.write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "turn off Mesa's threaded context"):
            self.stage(self.runtime(b"native fixture"))
        # The prebuilt Mesa reads GALLIUM_THREAD too, so no patched runtime is required.
        self.stage(host)
        self.assertEqual((self.layout / marker.name).read_bytes(), b"")
        self.shadow.write_text(f"{self.build}\nSlayTheSpire2.exe\nd3d12\ngodot\n\n\nloader-trace\n")
        with self.assertRaisesRegex(ValueError, "OpenGL renderer"):
            self.stage(host)

    def test_mesa_descriptor_arm_needs_patch_0004_and_host(self):
        marker = self.profile / f"steam-win32-mesa-descriptors-{self.app}.txt"
        marker.write_text("on")
        host = self.runtime("D3D12_DESCRIPTOR_POOL_LOCK\0".encode("utf-16le"))
        with self.assertRaisesRegex(ValueError, "stats or lock"):
            self.stage(host)
        marker.write_text("lock\n")
        with self.assertRaisesRegex(ValueError, "Mesa descriptor pool variables"):
            self.stage(self.runtime(b"native fixture"))
        (self.layout / "libgallium_wgl.dll").write_bytes(pe(stage_mesa_uwp.SHADER_CACHE_MARKER))
        with self.assertRaisesRegex(ValueError, "patch 0004"):
            self.stage(host)
        (self.layout / "libgallium_wgl.dll").write_bytes(
            pe(stage_mesa_uwp.SHADER_CACHE_MARKER + stage_mesa_uwp.DESCRIPTOR_POOL_MARKER))
        self.stage(host)
        self.assertEqual((self.layout / marker.name).read_bytes(), b"lock")
        marker.write_text("stats")
        self.stage(host)
        self.assertEqual((self.layout / marker.name).read_bytes(), b"stats")

    def test_gl_trace_needs_loader_trace_and_host_support(self):
        marker = self.profile / f"steam-win32-gl-trace-{self.app}.txt"
        marker.touch()
        variable = "XBOX_WIN32_GL_TRACE\0".encode("utf-16le")
        with self.assertRaisesRegex(ValueError, "GL first-use timing"):
            self.stage(self.runtime(variable))
        self.stage(self.runtime(variable, variable))
        self.assertEqual((self.layout / marker.name).read_bytes(), b"")
        self.shadow.write_text(f"{self.build}\nSlayTheSpire2.exe\nopengl3\ngodot\n")
        with self.assertRaisesRegex(ValueError, "loader trace"):
            self.stage(self.runtime(variable, variable))


if __name__ == "__main__":
    unittest.main()
