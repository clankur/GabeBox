#!/usr/bin/env bash
# Build the Steam Core diagnostic component with LLVM-MinGW and C++/WinRT.
set -euo pipefail
if (( $# != 6 )); then
    echo 'Usage: build-cross-native.sh CLANG++ CPPWINRT SDK_HEADERS.nupkg WindowsApp.lib STEAM_CORE.appx NEW_OUTPUT.dll' >&2
    exit 2
fi

source_dir=$(cd "$(dirname "$0")" && pwd)
compiler=$1
projection_generator=$(realpath "$2")
headers=$(realpath "$3")
windows_app=$(realpath "$4")
template=$(realpath "$5")
output=$6
case $("$compiler" -dumpmachine) in
    x86_64-*-windows-gnu*|x86_64-*-mingw32*) ;;
    *) echo 'Use the x64 LLVM-MinGW clang++ driver.' >&2; exit 2 ;;
esac
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

python3 - "$headers" "$template" "$work" <<'PY'
from pathlib import Path
import sys
from xml.etree import ElementTree
from zipfile import ZipFile

headers, template, work = map(Path, sys.argv[1:])
with ZipFile(headers) as archive:
    for location, name in (("c/UnionMetadata/", "Windows.winmd"), ("c/Include/", "fileapifromapp.h")):
        matches = [path for path in archive.namelist() if path.startswith(location) and path.endswith("/" + name)]
        if len(matches) != 1:
            raise SystemExit(f"The Windows SDK headers archive must contain one {name}.")
        (work / name).write_bytes(archive.read(matches[0]))

with ZipFile(template) as archive:
    identity = ElementTree.fromstring(archive.read("AppxManifest.xml")).find("{*}Identity")
    if (identity is None or identity.get("ProcessorArchitecture") != "x64"
            or identity.get("Name") != "XboxPortabilityProbe.ChildProcess.SteamCore"):
        raise SystemExit("Use the x64 Steam Core package targeted by this diagnostic build.")
    (work / "XboxProbe.Native.winmd").write_bytes(archive.read("XboxProbe.Native.winmd"))
PY

"$projection_generator" -input "$work/Windows.winmd" "$work/XboxProbe.Native.winmd" -output "$work/projection"
cat > "$work/ProbeNative.def" <<'EOF'
LIBRARY ProbeNative
EXPORTS
    DllGetActivationFactory=ProbeGetActivationFactory
    DllCanUnloadNow=ProbeCanUnloadNow
EOF
"$compiler" -std=c++20 -O2 -DWINAPI_FAMILY=WINAPI_FAMILY_APP -DWINVER=0x0a00 -D_WIN32_WINNT=0x0a00 \
    -DUNICODE -D_UNICODE "-I$work/projection" "-I$work" \
    -c "$source_dir/../runtime/steam/ProbeNative.cpp" -o "$work/ProbeNative.o"
"$compiler" -shared -static -Wl,--appcontainer "$work/ProbeNative.o" "$work/ProbeNative.def" "$windows_app" \
    -o "$work/ProbeNative.dll"
python3 - "$work/ProbeNative.dll" "$work/projection/winrt/impl/XboxProbe.Native.0.h" "$output" <<'PY'
from pathlib import Path
import re
import sys
from uuid import UUID

source, projection, output = map(Path, sys.argv[1:])
metadata = projection.read_text(encoding="utf-8-sig")
interface = re.search(r"guid_v<winrt::XboxProbe::Native::IProbeRunnerStatics>[^\n]*//\s*"
                      r"([0-9a-fA-F-]{36})", metadata)
if not interface:
    raise SystemExit("Could not read the projected Steam host interface ID.")
image = source.read_bytes()
if UUID(interface[1]).bytes_le not in image:
    raise SystemExit("The compiled Steam host interface does not match the package's WinRT metadata.")
with output.open("xb") as destination:
    destination.write(image)
PY
