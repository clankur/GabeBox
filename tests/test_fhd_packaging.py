import hashlib
from pathlib import Path
import runpy
import tempfile
import unittest
from xml.etree import ElementTree
from zipfile import ZipFile


prepare = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'probe' / 'prepare-steam-core.py'))
manifest = b'''<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
 xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10" IgnorableNamespaces="uap">
 <Identity Name="XboxPortabilityProbe.ChildProcess.SteamCore" ProcessorArchitecture="x64" Version="1.0.1.0"/>
 <Applications><Application StartPage="index.html"/></Applications><Extensions>
 <Extension Category="windows.activatableClass.inProcessServer"><InProcessServer>
 <Path>ProbeNative.dll</Path><ActivatableClass ActivatableClassId="XboxProbe.Native.ProbeRunner" ThreadingModel="both"/>
 </InProcessServer></Extension></Extensions></Package>'''


class PresenterPackagingTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.template = self.root / 'template.appx'
        with ZipFile(self.template, 'w') as signed:
            signed.writestr('AppxManifest.xml', manifest)
        self.artifact = self.root / 'artifact'
        self.artifact.mkdir()
        sums = []
        for name in prepare['FHD_COMPONENT']:
            data = ('signed ' + name).encode()
            (self.artifact / name).write_bytes(data)
            sums.append(f'{hashlib.sha256(data).hexdigest()}  {name}\r\n')
        contents = ''.join(sums).encode('ascii')
        (self.artifact / 'SHA256SUMS').write_bytes(contents)
        self.pin = hashlib.sha256(contents).hexdigest()

    def layout(self, path, **options):
        prepare['prepare_layout'](self.template, path, '1.0.2.0', None, None, False, False, **options)

    def test_regular_layout_and_sdk_use_the_same_registration_and_members(self):
        regular = self.root / 'regular'
        self.layout(regular, fhd_presenter=self.artifact, fhd_presenter_checksum=self.pin)
        sdk = self.root / 'sdk'
        self.layout(sdk)
        for _ in range(2):
            prepare['stage_native_presenter'](sdk, self.artifact, self.pin)
        result = (regular / 'AppxManifest.xml').read_bytes()
        self.assertEqual((sdk / 'AppxManifest.xml').read_bytes(), result)
        self.assertIn(b'xmlns:uap=', result)
        root = ElementTree.fromstring(result)
        self.assertEqual([s.findtext('{*}Path') for s in root.findall('{*}Extensions/{*}Extension/{*}InProcessServer')],
                         ['ProbeNative.dll', 'Fhd.Native.dll'])
        for name in prepare['FHD_COMPONENT']:
            self.assertEqual((regular / name).read_bytes(), (self.artifact / name).read_bytes())

    def test_reject_mismatched_component_before_creating_layout(self):
        (self.artifact / 'Fhd.Native.winmd').write_bytes(b'wrong API')
        with self.assertRaisesRegex(ValueError, 'does not match'):
            self.layout(self.root / 'out', fhd_presenter=self.artifact, fhd_presenter_checksum=self.pin)
        self.assertFalse((self.root / 'out').exists())
        with self.assertRaisesRegex(ValueError, 'differs'):
            self.layout(self.root / 'out', fhd_presenter=self.artifact, fhd_presenter_checksum='a'*64)
