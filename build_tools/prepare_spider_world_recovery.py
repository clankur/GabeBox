"""Reassemble the local, checksum-pinned Spider-Man world-entry trial."""

import argparse
from contextlib import contextmanager
import hashlib
from pathlib import Path
import re
import shutil
from tempfile import TemporaryDirectory
from zipfile import ZipFile

from build_tools.paths import ROOT
from build_tools.prepare_big_picture_library import release_layout


BASE_PIN = "ce8d0bfd526dc2c25dfa7aa5bfaf68ab7d316f7d9de959f14c7d4840d803d60f"
WORLD_PIN = "32f61750d4ac4049f49d7ae91598050caa166b706482f62df13472fd2d6a4dc7"
PRESENTER_PIN = "899cd8d1687df6dc7f21f37f4941dd9fedde483934f8246f868deed8d56042ae"
RELEASE_MARKER = "steam-win32-release-renderer-1817070.txt"
WORLD_MEMBERS = (
    "steam-win32-shadow-1817070.xwp", "steam-startup-snapshot-1817070.txt",
    "steam-startup-host-exit-trace.txt", "world-vs.dxil", "world-ps.dxil",
    "owned-compression-0.dxil", "owned-compression-1.dxil", "owned-compression-2.dxil", "owned-compression-3.dxil",
    "owned-mip-overlay/toc", "owned-mip-overlay/local_model1356_lod3_20260916.dsar",
)


@contextmanager
def verified_appx(path, pin):
    with path.open("rb") as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
        if digest.hexdigest() != pin:
            raise ValueError(f"Unexpected signed recovery input: {path}")
        stream.seek(0)
        with ZipFile(stream) as package:
            yield package


def copy_members(package, layout, names):
    for name in names:
        target = layout / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with package.open(name) as source, target.open("wb") as output:
            shutil.copyfileobj(source, output)


def prepare(base, world, presenter, layout, version, autoplay):
    if (not re.fullmatch(r"(?:0|[1-9][0-9]{0,4})(?:\.(?:0|[1-9][0-9]{0,4})){3}", version)
            or tuple(map(int, version.split("."))) <= (1, 0, 232, 13)):
        raise ValueError("Use a four-part package version greater than 1.0.232.13 and the installed version.")
    if layout.exists():
        raise FileExistsError(f"Use a new output layout: {layout}")
    with TemporaryDirectory(dir=layout.parent) as temporary:
        staged = Path(temporary) / "layout"
        staged.mkdir()
        release_layout(base, staged, version, verified_bases={BASE_PIN: "1.0.232.13"})
        with verified_appx(world, WORLD_PIN) as package:
            adapters = sorted(name for name in package.namelist() if re.fullmatch(r"XboxWin32[A-Za-z0-9]+\.dll", name))
            copy_members(package, staged, (*WORLD_MEMBERS, *adapters))
            if autoplay:
                copy_members(package, staged, ("steam-launch-via-big-picture.txt",))
        with verified_appx(presenter, PRESENTER_PIN) as package:
            copy_members(package, staged, ("Fhd.Native.dll", "Fhd.Native.winmd"))
        shutil.copyfile(ROOT / "profiles/1817070/23986256" / RELEASE_MARKER, staged / RELEASE_MARKER)
        shutil.copyfile(ROOT / "ui/big-picture-webview/big-picture-webview.js", staged / "big-picture-webview.js")
        if layout.exists():
            raise FileExistsError(f"Use a new output layout: {layout}")
        staged.rename(layout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path, help="verified post-rewrite 1.0.232.13 Steam Core APPX")
    parser.add_argument("world", type=Path, help="verified local 1.0.230.315 world-entry APPX")
    parser.add_argument("presenter", type=Path, help="verified local 1.0.230.323 presenter APPX")
    parser.add_argument("layout", type=Path, help="new output directory")
    parser.add_argument("--version", required=True, help="package version above the installed version")
    parser.add_argument("--autoplay", action="store_true", help="request one Big Picture Play trial after sign-in")
    args = parser.parse_args()
    prepare(args.base, args.world, args.presenter, args.layout, args.version, args.autoplay)


if __name__ == "__main__":
    main()
