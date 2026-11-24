"""Stage build-matched compatibility profiles in an Xbox package."""

import json
from pathlib import Path, PureWindowsPath
import re
from xml.etree import ElementTree
from zipfile import ZipFile

from build_tools.redirect_pe_imports import validate_patch_recipe
from build_tools.verify_win32_source import read_profile


HOSTED_SDL_SCRIPT = re.compile(
    rb'<script id="steam-sdl-profiles" data-apps="(?P<apps>[0-9 ]+)"'
    rb'(?: data-pointer-apps="(?P<pointer_apps>[0-9 ]+)")? src="steam-sdl-host\.js"></script>'
)


def hosted_sdl_script(apps, pointer_apps=()):
    pointer = f' data-pointer-apps="{" ".join(pointer_apps)}"' if pointer_apps else ""
    return f'<script id="steam-sdl-profiles" data-apps="{" ".join(apps)}"{pointer} src="steam-sdl-host.js"></script>'


def stage_legacy_pointer_mode(layout):
    # The hash-pinned FHD base predates explicit input modes. Its original
    # Dwarf Fortress host profile uses pointer input.
    app = "975370"
    if not (layout / f"steam-sdl-host-{app}.txt").is_file():
        return
    page = layout / "steam-signin.html"
    original = page.read_bytes()
    matches = list(HOSTED_SDL_SCRIPT.finditer(original))
    if len(matches) != 1:
        raise ValueError("The legacy SDL host profile needs one hosted game list.")
    match = matches[0]
    apps = match["apps"].decode("ascii").split()
    if app in apps and match["pointer_apps"] is None:
        script = hosted_sdl_script(apps, [app]).encode("ascii")
        page.write_bytes(original[:match.start()] + script + original[match.end():])


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
    matches = list(HOSTED_SDL_SCRIPT.finditer(original))
    if len(matches) != 1:
        raise ValueError("The package must contain one SDL game profile list.")
    match = matches[0]
    existing = match["apps"].decode("ascii").split()
    if any(app in existing for app in pairs):
        raise ValueError("A surfaceless profile cannot replace an existing SDL host profile.")
    for app, build in pairs.items():
        (layout / f"steam-sdl-surfaceless-{app}.txt").write_text(build, encoding="ascii")
    page.write_bytes(original[:match.start("apps")] + " ".join([*existing, *pairs]).encode("ascii")
                     + original[match.end("apps"):])


def stage_win32_trial_profiles(layout, profiles, runtime, source_profiles=()):
    if not (profiles or source_profiles) and runtime is None:
        return
    if not (profiles or source_profiles) or runtime is None:
        raise ValueError("Supply the Win32 profiles and their Win32 runtime template together.")
    files = {}

    def stage_source_profile(name, path):
        if name in files:
            raise ValueError("Use one APPID=FILE or profile-directory source fingerprint list per Win32 title.")
        builds = read_profile(path)
        files[name] = json.dumps(builds, separators=(",", ":")).encode("ascii")

    for entry in source_profiles:
        app, separator, path = entry.partition("=")
        if not separator or not path or not re.fullmatch(r"[1-9][0-9]{0,7}", app) or int(app) > 0xffffff:
            raise ValueError("Use one APPID=FILE entry per Win32 source profile.")
        stage_source_profile(f"steam-win32-source-{app}.json", Path(path))
    trace = False
    desktop_gl = False
    unity_host = False
    crash_reports = False
    steam_sdk = False
    legacy_steam_sdk = False
    desktop_dxgi = False
    desktop_documents = False
    storage_trace = False
    hashed_companions = False
    system_companions = False
    for entry in profiles:
        app, separator, directory = entry.partition("=")
        if (not separator or not directory or not re.fullmatch(r"[1-9][0-9]{0,7}", app)
                or int(app) > 0xffffff or f"steam-win32-shadow-{app}.txt" in files):
            raise ValueError("Use one APPID=DIRECTORY entry per Win32 trial.")
        root = Path(directory)
        profile_name = f"steam-win32-shadow-{app}.txt"
        recipe_name = f"steam-win32-shadow-{app}.xwp"
        companion_name = f"steam-win32-files-{app}.txt"
        source_name = f"steam-win32-source-{app}.json"
        if (root / source_name).is_file():
            stage_source_profile(source_name, root / source_name)
        profile = (root / profile_name).read_bytes()
        fields = profile.decode("ascii").splitlines()
        if (not 2 <= len(fields) <= 8 or not re.fullmatch(r"[1-9][0-9]{0,19}", fields[0])
                or not re.fullmatch(r"[A-Za-z0-9 _-][A-Za-z0-9 ._-]{0,95}\.exe", fields[1])
                or (len(fields) >= 3 and fields[2] not in ("", "opengl3", "d3d12"))
                or (len(fields) >= 4 and fields[3] not in ("", "godot", "unity"))
                or (len(fields) >= 3 and fields[2] and fields[3:4] not in (["godot"], ["unity"]))
                or (len(fields) >= 5 and fields[4] and not re.fullmatch(
                    r"[A-Za-z0-9 _-][A-Za-z0-9 ._-]{0,95}\.(?:exe|dll)", fields[4]))
                or (len(fields) >= 6 and len(fields[5]) > 260)
                or (len(fields) >= 7 and fields[6] not in ("", "loader-trace"))):
            raise ValueError(f"Invalid Win32 trial profile for AppID {app}.")
        if len(fields) == 8:
            hosted = layout / f"steam-sdl-surfaceless-{app}.txt"
            if (fields[7] != "accessibility=disabled" or fields[2:4] != ["opengl3", "godot"]
                    or not hosted.is_file() or hosted.read_text(encoding="ascii") != fields[0]):
                raise ValueError(f"The accessibility option needs a matching hosted OpenGL profile for AppID {app}.")
        source_profile = files.get(source_name)
        if source_profile is not None and fields[0] not in json.loads(source_profile):
            raise ValueError(f"The Win32 source profile does not include the trial build for AppID {app}.")
        trace |= fields[6:7] == ["loader-trace"]
        desktop_gl |= fields[2:3] == ["opengl3"]
        recipe = (root / recipe_name).read_bytes()
        validate_patch_recipe(recipe)
        files.update({profile_name: profile, recipe_name: recipe})
        storage_marker = f"steam-win32-storage-trace-{app}.txt"
        if (root / storage_marker).is_file():
            if (root / storage_marker).stat().st_size or fields[6:7] != ["loader-trace"]:
                raise ValueError(f"The storage marker requires an empty file and a loader trace for AppID {app}.")
            files[storage_marker] = b""
            storage_trace = True
        for option in ("d3d12-default-adapter", "streamline-unavailable"):
            marker = f"steam-win32-{option}-{app}.txt"
            if (root / marker).is_file():
                if (root / marker).stat().st_size or source_profile is None or fields[6:7] != ["loader-trace"]:
                    raise ValueError(f"The {option} option needs an empty marker, source profile and loader trace for AppID {app}.")
                files[marker] = b""
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
        documents_marker = f"steam-win32-documents-{app}.txt"
        if (root / documents_marker).is_file():
            if (root / documents_marker).stat().st_size:
                raise ValueError(f"Expected an empty Documents marker for AppID {app}.")
            files[documents_marker] = b""
            desktop_documents = True
        for option in ("working-directory", "original-executable", "dynamic-companions"):
            marker = f"steam-win32-{option}-{app}.txt"
            if (root / marker).is_file():
                if (root / marker).stat().st_size:
                    raise ValueError(f"Expected an empty {option} marker for AppID {app}.")
                files[marker] = b""
                trace = True
        shader_cache = f"steam-win32-godot-shader-cache-{app}.txt"
        if (root / shader_cache).is_file():
            with (root / shader_cache).open("rb") as source:
                cache_option = source.read(17)
            hosted = layout / f"steam-sdl-surfaceless-{app}.txt"
            if (len(cache_option) > 16 or cache_option.rstrip(b"\r\n") not in (b"control", b"disabled")
                    or fields[2:4] != ["opengl3", "godot"] or any(fields[4:6])
                    or f"steam-win32-original-executable-{app}.txt" in files or argument_name in files
                    or not hosted.is_file() or hosted.read_text(encoding="ascii") != fields[0]):
                raise ValueError(f"The shader cache trial needs a matching staged Godot OpenGL launcher for AppID {app}.")
            files[shader_cache] = cache_option.rstrip(b"\r\n")
        companions = root / companion_name
        if f"steam-win32-dynamic-companions-{app}.txt" in files and (
                f"steam-win32-original-executable-{app}.txt" not in files
                or not companions.is_file() or fields[6:7] != ["loader-trace"]):
            raise ValueError(f"Dynamic companions require the original executable path, a companion list, and loader trace for AppID {app}.")
        unity = fields[3:4] == ["unity"]
        if unity:
            hosted = layout / f"steam-sdl-surfaceless-{app}.txt"
            if (fields[2] != "opengl3" or fields[4:5] != [""] or fields[6:7] != ["loader-trace"]
                    or not companions.is_file() or not hosted.is_file()
                    or hosted.read_text(encoding="ascii") != fields[0]):
                raise ValueError(f"The Unity option needs a matching hosted OpenGL launcher and loader trace for AppID {app}.")
            unity_host = True
        sdk_marker = f"steam-win32-steam-sdk-{app}.txt"
        sdk_profile = root / sdk_marker
        legacy_sdk_marker = f"steam-win32-steam-legacy-sdk-{app}.txt"
        legacy_sdk_profile = root / legacy_sdk_marker
        legacy_sdk_hash = None
        if legacy_sdk_profile.is_file():
            with legacy_sdk_profile.open("rb") as source:
                raw = source.read(67)
            legacy_sdk_hash = raw.rstrip(b"\r\n")
            if (len(raw) > 66 or not re.fullmatch(rb"[0-9a-f]{64}", legacy_sdk_hash) or sdk_profile.is_file()
                    or source_profile is None or fields[6:7] != ["loader-trace"] or not companions.is_file()):
                raise ValueError(f"The legacy Steam SDK needs a hash, matching source profile, companion list and loader trace for AppID {app}.")
            files[legacy_sdk_marker] = legacy_sdk_hash
            legacy_steam_sdk = True
        sdk_relative = None
        if sdk_profile.is_file():
            if sdk_profile.stat().st_size > 512 or fields[2:4] != ["opengl3", "godot"] or not companions.is_file():
                raise ValueError(f"The Steam SDK option needs a staged OpenGL profile for AppID {app}.")
            sdk_relative = sdk_profile.read_text(encoding="ascii").rstrip("\r\n")
            hosted = layout / f"steam-sdl-surfaceless-{app}.txt"
            if (not re.fullmatch(r"(?:[A-Za-z0-9._ -]+/)*steam_api64\.dll", sdk_relative)
                    or any(part.endswith((".", " ")) for part in sdk_relative.split("/"))
                    or not hosted.is_file() or hosted.read_text(encoding="ascii") != fields[0]):
                raise ValueError(f"Invalid Steam SDK path or missing hosted profile for AppID {app}.")
            files[sdk_marker] = sdk_relative.encode("ascii")
            steam_sdk = True
        if companions.is_file():
            if companions.stat().st_size > 65536:
                raise ValueError(f"Win32 trial companion list is too large for AppID {app}.")
            data = companions.read_bytes()
            rows = data.decode("utf-8-sig").split("\n")
            if rows[-1] == "":
                rows.pop()
            if len(rows) > 1024:
                raise ValueError(f"Too many Win32 companions for AppID {app}.")
            companions_by_name = {}
            system_companion_chars = 0
            for row in rows:
                image, tab, key = row.removesuffix("\r").partition("\t")
                if (len(image.encode("utf-16-le")) > 520 or image.casefold() in companions_by_name
                        or any(not part or len(part.encode("utf-16-le")) > 510 or part.endswith((".", " "))
                               or re.search(r'[<>:"\\|?*\x00-\x1f]', part) or PureWindowsPath(part).is_reserved()
                               for part in image.split("/"))):
                    raise ValueError(f"Invalid or duplicate companion path for AppID {app}: {image!r}.")
                companions_by_name[image.casefold()] = key
                if tab and re.fullmatch(r"(?:system-)?sha256:[0-9a-f]{64}", key):
                    hashed_companions = True
                    if key.startswith("system-"):
                        system_companion_chars += len(image) + 1
                        if (not re.fullmatch(r"[A-Za-z0-9_-][A-Za-z0-9._-]{0,95}\.dll", image)
                                or system_companion_chars >= 1024 or not trace):
                            raise ValueError(f"A system companion needs a top-level DLL and loader trace for AppID {app}.")
                        system_companions = True
                    continue
                if not tab:
                    continue
                if not image.endswith((".exe", ".dll")) or not re.fullmatch(r"[a-z0-9-]{1,24}", key):
                    raise ValueError(f"Invalid companion recipe for AppID {app}.")
                name = f"steam-win32-shadow-{app}-{key}.xwp"
                if name not in files:
                    extra = (root / name).read_bytes()
                    validate_patch_recipe(extra)
                    files[name] = extra
            if sdk_relative is not None:
                sdk_key = companions_by_name.get(sdk_relative.casefold())
                if sdk_key is None or sdk_key and not sdk_key.startswith("sha256:"):
                    raise ValueError(f"The Steam SDK must appear as an unchanged companion for AppID {app}.")
            if legacy_sdk_hash is not None and companions_by_name.get("steam_api64.dll") != "sha256:" + legacy_sdk_hash.decode("ascii"):
                raise ValueError(f"The legacy Steam SDK must appear as a matching hash-pinned companion for AppID {app}.")
            if unity and "unityplayer.dll" not in companions_by_name:
                raise ValueError(f"UnityPlayer.dll must appear once as a companion for AppID {app}.")
            files[companion_name] = data

    with ZipFile(runtime) as package:
        identity = ElementTree.fromstring(package.read("AppxManifest.xml")).find("{*}Identity")
        if (identity is None or identity.get("Name") != "XboxPortabilityProbe.ChildProcess.SteamCore"
                or identity.get("ProcessorArchitecture") != "x64"):
            raise ValueError("Use an x64 Steam Core runtime template built for this checkout.")
        for name in ("ProbeNative.dll", "XboxProbe.Native.winmd"):
            files[name] = package.read(name)
        if any(name.startswith("steam-win32-source-") for name in files) and (
                "steam_source_check=".encode("utf-16le") not in files["ProbeNative.dll"]):
            raise ValueError("Use a Win32 runtime template with source-profile verification.")
        if hashed_companions and "steam_shadow_companion_hashes=verified".encode("utf-16le") not in files["ProbeNative.dll"]:
            raise ValueError("Use a Win32 runtime template with companion fingerprint verification.")
        if profiles:
            for name in ("XboxWin32Input.dll", "XboxWin32Desktop.dll"):
                files[name] = package.read(name)
            if "XboxWin32XInput910.dll" in package.namelist():
                files["XboxWin32XInput910.dll"] = package.read("XboxWin32XInput910.dll")
        if desktop_documents:
            variable = "XBOX_WIN32_DOCUMENTS\0".encode("utf-16le")
            if any(variable not in files[name] for name in ("ProbeNative.dll", "XboxWin32Desktop.dll")):
                raise ValueError("Use a Win32 runtime template with profile Documents support.")
        if "XboxWin32Timer.dll" in package.namelist():
            files["XboxWin32Timer.dll"] = package.read("XboxWin32Timer.dll")
        if desktop_gl or desktop_dxgi:
            files["XboxWin32Window.dll"] = package.read("XboxWin32Window.dll")
        if desktop_dxgi:
            files["XboxWin32Dxgi.dll"] = package.read("XboxWin32Dxgi.dll")
        if unity_host:
            files["XboxWin32Unity.dll"] = package.read("XboxWin32Unity.dll")
            if "XboxWin32DisplayConfig.dll" in package.namelist():
                files["XboxWin32DisplayConfig.dll"] = package.read("XboxWin32DisplayConfig.dll")
        if crash_reports:
            files["XboxWin32CrashReport.dll"] = package.read("XboxWin32CrashReport.dll")
        if steam_sdk or legacy_steam_sdk:
            files["XboxWin32SteamDiscovery.dll"] = package.read("XboxWin32SteamDiscovery.dll")
        if legacy_steam_sdk:
            variable = "XBOX_STEAM_HANDOFF_LEGACY\0".encode("utf-16le")
            if any(variable not in files[name] for name in ("ProbeNative.dll", "XboxWin32SteamDiscovery.dll")):
                raise ValueError("Use a Win32 runtime template with legacy Steam SDK support.")
        if trace:
            files["XboxWin32LoaderTrace.dll"] = package.read("XboxWin32LoaderTrace.dll")
        if system_companions:
            variable = "XBOX_WIN32_SYSTEM_COMPANIONS\0".encode("utf-16le")
            if any(variable not in files[name] for name in ("ProbeNative.dll", "XboxWin32LoaderTrace.dll")):
                raise ValueError("Use a Win32 runtime template with verified system companion support.")
        if storage_trace:
            files["XboxWin32StorageTrace.dll"] = package.read("XboxWin32StorageTrace.dll")
    for name, data in files.items():
        (layout / name).write_bytes(data)
