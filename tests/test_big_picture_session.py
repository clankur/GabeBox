import hashlib
from pathlib import Path
import runpy
from tempfile import TemporaryDirectory
import unittest
from zipfile import ZipFile


MODULE = runpy.run_path(str(Path(__file__).resolve().parents[1] / "build_tools/stage_session.py"))
MANIFEST = b"""<Package><Identity Name="XboxPortabilityProbe.ChildProcess.SteamCore"
    ProcessorArchitecture="x64" Publisher="CN=Xbox Portability Probe"/>
    <Extensions><Extension Category="windows.activatableClass.inProcessServer"><InProcessServer>
    <Path>ProbeNative.dll</Path><ActivatableClass ActivatableClassId="XboxProbe.Native.ProbeRunner"/>
    </InProcessServer></Extension></Extensions></Package>"""
METADATA = b"MZ\x00SteamSessionSteamId\x00RunSteamSignIn\x00ResumeSteamSignIn\x00LaunchSteamGame\x00LaunchSteamSdlGame\x00"


def stage(layout, artifact, checksum):
    with MODULE["open_session_artifact"](artifact, checksum) as package:
        MODULE["stage_session_component"](layout, package)


class SessionStageTests(unittest.TestCase):
    def setUp(self):
        temp = TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.layout = Path(temp.name) / "layout"
        self.layout.mkdir()
        self.artifact = Path(temp.name) / "artifact"
        self.artifact.mkdir()
        (self.layout / "AppxManifest.xml").write_bytes(MANIFEST)
        for name in MODULE["COMPONENT"]:
            (self.layout / name).write_bytes(b"old")
        self.appx = self.artifact / MODULE["APPX"]

    def package(self, metadata=METADATA, manifest=MANIFEST):
        with ZipFile(self.appx, "w") as zipfile:
            for name, content in [
                ("AppxManifest.xml", manifest), ("AppxSignature.p7x", b"fixture"),
                ("ProbeNative.dll", b"MZdll"), ("XboxProbe.Native.winmd", metadata),
                ("steam-signin.js", b"unrelated")
            ]:
                zipfile.writestr(name, content)
        sums = f"{hashlib.sha256(self.appx.read_bytes()).hexdigest()}  {self.appx.name}\n".encode()
        (self.artifact / "SHA256SUMS").write_bytes(sums)
        return hashlib.sha256(sums).hexdigest()

    def test_only_replaces_native_pair(self):
        stage(self.layout, self.artifact, self.package())
        self.assertEqual((self.layout / "XboxProbe.Native.winmd").read_bytes(), METADATA)
        self.assertEqual((self.layout / "ProbeNative.dll").read_bytes(), b"MZdll")
        self.assertFalse((self.layout / "steam-signin.js").exists())

    def test_rejects_changed_appx_or_local_checksum_manifest(self):
        pin = self.package()
        self.appx.write_bytes(self.appx.read_bytes() + b"changed")
        with self.assertRaisesRegex(ValueError, "APPX digest"):
            stage(self.layout, self.artifact, pin)
        with self.assertRaisesRegex(ValueError, "artifact checksums"):
            stage(self.layout, self.artifact, "0" * 64)

    def test_rejects_legacy_metadata_and_wrong_package(self):
        for arguments in [
            {"metadata": METADATA.replace(b"SteamSessionSteamId", b"OldQuery")},
            {"manifest": MANIFEST.replace(b"x64", b"x86")}
        ]:
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                stage(self.layout, self.artifact, self.package(**arguments))
            for member in MODULE["COMPONENT"]:
                self.assertEqual((self.layout / member).read_bytes(), b"old")


if __name__ == "__main__":
    unittest.main()
