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
VERIFIED_BASES = {
    BASE_SHA256: "1.0.230.0",
    # The verified recovery package preserves the 1.0.230.0 payload.
    "174a603ec8d3f5452f3474e564c53f739777359f65d240aed4a93fb56541d767": "1.0.231.0",
}
STEAM_CORE = runpy.run_path(str(PROBE / "prepare-steam-core.py"))
FOOTPRINT = STEAM_CORE["APPX_FOOTPRINT"]
stage_client = runpy.run_path(str(PROBE.parent / "analysis/stage-pinned-client.py"))["stage_client"]


def release_layout(base, destination, version):
    with base.open("rb") as source:
        digest = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
        source_version = VERIFIED_BASES.get(digest.hexdigest())
        if source_version is None:
            raise ValueError("Use the verified Steam Core FHD base release or its signed recovery package.")
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
    if (identity is None or identity.get("Version") != source_version
            or not re.fullmatch(r"(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*)){3}", version)):
        raise ValueError("Use a four-part package version greater than 1.0.230.0.")
    parts = tuple(map(int, version.split(".")))
    if parts <= (1, 0, 230, 0) or max(parts) > 65535:
        raise ValueError("Invalid upgrade version for the FHD release.")
    manifest.write_bytes(original.replace(f'Version="{source_version}"'.encode(), f'Version="{version}"'.encode(), 1))


def stage_surfaceless_profiles(layout, profiles):
    pairs = {}
    for entry in profiles:
        match = re.fullmatch(r"([1-9][0-9]{0,7})=([1-9][0-9]{0,19})", entry)
        if not match or int(match[1]) > 0xffffff or match[1] in pairs:
            raise ValueError(f"Expected one APPID=BUILD_ID entry per Steam game: {entry}")
        pairs[match[1]] = match[2]
    if not pairs:
        return
    if any(not (layout / dll).is_file() for dll in ("SDL2.dll", "opengl32.dll", "libEGL.dll", "dxil.dll")):
        raise ValueError("The surfaceless Steam profile needs the shared SDL and Mesa runtime.")
    page = layout / "steam-signin.html"
    original = page.read_bytes()
    marker = rb'(<script id="steam-sdl-profiles" data-apps=")([0-9 ]+)(" src="steam-sdl-host\.js"></script>)'
    matches = list(re.finditer(marker, original))
    if len(matches) != 1:
        raise ValueError("The package must contain one SDL game profile list.")
    match = matches[0]
    existing = match[2].decode("ascii").split()
    if any(app in existing for app in pairs):
        raise ValueError("A surfaceless profile cannot replace an existing SDL host profile.")
    for app, build in pairs.items():
        (layout / f"steam-sdl-surfaceless-{app}.txt").write_text(build, encoding="ascii")
    page.write_bytes(original[:match.start(2)] + " ".join([*existing, *pairs]).encode("ascii") + original[match.end(2):])


def stage_win32_trial_profiles(layout, profiles, runtime):
    if not profiles and runtime is None:
        return
    if not profiles or runtime is None:
        raise ValueError("Supply the Win32 trial profiles and their Win32 runtime template together.")
    validate_recipe = runpy.run_path(str(PROBE / "redirect-pe-imports.py"))["validate_patch_recipe"]
    files = {}
    trace = False
    desktop_gl = False
    crash_reports = False
    desktop_dxgi = False
    for entry in profiles:
        app, separator, directory = entry.partition("=")
        if (not separator or not directory or not re.fullmatch(r"[1-9][0-9]{0,7}", app)
                or int(app) > 0xffffff or f"steam-win32-shadow-{app}.txt" in files):
            raise ValueError("Use one APPID=DIRECTORY entry per Win32 trial.")
        root = Path(directory)
        profile_name = f"steam-win32-shadow-{app}.txt"
        recipe_name = f"steam-win32-shadow-{app}.xwp"
        companion_name = f"steam-win32-files-{app}.txt"
        profile = (root / profile_name).read_bytes()
        fields = profile.decode("ascii").splitlines()
        if (not 2 <= len(fields) <= 7 or not re.fullmatch(r"[1-9][0-9]{0,19}", fields[0])
                or not re.fullmatch(r"[A-Za-z0-9 _-][A-Za-z0-9 ._-]{0,95}\.exe", fields[1])
                or (len(fields) >= 3 and fields[2] not in ("", "opengl3", "d3d12"))
                or (len(fields) >= 4 and fields[3] not in ("", "godot"))
                or (len(fields) >= 3 and fields[2] and fields[3:4] != ["godot"])
                or (len(fields) >= 5 and fields[4] and not re.fullmatch(
                    r"[A-Za-z0-9 _-][A-Za-z0-9 ._-]{0,95}\.(?:exe|dll)", fields[4]))
                or (len(fields) >= 6 and len(fields[5]) > 260)
                or (len(fields) >= 7 and fields[6] not in ("", "loader-trace"))):
            raise ValueError(f"Invalid Win32 trial profile for AppID {app}.")
        trace |= fields[6:7] == ["loader-trace"]
        desktop_gl |= fields[2:3] == ["opengl3"]
        recipe = (root / recipe_name).read_bytes()
        validate_recipe(recipe)
        files.update({profile_name: profile, recipe_name: recipe})
        argument_name = f"steam-win32-arguments-{app}.txt"
        if (root / argument_name).is_file():
            with (root / argument_name).open("rb") as source:
                raw = source.read(2051)
            arguments = raw.rstrip(b"\r\n")
            if len(raw) > 2050 or not re.fullmatch(rb"[ -~]{1,2048}", arguments):
                raise ValueError(f"Expected a single printable launch argument line for AppID {app}.")
            files[argument_name] = arguments
        crash_marker = f"steam-win32-crash-report-{app}.txt"
        if (root / crash_marker).is_file():
            if (root / crash_marker).stat().st_size:
                raise ValueError(f"Expected an empty local crash report marker for AppID {app}.")
            files[crash_marker] = b""
            crash_reports = True
        dxgi_marker = f"steam-win32-dxgi-{app}.txt"
        if (root / dxgi_marker).is_file():
            if (root / dxgi_marker).stat().st_size or any(fields[2:4]):
                raise ValueError(f"The DXGI marker requires the game's default native renderer for AppID {app}.")
            files[dxgi_marker] = b""
            desktop_dxgi = True
        for option in ("working-directory", "original-executable"):
            marker = f"steam-win32-{option}-{app}.txt"
            if (root / marker).is_file():
                if (root / marker).stat().st_size:
                    raise ValueError(f"Expected an empty {option} marker for AppID {app}.")
                files[marker] = b""
                trace = True
        companions = root / companion_name
        if companions.is_file():
            if companions.stat().st_size > 65536:
                raise ValueError(f"Win32 trial companion list is too large for AppID {app}.")
            data = companions.read_bytes()
            rows = data.decode("utf-8-sig").splitlines()
            for row in rows:
                image, tab, key = row.partition("\t")
                if not tab:
                    continue
                if not image.endswith((".exe", ".dll")) or not re.fullmatch(r"[a-z0-9-]{1,24}", key):
                    raise ValueError(f"Invalid companion recipe for AppID {app}.")
                name = f"steam-win32-shadow-{app}-{key}.xwp"
                if name not in files:
                    extra = (root / name).read_bytes()
                    validate_recipe(extra)
                    files[name] = extra
            files[companion_name] = data

    with ZipFile(runtime) as package:
        identity = ElementTree.fromstring(package.read("AppxManifest.xml")).find("{*}Identity")
        if (identity is None or identity.get("Name") != "XboxPortabilityProbe.ChildProcess.SteamCore"
                or identity.get("ProcessorArchitecture") != "x64"):
            raise ValueError("Use an x64 Steam Core runtime template built for this checkout.")
        for name in ("ProbeNative.dll", "XboxProbe.Native.winmd", "XboxWin32Input.dll", "XboxWin32Desktop.dll"):
            files[name] = package.read(name)
        if desktop_gl or desktop_dxgi:
            files["XboxWin32Window.dll"] = package.read("XboxWin32Window.dll")
        if desktop_dxgi:
            files["XboxWin32Dxgi.dll"] = package.read("XboxWin32Dxgi.dll")
        if crash_reports:
            files["XboxWin32CrashReport.dll"] = package.read("XboxWin32CrashReport.dll")
        if trace:
            files["XboxWin32LoaderTrace.dll"] = package.read("XboxWin32LoaderTrace.dll")
    for name, data in files.items():
        (layout / name).write_bytes(data)


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


def prepare_layout(base, host_artifact, layout, version, archive_cache, session_artifact, session_checksum,
                   presenter_artifact=None, presenter_checksum=None, primary_xaml=None, primary_checksum=None,
                   dwarf_sdl=None, shared_sdl=None, library_inventory=False, surfaceless_profiles=(),
                   win32_trial_profiles=(), win32_runtime_template=None, migrate_legacy_library=False):
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
        if dwarf_sdl or shared_sdl:
            load_sdl = runpy.run_path(str(PROBE.parent / "xbox-deploy.py"))["load_sdl_artifact"]
            for profile, app_id in ((dwarf_sdl, 975370), (shared_sdl, 413150)):
                if not profile:
                    continue
                artifact, digest = profile
                data = load_sdl(Path(artifact), app_id)
                if hashlib.sha256(data).hexdigest() != digest:
                    raise ValueError(f"SDL runtime differs from its pinned Windows build for {app_id}.")
                target = staged
                if app_id == 975370:
                    target = staged / "DwarfRuntime"
                    target.mkdir()
                (target / "SDL2.dll").write_bytes(data)
                shutil.copyfile(Path(artifact) / "LICENSE.txt", target / "LICENSE.SDL2")
        if presenter_artifact is not None:
            STEAM_CORE["stage_native_presenter"](staged, presenter_artifact, presenter_checksum)
        stage_session = runpy.run_path(str(PROBE / "big-picture-bridge/stage-session.py"))["stage_session_component"]
        stage_session(staged, session_artifact, session_checksum)
        for name in ("xbox-network.js", "xbox-system-mode.js", "steam-signin.js", "steam-signin.css", "steam-game-launch.js", "steam-sdl-host.js",
                     "steam-sdl-native-view.js", "steam-sdl-gamepad.js", "steam-big-picture-broker.js", "xcom-dos.js"):
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
                                          b'    <script src="xbox-network.js"></script>\n'
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
        if library_inventory:
            (staged / "steam-library-inventory.txt").touch()
        stage_surfaceless_profiles(staged, surfaceless_profiles)
        stage_win32_trial_profiles(staged, win32_trial_profiles, win32_runtime_template)
        stage_storage_capability(staged)
        if b"SteamLibraryCommand\x00" not in (staged / "XboxProbe.Native.winmd").read_bytes():
            raise ValueError("Use a Steam Core native component built with the Xbox storage interface.")
        if migrate_legacy_library:
            (staged / "steam-migrate-legacy-library.txt").touch()
        staged.rename(layout)


if __name__ == "__main__":
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
    parser.add_argument("--win32-runtime-template", type=Path,
                        help="Steam Core APPX template containing the Win32 trial runtime built for this checkout")
    parser.add_argument("--migrate-legacy-library", action="store_true",
                        help="request a verified one-time move of existing app-local Steam games to Xbox internal storage")
    args = parser.parse_args()
    prepare_layout(args.base, args.host_artifact, args.layout, args.version, args.archive_cache,
                   args.session_artifact, args.session_checksum, args.presenter_artifact, args.presenter_checksum,
                   args.primary_xaml, args.primary_checksum, args.dwarf_sdl, args.shared_sdl, args.library_inventory,
                   args.surfaceless_profile, args.win32_trial_profile, args.win32_runtime_template, args.migrate_legacy_library)
