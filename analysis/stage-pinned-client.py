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


RELEASE = Path(__file__).with_name("valve-release.json")
MANIFEST = Path(__file__).with_name("steam_client_win64.manifest")


def extract_archive(archive, destination):
    # Valve ZIPs can mix Windows separators in directory entries with POSIX file paths.
    with ZipFile(archive) as contents:
        for member in contents.infolist():
            path = PureWindowsPath(member.filename)
            if not path.parts or path.anchor or ".." in path.parts:
                raise ValueError(f"Invalid Valve archive member: {member.filename!r}")
            target = destination.joinpath(*path.parts)
            if member.filename.endswith(("/", "\\")):
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with contents.open(member) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output)


def stage_client(destination, archive_cache=None):
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
                extract_archive(archive, staged)
        cache = staged / "package"
        cache.mkdir(exist_ok=True)
        (cache / MANIFEST.name).write_bytes(manifest)
        staged.rename(destination)
    print(f"Staged Valve Windows client manifest {pinned['version']}.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--archive-cache", type=Path,
                        help="offline directory of the content-addressed Valve ZIPs")
    args = parser.parse_args()
    stage_client(args.destination, args.archive_cache)
