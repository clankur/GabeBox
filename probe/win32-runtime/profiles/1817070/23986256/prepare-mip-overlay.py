#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# The mip and routing logic follows BBCPrince/UWP-Spider-Launcher asset preparation.
# https://github.com/BBCPrince/UWP-Spider-Launcher/tree/1865c248825f617b24ee75b258f10e634338ff0b/asset-preparation
# See LICENSE-mip-preparation.txt for the standalone tool's license.
"""Rebuild a separate, build-verified texture archive and TOC for an owned Spider-Man install."""

import argparse
from contextlib import ExitStack
from bisect import bisect_right
import ctypes
import ctypes.util
from dataclasses import dataclass
from hashlib import sha256
from pathlib import Path
import re
import struct
import tempfile
import zlib


TOC_SHA256 = '58e571cf1e54e08b18db4e8effb3f964eded1ddaa3e4feebd72f88fbbc066da1'
TOC_MAGIC = 0x77af12af
DSAR_HEADER = struct.Struct('<4sIIIQ8x')
DSAR_BLOCK = struct.Struct('<QQIIB7x')
ARCHIVE_NAME = 'local_mip256_hdclosed.dsar'
CHUNK_BYTES = 256 * 1024
MAX_ASSET_BYTES = 64 * 1024 * 1024
SOURCE_NAME = re.compile(r'[a-z0-9][a-z0-9_.-]{0,62}', re.ASCII)
SHA256 = re.compile(r'[0-9a-f]{64}', re.ASCII)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return sha256(data).hexdigest()


class Lz4:
    def __init__(self, library=None):
        name = library or ctypes.util.find_library('lz4')
        if not name:
            raise ValueError('Supply a path to liblz4 with --lz4.')
        dll = ctypes.CDLL(str(name))
        self.decode = dll.LZ4_decompress_safe
        self.encode = dll.LZ4_compress_default
        for function in (self.decode, self.encode):
            function.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
            function.restype = ctypes.c_int
        self.bound = dll.LZ4_compressBound
        self.bound.argtypes = [ctypes.c_int]
        self.bound.restype = ctypes.c_int

    def decompress(self, packed, size):
        buffer = ctypes.create_string_buffer(size)
        require(self.decode(packed, buffer, len(packed), size) == size, 'DSAR decompression size mismatch.')
        return buffer.raw

    def compress(self, source):
        capacity = self.bound(len(source))
        buffer = ctypes.create_string_buffer(capacity)
        written = self.encode(source, buffer, len(source), capacity)
        require(written > 0, 'Could not compress DSAR block.')
        return buffer.raw[:written]


def read_at(file, offset, size):
    file.seek(offset)
    return file.read(size)


class Archive:
    def __init__(self, read_at, codec, source_size):
        self.read_at = read_at
        self.codec = codec
        magic, version, count, data_start, self.length = DSAR_HEADER.unpack(read_at(0, DSAR_HEADER.size))
        table_bytes = count * DSAR_BLOCK.size
        require(magic == b'DSAR' and version == 0x10003 and 0 < count <= 262144
                and DSAR_HEADER.size + table_bytes <= data_start <= source_size, 'Invalid DSAR header.')
        table = read_at(DSAR_HEADER.size, table_bytes)
        require(len(table) == table_bytes, 'Short DSAR table.')
        self.blocks = tuple(DSAR_BLOCK.iter_unpack(table))
        self.starts = []
        expected = 0
        for offset, physical, expanded, packed, method in self.blocks:
            require(offset == expected and 0 < expanded <= 4 * 1024 * 1024 and 0 < packed <= 8 * 1024 * 1024
                    and data_start <= physical <= source_size - packed and method == 3, 'Invalid DSAR block.')
            self.starts.append(offset)
            expected += expanded
        require(expected == self.length, 'DSAR logical length mismatch.')
        self.cached_index = None
        self.cached_block = b''

    def read(self, offset, length):
        require(0 < length <= MAX_ASSET_BYTES and 0 <= offset <= self.length - length, 'Invalid asset range.')
        end = offset + length
        result = bytearray()
        index = bisect_right(self.starts, offset) - 1
        while offset < end:
            start, physical, expanded, packed, _ = self.blocks[index]
            if index != self.cached_index:
                data = self.read_at(physical, packed)
                require(len(data) == packed, 'Short DSAR block.')
                self.cached_block = self.codec.decompress(data, expanded)
                self.cached_index = index
            stop = min(start + expanded, end)
            result.extend(self.cached_block[offset - start:stop - start])
            index += 1
            offset = stop
        return result


class ArchiveWriter:
    def __init__(self, file, codec, size):
        self.file = file
        self.codec = codec
        self.size = size
        self.count = (size + CHUNK_BYTES - 1) // CHUNK_BYTES
        self.header_size = DSAR_HEADER.size + DSAR_BLOCK.size * self.count
        self.block_table = bytearray()
        self.position = 0
        self.pending = bytearray()
        file.seek(self.header_size)

    def write(self, data):
        self.pending.extend(data)
        while len(self.pending) >= CHUNK_BYTES:
            self.flush_block(bytes(self.pending[:CHUNK_BYTES]))
            del self.pending[:CHUNK_BYTES]

    def flush_block(self, block):
        packed = self.codec.compress(block)
        self.block_table.extend(DSAR_BLOCK.pack(self.position, self.file.tell(), len(block), len(packed), 3))
        self.file.write(packed)
        self.position += len(block)

    def finish(self):
        if self.pending:
            self.flush_block(bytes(self.pending))
        require(self.position == self.size and len(self.block_table) == DSAR_BLOCK.size * self.count,
                'Generated DSAR length mismatch.')
        self.file.seek(0)
        self.file.write(DSAR_HEADER.pack(b'DSAR', 0x10003, self.count, self.header_size, self.size))
        self.file.write(self.block_table)


class Index:
    def __init__(self, source):
        require(digest(source) == TOC_SHA256, 'Original TOC does not match Steam build 23986256.')
        magic, declared = struct.unpack_from('<II', source)
        require(magic == TOC_MAGIC and 16 <= declared <= MAX_ASSET_BYTES, 'Invalid TOC header.')
        # The original stream ends at a deflate flush marker. The exact input hash verifies that encoding.
        self.data = bytearray(zlib.decompressobj().decompress(source[8:], declared + 1))
        tag, _, size, count = struct.unpack_from('<IIIH', self.data)
        require(tag == 0x44415431 and size == declared == len(self.data) and 0 < count < 32, 'Invalid DAT1 index.')
        sections = {}
        for index in range(count):
            field = 16 + 12 * index
            name, offset, size = struct.unpack_from('<III', self.data, field)
            require(16 + 12 * count <= offset <= declared - size and name not in sections, 'Invalid DAT1 section.')
            sections[name] = (offset, size, field + 8)
        archives, size, self.archive_section_size = sections[0x398abff0]
        self.offsets, offset_bytes, _ = sections[0xdcd720b5]
        self.sizes, size_bytes, _ = sections[0x65bcf461]
        require(archives + size == declared and size % 72 == offset_bytes % 8 == size_bytes % 12 == 0
                and offset_bytes // 8 == size_bytes // 12, 'Unsupported DAT1 routing table.')
        self.count = offset_bytes // 8
        self.archives = tuple(self.data[at + 8:at + 72].split(b'\0', 1)[0].decode('ascii')
                              for at in range(archives, declared, 72))
        self.archive_row_prefix = self.data[archives:archives + 8]
        self.untouched_archive_bytes = size

    def route(self, index):
        require(0 <= index < self.count, 'Recipe asset index outside source TOC.')
        archive, offset = struct.unpack_from('<II', self.data, self.offsets + index * 8)
        count, size, identity = struct.unpack_from('<III', self.data, self.sizes + index * 12)
        require(archive < len(self.archives) and count == 1 and identity == index, 'Unexpected source asset route.')
        return self.archives[archive], offset, size

    def encode(self, changed):
        data = self.data.copy()
        data.extend(self.archive_row_prefix + ARCHIVE_NAME.encode('ascii').ljust(64, b'\0'))
        struct.pack_into('<I', data, self.archive_section_size, self.untouched_archive_bytes + 72)
        struct.pack_into('<I', data, 8, len(data))
        for index, position, size in changed:
            struct.pack_into('<II', data, self.offsets + index * 8, len(self.archives), position)
            struct.pack_into('<I', data, self.sizes + index * 12 + 4, size)
        return struct.pack('<II', TOC_MAGIC, len(data)) + zlib.compress(data)


@dataclass(frozen=True)
class Texture:
    index: int
    archive: str
    offset: int
    original_size: int
    original_hash: str
    output_size: int
    output_hash: str


def load_plan(directory, index, selected=(), limit=None):
    require(set(selected) <= set(index.archives), 'Unknown source archive in --archive.')
    with (directory / 'asset-index.tsv').open(encoding='ascii') as source:
        tag, original_hash, target_hash = source.readline().rstrip('\n').split('\t')
        require(tag == 'ASSETGEN_TOC_V1' and original_hash == TOC_SHA256 and SHA256.fullmatch(target_hash),
                'Texture route recipe has a different source.')
        added, routes, assets = [], {}, set()
        for line in source:
            fields = line.rstrip('\n').split('\t')
            if fields[0] == 'ARCHIVE':
                require(len(fields) == 2 and fields[1] not in added and not assets, 'Invalid archive recipe entry.')
                added.append(fields[1])
                continue
            require(len(fields) == 5 and fields[0] == 'ASSET', 'Invalid route recipe entry.')
            item, target, offset, size = map(int, fields[1:])
            require(item not in assets, 'Duplicate asset route.')
            assets.add(item)
            if len(index.archives) <= target < len(index.archives) + len(added) and added[target - len(index.archives)] == ARCHIVE_NAME:
                require((offset, size) not in routes, 'Duplicate destination route.')
                routes[offset, size] = item
    plan = []
    with (directory / 'mip256-full.tsv').open(encoding='ascii') as source:
        tag, row_count, _, original_hash = source.readline().rstrip('\n').split('\t')
        require(tag == 'ASSETGEN_MIP256_V1' and original_hash == TOC_SHA256, 'Mip recipe has a different source.')
        count, seen = 0, set()
        for line in source:
            count += 1
            name, start, before, input_hash, dest, after, output_hash = line.rstrip('\n').split('\t')
            offset, length, size = int(start), int(before), int(after)
            route = int(dest), size
            require(route not in seen and SOURCE_NAME.fullmatch(name) and SHA256.fullmatch(input_hash)
                    and SHA256.fullmatch(output_hash) and 0 < size <= length <= MAX_ASSET_BYTES and offset >= 0,
                    'Invalid mip recipe entry.')
            seen.add(route)
            if route in routes and (not selected or name in selected):
                item = routes[route]
                require(index.route(item) == (name, offset, length), 'Recipe source route differs from TOC.')
                plan.append(Texture(item, name, offset, length, input_hash, size, output_hash))
        require(count == int(row_count) and routes.keys() <= seen and plan, 'Incomplete or empty mip plan.')
    return plan[:limit]


def trim_texture(source):
    def u32(offset):
        return struct.unpack_from('<I', source, offset)[0]

    def u16(offset):
        return struct.unpack_from('<H', source, offset)[0]

    require(len(source) >= 128 and tuple(u32(at) for at in (0, 36, 44, 52, 56, 60))
            == (0x5c4580b9, 0x44415431, 92, 0x4ede3593, 48, 44) and u16(48) == u16(100) == 1,
            'Unsupported texture header.')
    width, height, format_code = u16(96), u16(98), u16(104)
    levels, hd_levels = source[114], source[116]
    block_bytes = {71: 8, 72: 8, 77: 16, 78: 16, 80: 8, 83: 16, 95: 16, 98: 16, 99: 16}
    require(format_code in block_bytes and 2 <= levels <= 16 and width and height
            and not (width & (width - 1) or height & (height - 1)), 'Unsupported texture dimensions or format.')

    def mips(w, h, count):
        return [(max(1, w >> n), max(1, h >> n)) for n in range(count)]

    def bytes_for(w, h):
        return (w + 3) // 4 * ((h + 3) // 4) * block_bytes[format_code]

    dimensions = mips(width, height, levels)
    sizes = [bytes_for(w, h) for w, h in dimensions]
    require(len(source) - 128 == sum(sizes) == u32(8) == u32(20) == u32(84), 'Inconsistent mip sizes.')
    first = next((n for n, pair in enumerate(dimensions) if max(pair) <= 256), levels)
    require(first < levels, 'Texture contains no 256-pixel mip.')
    if first == 0:
        hd_width, hd_height = u16(92), u16(94)
        require(1 <= hd_levels <= 16 and (max(1, hd_width >> hd_levels), max(1, hd_height >> hd_levels)) == dimensions[0]
                and sum(bytes_for(w, h) for w, h in mips(hd_width, hd_height, hd_levels)) == u32(88) > 0,
                'Inconsistent HD mip sizes.')
    pixels = source[128 + sum(sizes[:first]):]
    result = bytearray(source[:128])
    for offset in (8, 20, 84):
        struct.pack_into('<I', result, offset, len(pixels))
    struct.pack_into('<I', result, 88, 0)
    struct.pack_into('<HHHH', result, 92, *dimensions[first], *dimensions[first])
    result[114], result[116] = levels - first, 0
    return result + pixels


def build(index, plan, output, codec, read_asset):
    output = output.resolve()
    require(not output.exists(), 'Use a new output directory.')
    with tempfile.TemporaryDirectory(dir=output.parent, prefix='.mips-') as temporary:
        staged = Path(temporary) / 'assets'
        staged.mkdir()
        offset, routes = 0, []
        with (staged / ARCHIVE_NAME).open('xb') as destination:
            writer = ArchiveWriter(destination, codec, sum(item.output_size for item in plan))
            for item in plan:
                source = read_asset(item)
                require(len(source) == item.original_size and digest(source) == item.original_hash,
                        f'Installed asset {item.index} differs from its fingerprint.')
                resized = trim_texture(source)
                require(len(resized) == item.output_size and digest(resized) == item.output_hash,
                        f'Generated asset {item.index} differs from its fingerprint.')
                writer.write(resized)
                routes.append((item.index, offset, item.output_size))
                offset += item.output_size
            writer.finish()
        (staged / 'toc').write_bytes(index.encode(routes))
        archive_path = staged / ARCHIVE_NAME
        with archive_path.open('rb') as file:
            archive = Archive(lambda at, count: read_at(file, at, count), codec, archive_path.stat().st_size)
            for item, (_, start, size) in zip(plan, routes):
                require(digest(archive.read(start, size)) == item.output_hash,
                        f'Generated archive failed reread validation at asset {item.index}.')
        staged.rename(output)
    print(f'Prepared {len(plan):,} textures and {offset:,} logical bytes.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path, help='owned asset_archive directory')
    parser.add_argument('recipes', type=Path, help='reference recipe directory')
    parser.add_argument('output', type=Path, help='new directory for the generated index and archive')
    parser.add_argument('--lz4', type=Path, help='liblz4 path when it is outside the library search path')
    parser.add_argument('--archive', action='append', default=[], help='limit preparation to this source archive (repeatable)')
    parser.add_argument('--limit', type=int, help='prepare only the first N matching routes for a control')
    args = parser.parse_args()
    try:
        require(args.limit is None or args.limit > 0, 'Limit must be positive.')
        require(not args.output.resolve().is_relative_to(args.original.resolve()), 'Keep generated assets outside the original directory.')
        index = Index((args.original / 'toc').read_bytes())
        plan = load_plan(args.recipes, index, args.archive, args.limit)
        codec = Lz4(args.lz4)
        archives = {}
        with ExitStack() as stack:
            def read_asset(item):
                if item.archive not in archives:
                    path = args.original / item.archive
                    file = stack.enter_context(path.open('rb'))
                    archives[item.archive] = Archive(lambda at, count: read_at(file, at, count), codec, path.stat().st_size)
                return archives[item.archive].read(item.offset, item.original_size)

            build(index, plan, args.output, codec, read_asset)
    except (OSError, ValueError, KeyError, struct.error, zlib.error) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
