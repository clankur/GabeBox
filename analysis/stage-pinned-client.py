#!/usr/bin/env python3
"""Stage the pinned Windows client from Valve's own archives in a scratch directory."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
from tempfile import TemporaryDirectory
from urllib.parse import urljoin
from urllib.request import urlopen
from zipfile import ZipFile


def stage_client(destination):
    pinned = json.loads(Path(__file__).with_name("valve-release.json").read_text())
    url = pinned["manifest_url"]
    with urlopen(url, timeout=60) as source:
        manifest = source.read()
    if hashlib.sha256(manifest).hexdigest() != pinned["manifest_sha256"]:
        raise ValueError("Valve's CDN no longer serves the pinned manifest.")

    # The pinned manifest pairs each ZIP filename with its exact size and SHA-256.
    archives = re.findall(
        rb'"file"\s+"([a-z0-9_]+\.zip\.[0-9a-f]{40})"\s+'
        rb'"size"\s+"(\d+)"\s+"sha2"\s+"([0-9a-f]{64})"',
        manifest,
    )
    if not archives:
        raise ValueError("No client archives in the pinned manifest.")
    destination.mkdir()
    with TemporaryDirectory(dir=destination.parent) as temp:
        def download(entry):
            name, size, checksum = (field.decode("ascii") for field in entry)
            if name.startswith(("steam_win64_steamchina.", "steam_win64.zip.")):
                return None  # Use the explicit worldwide build, once.
            archive = Path(temp) / name
            with urlopen(urljoin(url, name), timeout=60) as source, archive.open("wb") as output:
                shutil.copyfileobj(source, output)
            with archive.open("rb") as source:
                digest = hashlib.file_digest(source, "sha256").hexdigest()
            if archive.stat().st_size != int(size) or digest != checksum:
                raise ValueError(f"Archive did not match the manifest: {name}")
            return archive

        with ThreadPoolExecutor(max_workers=6) as executor:
            for archive in executor.map(download, archives):
                if archive:
                    with ZipFile(archive) as contents:
                        contents.extractall(destination)
    cache = destination / "package"
    cache.mkdir(exist_ok=True)
    (cache / "steam_client_win64.manifest").write_bytes(manifest)
    print(f"Staged Valve Windows client manifest {pinned['version']}.")


if __name__ == "__main__":
    stage_client(Path(sys.argv[1]))
