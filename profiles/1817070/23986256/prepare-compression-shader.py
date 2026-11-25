#!/usr/bin/env python3
"""Prepare one of the owned BC shaders that uses a dynamically indexed register array."""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess

from shader_preparation import prepare_container


# Inputs are the verified DXIL outputs of Spider Launcher's owned-file converter.
# Each tuple starts with the two block-selection conditions, then the three loads.
SHADERS = {
    "f051e9e714b1210cc2a721c2bc801b34a79fbf8d3afbe2fed7c4a28c2c72d3f0": (
        2759, 2760, 2761, 2762, 2766, ((2767, 2768, 2769), (2770, 2771, 2772), (2773, 2774, 2775)),
    ),
    "9f98b8e26c7d067e9cac38ed4eca3433b39b52ecc0bd122a533d9f3fac349bf3": (
        3269, 3270, 3271, 3272, 3278, ((3279, 3280, 3281), (3282, 3283, 3284), (3285, 3286, 3287)),
    ),
}
REGISTER_ARRAY = "%dx.v32.x01"
STORE = re.compile(
    r"^  (%bc\d+) = getelementptr \[64 x i32\], \[64 x i32\]\* %dx\.v32\.x01, i32 0, i32 (\d+)\n"
    r"(  %bc\d+ = call i32 @dx\.op\.bitcastF32toI32\(i32 127, float %[\w.]+\)[^\n]*\n)?"
    r"  store i32 (%[\w.]+), i32\* \1, align 4\n",
    re.MULTILINE,
)


def require_once(text, line):
    if text.count(line) != 1:
        raise ValueError(f"Unexpected compression shader instruction: {line.strip()}")


def scalarize(text, profile):
    condition_two, two_or_fifteen, condition_eight, selected, stride, loads = profile
    text = re.sub(r"%(\d+)\b", r"%bc\1", text)
    require_once(text, f"%bc{two_or_fifteen} = select i1 %bc{condition_two}, i32 2, i32 15")
    require_once(text, f"%bc{selected} = select i1 %bc{condition_eight}, i32 8, i32 %bc{two_or_fifteen}")
    require_once(text, f"%bc{stride} = mul i32 %bc{selected}, 4")
    allocation = f"  {REGISTER_ARRAY} = alloca [64 x i32], align 4\n"
    require_once(text, allocation)
    text = text.replace(allocation, "")
    components = {}

    def replace_store(match):
        _, index, computation, value = match.groups()
        index = int(index)
        if index in components:
            raise ValueError(f"Duplicate store to compression array index {index}.")
        components[index] = value
        return computation or ""

    text = STORE.sub(replace_store, text)
    if components.keys() != {4 * block + channel for block in range(2, 16) for channel in range(3)}:
        raise ValueError("Unexpected compression shader array initialization.")

    # The original block index can only be 8, 2 or 15, in that order of precedence.
    for channel, (index, address, result) in enumerate(loads):
        require_once(text, f"%bc{index} = add i32 %bc{stride}, {channel}")
        load = (
            f"  %bc{address} = getelementptr [64 x i32], [64 x i32]* {REGISTER_ARRAY}, i32 0, i32 %bc{index}\n"
            f"  %bc{result} = load i32, i32* %bc{address}, align 4\n"
        )
        require_once(text, load)
        selection = (
            f"  %bc_middle{channel} = select i1 %bc{condition_eight}, i32 {components[32 + channel]}, "
            f"i32 {components[60 + channel]}\n"
            f"  %bc_notmiddle{channel} = xor i1 %bc{condition_eight}, true\n"
            f"  %bc_first{channel} = and i1 %bc{condition_two}, %bc_notmiddle{channel}\n"
            f"  %bc{result} = select i1 %bc_first{channel}, i32 {components[8 + channel]}, "
            f"i32 %bc_middle{channel}\n"
        )
        text = text.replace(load, selection)
    if REGISTER_ARRAY in text:
        raise ValueError("The compression shader has another use of the register array.")
    for name, opcode, source, target in (
        ("bitcastI32toF32", 126, "i32", "float"),
        ("bitcastF32toI32", 127, "float", "i32"),
    ):
        text = re.sub(rf"call {target} @dx\.op\.{name}\(i32 {opcode}, {source} ([^)]+)\)",
                      rf"bitcast {source} \1 to {target}", text)
        text = re.sub(rf"^declare {target} @dx\.op\.{name}[^\n]*\n", "", text, flags=re.MULTILINE)
        if f"@dx.op.{name}" in text:
            raise ValueError(f"Unexpected use of the {name} intrinsic.")
    return text


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("shader", type=Path, help="owned, hash-matched DXIL container")
    cli.add_argument("output", type=Path, help="write a new validated DXIL container")
    cli.add_argument("--dxc", default="dxc")
    cli.add_argument("--dxa", default="dxa")
    cli.add_argument("--dxv", default="dxv")
    args = cli.parse_args()
    try:
        limit = 1024 * 1024
        with args.shader.open("rb") as source:
            data = source.read(limit + 1)
        profile = SHADERS.get(hashlib.sha256(data).hexdigest())
        if len(data) > limit or profile is None:
            raise ValueError("The input does not match a supported owned compression shader.")
        prepare_container(data, args.output, lambda text: scalarize(text, profile),
                          dxc=args.dxc, dxa=args.dxa, dxv=args.dxv)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        cli.error(str(error))


if __name__ == "__main__":
    main()
