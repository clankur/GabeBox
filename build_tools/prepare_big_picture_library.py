#!/usr/bin/env python3
"""Add the WebView2 Big Picture library to the verified Series X FHD release."""
import argparse
import hashlib
from pathlib import Path, PureWindowsPath
import re
import shutil
from tempfile import TemporaryDirectory
from urllib.parse import unquote
from xml.etree import ElementTree
from zipfile import ZipFile

from build_tools.appx import APPX_FOOTPRINT
from build_tools.bundle_big_picture import bundle
from build_tools.paths import BRAND_ASSETS, DOS_RUNTIME, SIGNIN
from build_tools.presenter import stage_native_presenter
from build_tools.stage_mesa_uwp import stage_mesa_runtime
from build_tools.stage_pinned_client import stage_client
from build_tools.stage_primary import stage_primary
from build_tools.stage_profiles import stage_legacy_pointer_mode, stage_surfaceless_profiles, stage_win32_trial_profiles
from build_tools.stage_session import open_session_artifact, stage_session_component, stage_stardew_startup_hook
from build_tools.stage_webview import add_webview_host
from tools.artifacts import load_sdl_artifact


BASE_SHA256 = "4a158f27326f584130ba6fb45ed7c27dff185529753746b68ea6c372101df4fe"
VERIFIED_BASES = {
    BASE_SHA256: "1.0.230.0",
    # The verified recovery package preserves the 1.0.230.0 payload.
    "174a603ec8d3f5452f3474e564c53f739777359f65d240aed4a93fb56541d767": "1.0.231.0",
    # Reassembled after the 1.0.230.0 APPX was lost: Steam Core template from Build Xbox probe run
    # 45550462937 with the pinned Valve client, FHD presenter, js-dos and SDL 2.30.9, plus the
    # pinned Mesa UWP 34732bb and DXIL runtime carried over from a signed 1.0.230.40 build.
    "b048d049b0c07b0bba3c5b94d0801a50d972217cea512c2b6452a90f22617396": "1.0.232.60",
}
BRAND_NAME = "GabeBox"
BRAND_DESCRIPTION = "Steam Big Picture on Xbox"
BRAND_BACKGROUND = "#0B0B0B"
BRAND_FILES = ("Logo.png", "SmallLogo.png", "Splash.png", "StoreLogo.png")


def release_layout(base, destination, version, *, verified_bases=None):
    pins = VERIFIED_BASES if verified_bases is None else verified_bases
    with base.open("rb") as source:
        digest = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
        source_version = pins.get(digest.hexdigest())
        if source_version is None:
            raise ValueError("Use the verified Steam Core FHD base release or its signed recovery package.")
        source.seek(0)
        with ZipFile(source) as package:
            for entry in package.infolist():
                # OPC escapes file names, e.g. SlayConsoleDiagnostic%241.class.
                name = PureWindowsPath(unquote(entry.filename))
                if name.anchor or ".." in name.parts or not name.parts:
                    raise ValueError(f"Invalid APPX member {entry.filename!r}")
                if name.as_posix().lower() in APPX_FOOTPRINT or entry.is_dir():
                    continue
                target = destination.joinpath(*name.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                with package.open(entry) as member, target.open("xb") as output:
                    shutil.copyfileobj(member, output)

    manifest = destination / "AppxManifest.xml"
    original = manifest.read_bytes()
    identity = ElementTree.fromstring(original).find("{*}Identity")
    if (identity is None or identity.get("Version") != source_version
            or not re.fullmatch(r"(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*)){3}", version)):
        raise ValueError("Use a four-part package version greater than 1.0.230.0.")
    parts = tuple(map(int, version.split(".")))
    if parts <= (1, 0, 230, 0) or max(parts) > 65535:
        raise ValueError("Invalid upgrade version for the FHD release.")
    manifest.write_bytes(original.replace(f'Version="{source_version}"'.encode(), f'Version="{version}"'.encode(), 1))


def stage_bridge(layout):
    steamui = layout / "ValveClient/steamui"
    shell = steamui / "xbox"
    shell.mkdir()
    movie_assets = steamui if (steamui / "movies/bigpicture_startup.webm").is_file() else None
    (shell / "bridge.js").write_text(bundle(layout / "ValveClient/public/url_list.txt",
        movie_probe_assets=movie_assets, local_cloud_cache_reads=True), encoding="utf-8")


def stage_storage_capability(layout):
    manifest = layout / "AppxManifest.xml"
    original = manifest.read_bytes()
    capabilities = ElementTree.fromstring(original).find("{*}Capabilities")
    name = "{http://schemas.microsoft.com/appx/manifest/uap/windows10}Capability"
    if capabilities is None or original.count(b"</Capabilities>") != 1:
        raise ValueError("The Steam package must declare application capabilities.")
    if capabilities.find(f"{name}[@Name='removableStorage']") is None:
        manifest.write_bytes(original.replace(b"</Capabilities>",
            b'<uap:Capability Name="removableStorage" /></Capabilities>', 1))


def stage_configured_play(layout, app_id):
    """Request one authenticated Big Picture Play for a locally installed game."""
    if app_id is None:
        return
    if type(app_id) is not int or not 1 <= app_id <= 0xffffff:
        raise ValueError("--play-installed-app-id requires a Steam AppID from 1 to 16777215.")
    (layout / "steam-launch-via-big-picture.txt").write_bytes(f"{app_id}\n".encode("ascii"))


def stage_branding(layout):
    """Show the Steam Core package as GabeBox without changing its identity or LocalState."""
    manifest = layout / "AppxManifest.xml"
    raw = manifest.read_bytes()
    replacements = (
        (rb"<DisplayName>[^<]*</DisplayName>", f"<DisplayName>{BRAND_NAME}</DisplayName>"),
        (rb'(<uap:VisualElements\b[^>]*?\s)DisplayName="[^"]*"', rf'\g<1>DisplayName="{BRAND_NAME}"'),
        (rb'(<uap:VisualElements\b[^>]*?\s)Description="[^"]*"', rf'\g<1>Description="{BRAND_DESCRIPTION}"'),
        (rb'(<uap:VisualElements\b[^>]*?\s)BackgroundColor="[^"]*"', rf'\g<1>BackgroundColor="{BRAND_BACKGROUND}"'),
    )
    for pattern, replacement in replacements:
        raw, count = re.subn(pattern, replacement.encode(), raw)
        if count != 1:
            raise ValueError("Expected one package display name and one application tile in the Steam manifest.")
    for name in BRAND_FILES:
        target = layout / "Assets" / name
        if not target.is_file():
            raise ValueError(f"The Steam package is missing its {name} tile asset.")
        shutil.copyfile(BRAND_ASSETS / name, target)
    manifest.write_bytes(raw)


def prepare_layout(base, host_artifact, layout, version, archive_cache, session_artifact, session_checksum,
                   presenter_artifact=None, presenter_checksum=None, primary_xaml=None, primary_checksum=None,
                   dwarf_sdl=None, shared_sdl=None, library_inventory=False, surfaceless_profiles=(),
                   win32_trial_profiles=(), migrate_legacy_library=False,
                   early_library_reveal=False, win32_source_profiles=(), play_installed_app_id=None,
                   mesa_runtime=None):
    if (primary_xaml is None) != (primary_checksum is None):
        raise ValueError("Supply both --primary-xaml and --primary-checksum.")
    if early_library_reveal and primary_xaml is None:
        raise ValueError("--early-library-reveal requires --primary-xaml.")
    if (presenter_artifact is None) != (presenter_checksum is None):
        raise ValueError("Supply both --presenter-artifact and --presenter-checksum.")
    if layout.exists():
        raise FileExistsError(f"Use a new layout directory: {layout}")
    with TemporaryDirectory(dir=layout.parent) as temp:
        staged = Path(temp) / "layout"
        staged.mkdir()
        release_layout(base, staged, version)
        if mesa_runtime is not None:
            # Without it the base package keeps its prebuilt Mesa UWP 34732bb runtime.
            artifact, digest = mesa_runtime
            stage_mesa_runtime(staged, Path(artifact), digest)
        if dwarf_sdl or shared_sdl:
            for profile, app_id in ((dwarf_sdl, 975370), (shared_sdl, 413150)):
                if not profile:
                    continue
                artifact, digest = profile
                data = load_sdl_artifact(Path(artifact), app_id)
                if hashlib.sha256(data).hexdigest() != digest:
                    raise ValueError(f"SDL runtime differs from its pinned Windows build for {app_id}.")
                target = staged
                if app_id == 975370:
                    target = staged / "DwarfRuntime"
                    target.mkdir()
                (target / "SDL2.dll").write_bytes(data)
                shutil.copyfile(Path(artifact) / "LICENSE.txt", target / "LICENSE.SDL2")
        if presenter_artifact is not None:
            stage_native_presenter(staged, presenter_artifact, presenter_checksum)
        stage_legacy_pointer_mode(staged)
        stage_surfaceless_profiles(staged, surfaceless_profiles)
        with open_session_artifact(session_artifact, session_checksum) as package:
            stage_session_component(staged, package)
            stage_stardew_startup_hook(staged, package)
            stage_win32_trial_profiles(staged, win32_trial_profiles, package, win32_source_profiles)
        for name in ("xbox-network.js", "xbox-system-mode.js", "steam-signin.js", "steam-signin.css", "steam-game-launch.js", "steam-cloud.js", "steam-sdl-host.js",
                     "steam-sdl-native-view.js", "steam-sdl-gamepad.js", "steam-big-picture-broker.js"):
            shutil.copyfile(SIGNIN / name, staged / name)
        shutil.copyfile(DOS_RUNTIME / "xcom-dos.js", staged / "xcom-dos.js")
        shutil.copyfile(staged / "xcom-dos.js", staged / "xcom-runtime/xcom-dos.js")

        add_webview_host(staged, host_artifact, game_resources=True)
        page = staged / "steam-signin.html"
        original = page.read_bytes()
        html = b'<html lang="en">'
        marker = b'<script src="steam-signin.js"></script>'
        if original.count(marker) != 1 or original.count(html) != 1:
            raise ValueError("Unexpected Steam sign-in entry page in the release.")
        original = original.replace(html, b'<html lang="en" data-steam-big-picture="true">')
        page.write_bytes(original.replace(marker, b'<script src="xbox-system-mode.js"></script>\n'
                                          b'    <script src="xbox-network.js"></script>\n'
                                          b'    <script src="steam-big-picture-broker.js"></script>\n'
                                          b'    <script src="big-picture-webview.js"></script>\n    ' + marker))

        stage_client(staged / "ValveClient", archive_cache, ui_only=True, include_url_catalog=True)
        stage_bridge(staged)
        if primary_xaml is not None:
            stage_primary(staged, primary_xaml, primary_checksum, early_library_reveal)
        if library_inventory:
            (staged / "steam-library-inventory.txt").touch()
        stage_storage_capability(staged)
        stage_branding(staged)
        if b"SteamLibraryCommand\x00" not in (staged / "XboxProbe.Native.winmd").read_bytes():
            raise ValueError("Use a Steam Core native component built with the Xbox storage interface.")
        if migrate_legacy_library:
            (staged / "steam-migrate-legacy-library.txt").touch()
        stage_configured_play(staged, play_installed_app_id)
        staged.rename(layout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path, help="verified signed regular FHD release or recovery APPX")
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
    parser.add_argument("--early-library-reveal", action="store_true",
                        help="trial earlier foreground reveal after serving the authenticated Steam library on primary XAML")
    parser.add_argument("--dwarf-sdl", nargs=2, metavar=("ARTIFACT", "DLL_SHA256"),
                        help="Windows SDL 2.26.2 artifact for Dwarf Fortress")
    parser.add_argument("--shared-sdl", nargs=2, metavar=("ARTIFACT", "DLL_SHA256"),
                        help="Windows SDL 2.30.9 artifact for Slay the Spire and Stardew Valley")
    parser.add_argument("--library-inventory", action="store_true",
                        help="export game names and AppIDs locally after Steam sign-in")
    parser.add_argument("--surfaceless-profile", action="append", default=[], metavar="APPID=BUILD_ID",
                        help="opt a verified Steam build into the shared SDL and Mesa presenter")
    parser.add_argument("--win32-trial-profile", action="append", default=[], metavar="APPID=DIRECTORY",
                        help="stage a local import compatibility trial from an audited recipe directory")
    parser.add_argument("--win32-source-profile", action="append", default=[], metavar="APPID=FILE",
                        help="check installed source fingerprints for these builds before Play")
    parser.add_argument("--migrate-legacy-library", action="store_true",
                        help="request a verified one-time move of existing app-local Steam games to Xbox internal storage")
    parser.add_argument("--play-installed-app-id", type=int, metavar="APPID",
                        help="run one authenticated Big Picture Play trial for a locally installed game")
    parser.add_argument("--mesa-runtime", nargs=2, metavar=("ARTIFACT", "SUMS_SHA256"),
                        help="replace the base package's prebuilt Mesa UWP runtime with a pinned "
                             "xbox-mesa-uwp workflow artifact")
    args = parser.parse_args()
    prepare_layout(args.base, args.host_artifact, args.layout, args.version, args.archive_cache,
                   args.session_artifact, args.session_checksum, args.presenter_artifact, args.presenter_checksum,
                   args.primary_xaml, args.primary_checksum, args.dwarf_sdl, args.shared_sdl, args.library_inventory,
                   args.surfaceless_profile, args.win32_trial_profile,
                   args.migrate_legacy_library, args.early_library_reveal, args.win32_source_profile,
                   args.play_installed_app_id, args.mesa_runtime)


if __name__ == "__main__":
    main()
