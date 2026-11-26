"""Stage build-matched compatibility profiles in an Xbox package."""

import json
from pathlib import Path
import re
import struct

from build_tools.redirect_pe_imports import PeImage, validate_patch_recipe
from build_tools.stage_mesa_uwp import has_shader_cache
from build_tools.stage_session import read_package_member
from build_tools.verify_win32_source import read_profile, valid_win32_component
from tools.artifacts import pe_summary


WIN32_ADAPTERS = frozenset({
    "XboxWin32ControllerNotify.dll", "XboxWin32CrashReport.dll", "XboxWin32Desktop.dll", "XboxWin32DisplayConfig.dll",
    "XboxWin32DefaultAudio.dll",
    "XboxWin32Dxgi.dll",
    "XboxWin32GdiPlus.dll",
    "XboxWin32HostedDisplay.dll", "XboxWin32Input.dll", "XboxWin32LoaderTrace.dll", "XboxWin32PlayerPrefs.dll",
    "XboxWin32SteamDiscovery.dll", "XboxWin32StorageTrace.dll", "XboxWin32Timer.dll", "XboxWin32Unity.dll",
    "XboxWin32Window.dll", "XboxWin32XInput910.dll", "ktmw32.dll",
})

HOSTED_SDL_SCRIPT = re.compile(
    rb'<script id="steam-sdl-profiles" data-apps="(?P<apps>[0-9 ]+)"'
    rb'(?: data-pointer-apps="(?P<pointer_apps>[0-9 ]+)")? src="steam-sdl-host\.js"></script>'
)


# Persistent D3D12 pipeline cache marker: empty for passthrough (timing only, no
# cache call on the device), one line naming a driver cache mode and the console
# control result (probe/graphics-runtime-control --d3d12-pipeline-cache) that
# proved it: 0x5CCXLBFF with outcome 1 for the selected path (L library, B cached
# blobs), or mode=replay / mode=replay-idle, which pre-creates recorded pipelines
# with the title's own creation calls and needs no control result.
# Mirrors XboxPsoCacheParseMarker in runtime/win32/pipeline-cache-format.h.
PIPELINE_CACHE_MARKER = re.compile(rb"mode=(?P<mode>library|blobs) control=0x(?P<code>5CC[0-9A-F]{5})(?:\r?\n)?")
PIPELINE_REPLAY_MARKER = re.compile(rb"mode=(?P<mode>replay|replay-idle)(?:\r?\n)?")


def pipeline_cache_mode(marker):
    """Return passthrough, library, blobs, replay or replay-idle for a marker's bytes, or None when malformed."""
    if not marker:
        return "passthrough"
    replay = PIPELINE_REPLAY_MARKER.fullmatch(marker)
    if replay:
        return replay["mode"].decode("ascii")
    match = PIPELINE_CACHE_MARKER.fullmatch(marker)
    if not match:
        return None
    code = int(match["code"], 16)
    outcome = (code >> 12) & 15 if match["mode"] == b"library" else (code >> 8) & 15
    return match["mode"].decode("ascii") if outcome == 1 else None


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


MANAGED_WARMUP = "XboxManagedWarmup.dll"
# Profile options that select a Steam title override in the loader trace, and the
# launch variable each one sets (runtime/steam/ProbeNative.cpp, runtime/win32/loader-trace.c).
STEAM_OVERRIDES = {
    "no-steam-relay": "XBOX_WIN32_STEAM_RELAY_STUB",
    "steam-callback-trace": "XBOX_WIN32_STEAM_CALLBACK_TRACE",
    "steam-relay-worker": "XBOX_WIN32_STEAM_RELAY_WORKER",
    "steam-relay-status": "XBOX_WIN32_STEAM_RELAY_STATUS",
}
# The relay stub skips InitRelayNetworkAccess, so it cannot be combined with the options that run it.
STEAM_RELAY_RUNNERS = ("steam-relay-worker", "steam-relay-status")
WARMUP_ASSEMBLY = re.compile(
    r"assembly=([A-Za-z0-9_][A-Za-z0-9_.-]{0,63})"
    r"(?: mvid=[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})?")
WARMUP_PRIORITY = re.compile(r"priority=[A-Za-z_][A-Za-z0-9_.+`]{0,159}")


def read_managed_warmup_plan(path, app):
    """Validate a managed warm-up plan with the rules runtime/managed-warmup/Warmup.cs applies."""
    with path.open("rb") as source:
        data = source.read(4097)
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError:
        text = None
    lines = [] if text is None else [line.strip("\r ") for line in text.split("\n")]
    plan = [line for line in lines if line and not line.startswith("#")]
    assemblies = set()
    counts = {"priority": 0, "remaining": 0, "depth": 0, "collect": 0}
    valid = text is not None and len(data) <= 4096
    for line in plan:
        if match := WARMUP_ASSEMBLY.fullmatch(line):
            name = match[1].casefold()
            valid &= name not in assemblies
            assemblies.add(name)
        elif WARMUP_PRIORITY.fullmatch(line):
            counts["priority"] += 1
        elif line in ("remaining=yes", "remaining=no"):
            counts["remaining"] += 1
        elif re.fullmatch(r"depth=[0-8]", line):
            counts["depth"] += 1
        elif line == "collect=after":
            counts["collect"] += 1
        else:
            valid = False
    staged = "\n".join(plan).encode("ascii") if valid else b""
    if (not valid or not 1 <= len(assemblies) <= 8 or counts["priority"] > 32 or counts["remaining"] > 1
            or counts["depth"] > 1 or counts["collect"] > 1 or len(staged) > 2048):
        raise ValueError(f"Invalid managed warm-up plan for AppID {app}.")
    return staged


def read_adapter_requirements(path, app, build):
    with path.open("rb") as source:
        data = source.read(4097)
    if len(data) > 4096:
        raise ValueError(f"The Win32 adapter declaration is too large for AppID {app}.")
    try:
        declaration = json.loads(data)
    except ValueError as error:
        raise ValueError(f"Invalid Win32 adapter declaration JSON for AppID {app}.") from error
    if (not isinstance(declaration, dict) or set(declaration) - {"build_id", "required_adapters", "required_exports"}
            or declaration.get("build_id") != build):
        raise ValueError(f"The Win32 adapter declaration must match build {build} for AppID {app}.")
    names = declaration.get("required_adapters")
    if not isinstance(names, list) or any(not isinstance(name, str) or name not in WIN32_ADAPTERS for name in names):
        raise ValueError(f"Unknown or invalid Win32 adapter declaration for AppID {app}.")
    if len(names) != len(set(names)):
        raise ValueError(f"Duplicate Win32 adapter declaration for AppID {app}.")
    exports = declaration.get("required_exports", {})
    if not isinstance(exports, dict) or any(
            name not in names or not isinstance(symbols, list) or not symbols
            or any(not isinstance(symbol, str) or not re.fullmatch(r"[!-~]{1,128}", symbol) for symbol in symbols)
            or len(symbols) != len(set(symbols)) for name, symbols in exports.items()):
        raise ValueError(f"Invalid Win32 adapter export requirements for AppID {app}.")
    return declaration


def stage_win32_trial_profiles(layout, profiles, package, source_profiles=()):
    """Stage profile files and adapters from the verified session APPX."""
    if not (profiles or source_profiles):
        return
    files = {}
    required_adapters = set()
    required_exports = {}

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
    hosted_wgl = False
    unity_host = False
    crash_reports = False
    steam_sdk = False
    legacy_steam_sdk = False
    desktop_dxgi = False
    hosted_dxgi_output = False
    hosted_window_frame = False
    hosted_keyboard = False
    hosted_xinput = False
    disable_sdl_directinput = False
    hosted_controller_notify = False
    default_audio = False
    steam_overrides = set()
    desktop_documents = False
    unity_direct3d = False
    godot_direct3d = False
    local_low = False
    hosted_pad_only = False
    player_prefs = False
    roaming_appdata = False
    roaming_folders = False
    roaming_default = False
    storage_trace = False
    hashed_companions = False
    system_companions = False
    pipeline_cache = False
    pipeline_replay = False
    managed_warmup = False
    warmup_collect = False
    hostfxr_warmup = False
    gl_trace = False
    mesa_shader_cache = False
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
                or (len(fields) >= 3 and fields[2] not in ("", "opengl3", "d3d11", "d3d12"))
                or (len(fields) >= 4 and fields[3] not in ("", "godot", "unity"))
                or (len(fields) >= 3 and fields[2] == "d3d12" and fields[3:4] not in (["godot"], ["unity"]))
                or (fields[2:3] == ["d3d11"] and fields[3:4] != ["unity"])
                or (len(fields) >= 5 and fields[4] and not re.fullmatch(
                    r"[A-Za-z0-9 _-][A-Za-z0-9 ._-]{0,95}\.(?:exe|dll)", fields[4]))
                or (len(fields) >= 6 and len(fields[5]) > 260)
                or (len(fields) >= 7 and fields[6] not in ("", "loader-trace"))):
            raise ValueError(f"Invalid Win32 trial profile for AppID {app}.")
        # Godot's Direct3D 12 driver presents through the hosted DXGI and composition adapter.
        godot_dxgi = fields[2:4] == ["d3d12", "godot"] and (root / f"steam-win32-dxgi-{app}.txt").is_file()
        godot_hosted = fields[2:4] == ["opengl3", "godot"] or godot_dxgi
        if len(fields) == 8:
            hosted = layout / f"steam-sdl-surfaceless-{app}.txt"
            if (fields[7] != "accessibility=disabled" or not godot_hosted
                    or not hosted.is_file() or hosted.read_text(encoding="ascii") != fields[0]):
                raise ValueError(f"The accessibility option needs a matching hosted Godot profile for AppID {app}.")
        source_profile = files.get(source_name)
        if source_profile is not None and fields[0] not in json.loads(source_profile):
            raise ValueError(f"The Win32 source profile does not include the trial build for AppID {app}.")
        adapter_name = f"steam-win32-adapters-{app}.json"
        profile_adapters = ()
        profile_exports = {}
        if (root / adapter_name).is_file():
            declaration = read_adapter_requirements(root / adapter_name, app, fields[0])
            profile_adapters = declaration["required_adapters"]
            required_adapters.update(profile_adapters)
            profile_exports = declaration.get("required_exports", {})
            for name, symbols in profile_exports.items():
                required_exports.setdefault(name, set()).update(symbols)
            files[adapter_name] = json.dumps(declaration, separators=(",", ":")).encode("ascii")
        publisher_opengl = fields[2:3] == ["opengl3"] and not any(fields[3:4])
        if publisher_opengl:
            hosted = layout / f"steam-sdl-surfaceless-{app}.txt"
            if (source_profile is None or fields[6:7] != ["loader-trace"] or not hosted.is_file()
                    or hosted.read_text(encoding="ascii") != fields[0]
                    or not {"XboxWin32Window.dll", "XboxWin32LoaderTrace.dll"}.issubset(profile_adapters)):
                raise ValueError(f"Publisher OpenGL needs a matching hosted build, source profile, loader trace, "
                                 f"and declared Window and LoaderTrace adapters for AppID {app}.")
            hosted_wgl = True
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
        # The STEAM_OVERRIDES options are Steam title overrides served by the loader trace's
        # GetProcAddress for steam_api64.dll.
        title_overrides = set()
        for option in ("d3d12-default-adapter", "streamline-unavailable", *STEAM_OVERRIDES):
            marker = f"steam-win32-{option}-{app}.txt"
            if (root / marker).is_file():
                if (root / marker).stat().st_size or source_profile is None or fields[6:7] != ["loader-trace"]:
                    raise ValueError(f"The {option} option needs an empty marker, source profile and loader trace for AppID {app}.")
                files[marker] = b""
                if option in STEAM_OVERRIDES:
                    title_overrides.add(option)
        if "no-steam-relay" in title_overrides and title_overrides.intersection(STEAM_RELAY_RUNNERS):
            raise ValueError(f"The no-steam-relay option cannot be combined with "
                             f"{' or '.join(sorted(title_overrides.intersection(STEAM_RELAY_RUNNERS)))} for AppID {app}.")
        steam_overrides |= title_overrides
        cache_marker = f"steam-win32-d3d12-pipeline-cache-{app}.txt"
        if (root / cache_marker).is_file():
            content = (root / cache_marker).read_bytes() if (root / cache_marker).stat().st_size <= 64 else b"?"
            cache_mode = pipeline_cache_mode(content)
            if cache_mode is None or fields[6:7] != ["loader-trace"]:
                raise ValueError(
                    f"The D3D12 pipeline cache needs the loader trace and an empty marker (passthrough), "
                    f"'mode=replay', 'mode=replay-idle' or 'mode=library|blobs control=0x5CC…' with a passing "
                    f"control result for AppID {app}.")
            files[cache_marker] = content
            pipeline_cache = True
            pipeline_replay |= cache_mode.startswith("replay")
        release_marker = f"steam-win32-release-renderer-{app}.txt"
        if (root / release_marker).is_file():
            hosted = layout / f"steam-sdl-surfaceless-{app}.txt"
            if ((root / release_marker).stat().st_size or source_profile is None or not hosted.is_file()
                    or hosted.read_text(encoding="ascii") != fields[0]):
                raise ValueError(f"Renderer release needs a matching hosted build and source fingerprints for AppID {app}.")
            files[release_marker] = b""
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
        has_dxgi = (root / dxgi_marker).is_file()
        unity_dxgi = fields[2:4] == ["d3d11", "unity"]
        if has_dxgi:
            if (root / dxgi_marker).stat().st_size or (any(fields[2:4]) and not unity_dxgi and not godot_dxgi):
                raise ValueError(f"The DXGI marker requires the game's default native renderer, Unity Direct3D 11 "
                                 f"or Godot Direct3D 12 for AppID {app}.")
            files[dxgi_marker] = b""
            desktop_dxgi = True
            if godot_dxgi and fields[6:7] != ["loader-trace"]:
                raise ValueError(f"Godot Direct3D 12 needs the loader trace for its dynamic graphics imports for AppID {app}.")
            godot_direct3d |= godot_dxgi
        elif unity_dxgi:
            raise ValueError(f"Unity Direct3D 11 requires the DXGI marker for AppID {app}.")
        graphics_adapter = has_dxgi or fields[2:3] == ["opengl3"]
        local_low_marker = f"steam-win32-local-low-{app}.txt"
        if (root / local_low_marker).is_file():
            if (root / local_low_marker).stat().st_size or not graphics_adapter:
                raise ValueError(f"The LocalLow marker requires an empty file and a Win32 graphics adapter for AppID {app}.")
            files[local_low_marker] = b""
            local_low = True
        # A hosted graphics title can make the hosted pad its only controller source. The
        # build-matched steam-win32-hosted-xinput marker below hosts XInput without that.
        pad_marker = f"steam-win32-host-xinput-{app}.txt"
        if (root / pad_marker).is_file():
            hosted = layout / f"steam-sdl-surfaceless-{app}.txt"
            if ((root / pad_marker).stat().st_size or not graphics_adapter
                    or not hosted.is_file() or hosted.read_text(encoding="ascii") != fields[0]):
                raise ValueError(f"The XInput marker requires an empty file and a matching hosted profile for AppID {app}.")
            files[pad_marker] = b""
            hosted_pad_only = True
        prefs_marker = f"steam-win32-player-prefs-{app}.txt"
        if (root / prefs_marker).is_file():
            with (root / prefs_marker).open("rb") as source:
                settings = source.read(2049)
            prefix = settings.decode("utf-8-sig").rstrip("\r\n")
            parts = prefix.split("\\")
            if (len(settings) > 2048 or len(parts) != 3 or parts[0] != "Software"
                    or len(prefix.encode("utf-16le")) >= 1024 or any(ord(c) < 32 or ord(c) == 127 for c in prefix)
                    or any(not 0 < len(part.encode("utf-16le")) <= 510 for part in parts[1:])):
                raise ValueError(f"Expected one Software\\company\\product preference key for AppID {app}.")
            files[prefs_marker] = prefix.encode("utf-8")
            player_prefs = True
        dxgi_output_marker = f"steam-win32-dxgi-output-{app}.txt"
        if (root / dxgi_output_marker).is_file():
            hosted = layout / f"steam-sdl-surfaceless-{app}.txt"
            if ((root / dxgi_output_marker).stat().st_size or dxgi_marker not in files or source_profile is None
                    or fields[6:7] != ["loader-trace"] or not hosted.is_file()
                    or hosted.read_text(encoding="ascii") != fields[0]
                    or not {"XboxWin32Window.dll", "XboxWin32Dxgi.dll"}.issubset(profile_adapters)):
                raise ValueError(f"Hosted DXGI output needs a matching hosted build, source profile, loader trace, "
                                 f"and declared Window and DXGI adapters for AppID {app}.")
            files[dxgi_output_marker] = b""
            hosted_dxgi_output = True
        window_frame_marker = f"steam-win32-window-frame-{app}.txt"
        if (root / window_frame_marker).is_file():
            hosted = layout / f"steam-sdl-surfaceless-{app}.txt"
            if ((root / window_frame_marker).stat().st_size or (dxgi_marker not in files and not publisher_opengl)
                    or source_profile is None or fields[6:7] != ["loader-trace"] or not hosted.is_file()
                    or hosted.read_text(encoding="ascii") != fields[0]
                    or not {"XboxWin32Window.dll", "XboxWin32LoaderTrace.dll"}.issubset(profile_adapters)):
                raise ValueError(f"Hosted window frame needs a matching hosted build, source profile, loader trace, "
                                 f"and declared Window and LoaderTrace adapters for AppID {app}.")
            files[window_frame_marker] = b""
            hosted_window_frame = True
        keyboard_marker = f"steam-win32-hosted-keyboard-{app}.txt"
        if (root / keyboard_marker).is_file():
            hosted = layout / f"steam-sdl-surfaceless-{app}.txt"
            exports = profile_exports.get("XboxWin32Window.dll", ())
            if ((root / keyboard_marker).stat().st_size or dxgi_marker not in files or source_profile is None
                    or fields[6:7] != ["loader-trace"] or not hosted.is_file()
                    or hosted.read_text(encoding="ascii") != fields[0]
                    or not {"GetKeyState", "GetKeyboardState", "TranslateMessage"}.issubset(exports)):
                raise ValueError(f"Hosted keyboard needs a matching DXGI build, source profile, loader trace, "
                                 f"and declared Window keyboard exports for AppID {app}.")
            files[keyboard_marker] = b""
            hosted_keyboard = True
        xinput_marker = f"steam-win32-hosted-xinput-{app}.txt"
        if (root / xinput_marker).is_file():
            hosted = layout / f"steam-sdl-surfaceless-{app}.txt"
            if ((root / xinput_marker).stat().st_size or source_profile is None or fields[6:7] != ["loader-trace"]
                    or "XboxWin32XInput910.dll" not in profile_adapters or not hosted.is_file()
                    or hosted.read_text(encoding="ascii") != fields[0]):
                raise ValueError(f"Hosted XInput needs a matching hosted build, source profile, loader trace, and "
                                 f"declared XInput adapter for AppID {app}.")
            files[xinput_marker] = b""
            hosted_xinput = True
        sdl_directinput_marker = f"steam-win32-disable-sdl-directinput-{app}.txt"
        if (root / sdl_directinput_marker).is_file():
            if (root / sdl_directinput_marker).stat().st_size or xinput_marker not in files:
                raise ValueError(f"Disabling SDL DirectInput needs an empty marker and hosted XInput for AppID {app}.")
            files[sdl_directinput_marker] = b""
            disable_sdl_directinput = True
        notify_marker = f"steam-win32-hosted-controller-notify-{app}.txt"
        if (root / notify_marker).is_file():
            if ((root / notify_marker).stat().st_size or xinput_marker not in files
                    or "XboxWin32ControllerNotify.dll" not in profile_adapters):
                raise ValueError(f"Hosted controller notifications need hosted XInput and a declared "
                                 f"notification adapter for AppID {app}.")
            files[notify_marker] = b""
            hosted_controller_notify = True
        audio_marker = f"steam-win32-default-audio-{app}.txt"
        if (root / audio_marker).is_file():
            if ((root / audio_marker).stat().st_size or source_profile is None or fields[6:7] != ["loader-trace"]
                    or "XboxWin32DefaultAudio.dll" not in profile_adapters):
                raise ValueError(f"Default audio needs an empty marker, a source profile, loader trace, and "
                                 f"the declared audio adapter for AppID {app}.")
            files[audio_marker] = b""
            default_audio = True
        documents_marker = f"steam-win32-documents-{app}.txt"
        if (root / documents_marker).is_file():
            if (root / documents_marker).stat().st_size:
                raise ValueError(f"Expected an empty Documents marker for AppID {app}.")
            files[documents_marker] = b""
            desktop_documents = True
        roaming_marker = f"steam-win32-roaming-appdata-{app}.txt"
        if (root / roaming_marker).is_file():
            if ((root / roaming_marker).stat().st_size or source_profile is None
                    or "XboxWin32Desktop.dll" not in profile_adapters):
                raise ValueError(f"Roaming AppData needs an empty marker, a source profile, "
                                 f"and the declared desktop adapter for AppID {app}.")
            files[roaming_marker] = b""
            roaming_appdata = True
        roaming_folders_name = f"steam-win32-roaming-folders-{app}.txt"
        if (root / roaming_folders_name).is_file():
            with (root / roaming_folders_name).open("rb") as source:
                paths = source.read(2049)
            entries = paths.replace(b"\r\n", b"\n").removesuffix(b"\n").split(b"\n")
            if (roaming_marker not in files or len(paths) > 2048 or len(entries) > 16
                    or any(not re.fullmatch(rb"[A-Za-z0-9_./ -]{1,220}", path)
                           or any(not valid_win32_component(part) for part in path.decode("ascii").split("/"))
                           for path in entries)):
                raise ValueError(f"The Roaming folders need a matching Roaming profile and relative paths for AppID {app}.")
            files[roaming_folders_name] = b"\n".join(entries) + b"\n"
            roaming_folders = True
        roaming_default_name = f"steam-win32-roaming-default-{app}.txt"
        if (root / roaming_default_name).is_file():
            with (root / roaming_default_name).open("rb") as source:
                seed = source.read(4097)
            path, newline, content = seed.partition(b"\n")
            path = path.removesuffix(b"\r")
            if (roaming_marker not in files or len(seed) > 4096 or not newline or not path.endswith(b".ini")
                    or not re.fullmatch(rb"[A-Za-z0-9_./ -]{1,220}", path)
                    or any(not valid_win32_component(part) for part in path.decode("ascii").split("/"))
                    or not re.fullmatch(rb"[\x09\x0a\x0d\x20-\x7e]+", content) or not content.strip()):
                raise ValueError(f"The Roaming default needs a matching Roaming profile, a relative INI path, "
                                 f"and ASCII settings for AppID {app}.")
            files[roaming_default_name] = path + newline + content
            roaming_default = True
        for option in ("working-directory", "original-executable", "dynamic-companions"):
            marker = f"steam-win32-{option}-{app}.txt"
            if (root / marker).is_file():
                if (root / marker).stat().st_size:
                    raise ValueError(f"Expected an empty {option} marker for AppID {app}.")
                files[marker] = b""
                trace = True
        warmup_marker = f"steam-win32-managed-warmup-{app}.txt"
        if (root / warmup_marker).is_file():
            # Unity titles run Mono, which has no CoreCLR prestub or startup hooks.
            if fields[3:4] == ["unity"]:
                raise ValueError(f"The managed warm-up needs a CoreCLR title for AppID {app}.")
            files[warmup_marker] = read_managed_warmup_plan(root / warmup_marker, app)
            managed_warmup = True
            warmup_collect |= "collect=after" in files[warmup_marker].decode("ascii").split("\n")
            hostfxr_warmup |= fields[3:4] == ["godot"]
        gl_trace_marker = f"steam-win32-gl-trace-{app}.txt"
        if (root / gl_trace_marker).is_file():
            if ((root / gl_trace_marker).stat().st_size or fields[2:3] != ["opengl3"]
                    or fields[6:7] != ["loader-trace"]):
                raise ValueError(f"The GL trace needs an empty marker, the OpenGL renderer and a loader trace for AppID {app}.")
            files[gl_trace_marker] = b""
            gl_trace = True
        mesa_cache_marker = f"steam-win32-mesa-shader-cache-{app}.txt"
        if (root / mesa_cache_marker).is_file():
            if (root / mesa_cache_marker).stat().st_size or fields[2:3] != ["opengl3"]:
                raise ValueError(f"The Mesa shader cache needs an empty marker and the OpenGL renderer for AppID {app}.")
            files[mesa_cache_marker] = b""
            mesa_shader_cache = True
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
            if (fields[2] not in ("opengl3", "d3d11") or fields[4:5] != [""] or fields[6:7] != ["loader-trace"]
                    or not companions.is_file() or not hosted.is_file()
                    or hosted.read_text(encoding="ascii") != fields[0]):
                raise ValueError(f"The Unity option needs a matching hosted launcher and loader trace for AppID {app}.")
            unity_host = True
            unity_direct3d |= unity_dxgi
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
            if sdk_profile.stat().st_size > 512 or not godot_hosted or not companions.is_file():
                raise ValueError(f"The Steam SDK option needs a staged hosted Godot profile for AppID {app}.")
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
                        or any(len(part.encode("utf-16-le")) > 510 or not valid_win32_component(part)
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

    if profiles:
        required_adapters.update(("XboxWin32Input.dll", "XboxWin32Desktop.dll"))
    if desktop_gl or desktop_dxgi:
        required_adapters.add("XboxWin32Window.dll")
    if desktop_dxgi:
        required_adapters.add("XboxWin32Dxgi.dll")
    if unity_host:
        required_adapters.add("XboxWin32Unity.dll")
    if crash_reports:
        required_adapters.add("XboxWin32CrashReport.dll")
    if steam_sdk or legacy_steam_sdk:
        required_adapters.add("XboxWin32SteamDiscovery.dll")
    if trace:
        required_adapters.add("XboxWin32LoaderTrace.dll")
    if storage_trace:
        required_adapters.add("XboxWin32StorageTrace.dll")
    if hosted_pad_only:
        required_adapters.add("XboxWin32XInput910.dll")
    if player_prefs:
        required_adapters.add("XboxWin32PlayerPrefs.dll")

    available = set(package.namelist())
    missing = required_adapters - available
    if missing:
        raise ValueError("The verified Steam Core artifact is missing required Win32 adapters: " + ", ".join(sorted(missing)))
    # Older recipes may refer to these adapters without an explicit declaration.
    optional = {"XboxWin32Timer.dll"}
    if profiles:
        optional.add("XboxWin32XInput910.dll")
    if unity_host:
        optional.update(("XboxWin32DisplayConfig.dll", "XboxWin32HostedDisplay.dll"))
    for name in sorted(required_adapters | (optional & available)):
        files[name] = read_package_member(package, name)
    for name, symbols in sorted(required_exports.items()):
        try:
            actual = PeImage(files[name], read_only=True).named_exports()
        except (ValueError, struct.error) as error:
            raise ValueError(f"Cannot inspect Win32 adapter exports in {name}: {error}") from error
        missing = symbols - actual
        if missing:
            raise ValueError(f"{name} is missing required Win32 exports: {', '.join(sorted(missing))}.")

    native = read_package_member(package, "ProbeNative.dll")
    if roaming_folders and "steam_shadow_roaming_folders=".encode("utf-16le") not in native:
        raise ValueError("Use a Steam Core artifact with first-run Roaming folder support.")
    if roaming_default and "steam_shadow_roaming_default=".encode("utf-16le") not in native:
        raise ValueError("Use a Steam Core artifact with first-run Roaming defaults support.")
    if hosted_window_frame:
        variable = "XBOX_WIN32_HOSTED_WINDOW_FRAME\0".encode("utf-16le")
        window = files["XboxWin32Window.dll"]
        if (any(variable not in component for component in (native, files["XboxWin32LoaderTrace.dll"], window))
                or any(symbol not in window for symbol in (b"AdjustWindowRectEx\0", b"AdjustWindowRectExForDpi\0",
                                                          b"GetDpiForWindow\0"))):
            raise ValueError("Use a Steam Core artifact with launch-scoped window frame support.")
    if hosted_dxgi_output:
        variable = "XBOX_WIN32_HOSTED_DXGI_OUTPUT\0".encode("utf-16le")
        if variable not in native or variable not in files["XboxWin32Dxgi.dll"]:
            raise ValueError("Use a Steam Core artifact with launch-scoped DXGI output support.")
    if hosted_keyboard:
        variable = "XBOX_WIN32_HOSTED_KEYBOARD\0".encode("utf-16le")
        if variable not in native or variable not in files["XboxWin32Window.dll"]:
            raise ValueError("Use a Steam Core artifact with launch-scoped keyboard support.")
    if hosted_xinput:
        variable = "XBOX_WIN32_HOSTED_XINPUT\0".encode("utf-16le")
        if variable not in native or variable not in files["XboxWin32LoaderTrace.dll"]:
            raise ValueError("Use a Steam Core artifact with launch-scoped XInput support.")
    if disable_sdl_directinput and "SDL_JOYSTICK_DIRECTINPUT\0".encode("utf-16le") not in native:
        raise ValueError("Use a Steam Core artifact with launch-scoped SDL DirectInput selection.")
    if hosted_wgl:
        variable = "XBOX_WIN32_HOSTED_WGL\0".encode("utf-16le")
        if any(variable not in component for component in (
                native, files["XboxWin32LoaderTrace.dll"], files["XboxWin32Window.dll"])):
            raise ValueError("Use a Steam Core artifact with launch-scoped WGL support.")
    if hosted_controller_notify:
        variable = "XBOX_WIN32_HOSTED_CONTROLLER_NOTIFY\0".encode("utf-16le")
        if variable not in native or variable not in files["XboxWin32LoaderTrace.dll"]:
            raise ValueError("Use a Steam Core artifact with launch-scoped controller notification support.")
    for option in sorted(steam_overrides):
        variable = (STEAM_OVERRIDES[option] + "\0").encode("utf-16le")
        if variable not in native or variable not in files["XboxWin32LoaderTrace.dll"]:
            raise ValueError(f"Use a Steam Core artifact with the launch-scoped {option} option.")
    if default_audio:
        variable = "XBOX_WIN32_DEFAULT_AUDIO\0".encode("utf-16le")
        if variable not in native or variable not in files["XboxWin32LoaderTrace.dll"]:
            raise ValueError("Use a Steam Core artifact with launch-scoped default audio support.")
    if any(name.startswith("steam-win32-source-") for name in files) and (
            "steam_source_check=".encode("utf-16le") not in native):
        raise ValueError("Use a Steam Core artifact with source-profile verification.")
    if hashed_companions and "steam_shadow_companion_hashes=verified".encode("utf-16le") not in native:
        raise ValueError("Use a Steam Core artifact with companion fingerprint verification.")
    for selected, name, label in (
        (desktop_documents, "XBOX_WIN32_DOCUMENTS", "Documents"),
        (roaming_appdata, "XBOX_WIN32_ROAMING_APPDATA", "Roaming AppData"),
    ):
        if not selected:
            continue
        variable = (name + "\0").encode("utf-16le")
        if variable not in native or variable not in files["XboxWin32Desktop.dll"]:
            raise ValueError(f"Use a Steam Core artifact with profile {label} support.")
    if unity_direct3d and " -force-d3d11".encode("utf-16le") not in native:
        raise ValueError("Use a Steam Core artifact with the Unity Direct3D 11 launch path.")
    if godot_direct3d and (b"DCompositionCreateDevice" not in files["XboxWin32Dxgi.dll"]
                           or "dcomp.dll".encode("utf-16le") not in files["XboxWin32LoaderTrace.dll"]):
        raise ValueError("Use a Steam Core artifact with hosted DirectComposition for Godot Direct3D 12.")
    if local_low:
        variable = "XBOX_WIN32_LOCAL_LOW\0".encode("utf-16le")
        if variable not in native or variable not in files["XboxWin32Desktop.dll"]:
            raise ValueError("Use a Steam Core artifact with scoped LocalLow support.")
    if hosted_pad_only:
        variable = "XBOX_WIN32_HOST_XINPUT\0".encode("utf-16le")
        if variable not in native or variable not in files.get("XboxWin32LoaderTrace.dll", b""):
            raise ValueError("Use a Steam Core artifact with hosted XInput support for every XInput version.")
    if player_prefs:
        if any(name.encode("utf-16le") not in native for name in ("XBOX_WIN32_PREFS_KEY\0", "XBOX_WIN32_PREFS_HIVE\0")):
            raise ValueError("Use a Steam Core artifact with scoped PlayerPrefs support.")
    if legacy_steam_sdk:
        variable = "XBOX_STEAM_HANDOFF_LEGACY\0".encode("utf-16le")
        if variable not in native or variable not in files["XboxWin32SteamDiscovery.dll"]:
            raise ValueError("Use a Steam Core artifact with legacy Steam SDK support.")
    if pipeline_cache:
        # The mode variable marks a runtime whose cache defaults to passthrough and
        # checks for device removal; an older one probed CreatePipelineLibrary on
        # the title's device and lost it on Xbox (1.0.246.1).
        variables = ("XBOX_WIN32_PSO_CACHE\0", "XBOX_WIN32_PSO_CACHE_MODE\0")
        if any(name.encode("utf-16le") not in binary for name in variables
               for binary in (native, files["XboxWin32LoaderTrace.dll"])):
            raise ValueError("Use a Steam Core artifact with the persistent Direct3D 12 pipeline cache "
                             "and its profile-selected mode.")
        # An older runtime reads an unknown mode as passthrough: replay would silently not run.
        if pipeline_replay and ("replay-idle\0".encode("utf-16le") not in files["XboxWin32LoaderTrace.dll"]
                                or b"mode=replay-idle\0" not in native):
            raise ValueError("Use a Steam Core artifact with Direct3D 12 pipeline record and replay.")
    if system_companions:
        variable = "XBOX_WIN32_SYSTEM_COMPANIONS\0".encode("utf-16le")
        if variable not in native or variable not in files["XboxWin32LoaderTrace.dll"]:
            raise ValueError("Use a Steam Core artifact with verified system companion support.")
    if gl_trace:
        variable = "XBOX_WIN32_GL_TRACE\0".encode("utf-16le")
        if variable not in native or variable not in files["XboxWin32Window.dll"]:
            raise ValueError("Use a Steam Core artifact with GL first-use timing.")
    if mesa_shader_cache:
        if "MESA_SHADER_CACHE_DIR\0".encode("utf-16le") not in native:
            raise ValueError("Use a Steam Core artifact that sets the Mesa shader cache directory.")
        driver = layout / "libgallium_wgl.dll"
        if not driver.is_file() or not has_shader_cache(driver.read_bytes()):
            raise ValueError("The Mesa shader cache needs the shader-cache Mesa UWP runtime (--mesa-runtime).")

    if managed_warmup:
        variable = "XBOX_WIN32_MANAGED_WARMUP_PLAN\0".encode("utf-16le")
        if MANAGED_WARMUP not in available or variable not in native:
            raise ValueError("Use a Steam Core artifact with the managed warm-up component.")
        warmup = read_package_member(package, MANAGED_WARMUP)
        if pe_summary(warmup).get("clr_requires_x86") != "no":
            raise ValueError("Expected a managed warm-up assembly that loads in the x64 game.")
        # An older component rejects the whole plan when it has an option it does not know.
        if warmup_collect and "managed_warmup_collect=after".encode("utf-16le") not in warmup:
            raise ValueError("Use a Steam Core artifact whose managed warm-up supports collect=after.")
        # Godot hosts the runtime itself; its resolved hostfxr export is wrapped by the
        # adapter that serves the engine's GetProcAddress (window for OpenGL, loader trace
        # for Direct3D 12).
        hosts = [files[name] for name in ("XboxWin32Window.dll", "XboxWin32LoaderTrace.dll") if name in files]
        if hostfxr_warmup and (not hosts or any(b"hostfxr_get_runtime_delegate" not in data for data in hosts)):
            raise ValueError("Use a Steam Core artifact whose Win32 adapters start the managed warm-up.")
        files[MANAGED_WARMUP] = warmup

    for name, data in files.items():
        (layout / name).write_bytes(data)
