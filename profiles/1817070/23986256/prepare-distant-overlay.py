#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# The distant-light transform and recipe format follow BBCPrince/UWP-Spider-Launcher asset preparation:
# ResampledTexture.cs (ASSETGEN_DISTANT_V1) and the reference runtime-toc.tsv routes.
# https://github.com/BBCPrince/UWP-Spider-Launcher/tree/1865c248825f617b24ee75b258f10e634338ff0b/asset-preparation
# See LICENSE-mip-preparation.txt for the standalone asset tools' license.
"""Prepare the reference distant-light overlay for Spider-Man build 23986256 from owned files.

The city's five distant-light textures are 812 x 1472 arrays: 12 RGBA8 slices (57,372,800 bytes) and one
R8 mask each, plus a 164-byte descriptor. The reference halves their width and height
(local_distant_xyhalf, 292,841,780 to 73,212,020 bytes). The halving is DirectXTex's linear resize at an
exact 2:1 ratio, which equals the rounded 2 x 2 mean computed here, so every output matches the
reference SHA-256 in the recipe and the archive matches its logical SHA-256 in batch.tsv.

The tool appends the 15 assets to an existing overlay archive slot (--append-to, for example the trial's
model archive) and routes the same assets as the reference runtime index, so a loader that already
serves that archive needs no new file names. Sources are read locally or, with --remote-archives and
--portal-env, in bounded byte ranges from the Xbox install, cached under --cache.
"""

import argparse
from hashlib import sha256
from pathlib import Path
import runpy
import struct
import sys
import tempfile
import zlib


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from texture_restore import Toc, align16, load_batch_manifest, load_reference_routes, require  # noqa: E402

RESTORE = runpy.run_path(str(HERE / 'prepare-texture-restore.py'))
MIP = RESTORE['MIP']
TOC_SHA256 = MIP['TOC_SHA256']
RECIPE_NAME = 'local_distant_xyhalf.dsar'
RECIPE_TAG = 'ASSETGEN_DISTANT_V1'
DESCRIPTOR_MAGIC = 0x567cc2f0
TEXTURE_HEADER, TEXTURE_PAYLOAD = 84, 128
WIDTH, HEIGHT = 812, 1472
FORMATS = {28: (4, 12), 61: (1, 1)}  # DXGI R8G8B8A8_UNORM with 12 slices, DXGI R8_UNORM with one slice


def digest(data):
    return sha256(data).hexdigest()


def half_pixels(pixels, width, height, slices, channels):
    """Return the rounded 2 x 2 mean of each channel, slice by slice."""
    row = width * channels
    out_width, out_height = width // 2, height // 2
    plane = out_width * out_height
    output = bytearray(plane * slices * channels)
    for number in range(slices):
        base = number * row * height
        even = b''.join(pixels[base + y * row:base + (y + 1) * row] for y in range(0, height, 2))
        odd = b''.join(pixels[base + y * row:base + (y + 1) * row] for y in range(1, height, 2))
        target = number * plane * channels
        step = 2 * channels
        for channel in range(channels):
            mean = bytes((a + b + c + d + 2) >> 2 for a, b, c, d in zip(
                even[channel::step], even[channel + channels::step], odd[channel::step], odd[channel + channels::step]))
            output[target + channel:target + plane * channels:channels] = mean
    return bytes(output)


def transform(raw):
    """Halve one distant-light descriptor or texture asset, as ResampledTexture.Transform does."""
    if len(raw) == 164 and struct.unpack_from('<I', raw)[0] == DESCRIPTOR_MAGIC:
        require(struct.unpack_from('<II', raw, 116) == (WIDTH, HEIGHT), 'Invalid distant-light dimensions.')
        changed = bytearray(raw)
        struct.pack_into('<II', changed, 116, WIDTH // 2, HEIGHT // 2)
        return bytes(changed)
    require(len(raw) > TEXTURE_PAYLOAD, 'Invalid distant-light texture.')
    width, height, slices, _, texture_format = struct.unpack_from('<HHHHH', raw, TEXTURE_HEADER + 12)
    old = len(raw) - TEXTURE_PAYLOAD
    channels, expected_slices = FORMATS.get(texture_format, (0, 0))
    require((width, height, slices) == (WIDTH, HEIGHT, expected_slices) and old == width * height * slices * channels
            and struct.unpack_from('<I', raw, 8)[0] == old and struct.unpack_from('<I', raw, 20)[0] == old
            and struct.unpack_from('<I', raw, TEXTURE_HEADER)[0] == old, 'Unsupported distant-light texture.')
    converted = half_pixels(raw[TEXTURE_PAYLOAD:], width, height, slices, channels)
    output = bytearray(raw[:TEXTURE_PAYLOAD]) + converted
    for field in (8, 20, TEXTURE_HEADER):
        struct.pack_into('<I', output, field, len(converted))
    struct.pack_into('<HHHH', output, TEXTURE_HEADER + 8, width // 2, height // 2, width // 2, height // 2)
    return bytes(output)


def load_recipe(path):
    """Return the recipe's logical size and rows (archive, offset, size, sha, out offset, out size, out sha)."""
    lines = path.read_text(encoding='ascii').splitlines()
    tag, count, total, source = lines[0].split('\t')
    require(tag == RECIPE_TAG and source == TOC_SHA256 and int(count) == len(lines) - 1, 'Unexpected distant recipe.')
    rows = []
    for line in lines[1:]:
        archive, offset, size, sha, out_offset, out_size, out_sha = line.split('\t')
        rows.append((archive, int(offset), int(size), sha, int(out_offset), int(out_size), out_sha))
    return int(total), rows


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('original', type=Path, help='owned asset_archive directory with the original toc')
    parser.add_argument('recipes', type=Path, help='reference asset-preparation/recipes directory')
    parser.add_argument('output', type=Path, help='new directory for the generated TOC and archive')
    parser.add_argument('--base-toc', type=Path, required=True, help='overlay TOC to extend, derived from the original')
    parser.add_argument('--append-to', type=Path, required=True, help='overlay archive named in --base-toc to extend')
    parser.add_argument('--portal-env', type=Path, help='Device Portal settings for archives missing locally')
    parser.add_argument('--remote-archives', action='store_true', help='read every source archive through Device Portal')
    parser.add_argument('--remote-folder', default=RESTORE['REMOTE_FOLDER'], help='asset_archive path under DevelopmentFiles')
    parser.add_argument('--cache', type=Path, required=True, help='directory of cached archive byte ranges')
    parser.add_argument('--lz4', type=Path, help='liblz4 path when it is outside the library search path')
    args = parser.parse_args()
    opened = []
    try:
        output = args.output.resolve()
        require(not output.exists(), 'Use a new output directory.')
        require(not output.is_relative_to(args.original.resolve()), 'Keep generated assets outside the original directory.')
        original_bytes = (args.original / 'toc').read_bytes()
        MIP['Index'](original_bytes)  # checks the original TOC SHA-256 for build 23986256
        original = Toc(original_bytes)
        base = Toc(args.base_toc.read_bytes())
        base.require_derived_from(original)
        batch = load_batch_manifest(args.recipes / 'batch.tsv', TOC_SHA256)
        total, rows = load_recipe(args.recipes / f'{RECIPE_NAME}.tsv')
        require(batch[RECIPE_NAME][0] == total, 'Distant recipe size differs from batch.tsv.')
        added, routes = load_reference_routes(args.recipes / 'runtime-toc.tsv', TOC_SHA256, len(original.archives))
        slot = len(original.archives) + added.index(RECIPE_NAME)
        routed = {(offset, size): item for item, (target, offset, size) in routes.items() if target == slot}
        require(len(routed) == len(rows), 'The reference does not route every distant-light asset.')
        plan = []
        for archive, offset, size, sha, out_offset, out_size, out_sha in rows:
            item = routed[(out_offset, out_size)]
            require(original.archives[original.route(item)[0]] == archive and original.route(item)[1:] == (offset, size),
                    f'Distant-light source differs from the TOC route of asset {item}.')
            require(base.route(item) == original.route(item), f'Base TOC already reroutes asset {item}.')
            plan.append((item, archive, offset, size, sha, out_offset, out_size, out_sha))

        codec = MIP['Lz4'](args.lz4)
        cache = RESTORE['RangeCache'](args.cache)
        portal = RESTORE['PortalFiles'](args.portal_env, args.remote_folder) if args.portal_env else None
        archives = {}

        def open_archive(name):
            if name not in archives:
                path = args.original / name
                archives[name] = (RESTORE['LocalArchive'](path, codec) if path.is_file() and not args.remote_archives
                                  else RESTORE['RemoteArchive'](name, codec, cache, portal))
                opened.append(getattr(archives[name], 'file', None))
            return archives[name]

        for name in sorted({row[1] for row in plan}):
            fetch, spans = open_archive(name).plan([(row[2], row[3]) for row in plan if row[1] == name])
            if spans:
                print(f'{name}: {fetch:,} uncached bytes in {spans} Xbox reads.', flush=True)

        logical, position, assets = sha256(), 0, []
        for item, archive, offset, size, sha, out_offset, out_size, out_sha in plan:
            source = bytes(open_archive(archive).archive.read(offset, size))
            require(len(source) == size and digest(source) == sha, f'Asset {item}: owned source differs from its fingerprint.')
            changed = transform(source)
            require(len(changed) == out_size and digest(changed) == out_sha,
                    f'Asset {item}: generated distant-light asset differs from its fingerprint.')
            logical.update(bytes(out_offset - position))
            logical.update(changed)
            position = out_offset + out_size
            assets.append((item, changed))
        logical.update(bytes(total - position))
        require(logical.hexdigest() == batch[RECIPE_NAME][1], 'Distant archive differs from batch.tsv.')
        print(f'{RECIPE_NAME}: verified {len(assets)} assets and archive SHA-256 {logical.hexdigest()}.', flush=True)

        name = args.append_to.name
        require(name in base.archives[len(original.archives):], '--append-to is not an overlay archive in the base TOC.')
        target = base.archives.index(name)
        with tempfile.TemporaryDirectory(dir=output.parent, prefix='.distant-') as temporary:
            staged = Path(temporary) / 'assets'
            staged.mkdir()
            source_file, source = RESTORE['reread'](args.append_to, codec)
            opened.append(source_file)
            for item in range(base.count):
                archive, offset, size = base.route(item)
                if archive == target:
                    require(offset + size <= source.length, f'Base route {item} exceeds {name}.')
            prefix = RESTORE['logical_digest'](source, source.length)

            def base_chunks():
                for offset in range(0, source.length, 4 * 1024 * 1024):
                    yield source.read(offset, min(4 * 1024 * 1024, source.length - offset))

            pieces, new_routes, position = [(0, base_chunks)], {}, align16(source.length)
            for item, data in assets:
                pieces.append((position, data))
                new_routes[item] = target, position, len(data)
                position = align16(position + len(data))
            size = max(offset + len(data) for offset, data in pieces[1:])
            RESTORE['write_archive'](staged / name, codec, size, pieces)
            check_file, check = RESTORE['reread'](staged / name, codec)
            opened.append(check_file)
            require(RESTORE['logical_digest'](check, source.length) == prefix, f'{name} changed its existing content.')
            expected = dict(assets)
            for item, (_, offset, length) in new_routes.items():
                require(bytes(check.read(offset, length)) == expected[item], f'{name} returns a different asset {item}.')
            toc_bytes = base.encode([], new_routes)
            result = Toc(toc_bytes)
            result.require_derived_from(original)
            for item in range(result.count):
                require(result.route(item) == new_routes.get(item, base.route(item)), f'Generated TOC misroutes asset {item}.')
            (staged / 'toc').write_bytes(toc_bytes)
            lines = [f'{file}\t{(staged / file).stat().st_size}\t{RESTORE["file_digest"](staged / file)}'
                     for file in (name, 'toc')]
            (staged / 'prepared-files.tsv').write_text('\n'.join(lines) + '\n')
            for file in opened:
                if file:
                    file.close()
            opened.clear()
            staged.rename(output)
        print(f'{name}: kept {source.length:,} existing logical bytes (SHA-256 {prefix}) and appended '
              f'{len(new_routes)} distant-light assets, {size - source.length:,} bytes.', flush=True)
        for line in lines:
            print(line, flush=True)
    except (OSError, ValueError, KeyError, struct.error, zlib.error) as error:
        parser.exit(1, f'{error}\n')
    finally:
        for file in opened:
            if file:
                file.close()


if __name__ == '__main__':
    main()
