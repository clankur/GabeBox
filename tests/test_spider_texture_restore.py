from pathlib import Path
import runpy
import struct
import tempfile
import unittest
import zlib


PROFILE = Path(__file__).resolve().parents[1] / 'profiles/1817070/23986256'
RESTORE = runpy.run_path(str(PROFILE / 'texture_restore.py'))
BC1 = 71


def level_bytes(width, height):
    return max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * 8


def chain(width, height, count, first_tag):
    """Return a BC1 mip chain whose levels are filled with distinct tag bytes."""
    levels = [bytes([first_tag + n]) * level_bytes(max(1, width >> n), max(1, height >> n)) for n in range(count)]
    return b''.join(levels), levels


def texture(base, levels, hd=None, hd_levels=0, hd_size=(0, 0)):
    width, height = base
    pixels, _ = chain(width, height, levels, 0x10)
    header = bytearray(128)
    struct.pack_into('<I', header, 0, 0x5c4580b9)
    struct.pack_into('<I', header, 36, 0x44415431)
    struct.pack_into('<IH', header, 44, 92, 1)
    struct.pack_into('<III', header, 52, 0x4ede3593, 48, 44)
    for offset in (8, 20, 84):
        struct.pack_into('<I', header, offset, len(pixels))
    struct.pack_into('<I', header, 88, len(hd) if hd else 0)
    struct.pack_into('<HHHHH', header, 92, *hd_size, width, height, 1)
    struct.pack_into('<H', header, 104, BC1)
    header[114], header[116] = levels, hd_levels
    return bytes(header) + pixels


def fields(data):
    return {
        'sizes': tuple(struct.unpack_from('<I', data, offset)[0] for offset in (8, 20, 84, 88)),
        'dimensions': struct.unpack_from('<HHHH', data, 92),
        'levels': (data[114], data[116]),
    }


class TextureRestoreTests(unittest.TestCase):
    def test_hd_chain_is_joined_and_capped_at_the_requested_size(self):
        hd, hd_levels = chain(512, 512, 2, 0x80)
        source = texture((128, 128), 8, hd, 2, (512, 512))
        restored = RESTORE['restore_texture'](source, hd, 256)

        _, base_levels = chain(128, 128, 8, 0x10)
        pixels = hd_levels[1] + b''.join(base_levels)
        self.assertEqual(restored[128:], pixels)
        self.assertEqual(restored[:8], source[:8])
        self.assertEqual(fields(restored), {
            'sizes': (len(pixels), len(pixels), len(pixels), 0),
            'dimensions': (256, 256, 256, 256),
            'levels': (9, 0),
        })

    def test_base_only_row_closes_the_hd_chain_and_trims_larger_mips(self):
        hd, _ = chain(512, 256, 1, 0x80)
        source = texture((256, 128), 3, hd, 1, (512, 256))
        restored = RESTORE['restore_texture'](source, None, 128)

        _, base_levels = chain(256, 128, 3, 0x10)
        self.assertEqual(restored[128:], base_levels[1] + base_levels[2])
        size = len(base_levels[1] + base_levels[2])
        self.assertEqual(fields(restored), {
            'sizes': (size, size, size, 0),
            'dimensions': (128, 64, 128, 64),
            'levels': (2, 0),
        })

    def test_hd_chain_with_the_wrong_length_is_rejected(self):
        hd, _ = chain(512, 512, 2, 0x80)
        source = texture((128, 128), 8, hd, 2, (512, 512))
        with self.assertRaisesRegex(ValueError, 'Invalid HD chain'):
            RESTORE['restore_texture'](source, hd[:-8], 1024)

    def test_recipe_rows_must_follow_the_reference_alignment(self):
        sha = 'a' * 64
        row = ['g00s000', '0', '160', sha, '0', '152', sha, '512', '-', '0', '0', '-']
        moved = row[:4] + ['160', '152'] + row[6:]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'recipe.tsv'
            path.write_text('\t'.join(['ASSETGEN_TEXTURE_V2', '2', '312', sha]) + '\n'
                            + '\t'.join(row) + '\n' + '\t'.join(moved) + '\n')
            recipe = RESTORE['load_restore_recipe'](path, 'local_test.dsar', sha)
            self.assertEqual([item.output_offset for item in recipe.rows], [0, 160])
            path.write_text('\t'.join(['ASSETGEN_TEXTURE_V2', '2', '304', sha]) + '\n'
                            + '\t'.join(row) + '\n' + '\t'.join(row[:4] + ['152', '152'] + row[6:]) + '\n')
            with self.assertRaisesRegex(ValueError, 'Invalid row 1'):
                RESTORE['load_restore_recipe'](path, 'local_test.dsar', sha)

    def test_restore_route_replaces_an_overlay_route_and_keeps_the_rest(self):
        header_size = 16 + 3 * 12
        offsets = struct.pack('<IIII', 0, 100, 0, 400)
        sizes = struct.pack('<IIIIII', 1, 10, 0, 1, 20, 1)
        archive = b'\0' * 8 + b'g00s000'.ljust(64, b'\0')
        table, payload = bytearray(), bytearray()
        for tag, data in ((0xdcd720b5, offsets), (0x65bcf461, sizes), (0x398abff0, archive)):
            table.extend(struct.pack('<III', tag, header_size + len(payload), len(data)))
            payload.extend(data)
        length = header_size + len(payload)
        data = struct.pack('<IIIH2x', 0x44415431, 0, length, 3) + table + payload
        original = RESTORE['Toc'](struct.pack('<II', 0x77af12af, length) + zlib.compress(data))

        mip = RESTORE['Toc'](original.encode(['local_mip.dsar'], {0: (1, 0, 8), 1: (1, 8, 9)}))
        self.assertEqual(mip.require_derived_from(original), 2)
        restored = RESTORE['Toc'](mip.encode(['local_restore.dsar'], {0: (2, 0, 12)}))
        self.assertEqual(restored.require_derived_from(original), 2)
        self.assertEqual(restored.archives, ['g00s000', 'local_mip.dsar', 'local_restore.dsar'])
        self.assertEqual(restored.route(0), (2, 0, 12))
        self.assertEqual(restored.route(1), (1, 8, 9))

        tampered = RESTORE['Toc'](original.encode([], {1: (0, 4, 20)}))
        with self.assertRaisesRegex(ValueError, 'inside the original archives'):
            tampered.require_derived_from(original)


if __name__ == '__main__':
    unittest.main()
