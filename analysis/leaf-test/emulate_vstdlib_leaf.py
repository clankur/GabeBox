#!/usr/bin/env python3
"""Bounded Unicorn execution of one offline Valve PE leaf; no loader/imports."""
import random
import hashlib
import struct
import sys

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9, UC_X86_REG_RIP, UC_X86_REG_RSP

if len(sys.argv) != 2:
    raise SystemExit(f"Usage: {sys.argv[0]} /tmp/extracted/vstdlib_s64.dll")

dll_path = sys.argv[1]
with open(dll_path, "rb") as f:
    dll_bytes = f.read()
actual_sha256 = hashlib.sha256(dll_bytes).hexdigest()
expected_sha256 = "d1b604121cc8aea3ad49e60a9ed861e8afdee873d4ac3ba509bbec7e5b9f18ab"
if actual_sha256 != expected_sha256:
    raise SystemExit(f"Unexpected DLL SHA-256: {actual_sha256}")
pe = pefile.PE(dll_path, fast_load=False)
image_base = pe.OPTIONAL_HEADER.ImageBase
image_size = pe.OPTIONAL_HEADER.SizeOfImage
if pe.FILE_HEADER.Machine != 0x8664 or pe.OPTIONAL_HEADER.Magic != 0x20B:
    raise SystemExit("Input is not a PE32+ AMD64 image")

export = next((e for e in pe.DIRECTORY_ENTRY_EXPORT.symbols
              if e.name == b"V_binarytohex"), None)
if export is None:
    raise SystemExit("V_binarytohex export not found")
if export.address != 0x198B0:
    raise SystemExit(f"Unexpected V_binarytohex RVA: 0x{export.address:x}")
entry = image_base + export.address
sentinel = 0x73000000
function_start = entry
# Static objdump inspection placed the function's return at RVA 0x1996e;
# this guard prevents the emulator from wandering into any other code.
function_end = image_base + 0x1996f
if not (function_start < function_end and function_end <= image_base + image_size):
    raise SystemExit("Unexpected function range")

mu = Uc(UC_ARCH_X86, UC_MODE_64)
mu.mem_map(image_base, (image_size + 0xFFF) & ~0xFFF)
mu.mem_write(image_base, pe.get_memory_mapped_image())
for base in (0x70000000, 0x71000000, 0x72000000, sentinel):
    mu.mem_map(base, 0x10000)

instruction_count = 0
def guard(_uc, address, size, _user_data):
    global instruction_count
    if address == sentinel:
        return
    instruction_count += 1
    if instruction_count > 20000:
        raise RuntimeError("instruction limit exceeded")
    if not function_start <= address < function_end:
        raise RuntimeError(f"leaf escaped to 0x{address:x}; stopping without executing it")

mu.hook_add(UC_HOOK_CODE, guard)
input_base, output_base = 0x71000000, 0x72000000
stack_top = 0x70010000
stack_pointer = stack_top - 0x108  # 16-byte aligned before a Windows x64 call

def emulate(data):
    global instruction_count
    instruction_count = 0
    output_size = 2 * len(data) + 1
    if data:
        mu.mem_write(input_base, data)
    mu.mem_write(output_base, b"\xA5" * max(1, output_size))
    mu.mem_write(stack_pointer, struct.pack("<Q", sentinel))
    mu.reg_write(UC_X86_REG_RSP, stack_pointer)
    mu.reg_write(UC_X86_REG_RCX, input_base)
    mu.reg_write(UC_X86_REG_RDX, len(data))
    mu.reg_write(UC_X86_REG_R8, output_base)
    mu.reg_write(UC_X86_REG_R9, output_size)
    mu.emu_start(entry, sentinel, count=20000)
    final_rip = mu.reg_read(UC_X86_REG_RIP)
    if final_rip != sentinel:
        raise RuntimeError(f"execution stopped at 0x{final_rip:x}, not the return sentinel")
    return mu.mem_read(output_base, output_size), instruction_count

def check(data):
    raw, steps = emulate(data)
    expected = data.hex().encode("ascii") + b"\0"
    if raw != expected:
        first = next((i for i, pair in enumerate(zip(raw, expected)) if pair[0] != pair[1]), None)
        raise AssertionError(f"mismatch at input length {len(data)}, first differing output offset {first}, got {raw[first:first+8]!r}, expected {expected[first:first+8]!r}")
    if steps == 0:
        raise AssertionError("no guest instructions executed")

cases = 0
vectors = [b"", bytes.fromhex("000aabff")]
for data in vectors:
    check(data)
    cases += 1
for size in range(257):
    check(bytes(range(size)))
    cases += 1
rng = random.Random(0x56414C56)
for size in (1, 2, 3, 7, 16, 31, 64, 255, 1024):
    check(bytes(rng.randrange(256) for _ in range(size)))
    cases += 1

print(f"PASS: emulated original V_binarytohex for {cases} valid vectors; no external code executed; sha256={actual_sha256}")
