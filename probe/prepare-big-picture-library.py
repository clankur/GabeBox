#!/usr/bin/env python3
"""Add the WebView2 Big Picture library to the verified Series X FHD release."""
import argparse
import hashlib
from pathlib import Path, PureWindowsPath
import re
import runpy
import shutil
from tempfile import TemporaryDirectory
from urllib.parse import unquote
from xml.etree import ElementTree
from zipfile import ZipFile


PROBE = Path(__file__).resolve().parent
BASE_SHA256 = "4a158f27326f584130ba6fb45ed7c27dff185529753746b68ea6c372101df4fe"
STEAM_CORE = runpy.run_path(str(PROBE / "prepare-steam-core.py"))
FOOTPRINT = STEAM_CORE["APPX_FOOTPRINT"]
stage_client = runpy.run_path(str(PROBE.parent / "analysis/stage-pinned-client.py"))["stage_client"]


def release_layout(base, destination, version):
    with base.open("rb") as source:
        digest = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
        if digest.hexdigest() != BASE_SHA256:
            raise ValueError("Use the verified, signed Steam Core FHD 1.0.230.0 APPX.")
        source.seek(0)
        with ZipFile(source) as package:
            for entry in package.infolist():
                # OPC escapes file names, e.g. SlayConsoleDiagnostic%241.class.
                name = PureWindowsPath(unquote(entry.filename))
                if name.anchor or ".." in name.parts or not name.parts:
                    raise ValueError(f"Invalid APPX member {entry.filename!r}")
                if name.as_posix().lower() in FOOTPRINT or entry.is_dir():
                    continue
                target = destination.joinpath(*name.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                with package.open(entry) as member, target.open("xb") as output:
                    shutil.copyfileobj(member, output)

    manifest = destination / "AppxManifest.xml"
    original = manifest.read_bytes()
    identity = ElementTree.fromstring(original).find("{*}Identity")
    if (identity is None or identity.get("Version") != "1.0.230.0"
            or not re.fullmatch(r"(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*)){3}", version)):
        raise ValueError("Use a four-part package version greater than 1.0.230.0.")
    parts = tuple(map(int, version.split(".")))
    if parts <= (1, 0, 230, 0) or max(parts) > 65535:
        raise ValueError("Invalid upgrade version for the FHD release.")
    manifest.write_bytes(original.replace(b'Version="1.0.230.0"', f'Version="{version}"'.encode(), 1))


def prepare_layout(base, host_artifact, layout, version, archive_cache, session_artifact, session_checksum,
                   presenter_artifact=None, presenter_checksum=None, primary_xaml=None, primary_checksum=None):
    if (primary_xaml is None) != (primary_checksum is None):
        raise ValueError("Supply both --primary-xaml and --primary-checksum.")
    if (presenter_artifact is None) != (presenter_checksum is None):
        raise ValueError("Supply both --presenter-artifact and --presenter-checksum.")
    if layout.exists():
        raise FileExistsError(f"Use a new layout directory: {layout}")
    with TemporaryDirectory(dir=layout.parent) as temp:
        staged = Path(temp) / "layout"
        staged.mkdir()
        release_layout(base, staged, version)
        if presenter_artifact is not None:
            STEAM_CORE["stage_native_presenter"](staged, presenter_artifact, presenter_checksum)
        stage_session = runpy.run_path(str(PROBE / "big-picture-bridge/stage-session.py"))["stage_session_component"]
        stage_session(staged, session_artifact, session_checksum)
        for name in ("xbox-system-mode.js", "steam-signin.js", "steam-signin.css", "steam-game-launch.js", "steam-sdl-host.js",
                     "steam-sdl-native-view.js", "steam-big-picture-broker.js", "xcom-dos.js"):
            shutil.copyfile(PROBE / "signin" / name, staged / name)
        shutil.copyfile(staged / "xcom-dos.js", staged / "xcom-runtime/xcom-dos.js")

        add_host = runpy.run_path(str(PROBE / "big-picture-webview/stage.py"))["add_webview_host"]
        add_host(staged, host_artifact, game_resources=True)
        page = staged / "steam-signin.html"
        original = page.read_bytes()
        html = b'<html lang="en">'
        marker = b'<script src="steam-signin.js"></script>'
        if original.count(marker) != 1 or original.count(html) != 1:
            raise ValueError("Unexpected Steam sign-in entry page in the release.")
        original = original.replace(html, b'<html lang="en" data-steam-big-picture="true">')
        page.write_bytes(original.replace(marker, b'<script src="xbox-system-mode.js"></script>\n'
                                          b'    <script src="steam-big-picture-broker.js"></script>\n'
                                          b'    <script src="big-picture-webview.js"></script>\n    ' + marker))

        stage_client(staged / "ValveClient", archive_cache, ui_only=True, include_url_catalog=True)
        shell = staged / "ValveClient/steamui/xbox"
        shell.mkdir()
        bundle = runpy.run_path(str(PROBE / "big-picture-bridge/bundle.py"))["bundle"]
        (shell / "bridge.js").write_text(bundle(staged / "ValveClient/public/url_list.txt"), encoding="utf-8")
        if primary_xaml is not None:
            stage_primary = runpy.run_path(str(PROBE / "big-picture-primary/stage.py"))["stage_primary"]
            stage_primary(staged, primary_xaml, primary_checksum)
        staged.rename(layout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path, help="signed regular FHD 1.0.230.0 APPX")
    parser.add_argument("host_artifact", type=Path, help="built x64 Big Picture WebView2 host directory")
    parser.add_argument("layout", type=Path, help="new output directory")
    parser.add_argument("--version", required=True)
    parser.add_argument("--archive-cache", type=Path)
    parser.add_argument("--session-artifact", type=Path, required=True)
    parser.add_argument("--session-checksum", required=True, help="SHA256 of the Windows build SHA256SUMS file")
    parser.add_argument("--presenter-artifact", type=Path, help="optional Windows FHD native game presenter directory")
    parser.add_argument("--presenter-checksum", help="SHA256 of the presenter artifact SHA256SUMS file")
    parser.add_argument("--primary-xaml", type=Path, help="optional Windows native primary XAML host artifact")
    parser.add_argument("--primary-checksum", help="SHA256 of the XAML host artifact SHA256SUMS file")
    args = parser.parse_args()
    prepare_layout(args.base, args.host_artifact, args.layout, args.version, args.archive_cache,
                   args.session_artifact, args.session_checksum, args.presenter_artifact, args.presenter_checksum,
                   args.primary_xaml, args.primary_checksum)
