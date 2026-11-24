# SPDX-License-Identifier: GPL-3.0-only
# Model format and LOD selection follow BBCPrince/UWP-Spider-Launcher ModelLod.cs.
# https://github.com/BBCPrince/UWP-Spider-Launcher/blob/1865c248825f617b24ee75b258f10e634338ff0b/asset-preparation/ModelLod.cs
# See LICENSE-mip-preparation.txt for the standalone asset tools' license.
"""Retain authored model LODs and their geometry from an owned asset."""

from array import array
from dataclasses import dataclass
from itertools import accumulate
import operator
import struct


HEADER_BYTES = 36
VERTEX_BLOCK = 32768


def require(condition, detail):
    if not condition:
        raise ValueError(detail)


@dataclass(frozen=True)
class Section:
    start: int
    size: int
    table: int


@dataclass(frozen=True)
class Mesh:
    vertex: int
    index: int
    indices: int
    vertices: int
    flags: int


def selected_elements(total, ranges):
    require(0 < total <= 16_000_000, 'Unsupported model element count.')
    selected = bytearray(total)
    for start, count in ranges:
        selected[start:start + count] = b'\1' * count
    kept = array('I', (index for index, used in enumerate(selected) if used))
    require(kept, 'Model LOD selection retained no geometry.')
    mapping = array('i', [-1]) * total
    for index, old in enumerate(kept):
        mapping[old] = index
    return kept, mapping


def select_stream(raw, total, kept, channels, interleaved=False):
    stride = 4 if interleaved else sum(channels)
    require(len(raw) == total * stride, 'Invalid model vertex stream length.')
    result = bytearray(len(kept) * stride)
    component_start = 0
    for width in channels:
        component = struct.Struct({1: '<B', 2: '<H', 4: '<I'}[width])
        values = array('I')
        for start in range(0, total, VERTEX_BLOCK):
            count = min(total - start, VERTEX_BLOCK)
            begin = start * stride + component_start * count
            if interleaved:
                planes = [component.iter_unpack(raw[begin + plane * count * width:
                                                   begin + (plane + 1) * count * width])
                          for plane in range(stride // width)]
                deltas = (sum(value << (plane * width * 8) for plane, (value,) in enumerate(parts))
                          for parts in zip(*planes))
            else:
                deltas = (value for value, in component.iter_unpack(raw[begin:begin + count * width]))
            values.extend(accumulate(deltas, operator.xor))
        for start in range(0, len(kept), VERTEX_BLOCK):
            count = min(len(kept) - start, VERTEX_BLOCK)
            begin = start * stride + component_start * count
            previous = 0
            for local in range(count):
                value = values[kept[start + local]]
                delta = value ^ previous
                previous = value
                if interleaved:
                    for plane in range(stride // width):
                        part = delta >> (plane * width * 8) & ((1 << (width * 8)) - 1)
                        component.pack_into(result, begin + (plane * count + local) * width, part)
                else:
                    component.pack_into(result, begin + local * width, delta)
        component_start += width
    return result


def select_indices(raw, meshes, vertex_map, index_map, kept_count):
    indices = array('H')
    previous = 0
    for delta, in struct.iter_unpack('<H', raw):
        previous = (previous + delta) & 0xffff
        indices.append(previous)
    remapped = array('i', [-1]) * kept_count
    for mesh in meshes:
        require(mesh.vertices and mesh.indices, 'Empty retained model subset.')
        start_vertex, start_index = vertex_map[mesh.vertex], index_map[mesh.index]
        require(vertex_map[mesh.vertex + mesh.vertices - 1] == start_vertex + mesh.vertices - 1
                and index_map[mesh.index + mesh.indices - 1] == start_index + mesh.indices - 1,
                'Noncontiguous retained model subset.')
        for offset in range(mesh.indices):
            old = indices[mesh.index + offset]
            if mesh.flags & 0x10:
                require(old < mesh.vertices, 'Relative vertex index outside model subset.')
                value = old
            else:
                require(mesh.vertex <= old < mesh.vertex + mesh.vertices, 'Vertex index outside model subset.')
                value = vertex_map[old]
            current = start_index + offset
            require(0 <= value <= 0xffff and remapped[current] in (-1, value), 'Conflicting model index remap.')
            remapped[current] = value
    packed = bytearray(kept_count * 2)
    previous = 0
    for index, value in enumerate(remapped):
        require(value >= 0, 'Unmapped retained model index.')
        struct.pack_into('<H', packed, index * 2, (value - previous) & 0xffff)
        previous = value
    return packed


def trim_model(source):
    def u32(at):
        return struct.unpack_from('<I', source, at)[0]

    def u16(at):
        return struct.unpack_from('<H', source, at)[0]

    require(len(source) >= 64 and u32(0) == 0x98906b9f and u32(36) == 0x44415431
            and u32(44) == len(source) - HEADER_BYTES, 'Unsupported model header.')
    count, payload = u16(48), HEADER_BYTES + u32(4)
    require(8 <= count <= 40 and 52 + count * 12 <= payload <= len(source)
            and u32(8) == len(source) - payload, 'Unsupported model stream header.')
    sections = {}
    for table in range(52, 52 + count * 12, 12):
        name, start, size = struct.unpack_from('<III', source, table)
        start += HEADER_BYTES
        require(name not in sections and start <= len(source) - size
                and (start >= payload or start + size <= payload), 'Invalid model section range.')
        sections[name] = Section(start, size, table)

    metadata, looks, meshes, masks = (sections[tag] for tag in (0x283d0383, 0x06eb7efc, 0x78d9cbde, 0x811902d7))
    require(all(section.start < payload for section in (metadata, looks, meshes, masks)) and metadata.size >= 108
            and not (u32(metadata.start + 40) & 0x100000), 'Unsupported model metadata.')
    total_indices, total_vertices = struct.unpack_from('<II', source, metadata.start + 100)
    look_count, mesh_count = looks.size // 32, meshes.size // 64
    require(looks.size % 32 == meshes.size % 64 == 0 and mesh_count <= 2048
            and masks.size >= 80 * look_count, 'Unsupported model look or subset table.')
    subsets = [Mesh(*struct.unpack_from('<IIIIH', source, at + 20))
               for at in range(meshes.start, meshes.start + meshes.size, 64)]
    for subset in subsets:
        require(subset.vertex + subset.vertices <= total_vertices and subset.index + subset.indices <= total_indices,
                'Model subset extends past the input geometry.')

    fallback, selected = [], set()
    for at in range(looks.start, looks.start + looks.size, 32):
        pairs = [struct.unpack_from('<HH', source, at + lod * 4) for lod in range(8)]
        chosen = next((lod for lod in (3, 4, 5, 6, 7, 2, 1, 0) if pairs[lod][1]), None)
        require(chosen is not None and chosen <= 6, 'Unsupported authored model LOD.')
        fallback.append((chosen, *pairs[chosen]))
        for start, size in pairs[chosen:]:
            require(start + size <= mesh_count, 'Model LOD references an unknown subset.')
            selected.update(range(start, start + size))

    retained = [subsets[index] for index in sorted(selected)]
    vertices, vertex_map = selected_elements(total_vertices, ((mesh.vertex, mesh.vertices) for mesh in retained))
    indices, index_map = selected_elements(total_indices, ((mesh.index, mesh.indices) for mesh in retained))
    index_tag, vertex_tag = 0x0859863d, 0xa98be69b
    require(sections[index_tag].start == payload and sections[index_tag].size == total_indices * 2,
            'Invalid model index stream.')
    vertex_streams = {vertex_tag: (4, 2, 2, 2, 2, 2, 2)}
    for tag, width in ((0x6b855eed, 2), (0x5cba9de9, 1)):
        if tag in sections:
            vertex_streams[tag] = (width,)
    require(set(vertex_streams) | {index_tag} == {name for name, section in sections.items() if section.start >= payload},
            'Unsupported model geometry stream.')

    result = bytearray(source[:payload])
    struct.pack_into('<II', result, metadata.start + 100, len(indices), len(vertices))
    for look, (chosen, start, size) in enumerate(fallback):
        mask = struct.unpack_from('<Q', source, masks.start + look * 80 + 48)[0]
        require(mask + 1536 <= masks.size, 'Model look mask is outside its table.')
        for lod in range(chosen):
            struct.pack_into('<HH', result, looks.start + look * 32 + lod * 4, start, size)
            offset = masks.start + mask + lod * 256
            result[offset:offset + 256] = bytes(256)
            for subset in range(start, start + size):
                result[offset + subset // 8] |= 1 << (subset % 8)
    for index, mesh in enumerate(subsets):
        offset = meshes.start + index * 64
        if index in selected:
            struct.pack_into('<II', result, offset + 20, vertex_map[mesh.vertex], index_map[mesh.index])
        else:
            result[offset + 20:offset + 36] = bytes(16)
            result[offset + 42] = 0

    for name in sorted((index_tag, *vertex_streams), key=lambda tag: sections[tag].start):
        section = sections[name]
        raw = memoryview(source)[section.start:section.start + section.size]
        if name == index_tag:
            stream = select_indices(raw, retained, vertex_map, index_map, len(indices))
        else:
            stream = select_stream(raw, total_vertices, vertices, vertex_streams[name], name != vertex_tag)
        result.extend(bytes((HEADER_BYTES - len(result)) % 16))
        struct.pack_into('<II', result, section.table + 4, len(result) - HEADER_BYTES, len(stream))
        result.extend(stream)
    struct.pack_into('<I', result, 8, len(result) - payload)
    struct.pack_into('<I', result, 44, len(result) - HEADER_BYTES)
    return result
