#!/usr/bin/env bash
# Build an isolated Xbox control using the same Mesa DLLs as the game runtime.
set -euo pipefail
load_only=false
loader_order=false
sdl_video=false
win32_imports=false
hosted_xinput=false
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
    --hosted-xinput)
        hosted_xinput=true
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
    --dxgi-composition)
        dxgi_composition=true
        shift
        ;;
    --dxgi-window)
        dxgi_composition=true
        dxgi_window=true
        shift
        ;;
    --dxgi-exchange|--dxgi-d3d12)
        dxgi_composition=true
        dxgi_window=true
        dxgi_exchange=true
        if [[ $1 == --dxgi-d3d12 ]]; then dxgi_d3d12=true; fi
        shift
        ;;
    --sdl-runtime|--sdl-video|--loader-order|--win32-opengl)
        if [[ $1 == --loader-order ]]; then loader_order=true; fi
        if [[ $1 == --sdl-video ]]; then sdl_video=true; fi
        if [[ $1 == --win32-opengl ]]; then win32_opengl=true; fi
        sdl_artifact=$(realpath "${2:?Supply an SDL2 artifact directory}")
        sdl_version=${3:?Supply the SDL2 version}
        shift 3
        ;;
esac
if (( $# != 5 )); then
    echo 'Usage: build.sh [--load-only | --win32-imports | --hosted-xinput | --desktop-window | --desktop-endpoints | --desktop-documents | --display-config | --hosted-display | --known-folders | --player-prefs | --dxgi-composition | --dxgi-window | --dxgi-exchange | --dxgi-d3d12 | {--sdl-runtime|--sdl-video|--loader-order|--win32-opengl} SDL_ARTIFACT SDL_VERSION] WindowsApp.lib MESA_LAYOUT SDK_TEMPLATE.appx PACKAGE_VERSION NEW_BUILD_DIR' >&2
    exit 2
fi
src=$(cd "$(dirname "$0")" && pwd)
probe=$(dirname "$src")
repo=$(dirname "$probe")
windows_app=$(realpath "$1")
mesa=$(realpath "$2")
template=$(realpath "$3")
mkdir "$5"
build=$(cd "$5" && pwd)
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
if $dxgi_window || $win32_opengl || $hosted_display; then
    win32_compiler=("${WIN32_CC:?Set WIN32_CC to a Windows x64 Clang driver}" -std=c11 -O2
        -D_WIN32_WINNT=0x0a00 -DWINVER=0x0a00 -ffreestanding -Wall -Wextra -Werror)
    for source in window window-gl window-monitor window-gdi; do
        "${win32_compiler[@]}" -c "$repo/runtime/win32/$source.c" -o "$build/$source.obj"
    done
    "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/window.def" "/out:$layout/XboxWin32Window.dll" \
        "$build/window.obj" "$build/window-gl.obj" "$build/window-monitor.obj" "$build/window-gdi.obj" "$windows_app"
fi
if $hosted_display; then
    "${win32_compiler[@]}" -c "$repo/runtime/win32/hosted-display.c" -o "$build/hosted-display.obj"
    "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/hosted-display.def" "/out:$layout/XboxWin32HostedDisplay.dll" \
        "$build/hosted-display.obj" "$windows_app"
fi
if $dxgi_window; then
    for source in dxgi dxgi-frame; do
        "${WIN32_CXX:?Set WIN32_CXX to a Windows x64 Clang++ driver}" -std=c++20 -O2 -D_WIN32_WINNT=0x0a00 \
            -DWINVER=0x0a00 -ffreestanding -fno-rtti -fno-exceptions -Wall -Wextra -Werror \
            -c "$repo/runtime/win32/$source.cpp" -o "$build/$source.obj"
    done
    "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/dxgi.def" "/out:$layout/XboxWin32Dxgi.dll" \
        "$build/dxgi.obj" "$build/dxgi-frame.obj" "$windows_app"
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
if $win32_imports || $desktop_window || $hosted_xinput; then
    win32_compiler=("${WIN32_CC:?Set WIN32_CC to a Windows x64 Clang driver}" -std=c11 -O2
        -D_WIN32_WINNT=0x0a00 -DWINVER=0x0a00 -ffreestanding -Wall -Wextra -Werror)
    for source in input input-raw xinput; do
        "${win32_compiler[@]}" -c "$repo/runtime/win32/$source.c" -o "$build/win32-$source.obj"
    done
    "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/input.def" "/out:$layout/XboxWin32Input.dll" \
        "$build/win32-input.obj" "$build/win32-input-raw.obj" "$build/win32-xinput.obj" "$windows_app"
    if $hosted_xinput; then
        "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/xinput910.def" "/out:$layout/XboxWin32XInput910.dll" \
            "$build/win32-xinput.obj" "$windows_app"
    fi
fi
if $load_only; then
    compiler+=(-DPROBE_LOAD_MESA)
elif $dxgi_composition; then
    child="$src/dxgi-child.cpp"
elif $hosted_xinput; then
    cat > "$build/xinput1_4.def" <<'EOF'
LIBRARY xinput1_4.dll
EXPORTS
    XInputGetCapabilities
    XInputGetState
EOF
    "${LLD_LINK:-lld-link}" /lib /nologo /machine:x64 "/def:$build/xinput1_4.def" "/out:$build/xinput1_4.lib"
    graphics_libs=("$build/xinput1_4.lib")
    child="$src/xinput-child.c"
    child_output="$build/standard-xinput.exe"
elif $display_config; then
    child="$src/display-config-child.c"
elif $player_prefs; then
    child="$src/player-prefs-child.c"
    "${WIN32_CC:?Set WIN32_CC to a Windows x64 Clang driver}" -std=c11 -O2 -ffreestanding -Wall -Wextra -Werror \
        -D_WIN32_WINNT=0x0a00 -DWINVER=0x0a00 -c "$repo/runtime/win32/player-prefs.c" -o "$build/player-prefs.obj"
    "${linker[@]}" /dll /noentry "/def:$repo/runtime/win32/player-prefs.def" "/out:$layout/XboxWin32PlayerPrefs.dll" \
        "$build/player-prefs.obj" "$windows_app"
elif $win32_opengl; then
    window_bindings="$repo/runtime/win32/window-bindings.json"
    python3 - "$build" "$window_bindings" <<'PY'
import json
from pathlib import Path
import sys

build, bindings = map(Path, sys.argv[1:])
libraries = json.loads(bindings.read_text())
libraries["opengl32.dll"] = [*libraries["opengl32.dll"], "glClearColor", "glClear", "glReadPixels", "glGetError"]
for library, functions in libraries.items():
    (build / Path(library).with_suffix(".def")).write_text(
        f"LIBRARY {library}\nEXPORTS\n    " + "\n    ".join(functions) + "\n")
PY
    for library in user32 gdi32 kernel32 opengl32; do
        "${LLD_LINK:-lld-link}" /lib /nologo /machine:x64 "/def:$build/$library.def" "/out:$build/$library.lib"
        graphics_libs+=("$build/$library.lib")
    done
    child="$src/window-child.c"
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
if $win32_opengl || $hosted_xinput; then
    "${win32_compiler[@]}" -c "$child" -o "$build/graphics-child.obj"
elif $display_config || $known_folders || $player_prefs; then
    display_flags=()
    if $hosted_display; then display_flags=(-DPROBE_HOSTED_DISPLAY); fi
    "${WIN32_CC:?Set WIN32_CC to a Windows x64 Clang driver with Windows SDK headers}" \
        -std=c11 -O2 -D_WIN32_WINNT=0x0a00 -DWINVER=0x0a00 -ffreestanding -Wall -Wextra -Werror \
        "${display_flags[@]+${display_flags[@]}}" \
        -c "$child" -o "$build/graphics-child.obj"
elif $dxgi_composition; then
    if $dxgi_exchange; then
        dxgi_flags=(-DPROBE_DXGI_WINDOW -DPROBE_DXGI_EXCHANGE)
    elif $dxgi_window; then
        dxgi_flags=(-DPROBE_DXGI_WINDOW)
    else
        dxgi_flags=()
    fi
    if $dxgi_d3d12; then dxgi_flags+=(-DPROBE_DXGI_D3D12); fi
    "${WIN32_CXX:?Set WIN32_CXX to a Windows x64 Clang++ driver with Windows SDK headers}" \
        -std=c++20 -O2 -DNDEBUG -D_WIN32_WINNT=0x0a00 -DWINVER=0x0a00 "${dxgi_flags[@]+${dxgi_flags[@]}}" \
        -ffreestanding -fno-rtti -fno-exceptions \
        -Wall -Wextra -Werror -c "$child" -o "$build/graphics-child.obj"
else
    "${compiler[@]}" -c "$child" -o "$build/graphics-child.obj"
fi
"${linker[@]}" /subsystem:windows,6.02 /entry:ChildEntry /manifest:embed /manifestuac:no "/out:$child_output" \
    "$build/graphics-child.obj" ${graphics_libs[@]+"${graphics_libs[@]}"} "$windows_app"
if $win32_imports; then
    python3 "$repo/build_tools/redirect_pe_imports.py" "$child_output" "$repo/runtime/win32/bindings.json" "$layout/child.exe"
fi
if $hosted_xinput; then
    cat > "$build/xinput-control-bindings.json" <<'EOF'
{"xinput1_4.dll":{"XInputGetCapabilities":"XboxWin32Input.dll","XInputGetState":"XboxWin32Input.dll"}}
EOF
    python3 "$repo/build_tools/redirect_pe_imports.py" "$child_output" "$build/xinput-control-bindings.json" "$layout/child.exe"
fi
if $win32_opengl; then
    python3 - "$repo" "$child_output" "$window_bindings" "$build/window-control-bindings.json" <<'PY'
import json
from pathlib import Path
import sys

repo, child, bindings, output = map(Path, sys.argv[1:])
sys.path.insert(0, str(repo))
from build_tools.redirect_pe_imports import PeImage

image = PeImage(child.read_bytes())
rules = json.loads(bindings.read_text())
used = {}
for lookup, name, iat, _ in image.import_descriptors():
    library = image.string(name).lower()
    members = rules.get(library, {})
    for _, _, symbol in image.imported_symbols(lookup, iat):
        if symbol in members:
            used.setdefault(library, {})[symbol] = members[symbol]
output.write_text(json.dumps(used))
PY
    python3 "$repo/build_tools/redirect_pe_imports.py" "$child_output" "$build/window-control-bindings.json" "$layout/child.exe"
fi
if $dxgi_window; then
    python3 "$repo/build_tools/redirect_pe_imports.py" "$child_output" "$build/dxgi-window-bindings.json" "$layout/child.exe"
fi
if $desktop_documents; then
    printf '%s\n' '{"shell32.dll":{"SHGetFolderPathW":"XboxWin32Desktop.dll"}}' > "$build/documents-bindings.json"
    python3 "$repo/build_tools/redirect_pe_imports.py" "$child_output" "$build/documents-bindings.json" "$layout/child.exe"
fi
"${compiler[@]}" -DPROBE_CHILD_WAIT_MS=15000 -c "$probe/child-process.c" -o "$build/graphics-child-process.obj"
"${linker[@]}" /dll /noentry "/implib:$build/child-process.lib" "/out:$layout/child-process.dll" \
    "$build/graphics-child-process.obj" "$windows_app"
