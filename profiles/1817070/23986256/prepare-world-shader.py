#!/usr/bin/env python3
"""Prepare one of the verified opening-scene graphics shaders for a local Xbox trial."""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile


# Inputs are the raw outputs of the Xbox system DXBC converter, before validation.
SHADERS = {
    "4d8ffd3301ed59b5d784e7fcb7da48d74ec513838ba0044f30f3eb17884b0f7b": ("vs", 256, 24),
    "10f3bcb4f439751d834c9157a137515d3062796b0cef2571bada21a91a4f7e6f": ("ps", 16, 4),
}
ADDRESS_SPACE = " addrspace(5)"


def normalize(text, stage, constants, loads):
    if "target triple" in text or f'!{{!"{stage}", i32 6, i32 0}}' not in text:
        raise ValueError("Unexpected shader model or DXIL target.")
    for operation in ("tempRegStore.i32", "tempRegLoad.i32", "tempRegStore.f32", "tempRegLoad.f32"):
        symbol = f"@dx.op.{operation}("
        declarations = re.compile(rf"^declare [^\n]* {re.escape(symbol)}[^\n]*\n", re.MULTILINE)
        if text.count(symbol) != 1 or len(declarations.findall(text)) != 1:
            raise ValueError(f"Expected one unused {operation} declaration.")
        text = declarations.sub("", text)

    array = re.escape(f"[{constants} x float]")
    declaration = rf"^@dx\.icb = internal addrspace\(5\) constant {array} \[.*\]$"
    access = (
        rf"^  (%\d+) = getelementptr {array}, {array} addrspace\(5\)\* @dx\.icb, i32 0, i32 %\d+\n"
        r"  %\d+ = load float, float addrspace\(5\)\* \1, align 4$"
    )
    if (len(re.findall(declaration, text, re.MULTILINE)) != 1
            or len(re.findall(access, text, re.MULTILINE)) != loads
            or text.count("@dx.icb") != loads + 1
            or text.count(ADDRESS_SPACE) != 2 * loads + 1):
        raise ValueError("Unexpected use of the legacy immediate constant array.")

    # Keep the array read-only and preserve its contents and every index.
    text = text.replace(ADDRESS_SPACE, "")
    if stage == "vs":
        entry = re.search(r"^!(\d+) = !\{void \(\)\* @main, !\"main\", !\d+, !\d+, !(\d+)\}$",
                          text, re.MULTILINE)
        if not entry or f"!dx.entryPoints = !{{!{entry[1]}}}" not in text:
            raise ValueError("Unexpected vertex shader entry metadata.")
        flags = f"!{entry[2]} = !{{i32 0, i64 256}}\n"
        if text.count(flags) != 1:
            raise ValueError("Unexpected vertex shader feature flags.")
        text = text.replace(flags, f"!{entry[2]} = !{{i32 0, i64 272}}\n")
    return 'target triple = "dxil-ms-dx"\n' + text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shader", type=Path, help="raw Xbox converter output from an owned shader")
    parser.add_argument("output", type=Path, help="new file for the validated container")
    parser.add_argument("--dxc", default="dxc")
    parser.add_argument("--dxa", default="dxa")
    parser.add_argument("--dxv", default="dxv")
    args = parser.parse_args()
    try:
        with args.shader.open("rb") as shader:
            data = shader.read(64 * 1024 + 1)
        profile = SHADERS.get(hashlib.sha256(data).hexdigest())
        if not profile:
            raise ValueError("The input does not match a supported owned converter output.")
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            source = path / "input.dxil"
            ir = path / "shader.ll"
            assembled = path / "shader.dxil"
            signed = path / "validated.dxil"
            source.write_bytes(data)
            text = subprocess.check_output([args.dxc, "-dumpbin", str(source)], text=True, timeout=30)
            ir.write_text(normalize(text, *profile), encoding="utf-8")
            subprocess.run([args.dxa, f"-o={assembled}", str(ir)], timeout=30, check=True)
            subprocess.run([args.dxv, f"-o={signed}", str(assembled)], timeout=30, check=True)
            with args.output.open("xb") as output:
                output.write(signed.read_bytes())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
