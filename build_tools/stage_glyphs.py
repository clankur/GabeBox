"""Serve Valve's release 1794536281 Steam Input glyphs from their native URL path."""
import hashlib
from pathlib import Path
import re
from tempfile import TemporaryDirectory
from zipfile import ZipFile


# runtime/steam/pins/steam_client_win64.manifest, public_all. SteamUI.dll maps
# steaminputglyphs/ to the SVGs in controller_base/images/api/knockout/.
PUBLIC_SHA256 = "0a2e5641b37e30084a457bd4e0d740f1e6c477ca779190fd6365b8f97041cb00"
MEMBER = re.compile(r"controller_base/images/api/knockout/([A-Za-z0-9_-]+\.svg)")


def stage_glyphs(steamui: Path, public_archive: Path):
    target = steamui / "steaminputglyphs"
    if target.exists():
        raise FileExistsError(f"Glyph directory already exists: {target}")
    with public_archive.open("rb") as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
        if digest.hexdigest() != PUBLIC_SHA256:
            raise ValueError("Unexpected public_all archive for Steam Input glyphs.")
        steamui.mkdir(parents=True, exist_ok=True)
        stream.seek(0)
        with ZipFile(stream) as archive, TemporaryDirectory(dir=steamui) as temp:
            staged = Path(temp) / "steaminputglyphs"
            staged.mkdir()
            for entry in archive.infolist():
                match = MEMBER.fullmatch(entry.filename)
                if match:
                    with (staged / match[1]).open("xb") as output:
                        output.write(archive.read(entry))
            if not all((staged / f"shared_button_{button}.svg").is_file() for button in ("a", "b")):
                raise ValueError("Missing Valve gamepad glyphs in public_all.")
            staged.rename(target)
