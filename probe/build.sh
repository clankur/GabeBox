#!/usr/bin/env bash
# Compile native x64 diagnostic payloads. Use a new build directory outside the checkout.
set -euo pipefail
src=$(cd "$(dirname "$0")" && pwd)
if (( $# == 3 )) && [[ $1 == --mouse-source ]]; then
    version=$2
    if [[ ! $version =~ ^[0-9]{1,5}(\.[0-9]{1,5}){3}$ ]]; then
        echo 'Use a four-component APPX version.' >&2
        exit 2
    fi
    IFS=. read -r -a components <<< "$version"
    for component in "${components[@]}"; do
        if (( 10#$component > 65535 )); then
            echo 'APPX version components cannot exceed 65535.' >&2
            exit 2
        fi
    done
    mkdir "$3"
    mkdir "$3/package"
    cp -r "$src/../ui/Assets" "$3/package/"
    sed -e 's/Name="XboxPortabilityProbe.ChildProcess"/Name="XboxPortabilityProbe.ChildProcess.MouseSource"/' \
        -e "s/Version=\"1[.][0-9.]*\"/Version=\"$version\"/" \
        -e 's/Packaged process test/UWP mouse source control/g' \
        -e 's/Executable="probe.exe"/StartPage="index.html"/' \
        -e 's/EntryPoint="Probe.App"//' \
        -e 's/Measures launching one fixed packaged child process/Measures relative mouse input in a foreground CoreWindow/' \
        "$src/Package.appxmanifest" > "$3/package/AppxManifest.xml"
    cp "$src/mouse-source/index.html" "$src/mouse-source/mouse-source.js" "$src/web/probe.css" "$3/package/"
    exit 0
fi

if (( $# != 2 )); then
    echo 'Usage: build.sh /path/to/x64/WindowsApp.lib NEW_OUTPUT_DIR' >&2
    echo '       build.sh --mouse-source PACKAGE_VERSION NEW_OUTPUT_DIR' >&2
    exit 2
fi
library=$(realpath "$1")
mkdir "$2"
build=$(cd "$2" && pwd)
mkdir "$build/package"
cp -r "$src/../ui/Assets" "$build/package/"
cp "$src/Package.appxmanifest" "$build/package/AppxManifest.xml"
for target in probe child child-process; do
    "${CLANG:-clang}" --target=x86_64-pc-windows-msvc -O2 -ffreestanding \
        -Wall -Wextra -Werror -c "$src/$target.c" -o "$build/$target.obj"
    case "$target" in
        probe) options=(/entry:wWinMain); extension=exe ;;
        child) options=(/entry:ChildEntry); extension=exe ;;
        child-process) options=(/dll /noentry); extension=dll ;;
    esac
    "${LLD_LINK:-lld-link}" /nologo /machine:x64 /subsystem:windows,10.00 \
        /appcontainer /dynamicbase /nxcompat /highentropyva /nodefaultlib /Brepro \
        "${options[@]}" "/out:$build/package/$target.$extension" "$build/$target.obj" "$library"
done
