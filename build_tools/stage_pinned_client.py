#!/usr/bin/env python3
"""Stage the pinned Windows client from Valve's own archives in a new directory."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path, PureWindowsPath
import re
import shutil
from tempfile import TemporaryDirectory
from urllib.parse import urljoin
from urllib.request import urlopen
from zipfile import ZipFile


from build_tools.paths import STEAM_PINS
from build_tools.stage_glyphs import stage_glyphs


RELEASE = STEAM_PINS / "valve-release.json"
MANIFEST = STEAM_PINS / "steam_client_win64.manifest"
UI_ARCHIVES = {b"steamui_websrc_all", b"steamui_websrc_sounds_all", b"steamui_websrc_movies_all"}


def extract_archive(archive, destination, ui_only=False):
    # Valve ZIPs can mix Windows separators in directory entries with POSIX file paths.
    with ZipFile(archive) as contents:
        for member in contents.infolist():
            path = PureWindowsPath(member.filename)
            if not path.parts or path.anchor or ".." in path.parts:
                raise ValueError(f"Invalid Valve archive member: {member.filename!r}")
            if ui_only and path.parts[0] != "steamui":
                raise ValueError(f"Unexpected file outside steamui: {member.filename!r}")
            target = destination.joinpath(*path.parts)
            if member.filename.endswith(("/", "\\")):
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with contents.open(member) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output)


def stage_client(destination, archive_cache=None, ui_only=False, include_url_catalog=False):
    if destination.exists():
        raise FileExistsError(f"Use a new client directory: {destination}")
    pinned = json.loads(RELEASE.read_text())
    manifest = MANIFEST.read_bytes()
    if hashlib.sha256(manifest).hexdigest() != pinned["manifest_sha256"]:
        raise ValueError("The archived Valve manifest differs from the pinned SHA-256.")

    archives = re.findall(
        rb'"file"\s+"([a-z0-9_]+\.zip\.[0-9a-f]{40})"\s+'
        rb'"size"\s+"(\d+)"\s+"sha2"\s+"([0-9a-f]{64})"',
        manifest,
    )
    if not archives or len(archives) != manifest.count(b'"file"'):
        raise ValueError("Incomplete archive list in the pinned manifest.")
    if ui_only:
        selected = UI_ARCHIVES | ({b"public_all"} if include_url_catalog else set())
        archives = [entry for entry in archives if entry[0].split(b".", 1)[0] in selected]
        if sorted(entry[0].split(b".", 1)[0] for entry in archives) != sorted(selected):
            raise ValueError("Incomplete or duplicated steamui archive set in the pinned manifest.")
    else:
        # Use the explicit worldwide bootstrapper, once.
        archives = [entry for entry in archives
                    if not entry[0].startswith((b"steam_win64_steamchina.", b"steam_win64.zip."))]
    with TemporaryDirectory(dir=destination.parent) as temp:
        def fetch(entry):
            name, size, checksum = (field.decode("ascii") for field in entry)
            archive = Path(temp) / name
            if archive_cache is None:
                source = urlopen(urljoin(pinned["manifest_url"], name), timeout=60)
            else:
                source = (archive_cache / name).open("rb")
            digest = hashlib.sha256()
            # Extract from the private, verified copy even if a shared cache changes.
            with source, archive.open("xb") as output:
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(chunk)
                    output.write(chunk)
            if archive.stat().st_size != int(size) or digest.hexdigest() != checksum:
                raise ValueError(f"Archive did not match the manifest: {name}")
            return archive

        staged = Path(temp) / "client"
        staged.mkdir()
        with ThreadPoolExecutor(max_workers=6) as executor:
            for archive in executor.map(fetch, archives):
                if ui_only and archive.name.startswith("public_all.zip."):
                    stage_glyphs(staged / "steamui", archive)
                    catalog = staged / "public/url_list.txt"
                    catalog.parent.mkdir()
                    with ZipFile(archive) as package:
                        catalog.write_bytes(package.read("public/url_list.txt"))
                else:
                    extract_archive(archive, staged, ui_only)
        if not ui_only:
            cache = staged / "package"
            cache.mkdir(exist_ok=True)
            (cache / MANIFEST.name).write_bytes(manifest)
        staged.rename(destination)
    payload = "UI" if ui_only else "Windows client"
    print(f"Staged Valve {payload} from manifest {pinned['version']}.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--ui-only", action="store_true",
                        help="stage the original steamui document root, sounds, and movies")
    parser.add_argument("--archive-cache", type=Path,
                        help="offline directory of the content-addressed Valve ZIPs")
    args = parser.parse_args()
    stage_client(args.destination, args.archive_cache, args.ui_only)


if __name__ == "__main__":
    main()
