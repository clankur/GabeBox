#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# The restore and routing logic follows BBCPrince/UWP-Spider-Launcher asset preparation.
# https://github.com/BBCPrince/UWP-Spider-Launcher/tree/1865c248825f617b24ee75b258f10e634338ff0b/asset-preparation
# See LICENSE-mip-preparation.txt for the standalone tool's license.
"""Prepare the reference texture-restore overlays for Spider-Man build 23986256 from owned files.

The reference routes 242 opening-area textures to seven restore archives instead of the 256-pixel
mip archive. This tool rebuilds those textures from the owned SD and HD archives, verifies every
input and output SHA-256 and each archive's logical SHA-256 against the reference recipes, and
writes a TOC derived from an existing overlay TOC (for example the mip, model and zone trial TOC).

Without --append-to, each restore archive is written under its reference name and added to the
TOC as a new archive. With --append-to, the restored textures are appended to an existing overlay
archive slot so a loader that already routes that archive serves them without new file names.
Archives missing from the local directory (or every archive, with --remote-archives) are read in
bounded byte ranges from the Xbox install through Device Portal with --portal-env. Fetched ranges
are cached and reused; without --portal-env only cached ranges are read.
"""

import argparse
from bisect import bisect_right
from hashlib import sha256
from pathlib import Path
import re
import runpy
import struct
import sys
import tempfile
import zlib


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from texture_restore import (  # noqa: E402
    RESTORE_RECIPES, Toc, align16, load_batch_manifest, load_reference_routes, load_restore_recipe, require,
    restore_texture,
)

MIP = runpy.run_path(str(HERE / 'prepare-mip-overlay.py'))
TOC_SHA256 = MIP['TOC_SHA256']
REMOTE_FOLDER = r"\SteamLibrary\steamapps\common\Marvel's Spider-Man Remastered\asset_archive"
MAX_REQUEST = 8 * 1024 * 1024
CACHE_NAME = re.compile(r'(g\d{2}s\d{3})-(\d+)-(\d+)\.bin', re.ASCII)
REMOTE_NAME = re.compile(r'g\d{2}s\d{3}', re.ASCII)


def digest(data):
    return sha256(data).hexdigest()


class RangeCache:
    """Physical byte ranges of owned archives, stored as <archive>-<start>-<stop>.bin."""

    def __init__(self, directory):
        self.directory = directory
        self.files = {}
        if directory.is_dir():
            for path in directory.iterdir():
                match = CACHE_NAME.fullmatch(path.name)
                if match and path.stat().st_size == int(match[3]) - int(match[2]) > 0:
                    self.files.setdefault(match[1], []).append((int(match[2]), int(match[3]), path))

    def read(self, name, offset, count):
        for start, stop, path in self.files.get(name, ()):
            if start <= offset and offset + count <= stop:
                with path.open('rb') as file:
                    file.seek(offset - start)
                    return file.read(count)
        return None

    def store(self, name, start, data):
        self.directory.mkdir(parents=True, exist_ok=True)
        path = self.directory / f'{name}-{start}-{start + len(data)}.bin'
        partial = path.with_suffix('.partial')
        partial.write_bytes(data)
        partial.replace(path)
        self.files.setdefault(name, []).append((start, start + len(data), path))


class PortalFiles:
    """Bounded, read-only Range requests for files in the Xbox install's asset_archive folder."""

    def __init__(self, env, folder):
        root = HERE.parents[2]
        if str(root) not in sys.path:
            sys.path.insert(0, str(root))
        from tools.device_portal import DevicePortal, PortalError
        from tools.xbox_deploy import game_file_url, load_settings

        settings = load_settings(env)
        require('XBOX_PORTAL_URL' in settings and 'XBOX_PORTAL_CERT_SHA256' in settings,
                'The Device Portal environment file needs XBOX_PORTAL_URL and XBOX_PORTAL_CERT_SHA256.')
        self.portal = DevicePortal(settings['XBOX_PORTAL_URL'], settings['XBOX_PORTAL_CERT_SHA256'].lower())
        if settings.get('XBOX_PORTAL_USERNAME'):
            import base64

            account = f"{settings['XBOX_PORTAL_USERNAME']}:{settings.get('XBOX_PORTAL_PASSWORD', '')}"
            self.portal.authorization = 'Basic ' + base64.b64encode(account.encode()).decode()
        self.error = PortalError
        self.url = lambda name: game_file_url({}, folder, name, 'DevelopmentFiles')
        self.sizes = {}
        self.log = []

    def fetch(self, name, offset, length):
        require(REMOTE_NAME.fullmatch(name) and 0 < length <= MAX_REQUEST and offset >= 0,
                'Invalid bounded Xbox request.')
        end = offset + length - 1

        def receive(response):
            match = re.fullmatch(rf'bytes {offset}-{end}/(\d+)', response.getheader('Content-Range') or '')
            require(response.status == 206 and match, 'Unexpected Xbox byte range.')
            size = int(match[1])
            require(self.sizes.setdefault(name, size) == size, f'{name} changed size while it was read.')
            data = response.read(length + 1)
            require(len(data) == length, 'Short Xbox archive read.')
            return data

        for attempt in range(3):
            try:
                data = self.portal.request('GET', self.url(name), headers={'Range': f'bytes={offset}-{end}'},
                                           receive=receive)[1]
                self.log.append((name, offset, offset + length))
                return data
            except (OSError, self.error):
                if attempt == 2:
                    raise


class RemoteArchive:
    """A DSAR read through the range cache, then Device Portal, with block reads merged into spans."""

    def __init__(self, name, codec, cache, portal):
        self.name, self.cache, self.portal, self.spans = name, cache, portal, []
        header = self.read_at(0, MIP['DSAR_HEADER'].size, exact=True)
        count, data_start = struct.unpack_from('<II', header, 8)
        require(0 < count <= 262144 and data_start >= 32 + 32 * count, f'Invalid {name} header.')
        self.read_at(0, 32 + 32 * count, exact=True)
        size = portal.sizes.get(name) if portal else None
        self.archive = MIP['Archive'](self.read_at, codec, size if size is not None else 2 ** 63)

    def plan(self, ranges):
        blocks = set()
        archive = self.archive
        for offset, length in ranges:
            first = bisect_right(archive.starts, offset) - 1
            last = bisect_right(archive.starts, offset + length - 1) - 1
            require(0 <= first <= last < len(archive.blocks), f'Recipe range exceeds {self.name}.')
            blocks.update(range(first, last + 1))
        spans = []
        for start, stop in sorted((archive.blocks[i][1], archive.blocks[i][1] + archive.blocks[i][3]) for i in blocks):
            if self.cache.read(self.name, start, stop - start) is not None:
                continue
            if spans and start <= spans[-1][1] + 64 * 1024 and stop - spans[-1][0] <= MAX_REQUEST:
                spans[-1] = spans[-1][0], max(spans[-1][1], stop)
            else:
                spans.append((start, stop))
        self.spans = spans
        return sum(stop - start for start, stop in spans), len(spans)

    def read_at(self, offset, count, exact=False):
        data = self.cache.read(self.name, offset, count)
        if data is not None:
            return data
        require(self.portal is not None, f'{self.name} bytes {offset}-{offset + count} are not cached; '
                'supply --portal-env.')
        start, stop = offset, offset + count
        for span in self.spans:
            if not exact and span[0] <= offset and offset + count <= span[1]:
                start, stop = span
                break
        data = self.portal.fetch(self.name, start, stop - start)
        self.cache.store(self.name, start, data)
        return data[offset - start:offset - start + count]


class LocalArchive:
    def __init__(self, path, codec):
        self.file = path.open('rb')
        self.archive = MIP['Archive'](lambda at, count: MIP['read_at'](self.file, at, count), codec,
                                      path.stat().st_size)

    def plan(self, ranges):
        return 0, 0


def collect_plan(recipes, original, reference_added, reference_routes, toc_count):
    """Match each recipe row to its TOC asset through the reference routes."""
    plan = []
    for recipe in recipes:
        slot = toc_count + reference_added.index(recipe.name)
        routed = {(offset, size): item for item, (target, offset, size) in reference_routes.items() if target == slot}
        rows = []
        keys = set()
        for row in recipe.rows:
            keys.add((row.output_offset, row.output_size))
            item = routed.get((row.output_offset, row.output_size))
            if item is not None:
                archive, offset, size = original.route(item)
                require((original.archives[archive], offset, size) == (row.archive, row.offset, row.size),
                        f'{recipe.name} source differs from the TOC route of asset {item}.')
            rows.append((row, item))
        require(routed.keys() <= keys, f'{recipe.name} has reference routes without recipe rows.')
        plan.append((recipe, rows))
    items = [item for _, rows in plan for _, item in rows if item is not None]
    require(len(items) == len(set(items)), 'An asset is routed to more than one restore archive.')
    return plan


def generate(plan, open_archive):
    """Yield (recipe, [(row, item, data)], logical SHA-256) with every hash checked."""
    for recipe, rows in plan:
        logical, position, assets = sha256(), 0, []
        for number, (row, item) in enumerate(rows):
            source = open_archive(row.archive).read(row.offset, row.size)
            require(len(source) == row.size and digest(source) == row.sha256,
                    f'{recipe.name} row {number}: owned source differs from its fingerprint.')
            hd = None
            if row.hd_archive:
                hd = open_archive(row.hd_archive).read(row.hd_offset, row.hd_size)
                require(len(hd) == row.hd_size and digest(hd) == row.hd_sha256,
                        f'{recipe.name} row {number}: owned HD chain differs from its fingerprint.')
            changed = restore_texture(bytes(source), None if hd is None else bytes(hd), row.maximum)
            require(len(changed) == row.output_size and digest(changed) == row.output_sha256,
                    f'{recipe.name} row {number}: generated texture differs from its fingerprint.')
            logical.update(bytes(row.output_offset - position))
            logical.update(changed)
            position = row.output_offset + row.output_size
            assets.append((row, item, changed))
        yield recipe, assets, logical.hexdigest()


def file_digest(path):
    value = sha256()
    with path.open('rb') as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def write_archive(path, codec, size, pieces):
    """Write logical ``pieces`` [(offset, bytes or callable)] with zero padding between them."""
    with path.open('xb') as destination:
        writer = MIP['ArchiveWriter'](destination, codec, size)
        position = 0
        for offset, data in pieces:
            require(offset >= position, 'Overlapping archive pieces.')
            writer.write(bytes(offset - position))
            position = offset
            for chunk in (data() if callable(data) else (data,)):
                writer.write(chunk)
                position += len(chunk)
        writer.write(bytes(size - position))
        writer.finish()


def reread(path, codec):
    file = path.open('rb')
    return file, MIP['Archive'](lambda at, count: MIP['read_at'](file, at, count), codec, path.stat().st_size)


def logical_digest(archive, length):
    value = sha256()
    for offset in range(0, length, 4 * 1024 * 1024):
        value.update(archive.read(offset, min(4 * 1024 * 1024, length - offset)))
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('original', type=Path, help='owned asset_archive directory with the original toc')
    parser.add_argument('recipes', type=Path, help='reference asset-preparation/recipes directory')
    parser.add_argument('output', type=Path, help='new directory for the generated TOC and archives')
    parser.add_argument('--base-toc', type=Path, required=True,
                        help='overlay TOC to extend, derived from the original (for example the trial TOC)')
    parser.add_argument('--append-to', type=Path,
                        help='overlay archive named in --base-toc to extend instead of adding new archives')
    parser.add_argument('--recipe', action='append', default=[], help='restore archive name to include (repeatable)')
    parser.add_argument('--reference-toc', choices=('asset-index.tsv', 'runtime-toc.tsv'), default='asset-index.tsv',
                        help='reference route recipe to follow; base overlay routes it leaves original are reverted')
    parser.add_argument('--revert-asset', type=int, action='append', default=[],
                        help='return this base overlay route to its original archive (repeatable)')
    parser.add_argument('--revert-reference-archive', action='append', default=[],
                        help='return every asset the reference routes to this archive to its original route')
    parser.add_argument('--portal-env', type=Path, help='Device Portal settings for archives missing locally')
    parser.add_argument('--remote-archives', action='store_true',
                        help='read every source archive through the cache and Device Portal; use only the local toc')
    parser.add_argument('--remote-folder', default=REMOTE_FOLDER, help='asset_archive path under DevelopmentFiles')
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
        overlay_routes = base.require_derived_from(original)
        print(f'Base TOC {digest(args.base_toc.read_bytes())} keeps the original routes and adds '
              f'{overlay_routes:,} overlay routes in {base.archives[len(original.archives):]}.', flush=True)

        batch = load_batch_manifest(args.recipes / 'batch.tsv', TOC_SHA256)
        other = 'runtime-toc.tsv' if args.reference_toc == 'asset-index.tsv' else 'asset-index.tsv'
        added, routes = load_reference_routes(args.recipes / args.reference_toc, TOC_SHA256, len(original.archives))
        runtime_added, runtime = load_reference_routes(args.recipes / other, TOC_SHA256, len(original.archives))
        require(added == runtime_added, 'Reference TOC recipes list different archives.')
        names = args.recipe or list(RESTORE_RECIPES)
        require(set(names) <= set(RESTORE_RECIPES) and len(set(names)) == len(names), 'Unknown restore recipe.')
        for name in names:
            slot = len(original.archives) + added.index(name)
            mine = {item: route for item, route in routes.items() if route[0] == slot}
            require(mine == {item: route for item, route in runtime.items() if route[0] == slot},
                    f'Reference TOC recipes route {name} differently.')
        recipes = [load_restore_recipe(args.recipes / f'{name}.tsv', name, TOC_SHA256) for name in names]
        for recipe in recipes:
            require(batch.get(recipe.name, (None,))[0] == recipe.total, f'{recipe.name} size differs from batch.tsv.')
        plan = collect_plan(recipes, original, added, routes, len(original.archives))

        restored = {item for _, rows in plan for _, item in rows if item is not None}
        overridden = {}
        for item in restored:
            target = base.route(item)[0]
            if target >= len(original.archives):
                overridden[base.archives[target]] = overridden.get(base.archives[target], 0) + 1
        for name, count in sorted(overridden.items()):
            print(f'Restore routes replace {count:,} base routes into {name}.', flush=True)
        # The reference's runtime-toc.tsv leaves some authored-LOD vehicle models on their original routes.
        require(set(args.revert_reference_archive) <= set(added), 'Unknown --revert-reference-archive.')
        dropped = {len(original.archives) + added.index(name) for name in args.revert_reference_archive}
        reverted = {}
        for item in range(base.count):
            if base.route(item)[0] >= len(original.archives) and (
                    item not in routes or routes[item][0] in dropped or item in args.revert_asset):
                reverted[item] = original.route(item)
        require(set(args.revert_asset) <= reverted.keys(), '--revert-asset names an asset without an overlay route.')
        for item in sorted(reverted) if len(reverted) <= 32 else ():
            target = base.archives[base.route(item)[0]]
            reason = (f'{args.reference_toc} keeps it original' if item not in routes else 'requested')
            print(f'Asset {item}: {reason}, so its {target} route is reverted.', flush=True)
        if len(reverted) > 32:
            print(f'Reverted {len(reverted):,} base overlay routes to their original archives '
                  f'({sum(route[2] for route in reverted.values()):,} original bytes).', flush=True)
        for recipe, rows in plan:
            routed = sum(item is not None for _, item in rows)
            note = '' if routed else '; the reference routes none of its assets here, so it is verified only'
            print(f'{recipe.name}: {len(rows)} rows, {routed} routed{note}.', flush=True)

        codec = MIP['Lz4'](args.lz4)
        cache = RangeCache(args.cache)
        portal = PortalFiles(args.portal_env, args.remote_folder) if args.portal_env else None
        archives = {}

        def open_archive(name):
            if name not in archives:
                path = args.original / name
                archives[name] = (LocalArchive(path, codec) if path.is_file() and not args.remote_archives
                                  else RemoteArchive(name, codec, cache, portal))
                opened.append(getattr(archives[name], 'file', None))
            return archives[name].archive

        reads = {}
        for _, rows in plan:
            for row, _ in rows:
                reads.setdefault(row.archive, []).append((row.offset, row.size))
                if row.hd_archive:
                    reads.setdefault(row.hd_archive, []).append((row.hd_offset, row.hd_size))
        for name in sorted(reads):
            open_archive(name)
            fetch, spans = archives[name].plan(reads[name])
            if spans:
                print(f'{name}: {fetch:,} uncached bytes in {spans} Xbox reads.', flush=True)

        results = []
        for recipe, assets, logical in generate(plan, open_archive):
            require((recipe.total, logical) == batch[recipe.name],
                    f'{recipe.name} logical archive differs from batch.tsv.')
            print(f'{recipe.name}: verified {len(assets)} rows and archive SHA-256 {logical}.', flush=True)
            results.append((recipe, assets, logical))

        with tempfile.TemporaryDirectory(dir=output.parent, prefix='.restore-') as temporary:
            staged = Path(temporary) / 'assets'
            staged.mkdir()
            new_routes, new_archives, files = {}, [], []
            if args.append_to:
                name = args.append_to.name
                require(name in base.archives[len(original.archives):], '--append-to is not an overlay archive in the base TOC.')
                slot = base.archives.index(name)
                source_file, source = reread(args.append_to, codec)
                opened.append(source_file)
                for item in range(base.count):
                    target, offset, size = base.route(item)
                    if target == slot and item not in restored:
                        require(offset + size <= source.length, f'Base route {item} exceeds {name}.')
                prefix = logical_digest(source, source.length)
                position, pieces = align16(source.length), []

                def base_chunks():
                    for offset in range(0, source.length, 4 * 1024 * 1024):
                        yield source.read(offset, min(4 * 1024 * 1024, source.length - offset))

                pieces.append((0, base_chunks))
                for _, assets, _ in results:
                    for row, item, data in assets:
                        if item is None:
                            continue
                        pieces.append((position, data))
                        new_routes[item] = slot, position, len(data)
                        position = align16(position + len(data))
                require(new_routes, 'No restored texture is routed by the reference TOC.')
                size = max(offset + len(data) for offset, data in pieces[1:])
                write_archive(staged / name, codec, size, pieces)
                check_file, check = reread(staged / name, codec)
                opened.append(check_file)
                require(logical_digest(check, source.length) == prefix, f'{name} changed its existing content.')
                print(f'{name}: kept {source.length:,} existing logical bytes (SHA-256 {prefix}) and appended '
                      f'{len(new_routes)} textures, {size - source.length:,} bytes.', flush=True)
                files.append(name)
            else:
                for recipe, assets, logical in results:
                    if not any(item is not None for _, item, _ in assets):
                        continue
                    slot = len(base.archives) + len(new_archives)
                    write_archive(staged / recipe.name, codec, recipe.total,
                                  [(row.output_offset, data) for row, _, data in assets])
                    check_file, check = reread(staged / recipe.name, codec)
                    opened.append(check_file)
                    require(check.length == recipe.total and logical_digest(check, recipe.total) == logical,
                            f'{recipe.name} failed reread validation.')
                    for row, item, data in assets:
                        if item is not None:
                            new_routes[item] = slot, row.output_offset, row.output_size
                    new_archives.append(recipe.name)
                    files.append(recipe.name)
            require(not reverted.keys() & new_routes.keys(), 'A reverted asset is also restored.')
            toc_bytes = base.encode(new_archives, {**reverted, **new_routes})
            result = Toc(toc_bytes)
            result.require_derived_from(original)
            outputs = {}
            for name in files:
                file, archive = reread(staged / name, codec)
                opened.append(file)
                outputs[result.archives.index(name)] = archive
            expected = {item: row.output_sha256 for _, assets, _ in results for row, item, _ in assets
                        if item is not None}
            for item in range(result.count):
                route = result.route(item)
                if item in new_routes:
                    require(route == new_routes[item], f'Generated TOC misroutes asset {item}.')
                    archive = outputs[route[0]]
                    require(digest(archive.read(route[1], route[2])) == expected[item],
                            f'Generated archive returns a different texture for asset {item}.')
                elif item in reverted:
                    require(route == original.route(item), f'Generated TOC did not revert asset {item}.')
                else:
                    require(route == base.route(item), f'Generated TOC changed unrelated asset {item}.')
            (staged / 'toc').write_bytes(toc_bytes)
            files.append('toc')
            lines = [f'{name}\t{(staged / name).stat().st_size}\t{file_digest(staged / name)}' for name in files]
            (staged / 'prepared-files.tsv').write_text('\n'.join(lines) + '\n')
            if portal:
                (staged / 'remote-ranges.tsv').write_text(
                    ''.join(f'{name}\t{start}\t{stop}\n' for name, start, stop in portal.log))
            for file in opened:
                if file:
                    file.close()
            opened.clear()
            staged.rename(output)
        print(f'Prepared {len(new_routes)} restored textures for {len(results)} recipes in {output}.', flush=True)
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
