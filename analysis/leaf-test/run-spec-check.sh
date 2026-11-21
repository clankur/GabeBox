#!/usr/bin/env bash
# Builds only our re-expression on the host; never loads or executes Valve DLLs.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/leaf-spec-check.XXXXXX")
trap 'rm -rf -- "$work"' EXIT

host_cc=${CC:-cc}
"$host_cc" -m64 -O2 -Wall -Wextra -Werror -fPIC -shared \
  "$here/v_binarytohex_equiv.c" -o "$work/libequiv.so"
python3 - "$work/libequiv.so" <<'PY'
import ctypes
import random
import sys

lib = ctypes.CDLL(sys.argv[1])
fn = lib.encode_hex
fn.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
fn.restype = ctypes.c_int

def check(data):
    src = ctypes.create_string_buffer(data) if data else None
    expected = data.hex().encode("ascii")
    out = ctypes.create_string_buffer(len(expected) + 1)
    assert fn(src, len(data), out, len(out)) == 1
    assert out.value == expected
    if len(expected):
        short = ctypes.create_string_buffer(len(expected))
        assert fn(src, len(data), short, len(short)) == 0

assert fn(None, 0, None, 0) == 0
empty = ctypes.create_string_buffer(1)
assert fn(None, 0, empty, 1) == 1 and empty.value == b""
assert bytes.fromhex("000a abff").hex() == "000aabff"
check(bytes.fromhex("000aabff"))
for n in range(257):
    check(bytes(range(n)))
rng = random.Random(0x56414c56)
for n in [1, 2, 3, 7, 16, 31, 64, 255, 1024]:
    check(bytes(rng.randrange(256) for _ in range(n)))
print("PASS: 268 output/spec comparisons; 267 invalid/short-capacity guard checks")
PY

clang_bin=${CLANG:-clang}
if command -v "$clang_bin" >/dev/null 2>&1; then
  "$clang_bin" --target=x86_64-pc-windows-msvc -O2 -Wall -Wextra -Werror -c \
    "$here/v_binarytohex_equiv.c" -o "$work/v_binarytohex_equiv.obj"
  echo "COFF object: $(file -b "$work/v_binarytohex_equiv.obj")"
else
  echo "SKIP COFF: set CLANG to a Windows-targeting Clang executable"
fi
