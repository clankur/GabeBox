import shutil
import struct
import tempfile
import unittest
from pathlib import Path
from xml.etree import ElementTree as ET

from build_tools.prepare_controller_pointer import EXECUTABLE, HID_MARKER, HID_USAGES, prepare
from probe.catalog import CONTROLLER_POINTER_PROBE_NAME

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


class ControllerPointerPackagingTests(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.work)
        self.executable = self.work / 'ControllerPointer.exe'
        self.executable.write_bytes(appcontainer_executable())

    def layout(self, **options):
        layout = self.work / ('hid' if options.get('hid') else 'baseline')
        prepare(layout, self.executable, '1.0.2.0', **options)
        return layout, ET.parse(layout / 'AppxManifest.xml').getroot()

    def test_baseline_layout_has_its_own_identity_and_no_capabilities(self):
        layout, root = self.layout()
        self.assertEqual(root.find(f'{{{FOUNDATION}}}Identity').get('Name'), CONTROLLER_POINTER_PROBE_NAME)
        app = root.find(f'{{{FOUNDATION}}}Applications/{{{FOUNDATION}}}Application')
        self.assertEqual(app.get('Executable'), EXECUTABLE)
        self.assertTrue((layout / EXECUTABLE).is_file())
        self.assertIsNone(root.find(f'{{{FOUNDATION}}}Capabilities'))
        self.assertFalse((layout / HID_MARKER).exists())

    def test_hid_layout_declares_joystick_and_gamepad_collections(self):
        layout, root = self.layout(hid=True)
        device = root.find(f'{{{FOUNDATION}}}Capabilities/{{{FOUNDATION}}}DeviceCapability')
        self.assertEqual(device.get('Name'), 'humaninterfacedevice')
        functions = device.findall(f'{{{FOUNDATION}}}Device/{{{FOUNDATION}}}Function')
        self.assertEqual([item.get('Type') for item in functions], ['usage:' + usage for usage in HID_USAGES])
        self.assertTrue((layout / HID_MARKER).is_file())

    def test_rejects_a_version_outside_the_appx_range(self):
        with self.assertRaises(ValueError):
            prepare(self.work / 'bad', self.executable, '1.0.70000.0')


if __name__ == '__main__':
    unittest.main()
