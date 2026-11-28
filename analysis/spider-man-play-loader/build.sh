#!/bin/sh
# Build the Spider-Man play loader (XboxWin32LoaderTrace.dll) from this snapshot.
#
#   analysis/spider-man-play-loader/build.sh OUTDIR [play|measure] [extra clang flags]
#
# play     (default) 4 pipeline-compile slots, SoundBankAlloc 224 MiB, no per-second counters.
# measure  the same plus one "asset perf-sec" line per second (commit, available commit, compiles, audio).
#
# Needs Apple/Homebrew clang, Homebrew mingw-w64 headers and import libraries, an lld-link (zig's
# `zig lld-link` works) and llvm-dlltool (`zig dlltool`), and the UWP WindowsApp.lib from the Windows SDK.
#   MINGW=/opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32
#   LLD_LINK="zig lld-link"  DLLTOOL="zig dlltool"  WINDOWSAPP_LIB=/path/to/um/x64/WindowsApp.lib
# The link uses /Brepro, so the same inputs give the same DLL; see the compatibility note for the SHA-256 of
# each recorded build.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=$1; mode=${2:-play}; [ $# -ge 2 ] && shift 2 || shift 1
MINGW=${MINGW:-/opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32}
LLD_LINK=${LLD_LINK:-lld-link}; DLLTOOL=${DLLTOOL:-llvm-dlltool}
: "${WINDOWSAPP_LIB:?set WINDOWSAPP_LIB to the UWP WindowsApp.lib}"
case $mode in
    play) mode_flags="-DPSO_SLOTS=4 -DSOUNDBANK_MIB=224 -DNO_PERF_SEC" ;;
    measure) mode_flags="-DPSO_SLOTS=4 -DSOUNDBANK_MIB=224" ;;
    *) echo "mode must be play or measure" >&2; exit 2 ;;
esac
mkdir -p "$out"
clang --target=x86_64-w64-mingw32 -isystem "$MINGW/include" -O2 -D_WIN32_WINNT=0x0a00 -DWINVER=0x0a00 \
    -DPLAY_BUILD -DHEAP_ADDR -DMEM_CENSUS -DMC_NO_CENSUS -DNO_PLACER -DLAZY_GENERAL -DPROC_VIEW $mode_flags "$@" \
    -fms-extensions -ffreestanding -fno-stack-protector -Wall -Wno-unused-function -Wno-unused-variable \
    -I"$here" -I"$root/runtime/win32" -c "$here/trace.c" -o "$out/trace.o"
$DLLTOOL -m i386:x86-64 -d "$here/ntdll-eh.def" -l "$out/ntdll-eh.lib"
$LLD_LINK /nologo /machine:x64 /subsystem:windows,10.00 /appcontainer /dynamicbase /nxcompat /highentropyva \
    /nodefaultlib /Brepro /dll /noentry /def:"$here/trace.def" /out:"$out/XboxWin32LoaderTrace.dll" "$out/trace.o" \
    "$WINDOWSAPP_LIB" "$MINGW/lib/libbcrypt.a" "$out/ntdll-eh.lib"
shasum -a 256 "$out/XboxWin32LoaderTrace.dll"
