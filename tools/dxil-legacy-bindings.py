#!/usr/bin/env python3
"""Lower fixed Shader Model 6.6 resource bindings in DXC assembly to 6.4."""

import argparse
from pathlib import Path
import re


METADATA_NODE = re.compile(r"^!(\d+) = !\{(.*)\}$", re.MULTILINE)
RESOURCE = re.compile(
    r'i32 (\d+), .*?, !"(?:[^"\\]|\\[0-9A-Fa-f]{2})*", '
    r'i32 (-?\d+), i32 (-?\d+), i32 (-?\d+)(?:,|$)'
)
HANDLE = "%dx.types.Handle"
HANDLE_VALUE = r"%[-a-zA-Z$._0-9]+"
LIFETIME_CALL = re.compile(r"^  call void @llvm\.lifetime\.(?:start|end)\(")
LIFETIME_CAST = re.compile(rf"^  ({HANDLE_VALUE}) = bitcast .* to i8\*$")
BOUND_HANDLE = re.compile(
    rf"^(  {HANDLE_VALUE} = )call {re.escape(HANDLE)} @dx\.op\.createHandleFromBinding"
    rf"\(i32 217, %dx\.types\.ResBind "
    rf"(?:\{{ i32 (-?\d+), i32 (-?\d+), i32 (-?\d+), i8 ([0-3]) \}}|zeroinitializer), "
    rf"i32 ({HANDLE_VALUE}|-?\d+), i1 (true|false)\)$"
)
ANNOTATION = re.compile(
    rf"^(  {HANDLE_VALUE} = )call {re.escape(HANDLE)} @dx\.op\.annotateHandle"
    rf"\(i32 216, {re.escape(HANDLE)} ({HANDLE_VALUE}), "
    rf"%dx\.types\.ResourceProperties (\{{ i32 -?\d+, i32 -?\d+ \}}|zeroinitializer)\)$"
)


def metadata_reference(assembly, name, *, optional=False):
    reference = re.search(rf"^!dx\.{name} = !\{{!(\d+)\}}$", assembly, re.MULTILINE)
    if not reference:
        if optional and f"!dx.{name} =" not in assembly:
            return None
        raise ValueError(f"Missing DXIL {name} metadata.")
    return reference[1]


def resource_ranges(nodes, resources):
    groups = nodes[resources].split(", ")
    if len(groups) != 4:
        raise ValueError("Expected SRV, UAV, constant buffer and sampler groups.")
    bindings = {}
    for kind, group in enumerate(groups):
        if group == "null":
            continue
        if not re.fullmatch(r"!\d+", group):
            raise ValueError("Expected a DXIL resource group reference.")
        for reference in nodes[group[1:]].split(", "):
            if not re.fullmatch(r"!\d+", reference):
                raise ValueError("Expected a DXIL resource metadata reference.")
            binding = RESOURCE.match(nodes[reference[1:]])
            if not binding:
                raise ValueError("Could not read the DXIL resource range.")
            range_id, space, first, count = map(int, binding.groups())
            space, first, count = (value & 0xFFFFFFFF for value in (space, first, count))
            last = 0xFFFFFFFF if count == 0xFFFFFFFF else first + count - 1
            if not count or last > 0xFFFFFFFF:
                raise ValueError("Invalid DXIL resource range bounds.")
            key = kind, space, first, last
            if key in bindings:
                raise ValueError("Ambiguous DXIL resource range.")
            bindings[key] = range_id
    return bindings


def remove_lifetime_markers(assembly):
    lines = assembly.splitlines()
    start = None
    for index, line in enumerate(lines):
        if line.startswith("define "):
            start = index
        if line != "}" or start is None:
            continue
        first, start = start, None
        body = lines[first:index]
        if not any(LIFETIME_CALL.match(instruction) for instruction in body):
            continue
        retained = "\n".join(
            instruction for instruction in body if not LIFETIME_CALL.match(instruction)
        )
        for position in range(first, index):
            instruction = lines[position]
            if LIFETIME_CALL.match(instruction):
                lines[position] = ""
                continue
            pointer = LIFETIME_CAST.fullmatch(instruction)
            if pointer and len(re.findall(re.escape(pointer[1]) + r"(?![-\w.$])", retained)) == 1:
                # Keep the unnamed value slot. The GPU compiler can discard the
                # unused integer and allocate the original object for its full lifetime.
                lines[position] = f"  {pointer[1]} = add i32 0, 0"
    return "\n".join(
        line for line in lines if not line.startswith("declare void @llvm.lifetime.")
    ) + "\n"


def write_legacy_uav_flag(assembly, entry, ranges):
    # The 1.4 validator counts declared UAV ranges. The 1.6 validator counts
    # the individual UAV bindings within each range.
    uavs = sum(kind == 1 for kind, _, _, _ in ranges)
    properties = re.search(r", !(\d+)$", entry)
    if properties is None:
        return assembly
    def revise_flags(match):
        flag = 1 << 15
        current = int(match[2])
        updated = (current | flag) if uavs > 8 else (current & ~flag)
        return match[1] + str(updated)
    return re.sub(rf"^(!{properties[1]} = !\{{i32 0, i64 )(\d+)", revise_flags,
                  assembly, flags=re.MULTILINE)


def lower_fixed_bindings(assembly):
    nodes = dict(METADATA_NODE.findall(assembly))
    version = metadata_reference(assembly, "version")
    validator = metadata_reference(assembly, "valver")
    shader_model = metadata_reference(assembly, "shaderModel")
    model = re.fullmatch(r'!"(cs|ps|vs|gs|hs|ds)", i32 6, i32 6', nodes[shader_model])
    if nodes[version] != "i32 1, i32 6" or not model:
        raise ValueError("Expected DXIL 1.6 and Shader Model 6.6.")
    if re.search(r"@dx\.op\.createHandle(?:FromHeap|ForLib)\(", assembly):
        raise ValueError("Shader requires heap or library resource binding.")
    resources = metadata_reference(assembly, "resources", optional=True)
    bindings = resource_ranges(nodes, resources) if resources is not None else {}
    handles = {}
    annotations = {}
    lines = []
    for line in assembly.splitlines():
        instruction = line.split("  ;", 1)[0].rstrip()
        constructor = BOUND_HANDLE.fullmatch(instruction)
        annotation = ANNOTATION.fullmatch(instruction)
        if instruction.startswith("define "):
            handles.clear()
        if constructor:
            assignment, first, last, space, kind, index, nonuniform = constructor.groups()
            kind, space, first, last = (
                int(value or "0") & 0xFFFFFFFF for value in (kind, space, first, last)
            )
            range_id = bindings.get((kind, space, first, last))
            if range_id is None:
                raise ValueError("Resource binding did not match a declared DXIL range.")
            call = (f"call {HANDLE} @dx.op.createHandle"
                    f"(i32 57, i8 {kind}, i32 {range_id}, i32 {index}, i1 {nonuniform})")
            handles[assignment.split()[0]] = ((kind, range_id), call)
            lines.append(assignment + call)
        elif annotation:
            assignment, handle, properties = annotation.groups()
            if handle not in handles:
                raise ValueError("Handle annotation did not refer to a fixed binding.")
            resource_range, call = handles[handle]
            if annotations.setdefault(resource_range, properties) != properties:
                raise ValueError("Resource range has conflicting handle annotations.")
            # A pure handle creation keeps the original SSA numbering and the
            # same descriptor. DXIL 1.4 obtains its properties from the range.
            lines.append(assignment + call)
        elif instruction.startswith(f"declare {HANDLE} @dx.op.createHandleFromBinding("):
            lines.append(f"declare {HANDLE} @dx.op.createHandle(i32, i8, i32, i32, i1)")
        elif instruction.startswith(f"declare {HANDLE} @dx.op.annotateHandle("):
            continue
        elif "@dx.op.createHandleFromBinding(" in instruction or "@dx.op.annotateHandle(" in instruction:
            raise ValueError("Unsupported DXIL resource handle instruction.")
        else:
            lines.append(line)

    lowered = remove_lifetime_markers("\n".join(lines))
    entry = nodes[metadata_reference(assembly, "entryPoints")]
    lowered = write_legacy_uav_flag(lowered, entry, bindings)
    lowered = re.sub(rf"^!(?:{version}|{validator}) = !\{{.*\}}$",
                     lambda match: match[0].split(" = ", 1)[0] + " = !{i32 1, i32 4}",
                     lowered, flags=re.MULTILINE)
    return re.sub(rf"^!{shader_model} = !\{{.*\}}$",
                  f'!{shader_model} = !{{!"{model[1]}", i32 6, i32 4}}',
                  lowered, flags=re.MULTILINE)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assembly", type=Path, help="DXC disassembly of a validated container")
    parser.add_argument("output", type=Path, help="assembly to reassemble and validate with DXC")
    args = parser.parse_args()
    try:
        lowered = lower_fixed_bindings(args.assembly.read_text(encoding="utf-8"))
        with args.output.open("x", encoding="utf-8") as output:
            output.write(lowered)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"{error}\n")
