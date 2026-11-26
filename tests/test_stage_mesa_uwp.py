import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile

from build_tools import stage_mesa_uwp as mesa


def archive(path, members):
    with ZipFile(path, 'w') as output:
        for name, data in members.items():
            output.writestr(name, data)
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ControlRuntimeTests(unittest.TestCase):
    def test_writes_the_flat_layout_the_graphics_control_copies(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            mesa_digest = archive(root / 'mesa.zip', {f'bin/{name}': name.encode() for name in mesa.LIBRARIES})
            dxc_digest = archive(root / 'dxc.zip', {entry: name.encode() for name, entry in mesa.DXIL_FILES.items()})
            (root / 'license.rst').write_bytes(b'license')
            release = ('https://example.invalid/mesa', hashlib.sha256(b'license').hexdigest(), 'bin/', ())
            with (patch.dict(mesa.RELEASES, {mesa_digest: release}, clear=True),
                  patch.object(mesa, 'DXIL_ARCHIVE_SHA256', dxc_digest)):
                names = mesa.write_control_runtime(root / 'runtime', root / 'mesa.zip', root / 'license.rst',
                                                   root / 'dxc.zip')
                # build.sh copies these names from MESA_LAYOUT.
                for name in ('opengl32.dll', 'libEGL.dll', 'libGLESv2.dll', 'libgallium_wgl.dll', 'z-1.dll', 'dxil.dll',
                             'LICENSE.mesa-uwp', 'LICENSE.DXIL-MS.txt', 'LICENSE.DXIL-LLVM.txt', 'LICENSE.DXIL-MIT.txt'):
                    self.assertIn(name, names)
                self.assertEqual((root / 'runtime/dxil.dll').read_bytes(), b'dxil.dll')
                self.assertEqual((root / 'runtime/mesa-uwp-source.txt').read_text(), 'https://example.invalid/mesa\n')
                with self.assertRaises(FileExistsError):
                    mesa.write_control_runtime(root / 'runtime', root / 'mesa.zip', root / 'license.rst', root / 'dxc.zip')
                (root / 'license.rst').write_bytes(b'changed')
                with self.assertRaisesRegex(ValueError, 'license'):
                    mesa.write_control_runtime(root / 'other', root / 'mesa.zip', root / 'license.rst', root / 'dxc.zip')
            with self.assertRaisesRegex(ValueError, 'pinned, unchanged Mesa'):
                mesa.write_control_runtime(root / 'other', root / 'mesa.zip', root / 'license.rst', root / 'dxc.zip')
            self.assertFalse((root / 'other').exists())


if __name__ == '__main__':
    unittest.main()
