#!/usr/bin/env bash
# Extracts the named DLL only into /tmp, then runs bounded Unicorn emulation.
set -euo pipefail
[[ $# -eq 1 ]] || { echo "Usage: $0 /path/to/bins_win64.zip" >&2; exit 2; }
archive=$1
[[ -f "$archive" ]] || { echo "Not a file: $archive" >&2; exit 2; }
here=$(cd -- "$(dirname -- "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/vstdlib-leaf.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
unzip -p "$archive" vstdlib_s64.dll > "$work/vstdlib_s64.dll"
uv run --locked --script "$here/emulate_vstdlib_leaf.py" "$work/vstdlib_s64.dll"
