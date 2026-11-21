#!/usr/bin/env bash
# Compile native x64 diagnostic payloads. Use a new build directory outside the checkout.
set -euo pipefail
if (( $# != 2 )); then
    echo 'Usage: build.sh /path/to/x64/WindowsApp.lib NEW_OUTPUT_DIR' >&2
    exit 2
fi
src=$(cd "$(dirname "$0")" && pwd)
library=$(realpath "$1")
mkdir "$2"
build=$(cd "$2" && pwd)
mkdir "$build/package"
cp -r "$src/Assets" "$build/package/"
cp "$src/Package.appxmanifest" "$build/package/AppxManifest.xml"
for target in probe child; do
    "${CLANG:-clang}" --target=x86_64-pc-windows-msvc -O2 -ffreestanding \
        -Wall -Wextra -Werror -c "$src/$target.c" -o "$build/$target.obj"
    if [[ "$target" == probe ]]; then entry=wWinMain; else entry=ChildEntry; fi
    "${LLD_LINK:-lld-link}" /nologo /machine:x64 /subsystem:windows,10.00 \
        /appcontainer /dynamicbase /nxcompat /highentropyva /nodefaultlib /Brepro \
        "/entry:$entry" "/out:$build/package/$target.exe" "$build/$target.obj" "$library"
done
