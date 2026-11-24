#!/usr/bin/env bash
# Compile native x64 diagnostic payloads. Use a new build directory outside the checkout.
set -euo pipefail
mouse_source=false
if (( $# == 4 )) && [[ $1 == --mouse-source ]]; then
    mouse_source=true
    version=$2
    if [[ ! $version =~ ^[0-9]{1,5}(\.[0-9]{1,5}){3}$ ]]; then
        echo 'Use a four-component APPX version.' >&2
        exit 2
    fi
    shift 2
fi
if (( $# != 2 )); then
    echo 'Usage: build.sh [--mouse-source PACKAGE_VERSION] /path/to/x64/WindowsApp.lib NEW_OUTPUT_DIR' >&2
    exit 2
fi
src=$(cd "$(dirname "$0")" && pwd)
library=$(realpath "$1")
mkdir "$2"
build=$(cd "$2" && pwd)
mkdir "$build/package"
cp -r "$src/../ui/Assets" "$build/package/"
targets=(probe child child-process)
flags=(-O2 -ffreestanding -Wall -Wextra -Werror)
if $mouse_source; then
    targets=(probe)
    flags+=(-DFHD_MOUSE_SOURCE_CONTROL)
    sed -e 's/Name="XboxPortabilityProbe.ChildProcess"/Name="XboxPortabilityProbe.ChildProcess.MouseSource"/' \
        -e "s/Version=\"1[.][0-9.]*\"/Version=\"$version\"/" \
        -e 's/Packaged process test/UWP mouse source control/g' \
        -e 's/Measures launching one fixed packaged child process/Measures relative mouse input in a foreground CoreWindow/' \
        "$src/Package.appxmanifest" > "$build/package/AppxManifest.xml"
else
    cp "$src/Package.appxmanifest" "$build/package/AppxManifest.xml"
fi
for target in "${targets[@]}"; do
    "${CLANG:-clang}" --target=x86_64-pc-windows-msvc "${flags[@]}" -c "$src/$target.c" -o "$build/$target.obj"
    case "$target" in
        probe) options=(/entry:wWinMain); extension=exe ;;
        child) options=(/entry:ChildEntry); extension=exe ;;
        child-process) options=(/dll /noentry); extension=dll ;;
    esac
    "${LLD_LINK:-lld-link}" /nologo /machine:x64 /subsystem:windows,10.00 \
        /appcontainer /dynamicbase /nxcompat /highentropyva /nodefaultlib /Brepro \
        "${options[@]}" "/out:$build/package/$target.$extension" "$build/$target.obj" "$library"
done
