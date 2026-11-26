#!/usr/bin/env bash
# Build an isolated Xbox control using the same Mesa DLLs as the game runtime.
set -euo pipefail
# Under Git Bash on Windows (CI), hand native tools drive-letter paths and run with
# MSYS_NO_PATHCONV=1 so options such as /out: and /def: reach lld-link unchanged.
native_path() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi
}
load_only=false
loader_order=false
sdl_video=false
win32_imports=false
hosted_xinput=false
hosted_xinput_loader=false
hosted_xinput_rumble=false
controller_notification=false
hosted_controller_notify=false
raw_input=false
ime_filename=false
wic_stream=false
gdi_plus=false
ani_cursor=false
ani_adapter=false
desktop_window=false
desktop_endpoints=false
desktop_documents=false
dxgi_composition=false
dxgi_window=false
dxgi_exchange=false
dxgi_d3d12=false
display_config=false
hosted_display=false
known_folders=false
player_prefs=false
win32_opengl=false
pipeline_cache=false
pipeline_replay=false
dxgi_d3d11_resize=false
dxgi_present_commit=false
dxgi_output=false
dxgi_output_hosted=false
wgl_loader=false
window_control=false
window_geometry=false
window_frame=false
window_keyboard=false
d3d11_combined=false
d3d11_hosted=false
d3d11_hosted_output=false
d3d11_nonblocking_present=false
wasapi_desktop=false
wasapi_async=false
wasapi_hosted=false
steam_sdk_versioned=false
hosted_xinput_session=false
sdl_artifact=
sdl_version=
case ${1:-} in
    --load-only)
        load_only=true
        shift
        ;;
    --win32-imports)
        win32_imports=true
        shift
        ;;
    --hosted-xinput|--hosted-xinput-session|--hosted-xinput-loader|--hosted-xinput-rumble)
        hosted_xinput=true
        if [[ $1 == --hosted-xinput-session ]]; then hosted_xinput_session=true; fi
        if [[ $1 == --hosted-xinput-loader ]]; then hosted_xinput_loader=true; fi
        if [[ $1 == --hosted-xinput-rumble ]]; then hosted_xinput_rumble=true; fi
        shift
        ;;
    --controller-notification|--hosted-controller-notification)
        controller_notification=true
        if [[ $1 == --hosted-controller-notification ]]; then hosted_controller_notify=true; fi
        shift
        ;;
    --wasapi-desktop|--wasapi-async|--wasapi-hosted)
        case $1 in
            --wasapi-desktop) wasapi_desktop=true ;;
            --wasapi-async) wasapi_async=true ;;
            --wasapi-hosted) wasapi_hosted=true ;;
        esac
        shift
        ;;
    --steam-sdk-versioned)
        steam_sdk_versioned=true
        shift
        ;;
    --raw-input)
        raw_input=true
        shift
        ;;
    --ime-filename)
        ime_filename=true
        shift
        ;;
    --wic-stream)
        wic_stream=true
        shift
        ;;
    --gdi-plus)
        gdi_plus=true
        shift
        ;;
    --ani-cursor|--ani-cursor-adapter)
        ani_cursor=true
        if [[ $1 == --ani-cursor-adapter ]]; then ani_adapter=true; fi
        shift
        ;;
    --desktop-window)
        desktop_window=true
        shift
        ;;
    --desktop-endpoints)
        desktop_window=true
        desktop_endpoints=true
        shift
        ;;
    --desktop-documents)
        desktop_documents=true
        shift
        ;;
    --display-config|--hosted-display)
        display_config=true
        if [[ $1 == --hosted-display ]]; then hosted_display=true; fi
        shift
        ;;
    --known-folders)
        known_folders=true
        shift
        ;;
    --player-prefs)
        player_prefs=true
        shift
        ;;
    --d3d12-pipeline-cache)
        pipeline_cache=true
        shift
        ;;
    --d3d12-pipeline-replay)
        # The same child and cache source, built for the record-and-replay rounds.
        pipeline_cache=true
        pipeline_replay=true
        shift
        ;;
    --window-properties|--window-geometry|--window-frame|--window-keyboard)
        window_control=true
        if [[ $1 == --window-geometry ]]; then window_geometry=true; fi
        if [[ $1 == --window-frame ]]; then window_frame=true; fi
        if [[ $1 == --window-keyboard ]]; then window_keyboard=true; fi
        shift
        ;;
    --d3d11-combined|--d3d11-hosted|--d3d11-hosted-output|--d3d11-nonblocking-present)
        d3d11_combined=true
        if [[ $1 != --d3d11-combined ]]; then d3d11_hosted=true; fi
        if [[ $1 == --d3d11-hosted-output ]]; then d3d11_hosted_output=true; fi
        if [[ $1 == --d3d11-nonblocking-present ]]; then d3d11_nonblocking_present=true; fi
        shift
        ;;
    --dxgi-composition)
        dxgi_composition=true
        shift
        ;;
    --dxgi-window)
        dxgi_composition=true
        dxgi_window=true
        shift
        ;;
    --dxgi-exchange|--dxgi-d3d12|--dxgi-present-commit|--dxgi-d3d11-resize)
        dxgi_composition=true
        dxgi_window=true
        dxgi_exchange=true
        if [[ $1 == --dxgi-d3d12 ]]; then dxgi_d3d12=true; fi
        if [[ $1 == --dxgi-d3d11-resize ]]; then dxgi_d3d11_resize=true; fi
        if [[ $1 == --dxgi-present-commit ]]; then dxgi_present_commit=true; fi
        shift
        ;;
    --dxgi-output-native|--dxgi-output-hosted)
        dxgi_output=true
        if [[ $1 == --dxgi-output-hosted ]]; then dxgi_output_hosted=true; fi
        shift
        ;;
    --sdl-runtime|--sdl-video|--loader-order|--win32-opengl|--wgl-loader)
        if [[ $1 == --loader-order ]]; then loader_order=true; fi
        if [[ $1 == --sdl-video ]]; then sdl_video=true; fi
        if [[ $1 == --win32-opengl ]]; then win32_opengl=true; fi
        if [[ $1 == --wgl-loader ]]; then win32_opengl=true; wgl_loader=true; fi
        sdl_artifact=$(native_path "$(realpath "${2:?Supply an SDL2 artifact directory}")")
        sdl_version=${3:?Supply the SDL2 version}
        shift 3
        ;;
esac
if (( $# != 5 )); then
    echo 'Usage: build.sh [--load-only | --win32-imports | --hosted-xinput | --hosted-xinput-session | --hosted-xinput-loader | --hosted-xinput-rumble | --controller-notification | --hosted-controller-notification | --wasapi-desktop | --wasapi-async | --wasapi-hosted | --steam-sdk-versioned | --raw-input | --ime-filename | --wic-stream | --gdi-plus | --ani-cursor | --ani-cursor-adapter | --desktop-window | --desktop-endpoints | --desktop-documents | --display-config | --hosted-display | --known-folders | --player-prefs | --d3d12-pipeline-cache | --d3d12-pipeline-replay | --window-properties | --window-geometry | --window-frame | --window-keyboard | --d3d11-combined | --d3d11-hosted | --d3d11-hosted-output | --d3d11-nonblocking-present | --dxgi-composition | --dxgi-window | --dxgi-exchange | --dxgi-d3d12 | --dxgi-present-commit | --dxgi-d3d11-resize | --dxgi-output-native | --dxgi-output-hosted | {--sdl-runtime|--sdl-video|--loader-order|--win32-opengl|--wgl-loader} SDL_ARTIFACT SDL_VERSION] WindowsApp.lib MESA_LAYOUT SDK_TEMPLATE.appx PACKAGE_VERSION NEW_BUILD_DIR' >&2
    exit 2
fi
src=$(native_path "$(cd "$(dirname "$0")" && pwd)")
probe=$(dirname "$src")
repo=$(dirname "$probe")
window_bindings="$repo/runtime/win32/window-bindings.json"
windows_app=$(native_path "$(realpath "$1")")
mesa=$(native_path "$(realpath "$2")")
template=$(native_path "$(realpath "$3")")
mkdir "$5"
build=$(native_path "$(cd "$5" && pwd)")
layout="$build/package"
mkdir "$layout"

python3 - "$layout" "$mesa" "$template" "$4" "$sdl_artifact" "$sdl_version" "$desktop_window" <<'PY'
import hashlib
from pathlib import Path
import re
from shutil import copyfile
import sys
from zipfile import ZipFile

layout, mesa, template = map(Path, sys.argv[1:4])
version = sys.argv[4]
if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){3}", version) or any(int(part) > 65535 for part in version.split(".")):
    raise SystemExit("Expected an APPX package version.")
with ZipFile(template) as archive:
    manifest = archive.read("AppxManifest.xml").decode("utf-8-sig")
    if 'StartPage="index.html"' not in manifest or 'Name="XboxPortabilityProbe.ChildProcess.SteamCore"' not in manifest:
        raise SystemExit("Expected the original Windows SDK Steam Core probe template.")
    for name in ("index.html", "probe.js", "probe.css", "ProbeNative.dll", "XboxProbe.Native.winmd",
                 "pipe-child.exe", "resources.pri", "Assets/StoreLogo.png", "Assets/Logo.png",
                 "Assets/SmallLogo.png", "Assets/Splash.png"):
        target = layout / name
        target.parent.mkdir(exist_ok=True)
        target.write_bytes(archive.read(name))
for name in ("opengl32.dll", "libEGL.dll", "libGLESv2.dll", "libgallium_wgl.dll",
             "z-1.dll", "dxil.dll", "LICENSE.mesa-uwp", "LICENSE.DXIL-MS.txt",
             "LICENSE.DXIL-LLVM.txt", "LICENSE.DXIL-MIT.txt"):
    copyfile(mesa / name, layout / name)
if (mesa / "libglapi.dll").is_file():
    copyfile(mesa / "libglapi.dll", layout / "libglapi.dll")
if sys.argv[5]:
    artifact = Path(sys.argv[5])
    match = re.fullmatch(r"2\.([0-9]{1,3})\.([0-9]{1,3})", sys.argv[6])
    if not match or any(int(component) > 255 for component in match.groups()):
        raise SystemExit("Expected an SDL2 version with byte-sized components.")
    sdl = (artifact / "SDL2.dll").read_bytes()
    expected = f"{hashlib.sha256(sdl).hexdigest()}  SDL2.dll"
    if (artifact / "SHA256SUMS").read_text(encoding="utf-8-sig").strip() != expected:
        raise SystemExit("SDL2 artifact checksum mismatch.")
    (layout / "SDL2.dll").write_bytes(sdl)
    copyfile(artifact / "LICENSE.txt", layout / "LICENSE.SDL2.txt")
    minor, patch = map(int, match.groups())
    (layout.parent / "sdl-version.h").write_text(f"#define SDL_EXPECTED_MINOR {minor}\n#define SDL_EXPECTED_PATCH {patch}\n")
manifest = manifest.replace('Name="XboxPortabilityProbe.ChildProcess.SteamCore"',
                            'Name="XboxPortabilityProbe.ChildProcess.GraphicsControl"')
manifest = re.sub(r'(?<![A-Za-z])Version="[0-9.]+"', f'Version="{version}"', manifest)
manifest = manifest.replace("Steam Core Loader Probe", "Graphics runtime control")
publisher = "CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US"
frameworks = [("Microsoft.VCLibs.140.00", "14.0.33519.0")]
if sys.argv[7] == "true":
    frameworks.append(("Microsoft.VCLibs.140.00.UWPDesktop", "14.0.33321.0"))
for name, minimum in frameworks:
    if f'Name="{name}"' not in manifest:
        manifest = manifest.replace("</Dependencies>",
                                    f'<PackageDependency Name="{name}" Publisher="{publisher}" '
                                    f'MinVersion="{minimum}" />\n  </Dependencies>')
(layout / "AppxManifest.xml").write_text(manifest)
PY

cat > "$build/OpenGL.def" <<'EOF'
LIBRARY opengl32.dll
EXPORTS
    glGetString
    glClearColor
    glClear
    glGetError
    glReadPixels
    wglGetProcAddress
EOF
cat > "$build/SDL.def" <<'EOF'
LIBRARY SDL2.dll
EXPORTS
    SDL_GetVersion
EOF
cat > "$build/EGL.def" <<'EOF'
LIBRARY libEGL.dll
EXPORTS
    eglGetDisplay
    eglInitialize
    eglBindAPI
    eglChooseConfig
    eglCreateContext
    eglGetProcAddress
    eglMakeCurrent
    eglTerminate
EOF

compiler=("${CLANG:-clang}" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -Wall -Wextra -Werror)
subsystem=windows,10.00
if $desktop_window; then subsystem=windows,6.02; fi
linker=("${LLD_LINK:-lld-link}" /nologo /machine:x64 "/subsystem:$subsystem" /appcontainer
    /dynamicbase /nxcompat /highentropyva /nodefaultlib /Brepro)
graphics_libs=()
child="$src/child.c"
child_output="$layout/child.exe"
if $dxgi_window || $dxgi_output_hosted || $win32_opengl || $window_control || $d3d11_combined || $hosted_display ||
        $win32_imports || $desktop_window || $hosted_xinput || $controller_notification ||
        $raw_input || $ime_filename || $ani_adapter || $steam_sdk_versioned; then
    win32_compiler=("${WIN32_CC:?Set WIN32_CC to a Windows x64 Clang driver}" -std=c11 -O2
        -D_WIN32_WINNT=0x0a00 -DWINVER=0x0a00 -ffreestanding -Wall -Wextra -Werror)
fi
if $dxgi_window || $dxgi_output_hosted || $win32_opengl || $window_control || $d3d11_combined || $hosted_display; then
    for source in window window-gl window-monitor window-gdi; do
        "${win32_compiler[@]}" -c "$repo/runtime/win32/$source.c" -o "$build/$source.obj"
    done
    "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/window.def" "/out:$layout/XboxWin32Window.dll" \
        "/implib:$build/XboxWin32Window.lib" "$build/window.obj" "$build/window-gl.obj" "$build/window-monitor.obj" \
        "$build/window-gdi.obj" "$windows_app"
fi
if $hosted_display; then
    "${win32_compiler[@]}" -c "$repo/runtime/win32/hosted-display.c" -o "$build/hosted-display.obj"
    "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/hosted-display.def" "/out:$layout/XboxWin32HostedDisplay.dll" \
        "$build/hosted-display.obj" "$windows_app"
fi
if $dxgi_window || $dxgi_output_hosted || $d3d11_hosted; then
    for source in dxgi dxgi-frame; do
        "${WIN32_CXX:?Set WIN32_CXX to a Windows x64 Clang++ driver}" -std=c++20 -O2 -D_WIN32_WINNT=0x0a00 \
            -DWINVER=0x0a00 -ffreestanding -fno-rtti -fno-exceptions -Wall -Wextra -Werror \
            -c "$repo/runtime/win32/$source.cpp" -o "$build/$source.obj"
    done
    "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/dxgi.def" "/out:$layout/XboxWin32Dxgi.dll" \
        "/implib:$build/XboxWin32Dxgi.lib" "$build/dxgi.obj" "$build/dxgi-frame.obj" "$windows_app"
fi
if $d3d11_hosted || $hosted_xinput_loader || $hosted_controller_notify || $wgl_loader || $window_frame; then
    "${LLD_LINK:-lld-link}" /lib /nologo /machine:x64 \
        "/def:$repo/runtime/win32/ntdll-eh.def" "/out:$build/ntdll-eh.lib"
    "${win32_compiler[@]}" -fms-extensions -funwind-tables -fasynchronous-unwind-tables \
        -c "$repo/runtime/win32/loader-trace.c" -o "$build/loader-trace.obj"
    # The same sources as the packaged adapter (.github/workflows/xbox-probe.yml).
    for source in d3d12-pipeline-cache d3d12-residency d3d12-fence-trace; do
        "${win32_compiler[@]}" -c "$repo/runtime/win32/$source.c" -o "$build/$source.obj"
    done
    "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/loader-trace.def" \
        "/out:$layout/XboxWin32LoaderTrace.dll" "/implib:$build/XboxWin32LoaderTrace.lib" \
        "$build/loader-trace.obj" "$build/d3d12-pipeline-cache.obj" "$build/d3d12-residency.obj" \
        "$build/d3d12-fence-trace.obj" \
        "$windows_app" "$build/ntdll-eh.lib"
fi
if $hosted_controller_notify; then
    "${win32_compiler[@]}" -c "$repo/runtime/win32/controller-notify.c" -o "$build/controller-notify.obj"
    "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/controller-notify.def" \
        "/out:$layout/XboxWin32ControllerNotify.dll" "$build/controller-notify.obj" "$windows_app"
fi
if $dxgi_window; then
    factory=CreateDXGIFactory1
    if $dxgi_d3d12; then factory=CreateDXGIFactory2; fi
    cat > "$build/dxgi-window-bindings.json" <<EOF
{"user32.dll":{"RegisterClassW":"XboxWin32Window.dll","CreateWindowExW":"XboxWin32Window.dll","DefWindowProcW":"XboxWin32Window.dll"},"dxgi.dll":{"$factory":"XboxWin32Dxgi.dll"}}
EOF
    cat > "$build/dxgi-user32.def" <<'EOF'
LIBRARY user32.dll
EXPORTS
    RegisterClassW
    CreateWindowExW
    DefWindowProcW
EOF
    "${LLD_LINK:-lld-link}" /lib /nologo /machine:x64 "/def:$build/dxgi-user32.def" "/out:$build/dxgi-user32.lib"
    graphics_libs+=("$build/dxgi-user32.lib")
    child_output="$build/standard-dxgi.exe"
fi
if $win32_imports || $desktop_window || $hosted_xinput || $hosted_controller_notify ||
        $raw_input || $ime_filename || $ani_adapter; then
    for source in input input-raw input-cursor xinput; do
        "${win32_compiler[@]}" -c "$repo/runtime/win32/$source.c" -o "$build/win32-$source.obj"
    done
    "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/input.def" "/out:$layout/XboxWin32Input.dll" \
        "$build/win32-input.obj" "$build/win32-input-raw.obj" "$build/win32-input-cursor.obj" \
        "$build/win32-xinput.obj" "$windows_app"
    if $hosted_xinput || $hosted_controller_notify; then
        "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/xinput910.def" "/out:$layout/XboxWin32XInput910.dll" \
            "$build/win32-xinput.obj" "$windows_app"
    fi
fi
if $load_only; then
    compiler+=(-DPROBE_LOAD_MESA)
elif $dxgi_composition; then
    child="$src/dxgi-child.cpp"
elif $dxgi_output; then
    child="$src/dxgi-output-child.cpp"
    if $dxgi_output_hosted; then
        cat > "$build/output-user32.def" <<'EOF'
LIBRARY user32.dll
EXPORTS
    MonitorFromPoint
    GetMonitorInfoA
    EnumDisplaySettingsA
    EnumDisplaySettingsW
EOF
        "${LLD_LINK:-lld-link}" /lib /nologo /machine:x64 "/def:$build/output-user32.def" "/out:$build/output-user32.lib"
        graphics_libs+=("$build/output-user32.lib")
        child_output="$build/standard-dxgi-output.exe"
    fi
elif $wic_stream; then
    child="$src/wic-stream-child.cpp"
elif $gdi_plus; then
    python3 - "$repo/runtime/win32/gdi-plus.def" "$build" <<'PY'
import json
from pathlib import Path
import sys

exports, build = map(Path, sys.argv[1:])
names = [line.strip().partition('=')[0] for line in exports.read_text().splitlines()[2:]]
(build / 'original-gdi-plus.def').write_text('LIBRARY gdiplus.dll\nEXPORTS\n' + '\n'.join(names) + '\n')
(build / 'gdi-plus-bindings.json').write_text(json.dumps({'gdiplus.dll': dict.fromkeys(names, 'XboxWin32GdiPlus.dll')}))
PY
    "${LLD_LINK:-lld-link}" /lib /nologo /machine:x64 "/def:$build/original-gdi-plus.def" "/out:$build/original-gdi-plus.lib"
    graphics_libs=("$build/original-gdi-plus.lib")
    child="$src/gdi-plus-child.cpp"
    child_output="$build/standard-gdi-plus.exe"
elif $ani_cursor; then
    python3 "$src/ani-cursor-fixture.py" "$build/ani-cursor-fixtures.h"
    child="$src/ani-cursor-child.cpp"
    if $ani_adapter; then
        printf '%s\n' 'LIBRARY user32.dll' 'EXPORTS' '    CreateIconFromResourceEx' > "$build/ani-user32.def"
        "${LLD_LINK:-lld-link}" /lib /nologo /machine:x64 "/def:$build/ani-user32.def" "/out:$build/ani-user32.lib"
        graphics_libs=("$build/ani-user32.lib")
        child_output="$build/standard-ani-cursor.exe"
    fi
elif $hosted_xinput_loader; then
    child="$src/xinput-child.c"
    child_output="$build/standard-xinput-loader.exe"
elif $hosted_xinput; then
    cat > "$build/xinput1_4.def" <<'EOF'
LIBRARY xinput1_4.dll
EXPORTS
    XInputGetCapabilities
    XInputGetState
    XInputSetState
EOF
    "${LLD_LINK:-lld-link}" /lib /nologo /machine:x64 "/def:$build/xinput1_4.def" "/out:$build/xinput1_4.lib"
    graphics_libs=("$build/xinput1_4.lib")
    child="$src/xinput-child.c"
    child_output="$build/standard-xinput.exe"
elif $display_config; then
    child="$src/display-config-child.c"
elif $pipeline_cache; then
    # The same cache source as XboxWin32LoaderTrace.dll, linked into the child.
    child="$src/pipeline-cache-child.c"
    "${WIN32_CC:?Set WIN32_CC to a Windows x64 Clang driver with Windows SDK headers}" -std=c11 -O2 -ffreestanding \
        -Wall -Wextra -Werror -D_WIN32_WINNT=0x0a00 -DWINVER=0x0a00 \
        -c "$repo/runtime/win32/d3d12-pipeline-cache.c" -o "$build/d3d12-pipeline-cache.obj"
    "${LLVM_DLLTOOL:-llvm-dlltool}" -m i386:x86-64 -d "$repo/runtime/win32/ntdll-eh.def" -l "$build/ntdll-eh.lib"
    graphics_libs=("$build/d3d12-pipeline-cache.obj" "$build/ntdll-eh.lib")
elif $player_prefs; then
    child="$src/player-prefs-child.c"
    "${WIN32_CC:?Set WIN32_CC to a Windows x64 Clang driver}" -std=c11 -O2 -ffreestanding -Wall -Wextra -Werror \
        -D_WIN32_WINNT=0x0a00 -DWINVER=0x0a00 -c "$repo/runtime/win32/player-prefs.c" -o "$build/player-prefs.obj"
    "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/player-prefs.def" "/out:$layout/XboxWin32PlayerPrefs.dll" \
        "$build/player-prefs.obj" "$windows_app"
elif $controller_notification; then
    child="$src/controller-notification-child.c"
    if $hosted_controller_notify; then child_output="$build/standard-controller-notification.exe"; fi
elif $wasapi_desktop || $wasapi_async || $wasapi_hosted; then
    child="$src/wasapi-child.cpp"
elif $win32_opengl || $window_control || $d3d11_combined; then
    python3 - "$build" "$window_bindings" "$win32_opengl" "$wgl_loader" "$window_keyboard" <<'PY'
import json
from pathlib import Path
import sys

build, bindings = map(Path, sys.argv[1:3])
libraries = json.loads(bindings.read_text())
if sys.argv[3] == "true":
    gl = ["glClearColor", "glClear", "glReadPixels", "glGetError"]
    if sys.argv[4] == "true":
        libraries["opengl32.dll"] = gl
        libraries["kernel32.dll"] = ["GetProcAddress", "LoadLibraryW"]
    else:
        # The window control compares the WGL functions it resolves with its own imports.
        libraries["opengl32.dll"] = [*libraries["opengl32.dll"], *gl]
else:
    libraries = {"user32.dll": libraries["user32.dll"]}
if sys.argv[5] == "true":
    libraries["user32.dll"] = [*libraries["user32.dll"], "MapVirtualKeyW"]
for library, functions in libraries.items():
    (build / Path(library).with_suffix(".def")).write_text(
        f"LIBRARY {library}\nEXPORTS\n    " + "\n    ".join(functions) + "\n")
PY
    window_libraries=(user32)
    if $win32_opengl; then window_libraries+=(gdi32 kernel32 opengl32); fi
    for library in "${window_libraries[@]}"; do
        "${LLD_LINK:-lld-link}" /lib /nologo /machine:x64 "/def:$build/$library.def" "/out:$build/$library.lib"
        graphics_libs+=("$build/$library.lib")
    done
    if $window_keyboard; then
        child="$src/window-keyboard-child.c"
    elif $window_frame; then
        child="$src/window-frame-child.c"
    elif $window_geometry; then
        child="$src/window-geometry-child.c"
    elif $window_control; then
        child="$src/window-properties-child.c"
    elif $d3d11_combined; then
        child="$src/d3d11-combined-child.cpp"
    else
        child="$src/window-child.c"
    fi
    child_output="$build/standard-window.exe"
elif $desktop_window || $desktop_documents || $known_folders; then
    if $desktop_documents; then
        child="$src/documents-child.c"
        child_output="$build/standard-documents.exe"
        cat > "$build/documents-shell32.def" <<'EOF'
LIBRARY shell32.dll
EXPORTS
    SHGetFolderPathW
EOF
        "${LLD_LINK:-lld-link}" /lib /nologo /machine:x64 "/def:$build/documents-shell32.def" "/out:$build/documents-shell32.lib"
        graphics_libs+=("$build/documents-shell32.lib")
    elif $known_folders; then
        child="$src/known-folders-child.c"
    else
        child="$src/desktop-control.c"
        if $desktop_endpoints; then compiler+=(-DPROBE_DESKTOP_ENDPOINTS); fi
    fi
    "${compiler[@]}" -c "$repo/runtime/win32/desktop.c" -o "$build/win32-desktop.obj"
    "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/desktop.def" "/out:$layout/XboxWin32Desktop.dll" \
        "$build/win32-desktop.obj" "$windows_app"
elif $steam_sdk_versioned; then
    "${win32_compiler[@]}" -c "$repo/runtime/win32/steam-discovery.c" -o "$build/steam-discovery.obj"
    "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/steam-discovery.def" \
        "/out:$layout/XboxWin32SteamDiscovery.dll" "/implib:$build/XboxWin32SteamDiscovery.lib" \
        "$build/steam-discovery.obj" "$windows_app"
    "${win32_compiler[@]}" -c "$src/steam-sdk-versioned-fixture.c" -o "$build/steam-sdk-fixture.obj"
    "${linker[@]}" /dll /noentry "/def:$src/steam-sdk-versioned-fixture.def" \
        "/out:$layout/steam_api64.dll" "/implib:$build/steam_api64.lib" "$build/steam-sdk-fixture.obj" "$windows_app"
    graphics_libs=("$build/steam_api64.lib")
    child="$src/steam-sdk-versioned-child.c"
    child_output="$build/standard-steam-sdk.exe"
elif $raw_input; then
    cat > "$build/user32.def" <<'EOF'
LIBRARY user32.dll
EXPORTS
    GetRawInputBuffer
EOF
    "${LLD_LINK:-lld-link}" /lib /nologo /machine:x64 "/def:$build/user32.def" "/out:$build/user32.lib"
    graphics_libs=("$build/user32.lib")
    child="$src/raw-input-child.c"
    child_output="$build/standard-raw-input.exe"
elif $ime_filename; then
    cat > "$build/imm32.def" <<'EOF'
LIBRARY imm32.dll
EXPORTS
    ImmGetIMEFileNameA
EOF
    "${LLD_LINK:-lld-link}" /lib /nologo /machine:x64 "/def:$build/imm32.def" "/out:$build/imm32.lib"
    graphics_libs=("$build/imm32.lib")
    child="$src/ime-filename-child.c"
    child_output="$build/standard-ime-filename.exe"
elif $win32_imports; then
    cat > "$build/ole32.def" <<'EOF'
LIBRARY ole32.dll
EXPORTS
    CoInitializeEx
    CoUninitialize
    GetErrorInfo
    SetErrorInfo
EOF
    cat > "$build/user32.def" <<'EOF'
LIBRARY user32.dll
EXPORTS
    MapVirtualKeyExA
    MapVirtualKeyExW
    GetPointerPenInfo
EOF
    for name in ole32 user32; do
        "${LLD_LINK:-lld-link}" /lib /nologo /machine:x64 "/def:$build/$name.def" "/out:$build/$name.lib"
    done
    graphics_libs=("$build/ole32.lib" "$build/user32.lib")
    child="$src/launch-control.c"
    child_output="$build/standard-imports.exe"
elif $loader_order; then
    for name in OpenGL SDL; do
        "${LLD_LINK:-lld-link}" /lib /nologo /machine:x64 "/def:$build/$name.def" "/out:$build/$name.lib"
    done
    compiler+=(-I"$build")
    child="$src/loader-child.c"
    graphics_libs=("$build/OpenGL.lib" "$build/SDL.lib")
    "${compiler[@]}" -c "$src/loader-sentinel.c" -o "$build/loader-sentinel.obj"
    "${linker[@]}" /dll /noentry "/out:$layout/probe-local-sdl.dll" "$build/loader-sentinel.obj"
elif [[ -n $sdl_artifact ]]; then
    compiler+=(-I"$build")
    child="$src/sdl-child.c"
    if $sdl_video; then
        compiler+=(-DPROBE_SDL_VIDEO)
        "${LLD_LINK:-lld-link}" /lib /nologo /machine:x64 "/def:$build/OpenGL.def" "/out:$build/OpenGL.lib"
        graphics_libs=("$build/OpenGL.lib")
    fi
else
    for name in OpenGL EGL; do
        "${LLD_LINK:-lld-link}" /lib /nologo /machine:x64 "/def:$build/$name.def" "/out:$build/$name.lib"
    done
    graphics_libs=("$build/OpenGL.lib" "$build/EGL.lib")
fi
if $win32_opengl || $window_control || $hosted_xinput || $controller_notification ||
        $raw_input || $ime_filename || $steam_sdk_versioned; then
    if $hosted_xinput_loader; then win32_compiler+=(-DPROBE_XINPUT_LOADER); fi
    if $hosted_xinput_rumble; then win32_compiler+=(-DPROBE_XINPUT_RUMBLE); fi
    if $hosted_xinput_session; then win32_compiler+=(-DPROBE_XINPUT_SESSION); fi
    if $hosted_controller_notify; then win32_compiler+=(-DPROBE_HOSTED_CONTROLLER_NOTIFY); fi
    if $wgl_loader; then win32_compiler+=(-DPROBE_WGL_LOADER); fi
    "${win32_compiler[@]}" -c "$child" -o "$build/graphics-child.obj"
elif $display_config || $known_folders || $player_prefs || $pipeline_cache; then
    display_flags=()
    if $hosted_display; then display_flags=(-DPROBE_HOSTED_DISPLAY); fi
    if $pipeline_replay; then display_flags=(-DXBOX_PSO_REPLAY_CONTROL); fi
    "${WIN32_CC:?Set WIN32_CC to a Windows x64 Clang driver with Windows SDK headers}" \
        -std=c11 -O2 -D_WIN32_WINNT=0x0a00 -DWINVER=0x0a00 -ffreestanding -Wall -Wextra -Werror \
        "${display_flags[@]+${display_flags[@]}}" \
        -c "$child" -o "$build/graphics-child.obj"
elif $dxgi_composition || $dxgi_output || $wic_stream || $gdi_plus || $ani_cursor || $d3d11_combined || $wasapi_desktop || $wasapi_async || $wasapi_hosted; then
    if $dxgi_exchange; then
        child_flags=(-DPROBE_DXGI_WINDOW -DPROBE_DXGI_EXCHANGE)
    elif $dxgi_window; then
        child_flags=(-DPROBE_DXGI_WINDOW)
    else
        child_flags=()
    fi
    if $dxgi_d3d12; then child_flags+=(-DPROBE_DXGI_D3D12); fi
    if $dxgi_d3d11_resize; then child_flags+=(-DPROBE_DXGI_D3D11_RESIZE); fi
    if $dxgi_present_commit; then child_flags+=(-DPROBE_DXGI_PRESENT_COMMIT); fi
    if $dxgi_output_hosted; then child_flags+=(-DPROBE_DXGI_OUTPUT_HOSTED); fi
    if $d3d11_hosted; then child_flags+=(-DPROBE_D3D11_HOSTED); fi
    if $d3d11_hosted_output; then child_flags+=(-DPROBE_D3D11_HOSTED_OUTPUT); fi
    if $d3d11_nonblocking_present; then child_flags+=(-DPROBE_D3D11_NONBLOCKING_PRESENT); fi
    if $wasapi_async; then child_flags+=(-DPROBE_WASAPI_ASYNC); fi
    if $wasapi_hosted; then child_flags+=(-DPROBE_WASAPI_HOSTED); fi
    if $ani_cursor; then child_flags+=(-I"$build"); fi
    if $ani_adapter; then child_flags+=(-DPROBE_ANI_ADAPTER); fi
    win32_cxx=("${WIN32_CXX:?Set WIN32_CXX to a Windows x64 Clang++ driver with Windows SDK headers}"
        -std=c++20 -O2 -DNDEBUG -D_WIN32_WINNT=0x0a00 -DWINVER=0x0a00 -ffreestanding
        -fno-rtti -fno-exceptions -Wall -Wextra -Werror)
    "${win32_cxx[@]}" "${child_flags[@]+${child_flags[@]}}" -c "$child" -o "$build/graphics-child.obj"
    if $gdi_plus; then
        "${win32_cxx[@]}" -c "$repo/runtime/win32/gdi-plus.cpp" -o "$build/win32-gdi-plus.obj"
        "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/gdi-plus.def" "/out:$layout/XboxWin32GdiPlus.dll" \
            "$build/win32-gdi-plus.obj" "$windows_app"
    fi
    if $wasapi_hosted; then
        "${win32_cxx[@]}" -c "$repo/runtime/win32/mmdevice.cpp" -o "$build/win32-mmdevice.obj"
        "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/mmdevice.def" "/out:$layout/XboxWin32DefaultAudio.dll" \
            "$build/win32-mmdevice.obj" "$windows_app"
    fi
else
    "${compiler[@]}" -c "$child" -o "$build/graphics-child.obj"
fi
"${linker[@]}" /subsystem:windows,6.02 /entry:ChildEntry /manifest:embed /manifestuac:no "/out:$child_output" \
    "$build/graphics-child.obj" ${graphics_libs[@]+"${graphics_libs[@]}"} "$windows_app"
if $win32_imports; then
    python3 "$repo/build_tools/redirect_pe_imports.py" "$child_output" "$repo/runtime/win32/bindings.json" "$layout/child.exe"
fi
if $raw_input; then
    printf '%s\n' '{"user32.dll":{"GetRawInputBuffer":"XboxWin32Input.dll"}}' > "$build/raw-input-bindings.json"
    python3 "$repo/build_tools/redirect_pe_imports.py" "$child_output" "$build/raw-input-bindings.json" "$layout/child.exe"
fi
if $ime_filename; then
    printf '%s\n' '{"imm32.dll":{"ImmGetIMEFileNameA":"XboxWin32Input.dll"}}' > "$build/ime-filename-bindings.json"
    python3 "$repo/build_tools/redirect_pe_imports.py" "$child_output" "$build/ime-filename-bindings.json" "$layout/child.exe"
fi
if $gdi_plus; then
    python3 "$repo/build_tools/redirect_pe_imports.py" "$child_output" "$build/gdi-plus-bindings.json" "$layout/child.exe"
fi
if $ani_adapter; then
    printf '%s\n' '{"user32.dll":{"CreateIconFromResourceEx":"XboxWin32Input.dll"}}' > "$build/ani-cursor-bindings.json"
    python3 "$repo/build_tools/redirect_pe_imports.py" "$child_output" "$build/ani-cursor-bindings.json" "$layout/child.exe"
fi
if $hosted_xinput && ! $hosted_xinput_loader; then
    cat > "$build/xinput-control-bindings.json" <<'EOF'
{"xinput1_4.dll":{"XInputGetCapabilities":"XboxWin32Input.dll","XInputGetState":"XboxWin32Input.dll","XInputSetState":"XboxWin32Input.dll"}}
EOF
    python3 "$repo/build_tools/redirect_pe_imports.py" "$child_output" "$build/xinput-control-bindings.json" "$layout/child.exe"
fi
if $win32_opengl || $window_control || $d3d11_combined || $hosted_xinput_loader || $hosted_controller_notify; then
    python3 - "$repo" "$child_output" "$window_bindings" "$build/window-control-bindings.json" \
        "$win32_opengl" "$d3d11_hosted" "$hosted_xinput_loader" "$hosted_controller_notify" "$wgl_loader" "$window_frame" <<'PY'
import json
from pathlib import Path
import sys

repo, child, bindings, output = map(Path, sys.argv[1:5])
sys.path.insert(0, str(repo))
from build_tools.redirect_pe_imports import PeImage

image = PeImage(child.read_bytes())
rules = json.loads(bindings.read_text())
loader_bindings = set()
if sys.argv[5] != "true":
    rules = {"user32.dll": rules["user32.dll"]}
if sys.argv[6] == "true":
    loader_bindings = {"LoadLibraryA", "GetProcAddress"}
if sys.argv[7] == "true":
    rules = {}
    loader_bindings = {"LoadLibraryW", "GetProcAddress"}
if sys.argv[8] == "true":
    rules = {}
    loader_bindings = {"LoadLibraryA", "LoadLibraryW", "GetProcAddress"}
if sys.argv[9] == "true":
    rules.pop("kernel32.dll")
    loader_bindings = {"LoadLibraryW", "GetProcAddress"}
if sys.argv[10] == "true":
    loader_bindings = {"LoadLibraryW", "GetProcAddress"}
used = {}
for lookup, name, iat, _ in image.import_descriptors():
    library = image.string(name).lower()
    members = rules.get(library, {})
    for _, _, symbol in image.imported_symbols(lookup, iat):
        if symbol in members:
            used.setdefault(library, {})[symbol] = members[symbol]
        elif (symbol in loader_bindings
              and (library == "kernel32.dll" or library.startswith("api-ms-win-core-libraryloader-"))):
            used.setdefault(library, {})[symbol] = "XboxWin32LoaderTrace.dll"
if loader_bindings:
    loader_imports = {symbol for members in used.values() for symbol, dll in members.items()
                      if dll == "XboxWin32LoaderTrace.dll"}
    if loader_imports != loader_bindings:
        raise SystemExit("The control is missing a required loader trace binding.")
output.write_text(json.dumps(used))
PY
    python3 "$repo/build_tools/redirect_pe_imports.py" "$child_output" "$build/window-control-bindings.json" "$layout/child.exe"
fi
if $dxgi_window; then
    python3 "$repo/build_tools/redirect_pe_imports.py" "$child_output" "$build/dxgi-window-bindings.json" "$layout/child.exe"
fi
if $dxgi_output_hosted; then
    cat > "$build/dxgi-output-bindings.json" <<'EOF'
{"user32.dll":{"MonitorFromPoint":"XboxWin32Window.dll","GetMonitorInfoA":"XboxWin32Window.dll","EnumDisplaySettingsA":"XboxWin32Window.dll","EnumDisplaySettingsW":"XboxWin32Window.dll"}}
EOF
    python3 "$repo/build_tools/redirect_pe_imports.py" "$child_output" "$build/dxgi-output-bindings.json" "$layout/child.exe"
fi
if $desktop_documents; then
    printf '%s\n' '{"shell32.dll":{"SHGetFolderPathW":"XboxWin32Desktop.dll"}}' > "$build/documents-bindings.json"
    python3 "$repo/build_tools/redirect_pe_imports.py" "$child_output" "$build/documents-bindings.json" "$layout/child.exe"
fi
if $steam_sdk_versioned; then
    cat > "$build/steam-sdk-bindings.json" <<'EOF'
{"steam_api64.dll":{"SteamAPI_RestartAppIfNecessary":"XboxWin32SteamDiscovery.dll","SteamInternal_SteamAPI_Init":"XboxWin32SteamDiscovery.dll"}}
EOF
    python3 "$repo/build_tools/redirect_pe_imports.py" "$child_output" "$build/steam-sdk-bindings.json" "$layout/child.exe"
fi
"${compiler[@]}" -DPROBE_CHILD_WAIT_MS=15000 -c "$probe/child-process.c" -o "$build/graphics-child-process.obj"
"${linker[@]}" /dll /noentry "/implib:$build/child-process.lib" "/out:$layout/child-process.dll" \
    "$build/graphics-child-process.obj" "$windows_app"
