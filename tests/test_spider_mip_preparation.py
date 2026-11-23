import hashlib
from pathlib import Path
import runpy
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib


PROFILE = Path(__file__).resolve().parents[1] / 'probe/win32-runtime/profiles/1817070/23986256'
MIP = runpy.run_path(str(PROFILE / 'prepare-mip-overlay.py'))
MODEL = runpy.run_path(str(PROFILE / 'model_lod.py'))


def stock_index():
    header_size = 16 + 3 * 12
    offsets = struct.pack('<IIII', 0, 100, 0, 400)
    sizes = struct.pack('<IIIIII', 1, 10, 0, 1, 20, 1)
    archive = b'\0' * 8 + b'g00s000'.ljust(64, b'\0')
    sections = ((0xdcd720b5, offsets), (0x65bcf461, sizes), (0x398abff0, archive))
    table, payload = bytearray(), bytearray()
    for tag, data in sections:
        table.extend(struct.pack('<III', tag, header_size + len(payload), len(data)))
        payload.extend(data)
    length = header_size + len(payload)
    data = struct.pack('<IIIH2x', 0x44415431, 0, length, len(sections)) + table + payload
    return struct.pack('<II', MIP['TOC_MAGIC'], length) + zlib.compress(data)


class LocalCodec:
    @staticmethod
    def compress(data):
        return zlib.compress(data)

    @staticmethod
    def decompress(packed, size):
        data = zlib.decompress(packed)
        if len(data) != size:
            raise ValueError('Fixture block size mismatch.')
        return data


class MipPreparationTests(unittest.TestCase):
    def test_index_keeps_other_assets_routed_to_the_owned_archive(self):
        source = stock_index()
        index_class = MIP['Index']
        with patch.dict(index_class.__init__.__globals__, TOC_SHA256=hashlib.sha256(source).hexdigest()):
            index = index_class(source)
            self.assertEqual(index.route(0), ('g00s000', 100, 10))
            original = index.route(1)
            prepared = index.encode([(0, 256, 8)])

        with patch.dict(index_class.__init__.__globals__, TOC_SHA256=hashlib.sha256(prepared).hexdigest()):
            rewritten = index_class(prepared)
            self.assertEqual(rewritten.route(0), (MIP['ARCHIVE_NAME'], 256, 8))
            self.assertEqual(rewritten.route(1), original)

    def test_bad_owned_payload_leaves_no_staged_overlay(self):
        item = MIP['Asset'](0, 'g00s000', 0, 4, hashlib.sha256(b'good').hexdigest(), 2, '')
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'prepared'
            with self.assertRaisesRegex(ValueError, 'differs from its fingerprint'):
                MIP['build'](None, [item], output, LocalCodec(), lambda _: b'bad!')
            self.assertFalse(output.exists())
            self.assertEqual(list(output.parent.iterdir()), [])

    def test_model_overlay_has_its_own_archive_and_cannot_replace_a_texture_route(self):
        source = stock_index()
        index_class = MIP['Index']
        with patch.dict(index_class.__init__.__globals__, TOC_SHA256=hashlib.sha256(source).hexdigest()):
            index = index_class(source)
            prepared = index.encode([(0, 0, 7)], [(1, 0, 11)])
            with self.assertRaisesRegex(ValueError, 'Duplicate or invalid overlay asset route'):
                index.encode([(0, 0, 7)], [(0, 0, 11)])

        with patch.dict(index_class.__init__.__globals__, TOC_SHA256=hashlib.sha256(prepared).hexdigest()):
            rewritten = index_class(prepared)
            self.assertEqual(rewritten.route(0), (MIP['ARCHIVE_NAME'], 0, 7))
            self.assertEqual(rewritten.route(1), (MIP['MODEL_ARCHIVE'], 0, 11))

    def test_interleaved_model_stream_resets_xor_state_at_block_boundaries(self):
        select_stream = MODEL['select_stream']
        # Two word planes. The first three vertices and last two vertices reset independently.
        raw = struct.pack('<10H', 2, 6, 2, 1, 2, 6, 8, 2, 5, 3)
        with patch.dict(select_stream.__globals__, VERTEX_BLOCK=3):
            result = select_stream(memoryview(raw), 5, [1, 3, 4], (2,), interleaved=True)
        self.assertEqual(result, struct.pack('<6H', 4, 12, 2, 3, 6, 3))

    def test_generated_dsar_handles_logical_reads_across_blocks(self):
        payload = b'A' * MIP['CHUNK_BYTES'] + b'last'
        with tempfile.TemporaryFile() as file:
            writer = MIP['ArchiveWriter'](file, LocalCodec(), len(payload))
            writer.write(payload)
            writer.finish()
            file_size = file.seek(0, 2)
            archive = MIP['Archive'](lambda at, size: MIP['read_at'](file, at, size), LocalCodec(), file_size)
            self.assertEqual(archive.read(len(payload) - 6, 6), b'AAlast')
            with self.assertRaisesRegex(ValueError, 'Invalid asset range'):
                archive.read(len(payload), 1)
