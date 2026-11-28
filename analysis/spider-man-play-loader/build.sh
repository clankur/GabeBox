#!/bin/sh
# Build the Spider-Man play loader (XboxWin32LoaderTrace.dll) from this snapshot.
#
#   analysis/spider-man-play-loader/build.sh OUTDIR [play|measure] [extra clang flags]
#
# play     (default) 4 pipeline-compile slots plus 2 while at least 768 MiB of commit is available (boot),
#          render/main-thread compiles first and replay compiles only when no title compile waits,
#          SoundBankAlloc 128 MiB, a 1280x720 hosted render size, no per-second counters, and the pruned
#          warm-up: the game warms from the recorded session list while three below-normal replay threads
#          precompile the rest with one compile slot left for the game (load_flags below).
# measure  the same plus one "asset perf-sec" line per second (commit, available commit, compiles, audio, ReadFile
#          calls and time).
# Frame-limit diagnostics for measure builds (off by default; analysis note "Fisk Tower frame rate"):
#   -DWAIT_PROF       samples the main and render threads about every 2 ms and writes their six busiest places and
#                     two call sites per second ("asset waitprof", waitprof.h).
#   -DGPU_PROF        hooks the device's queues: per-frame GPU busy, submission span and pacing gap every 5 s
#                     ("asset qtlframes", qtlframes.h) and per-pipeline GPU time every 10 s ("asset psotime").
#   -DNO_LISTPROF     with GPU_PROF, leaves the command lists unhooked (no per-pipeline timing, less overhead).
# Memory budget (Fisk Tower headroom, analysis note "Memory headroom variants"): SoundBankAlloc 128 MiB and a
# hosted monitor of 1280x720 (the game renders 1280x720 and upscales into the 1920x1080 swap chain) keep at
# least 250 MiB of commit free. Append -USOUNDBANK_MIB -DSOUNDBANK_MIB=224 -UHOSTED_RENDER_WIDTH
# -UHOSTED_RENDER_HEIGHT for the earlier native-1080p, 224 MiB sound-bank budget.
# The installed 1.0.63026.0 loader (29c66e2a) is play mode with -DPSO_BOOST_SLOTS=0 -DPSO_PRIORITY=0 before
# the compile pool moved to d3d12-compile-slots.h; append those flags to drop the boost and priority.
# Load time (issue #95): play and measure set -DPSO_WARMUP_LIST=1 -DPSO_REPLAY_THREADS=3 -DPSO_BG_RESERVE=1, which
# serves the game the recorded session list in place of xbox0.pso (pso-warmup.h) and replays the rest on three
# threads; unattended Play->gameplay went from about 82 s to 50 s (analysis note "Prune with three replay
# threads after the root-signature fix"). Append -UPSO_WARMUP_LIST -UPSO_REPLAY_THREADS -UPSO_BG_RESERVE for the
# earlier full warm-up (play 49071d0a at 939141e). -DPSO_SKIP_WARMUP=1 -DPSO_REPLAY_THREADS=6 -DPSO_BG_RESERVE=2
# instead hides the warm-up list entirely (trial only, off by default).
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
budget_flags="-DSOUNDBANK_MIB=128 -DHOSTED_RENDER_WIDTH=1280 -DHOSTED_RENDER_HEIGHT=720"
load_flags="-DPSO_WARMUP_LIST=1 -DPSO_REPLAY_THREADS=3 -DPSO_BG_RESERVE=1"
case $mode in
    play) mode_flags="-DPSO_SLOTS=4 -DPSO_BOOST_SLOTS=2 -DPSO_PRIORITY=1 $budget_flags $load_flags -DNO_PERF_SEC" ;;
    measure) mode_flags="-DPSO_SLOTS=4 -DPSO_BOOST_SLOTS=2 -DPSO_PRIORITY=1 $budget_flags $load_flags" ;;
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
