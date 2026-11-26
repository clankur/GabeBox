# SPDX-License-Identifier: GPL-3.0-only
# The restore transform and recipe format follow BBCPrince/UWP-Spider-Launcher asset preparation:
# AssetGenerator.cs RestoreTexture.Transform and Generate.Run (ASSETGEN_TEXTURE_V2), AssetIndex.cs.
# https://github.com/BBCPrince/UWP-Spider-Launcher/tree/1865c248825f617b24ee75b258f10e634338ff0b/asset-preparation
# See LICENSE-mip-preparation.txt for the standalone asset tools' license.
"""Rebuild selected textures at 512 or 1024 pixels from their SD and HD mip chains."""

from array import array
from dataclasses import dataclass
import re
import struct
import sys
import zlib


TEXTURE_MAGIC = 0x5c4580b9
DAT1_MAGIC = 0x44415431
TOC_MAGIC = 0x77af12af
ARCHIVE_SECTION = 0x398abff0
OFFSET_SECTION = 0xdcd720b5
SIZE_SECTION = 0x65bcf461
RESTORE_RECIPE = 'ASSETGEN_TEXTURE_V2'
MAX_ASSET_BYTES = 64 * 1024 * 1024
BLOCK_BYTES = {71: 8, 72: 8, 80: 8, 77: 16, 78: 16, 83: 16, 95: 16, 98: 16, 99: 16}
SHA256 = re.compile(r'[0-9a-f]{64}', re.ASCII)
SOURCE_NAME = re.compile(r'[A-Za-z0-9_.-]{1,64}', re.ASCII)
OVERLAY_NAME = re.compile(r'local_[a-z0-9_.]{1,57}\.dsar', re.ASCII)
# Reference restore recipes for Steam build 23986256, in the reference TOC's archive order.
RESTORE_RECIPES = (
    'local_street12_sd512_20260918.dsar',
    'local_opening51_1024_20260919.dsar',
    'local_openingroom36_1024_20260919.dsar',
    'local_openingfinishes9_1024_20260919.dsar',
    'local_openingscreen3_1024_20260919.dsar',
    'local_openingbroad111_512_20260919.dsar',
    'local_openingbill3_1024_20260919.dsar',
    'local_ground29_1024_20260923.dsar',
)


def require(condition, detail):
    if not condition:
        raise ValueError(detail)


def _pow2(value):
    return value > 0 and not value & (value - 1)


def _level(width, height, block):
    return max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * block


def _chain(width, height, count, block):
    return sum(_level(max(1, width >> n), max(1, height >> n), block) for n in range(count))


def restore_texture(source, hd, maximum):
    """Join the HD and base mip chains, then drop mips wider or taller than ``maximum``.

    ``hd`` is the texture's HD mip chain from its HD archive, or None to keep the base chain.
    The result has its HD chain closed: every retained mip is inline after the 128-byte header.
    """
    require(128 <= maximum <= 4096 and _pow2(maximum), 'Unsupported maximum dimension.')

    def u32(offset):
        return struct.unpack_from('<I', source, offset)[0]

    def u16(offset):
        return struct.unpack_from('<H', source, offset)[0]

    require(len(source) >= 128 and (u32(0), u32(36), u32(44)) == (TEXTURE_MAGIC, DAT1_MAGIC, 92),
            'Unsupported texture layout.')
    require(u16(48) == 1 and (u32(52), u32(56), u32(60)) == (0x4ede3593, 48, 44) and u16(100) == 1,
            'Unsupported texture header.')
    require(u16(104) in BLOCK_BYTES, 'Unsupported texture format.')
    sd, width, height, levels, block = u32(84), u16(96), u16(98), source[114], BLOCK_BYTES[u16(104)]
    require(_pow2(width) and _pow2(height) and 1 <= levels <= 16 and len(source) == 128 + sd
            and u32(8) == sd and u32(20) == sd, 'Bad source dimensions.')
    require(_chain(width, height, levels, block) == sd, 'Invalid base mip chain.')
    hd_length = 0
    if hd is not None:
        hd_levels, width, height, hd_length = source[116], u16(92), u16(94), len(hd)
        require(0 < hd_levels <= 16 and width and height and u32(88) == hd_length
                and max(1, width >> hd_levels) == u16(96) and max(1, height >> hd_levels) == u16(98),
                'Invalid HD chain.')
        require(_chain(width, height, hd_levels, block) == hd_length, 'Invalid HD length.')
        levels += hd_levels
    skip = cut = 0
    while max(max(1, width >> skip), max(1, height >> skip)) > maximum:
        require(skip < levels, 'No requested mip.')
        cut += _level(max(1, width >> skip), max(1, height >> skip), block)
        skip += 1
    pixels = hd_length + sd - cut
    require(0 < pixels <= MAX_ASSET_BYTES - 128 and levels - skip <= 255, 'Invalid output length.')
    output = bytearray(source[:128])
    if cut < hd_length:
        output += hd[cut:]
    output += source[128 + max(0, cut - hd_length):]
    new_width, new_height = max(1, width >> skip), max(1, height >> skip)
    for offset in (8, 20, 84):
        struct.pack_into('<I', output, offset, pixels)
    struct.pack_into('<I', output, 88, 0)
    struct.pack_into('<HHHH', output, 92, new_width, new_height, new_width, new_height)
    output[114], output[116] = levels - skip, 0
    return bytes(output)


@dataclass(frozen=True)
class RestoreRow:
    archive: str
    offset: int
    size: int
    sha256: str
    output_offset: int
    output_size: int
    output_sha256: str
    maximum: int
    hd_archive: object  # str, or None when the row keeps only the base chain
    hd_offset: int
    hd_size: int
    hd_sha256: object


@dataclass(frozen=True)
class RestoreRecipe:
    name: str
    total: int
    rows: tuple


def align16(value):
    return (value + 15) & ~15


def load_restore_recipe(path, name, toc_sha256):
    """Parse one ASSETGEN_TEXTURE_V2 recipe and check the reference generator's layout rules."""
    with path.open(encoding='ascii') as source:
        header = source.readline().rstrip('\n').split('\t')
        require(len(header) == 4 and header[0] == RESTORE_RECIPE and header[3] == toc_sha256,
                f'{path.name} is not a restore recipe for this build.')
        count, total = int(header[1]), int(header[2])
        require(0 < count <= 100000 and 0 < total < 2 ** 32, f'{path.name} exceeds recipe limits.')
        rows, position = [], 0
        for line in source:
            fields = line.rstrip('\n').split('\t')
            require(len(fields) == 12, f'Malformed row in {path.name}.')
            hd = fields[8] != '-'
            row = RestoreRow(fields[0], int(fields[1]), int(fields[2]), fields[3], int(fields[4]),
                             int(fields[5]), fields[6], int(fields[7]), fields[8] if hd else None,
                             int(fields[9]), int(fields[10]), fields[11] if hd else None)
            require(SOURCE_NAME.fullmatch(row.archive) and SHA256.fullmatch(row.sha256)
                    and SHA256.fullmatch(row.output_sha256) and row.offset >= 0
                    and 0 < row.size <= MAX_ASSET_BYTES and 0 < row.output_size <= MAX_ASSET_BYTES
                    and row.output_offset == align16(position), f'Invalid row {len(rows)} in {path.name}.')
            require((not hd and (row.hd_offset, row.hd_size, fields[11]) == (0, 0, '-'))
                    or (hd and SOURCE_NAME.fullmatch(row.hd_archive) and SHA256.fullmatch(row.hd_sha256)
                        and row.hd_offset >= 0 and 0 < row.hd_size <= MAX_ASSET_BYTES),
                    f'Invalid HD source in row {len(rows)} of {path.name}.')
            rows.append(row)
            position = row.output_offset + row.output_size
        require(len(rows) == count and position == total, f'{path.name} row count or size differs from its header.')
    return RestoreRecipe(name, total, tuple(rows))


def load_batch_manifest(path, toc_sha256):
    """Return {archive name: (logical size, logical SHA-256)} from the reference batch.tsv."""
    with path.open(encoding='ascii') as source:
        header = source.readline().rstrip('\n').split('\t')
        require(header == ['ASSETGEN_BATCH_V1', toc_sha256], 'Batch manifest has a different source.')
        result = {}
        for line in source:
            kind, name, _, size, expected = line.rstrip('\n').split('\t')
            require(kind in ('ARCHIVE', 'INDEX') and SHA256.fullmatch(expected) and name not in result,
                    'Invalid batch manifest entry.')
            result[name] = int(size), expected
    return result


def load_reference_routes(path, toc_sha256, archive_count):
    """Return the reference TOC's added archives and {asset: (archive slot, offset, size)}."""
    with path.open(encoding='ascii') as source:
        tag, original, _ = source.readline().rstrip('\n').split('\t')
        require(tag == 'ASSETGEN_TOC_V1' and original == toc_sha256, 'Route recipe has a different source.')
        added, routes = [], {}
        for line in source:
            fields = line.rstrip('\n').split('\t')
            if fields[0] == 'ARCHIVE':
                require(len(fields) == 2 and OVERLAY_NAME.fullmatch(fields[1]) and fields[1] not in added
                        and not routes, 'Invalid archive recipe entry.')
                added.append(fields[1])
                continue
            require(len(fields) == 5 and fields[0] == 'ASSET', 'Invalid route recipe entry.')
            item, slot, offset, size = map(int, fields[1:])
            require(item not in routes and archive_count <= slot < archive_count + len(added)
                    and offset >= 0 and 0 < size <= MAX_ASSET_BYTES, 'Invalid asset route.')
            routes[item] = slot, offset, size
    return tuple(added), routes


class Toc:
    """A decoded DAT1 asset index, original or derived from the original by added overlay archives."""

    def __init__(self, source):
        magic, declared = struct.unpack_from('<II', source)
        require(magic == TOC_MAGIC and 16 <= declared <= MAX_ASSET_BYTES, 'Invalid TOC header.')
        self.data = bytearray(zlib.decompressobj().decompress(source[8:], declared + 1))
        tag, _, size, count = struct.unpack_from('<IIIH', self.data)
        require(tag == DAT1_MAGIC and size == declared == len(self.data) and 0 < count < 32, 'Invalid DAT1 index.')
        sections = {}
        for number in range(count):
            field = 16 + 12 * number
            name, offset, length = struct.unpack_from('<III', self.data, field)
            require(16 + 12 * count <= offset <= declared - length and name not in sections, 'Invalid DAT1 section.')
            sections[name] = offset, length, field + 8
        self.archive_start, archive_bytes, self.archive_size_field = sections[ARCHIVE_SECTION]
        self.offsets, offset_bytes, _ = sections[OFFSET_SECTION]
        self.sizes, size_bytes, _ = sections[SIZE_SECTION]
        require(self.archive_start + archive_bytes == declared and archive_bytes % 72 == 0
                and offset_bytes % 8 == 0 and size_bytes % 12 == 0 and offset_bytes // 8 == size_bytes // 12,
                'Unsupported DAT1 routing table.')
        self.count = offset_bytes // 8
        self.archives = [bytes(self.data[at + 8:at + 72]).split(b'\0', 1)[0].decode('ascii')
                         for at in range(self.archive_start, declared, 72)]
        self.row_prefix = bytes(self.data[self.archive_start:self.archive_start + 8])

    def route(self, item):
        require(0 <= item < self.count, 'Asset index outside the TOC.')
        archive, offset = struct.unpack_from('<II', self.data, self.offsets + 8 * item)
        count, size, identity = struct.unpack_from('<III', self.data, self.sizes + 12 * item)
        require(archive < len(self.archives) and count == 1 and identity == item, 'Unexpected asset route.')
        return archive, offset, size

    def size_fields(self):
        table = array('I', bytes(self.data[self.sizes:self.sizes + 12 * self.count]))
        if sys.byteorder != 'little':
            table.byteswap()
        return table

    def require_derived_from(self, original):
        """Check that this TOC equals ``original`` apart from routes into added overlay archives."""
        base = len(original.archives)
        require(self.count == original.count and self.archives[:base] == original.archives
                and all(OVERLAY_NAME.fullmatch(name) for name in self.archives[base:])
                and len(set(self.archives)) == len(self.archives), 'Base TOC has unexpected archives.')
        mine, theirs = self.size_fields(), original.size_fields()
        require(mine[0::3] == theirs[0::3] and mine[2::3] == theirs[2::3], 'Base TOC changes asset identities.')
        check = bytearray(self.data[:len(original.data)])
        check[self.offsets:self.offsets + 8 * self.count] = original.data[self.offsets:self.offsets + 8 * self.count]
        check[self.sizes:self.sizes + 12 * self.count] = original.data[self.sizes:self.sizes + 12 * self.count]
        for field in (8, self.archive_size_field):
            check[field:field + 4] = original.data[field:field + 4]
        require(check == original.data, 'Base TOC changes data outside its asset routes.')
        for at in range(len(original.data), len(self.data), 72):
            require(self.data[at:at + 8] == original.row_prefix, 'Base TOC has an unexpected archive row.')
        changed = 0
        for item in range(self.count):
            archive, offset, size = self.route(item)
            if (archive, offset, size) != original.route(item):
                require(archive >= base, f'Base TOC reroutes asset {item} inside the original archives.')
                changed += 1
        return changed

    def encode(self, added, routes):
        """Append ``added`` archive names and apply {asset: (archive slot, offset, size)}."""
        data = self.data.copy()
        names = self.archives + list(added)
        require(len(set(names)) == len(names) and all(OVERLAY_NAME.fullmatch(name) for name in added),
                'Duplicate or invalid added archive.')
        for name in added:
            data.extend(self.row_prefix + name.encode('ascii').ljust(64, b'\0'))
        for item, (archive, offset, size) in routes.items():
            require(0 <= item < self.count and 0 <= archive < len(names) and 0 <= offset < 2 ** 32
                    and 0 < size <= MAX_ASSET_BYTES, 'Invalid overlay route.')
            struct.pack_into('<II', data, self.offsets + 8 * item, archive, offset)
            struct.pack_into('<I', data, self.sizes + 12 * item + 4, size)
        archive_bytes = len(data) - self.archive_start
        struct.pack_into('<I', data, self.archive_size_field, archive_bytes)
        struct.pack_into('<I', data, 8, len(data))
        return struct.pack('<II', TOC_MAGIC, len(data)) + zlib.compress(data)
