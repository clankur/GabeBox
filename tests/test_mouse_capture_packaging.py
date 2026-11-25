import struct
import tempfile
import unittest
from pathlib import Path
from xml.etree import ElementTree as ET

from build_tools.prepare_mouse_capture import OBSERVATION, RESTRICTED, USAGES, prepare

FOUNDATION = 'http://schemas.microsoft.com/appx/manifest/foundation/windows10'


def appcontainer_executable():
    """Build the smallest x64 GUI AppContainer header the preparer accepts."""
    image = bytearray(0x400)
    image[:2] = b'MZ'
    struct.pack_into('<I', image, 0x3c, 0x80)
    image[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HH', image, 0x84, 0x8664, 1)
    struct.pack_into('<H', image, 0x94, 240)
    struct.pack_into('<H', image, 0x96, 0x0002)
    optional = 0x98
    struct.pack_into('<H', image, optional, 0x20b)
    struct.pack_into('<II', image, optional + 32, 0x1000, 0x200)
    struct.pack_into('<I', image, optional + 60, 0x200)
    struct.pack_into('<H', image, optional + 68, 2)
    struct.pack_into('<H', image, optional + 70, 0x1000)
    struct.pack_into('<I', image, optional + 108, 16)
    struct.pack_into('<8sIIIIIIHHI', image, optional + 240, b'.text', 0x200, 0x1000,
                     0x200, 0x200, 0, 0, 0, 0, 0x60000020)
    return image


class MouseCapturePackagingTests(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp())
        self.executable = self.work / 'MouseSource.exe'
        self.executable.write_bytes(appcontainer_executable())

    def manifest(self, **options):
        layout = self.work / f'layout-{len(options)}-{"-".join(sorted(options))}'
        prepare(layout, self.executable, '1.0.1.0', **options)
        return ET.parse(layout / 'AppxManifest.xml').getroot()

    def test_default_layout_declares_no_capabilities(self):
        root = self.manifest()
        self.assertIsNone(root.find(f'{{{FOUNDATION}}}Capabilities'))

    def test_observation_layout_declares_restricted_input_and_hid_collections(self):
        root = self.manifest(input_observation=True)
        self.assertIn('rescap', root.get('IgnorableNamespaces').split())
        capabilities = root.find(f'{{{FOUNDATION}}}Capabilities')
        self.assertEqual([item.get('Name') for item in capabilities.findall(f'{{{RESTRICTED}}}Capability')],
                         list(OBSERVATION))
        device = capabilities.find(f'{{{FOUNDATION}}}DeviceCapability')
        self.assertEqual(device.get('Name'), 'humaninterfacedevice')
        collection = device.find(f'{{{FOUNDATION}}}Device')
        self.assertEqual(collection.get('Id'), 'any')
        self.assertEqual([item.get('Type') for item in collection.findall(f'{{{FOUNDATION}}}Function')],
                         ['usage:' + usage for usage in USAGES])


if __name__ == '__main__':
    unittest.main()
