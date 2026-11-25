#!/usr/bin/env python3
"""Create x64 PE import bindings for a locally staged Xbox trial."""

import argparse
from itertools import groupby
import hashlib
import json
import mmap
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile


ORDINAL = 1 << 63
DESCRIPTOR = struct.Struct("<IIIII")
DLL_NAME = re.compile(r"[a-zA-Z0-9_.-]+\.dll", re.I)
SYMBOL = re.compile(r"[!-~]+")
PATCH_HEADER = struct.Struct("<8sQII32s")
PATCH_MAGIC = b"XWPEV1\r\n"
TRIMMED_PATCH_HEADER = struct.Struct("<8sQQII32s")
TRIMMED_PATCH_MAGIC = b"XWPEV2\r\n"
MAX_PATCH_BYTES = 1024 * 1024
WINDOW_CLASS_IMPORTS = {"RegisterClassW", "RegisterClassExW", "UnregisterClassW"}
NONZERO = re.compile(rb"[^\x00]")
DWARF_SECTIONS = frozenset({
    b".debug_abbrev", b".debug_addr", b".debug_aranges", b".debug_frame", b".debug_info",
    b".debug_line", b".debug_line_str", b".debug_loc", b".debug_loclists", b".debug_macro",
    b".debug_pubnames", b".debug_pubtypes", b".debug_ranges", b".debug_rnglists", b".debug_str", b".debug_str_offsets",
})


def align(value, alignment):
    if not alignment or alignment & (alignment - 1):
        raise ValueError("Invalid PE file or section alignment.")
    return (value + alignment - 1) & -alignment


def parse_rules(document):
    if not isinstance(document, dict) or not document:
        raise ValueError("Supply a nonempty mapping from DLLs and functions to replacement DLLs.")
    rules = {}
    for module, functions in document.items():
        if not DLL_NAME.fullmatch(module) or not isinstance(functions, dict) or not functions:
            raise ValueError(f"Invalid import mapping for {module!r}.")
        for symbol, target in functions.items():
            key = (module.lower(), symbol)
            if (not SYMBOL.fullmatch(symbol) or not isinstance(target, str) or not DLL_NAME.fullmatch(target)
                    or key in rules or key[0] == target.lower()):
                raise ValueError(f"Invalid or duplicate binding for {module}!{symbol}.")
            rules[key] = target
    return rules


class PeLayout:
    def __init__(self, image, file_size=None):
        self.image = image
        if file_size is None:
            file_size = len(image)
        pe = self.word(0x3c)
        if image[:2] != b"MZ" or image[pe:pe + 4] != b"PE\0\0" or self.short(pe + 4) != 0x8664:
            raise ValueError("Use a Windows x64 PE executable or DLL.")
        self.section_count_offset = pe + 6
        count = self.short(self.section_count_offset)
        self.optional = pe + 24
        optional_size = self.short(pe + 20)
        self.section_table = self.optional + optional_size
        if (self.short(self.optional) != 0x20b or optional_size < 240 or self.word(self.optional + 108) < 16
                or not 1 <= count <= 96):
            raise ValueError("Unsupported PE header layout.")
        self.headers_size = self.word(self.optional + 60)
        self.file_alignment = self.word(self.optional + 36)
        self.section_alignment = self.word(self.optional + 32)
        self.new_section_offset = self.section_table + count * 40
        if not self.new_section_offset <= self.headers_size <= len(image):
            raise ValueError("Invalid PE section headers.")
        self.sections = []
        for index in range(count):
            offset = self.section_table + index * 40
            virtual_size, address, raw_size, raw = struct.unpack_from("<IIII", image, offset + 8)
            if raw_size and (raw < self.headers_size or raw + raw_size > file_size):
                raise ValueError("Use a complete PE file with its original sections.")
            self.sections.append((virtual_size, address, raw_size, raw))

    def short(self, offset):
        return struct.unpack_from("<H", self.image, offset)[0]

    def word(self, offset):
        return struct.unpack_from("<I", self.image, offset)[0]

    def directory(self, index):
        return self.optional + 112 + 8 * index

    def locate(self, rva, size, sections=None):
        for _, address, raw_size, raw in self.sections if sections is None else sections:
            if address <= rva and rva - address + size <= raw_size:
                return raw + rva - address
        raise ValueError(f"PE RVA 0x{rva:x} is outside the file-backed sections.")

    def string(self, rva):
        offset = self.locate(rva, 1)
        end = self.image.find(b"\0", offset, offset + 4096)
        if end < 0:
            raise ValueError("Unterminated PE string.")
        self.locate(rva, end - offset + 1)
        return self.image[offset:end].decode("ascii")

    def exported_symbols(self):
        rva, size = struct.unpack_from("<II", self.image, self.directory(0))
        if not rva and not size:
            return set()
        if not rva or size < 40:
            raise ValueError("Invalid PE export directory.")
        offset = self.locate(rva, 40)
        base, function_count, name_count, functions, names, ordinals = struct.unpack_from("<6I", self.image, offset + 16)
        if function_count > 65536 or name_count > 65536:
            raise ValueError("Invalid PE export table size.")
        addresses = (struct.unpack_from(f"<{function_count}I", self.image, self.locate(functions, 4 * function_count))
                     if function_count else ())
        for address in addresses:
            if (address >= self.headers_size and
                    not any(rva <= address < rva + max(virtual, raw_size)
                            for virtual, rva, raw_size, _ in self.sections)):
                raise ValueError("A PE export points outside the mapped image.")
        symbols = {f"#{base + index}" for index, address in enumerate(addresses) if address}
        if name_count:
            name_offsets = self.locate(names, 4 * name_count)
            ordinal_offsets = self.locate(ordinals, 2 * name_count)
            for i in range(name_count):
                index = self.short(ordinal_offsets + i * 2)
                if index >= function_count or not addresses[index]:
                    raise ValueError("A PE export name refers to a missing entry.")
                symbols.add(self.string(self.word(name_offsets + i * 4)))
        return symbols


class PeImage(PeLayout):
    def __init__(self, image, *, allow_signed_source=False):
        super().__init__(image)
        if len(self.sections) == 96 or self.new_section_offset + 40 > self.headers_size:
            raise ValueError("The PE headers have no room for another section.")
        if self.word(self.directory(11)) or self.word(self.directory(11) + 4):
            raise ValueError("Use a PE without a bound import table.")
        self.certificate_offset = None
        offset, size = struct.unpack_from("<II", image, self.directory(4))
        if offset or size:
            if not allow_signed_source:
                raise ValueError("Use an unsigned PE or explicitly pin the signed source for a local recipe.")
            self.certificate_offset = self.certificate_suffix(offset, size)
        elif allow_signed_source:
            raise ValueError("The supplied PE does not contain an Authenticode certificate table.")

    def certificate_suffix(self, offset, size):
        if self.short(self.optional + 70) & 0x0080:  # IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY
            raise ValueError("The publisher requires image integrity checking for this PE.")
        spans = sorted((raw, raw + length) for _, _, length, raw in self.sections if length)
        raw_end = self.headers_size
        for start, end in spans:
            if start < raw_end:
                raise ValueError("The signed PE contains overlapping sections.")
            raw_end = end
        if (offset != raw_end or offset % 8 or size < 8 or size % 8 or offset + size != len(self.image)
                or self.word(self.optional - 12) or self.word(self.optional - 8)
                or any(self.image[self.new_section_offset:self.new_section_offset + 40])):
            raise ValueError("The certificate table must be a separate trailing region of the PE.")
        for index in range(len(self.sections)):
            file_fields = self.section_table + index * 40 + 24
            if any(struct.unpack_from("<IIHH", self.image, file_fields)):
                raise ValueError("The signed PE has section relocation or line number records.")
        debug_rva, debug_size = struct.unpack_from("<II", self.image, self.directory(6))
        if debug_rva or debug_size:
            if not debug_rva or not debug_size or debug_size % 28:
                raise ValueError("Invalid signed PE debug directory.")
            debug = self.locate(debug_rva, debug_size)
            for record in range(debug, debug + debug_size, 28):
                length, _, pointer = struct.unpack_from("<III", self.image, record + 16)
                if length and not any(start <= pointer < pointer + length <= end for start, end in spans):
                    raise ValueError("The signed PE references debug data outside retained sections.")
        cursor = offset
        while cursor < len(self.image):
            length, revision, kind = struct.unpack_from("<IHH", self.image, cursor)
            if length < 8 or revision != 0x0200 or kind != 2 or cursor + align(length, 8) > len(self.image):
                raise ValueError("Invalid Authenticode certificate record.")
            cursor += align(length, 8)
        return offset

    def import_descriptors(self):
        table = self.word(self.directory(1))
        count = self.word(self.directory(1) + 4) // DESCRIPTOR.size
        if not table or not count or count > 65536:
            raise ValueError("Invalid or empty PE import directory.")
        for index in range(count):
            offset = self.locate(table + index * DESCRIPTOR.size, DESCRIPTOR.size)
            lookup, timestamp, chain, name, iat = DESCRIPTOR.unpack_from(self.image, offset)
            if not (lookup or timestamp or chain or name or iat):
                return
            if not name or not iat or timestamp:
                raise ValueError("Unsupported incomplete or bound import descriptor.")
            yield lookup or iat, name, iat, chain
        raise ValueError("Unterminated PE import directory.")

    def imported_symbols(self, lookup, iat):
        for index in range(65536):
            offset = self.locate(lookup + 8 * index, 8)
            thunk = struct.unpack_from("<Q", self.image, offset)[0]
            if not thunk:
                return
            self.locate(iat + 8 * index, 8)
            symbol = f"#{thunk & 0xffff}" if thunk & ORDINAL else self.string(thunk + 2)
            yield index, thunk, symbol
        raise ValueError("Unterminated PE import lookup table.")

    def compact_mapped_sections(self, trim_dwarf):
        symbol_table, symbol_count = struct.unpack_from("<II", self.image, self.optional - 12)
        end_of_raw_sections = max((raw + size for _, _, size, raw in self.sections if size), default=0)
        if (symbol_count or not 0 < end_of_raw_sections <= symbol_table <= len(self.image) - 4
                or align(self.headers_size, self.file_alignment) != self.headers_size
                or self.word(self.optional + 108) != 16):
            raise ValueError("PE debug compaction requires an unambiguous COFF name table and directory layout.")
        table_length = self.word(symbol_table)
        if not 4 <= table_length <= len(self.image) - symbol_table:
            raise ValueError("Invalid PE debug name table.")

        names, flags = [], []
        for index in range(len(self.sections)):
            offset = self.section_table + index * 40
            name = bytes(self.image[offset:offset + 8]).rstrip(b"\0")
            if name.startswith(b"/"):
                if not name[1:].isdigit():
                    raise ValueError("Invalid PE long section name.")
                relative = int(name[1:])
                start = symbol_table + relative
                if not 4 <= relative < table_length or (relative != 4 and self.image[start - 1] != 0):
                    raise ValueError("Invalid PE long section name offset.")
                end = self.image.find(b"\0", start, symbol_table + table_length)
                if end < 0:
                    raise ValueError("Unterminated PE long section name.")
                name = bytes(self.image[start:end])
            names.append(name)
            flags.append(self.word(offset + 36))

        first = next((index for index, name in enumerate(names) if name.startswith(b".debug_")), len(names))
        removed = names[first:]
        if (not 0 < first < len(names) or len(set(removed)) != len(removed)
                or any(name not in DWARF_SECTIONS for name in removed)
                or any(value != 0x42000040 for value in flags[first:])):
            raise ValueError("PE debug compaction requires trailing, read-only discardable DWARF sections.")

        end_rva = align(self.headers_size, self.section_alignment)
        end_raw = self.headers_size
        for index, (virtual, rva, size, raw) in enumerate(self.sections):
            if (rva < end_rva or rva % self.section_alignment
                    or (size and (raw < end_raw or raw % self.file_alignment or size % self.file_alignment))):
                raise ValueError("PE debug compaction requires ordered, non-overlapping section ranges.")
            if index >= first:
                if (rva != end_rva or not 0 < virtual <= size or raw != end_raw or
                        self.image[self.section_table + index * 40 + 24:self.section_table + index * 40 + 36] != bytes(12)):
                    raise ValueError("PE debug compaction requires contiguous DWARF data without COFF relocations.")
            end_rva = align(rva + max(virtual, size), self.section_alignment)
            if size:
                end_raw = raw + size
        if end_rva != self.word(self.optional + 56):
            raise ValueError("PE debug compaction requires SizeOfImage to end with the DWARF suffix.")

        retained = self.sections[:first]
        self.validate_compact_references(retained, flags[:first], self.sections[first][1], end_rva)
        initialized = sum(section[2] for section, flag in zip(retained, flags) if flag & 0x40)
        retained_prefix = len(self.image)
        if trim_dwarf:
            for index in range(first):
                offset = self.section_table + index * 40
                if (self.image[offset:offset + 1] == b"/" or
                        self.image[offset + 24:offset + 36] != bytes(12)):
                    raise ValueError("Retained sections depend on COFF names or auxiliary records.")
            # Only the verified DWARF sections, their names, and zero padding may be discarded.
            table_end = symbol_table + table_length
            if (table_length != 4 + sum(len(name) + 1 for name in removed)
                    or sorted(self.image[symbol_table + 4:table_end].split(b"\0")) != [b""] + sorted(removed)
                    or NONZERO.search(self.image, end_raw, symbol_table)
                    or NONZERO.search(self.image, table_end)):
                raise ValueError("PE debug trimming cannot discard an unrelated file overlay.")
            retained_prefix = self.sections[first][3]
        return retained, initialized, retained_prefix

    def validate_compact_references(self, retained, flags, removed_rva, old_image_end):
        def kept(rva, size):
            try:
                return self.locate(rva, size, retained)
            except ValueError as error:
                raise ValueError(f"PE runtime reference 0x{rva:x} is outside retained data.") from error

        entry = self.word(self.optional + 16)
        if entry:
            kept(entry, 1)
            if not any(rva <= entry < rva + raw_size and flag & 0x20000000
                       for (_, rva, raw_size, _), flag in zip(retained, flags)):
                raise ValueError("PE entry point is outside retained executable code.")
        directories = {}
        for index in range(16):
            rva, size = struct.unpack_from("<II", self.image, self.directory(index))
            if rva or size:
                # Delay imports, managed images and reserved formats need their own reference validation.
                if not rva or not size or index not in {0, 1, 2, 3, 5, 6, 9, 10, 12}:
                    raise ValueError(f"PE debug compaction does not support data directory {index}.")
                directories[index] = kept(rva, size), size

        for lookup, name, iat, _ in self.import_descriptors():
            kept(name, len(self.string(name)) + 1)
            count = 0
            for _, thunk, symbol in self.imported_symbols(lookup, iat):
                count += 1
                if not thunk & ORDINAL:
                    kept(thunk, len(symbol) + 3)
            kept(lookup, (count + 1) * 8)
            kept(iat, (count + 1) * 8)

        if 6 in directories:
            debug_offset, debug_size = directories[6]
            debug = struct.Struct("<IIHHIIII")
            if debug_size % debug.size:
                raise ValueError("Invalid PE debug directory size.")
            for offset in range(debug_offset, debug_offset + debug_size, debug.size):
                _, _, _, _, _, length, rva, raw = debug.unpack_from(self.image, offset)
                if (length or rva or raw) and (not length or not rva or kept(rva, length) != raw):
                    raise ValueError("PE debug directory references data outside retained sections.")

        if 5 in directories:
            offset, size = directories[5]
            end = offset + size
            image_base = struct.unpack_from("<Q", self.image, self.optional + 24)[0]
            while offset < end:
                if end - offset < 8:
                    raise ValueError("Truncated PE base relocation block.")
                page, length = struct.unpack_from("<II", self.image, offset)
                if not 8 <= length <= end - offset or length % 4 or page % 0x1000:
                    raise ValueError("Invalid PE base relocation block.")
                for location in range(offset + 8, offset + length, 2):
                    entry = self.short(location)
                    kind, position = entry >> 12, entry & 0xfff
                    if not kind:
                        continue
                    if kind != 10:
                        raise ValueError("Unsupported PE x64 base relocation.")
                    target = kept(page + position, 8)
                    address = struct.unpack_from("<Q", self.image, target)[0] - image_base
                    if removed_rva <= address < old_image_end:
                        raise ValueError("PE base relocation points into removed DWARF sections.")
                offset += length

    def redirect(self, rules, *, omit_dwarf_mapping=False, trim_dwarf=False):
        omit_dwarf_mapping = omit_dwarf_mapping or trim_dwarf
        if self.certificate_offset is not None and omit_dwarf_mapping:
            raise ValueError("Build the signed-source recipe without a DWARF layout change.")
        mapped, initialized, retained_prefix = (self.compact_mapped_sections(trim_dwarf) if omit_dwarf_mapping
                                                else (self.sections, self.word(self.optional + 8), len(self.image)))
        if self.certificate_offset is not None:
            retained_prefix = self.certificate_offset
        last_rva = max(address + max(virtual, raw_size) for virtual, address, raw_size, _ in mapped)
        if not omit_dwarf_mapping:
            last_rva = max(last_rva, self.word(self.optional + 56))
        section_rva = align(last_rva, self.section_alignment)
        section_file = align(retained_prefix, self.file_alignment)
        found, descriptors, data = set(), [], bytearray()
        window_adapter = rules.get(("user32.dll", "CreateWindowExW"), "")
        hosted_classes = window_adapter.lower() == "xboxwin32window.dll"

        def save(content, alignment=1):
            data.extend(bytes(align(len(data), alignment) - len(data)))
            rva = section_rva + len(data)
            data.extend(content)
            return rva

        for lookup, name, iat, chain in self.import_descriptors():
            module = self.string(name)
            entries = []
            for index, thunk, symbol in self.imported_symbols(lookup, iat):
                key = (module.lower(), symbol)
                target = rules.get(key, module)
                if key in rules:
                    found.add(key)
                if hosted_classes and key[0] == "user32.dll" and symbol in WINDOW_CLASS_IMPORTS:
                    if key in rules and target.lower() != window_adapter.lower():
                        raise ValueError(f"Use the hosted window adapter for {module}!{symbol}.")
                    target = window_adapter
                entries.append((target, index, thunk, symbol))
            if not entries or all(entry[0] == module for entry in entries):
                descriptors.append((lookup, 0, chain, name, iat))
                continue
            for target, items in groupby(entries, key=lambda entry: entry[0]):
                run = list(items)
                # Keep each IAT cell at its original address. The loader reads
                # the relocated lookup table to determine the run's length.
                # Keep redirected symbol names in the recipe so packaging can
                # check the new DLL without reading the installed game.
                thunks = [save(b"\0\0" + symbol.encode("ascii") + b"\0", 2)
                          if target != module and not thunk & ORDINAL else thunk
                          for _, _, thunk, symbol in run]
                table = struct.pack(f"<{len(run) + 1}Q", *thunks, 0)
                target_name = name if target == module else save(target.encode("ascii") + b"\0")
                descriptors.append((save(table, 8), 0, 0, target_name, iat + run[0][1] * 8))
        if found != rules.keys():
            missing = ", ".join(f"{dll}!{symbol}" for dll, symbol in sorted(rules.keys() - found))
            raise ValueError(f"The input PE does not import {missing}.")

        descriptors.append((0, 0, 0, 0, 0))
        directory = b"".join(DESCRIPTOR.pack(*entry) for entry in descriptors)
        directory_rva = save(directory, 4)
        raw_size = align(len(data), self.file_alignment)
        header = bytearray(self.image[:self.headers_size])
        section_offset = self.section_table + len(mapped) * 40
        if omit_dwarf_mapping:
            header[section_offset:self.new_section_offset] = bytes(self.new_section_offset - section_offset)
        if trim_dwarf:
            struct.pack_into("<I", header, self.optional - 12, 0)  # No retained COFF names.
        if self.certificate_offset is not None:
            # The certificate authenticates the original file. The compatibility copy has new imports.
            struct.pack_into("<II", header, self.directory(4), 0, 0)
        struct.pack_into("<H", header, self.section_count_offset, len(mapped) + 1)
        struct.pack_into("<I", header, self.optional + 8, initialized + raw_size)
        struct.pack_into("<II", header, self.optional + 56, align(section_rva + len(data), self.section_alignment),
                         self.headers_size)
        struct.pack_into("<I", header, self.optional + 64, 0)  # Recalculate if a signer adds a checksum.
        struct.pack_into("<II", header, self.directory(1), directory_rva, len(directory))
        struct.pack_into("<8sIIIIIIHHI", header, section_offset, b".xboxabi", len(data), section_rva,
                         raw_size, section_file, 0, 0, 0, 0, 0x40000040)
        return header, bytes(section_file - retained_prefix), data + bytes(raw_size - len(data)), retained_prefix


def patch_recipe(image, header, padding, section, retained_prefix, *, signed_source_sha256=None):
    trailer_size = len(padding) + len(section)
    trimmed = retained_prefix != len(image)
    metadata = TRIMMED_PATCH_HEADER if trimmed else PATCH_HEADER
    if not 64 <= len(header) <= retained_prefix <= len(image) or not trailer_size:
        raise ValueError("Invalid compatibility recipe ranges.")
    if len(header) + trailer_size > MAX_PATCH_BYTES - metadata.size:
        raise ValueError("The compatibility patch exceeds the package recipe size limit.")
    digest = hashlib.sha256(image).digest()
    if signed_source_sha256 is not None and digest.hex() != signed_source_sha256:
        raise ValueError("The signed source does not match the requested SHA-256.")
    identity = (len(header), trailer_size, digest)
    prefix = (metadata.pack(TRIMMED_PATCH_MAGIC, len(image), retained_prefix, *identity) if trimmed else
              metadata.pack(PATCH_MAGIC, len(image), *identity))
    return prefix + header + padding + section


def validate_patch_recipe(recipe):
    if not PATCH_HEADER.size < len(recipe) <= MAX_PATCH_BYTES:
        raise ValueError("Invalid compatibility recipe size.")
    magic = recipe[:8]
    metadata = PATCH_HEADER
    if magic == PATCH_MAGIC:
        _, source_size, header_size, tail_size, _ = metadata.unpack_from(recipe)
        retained_prefix = source_size
    elif magic == TRIMMED_PATCH_MAGIC and len(recipe) > TRIMMED_PATCH_HEADER.size:
        metadata = TRIMMED_PATCH_HEADER
        _, source_size, retained_prefix, header_size, tail_size, _ = metadata.unpack_from(recipe)
        if retained_prefix >= source_size:
            raise ValueError("Invalid compatibility recipe retained range.")
    else:
        raise ValueError("Invalid compatibility recipe format.")
    if (not 64 <= header_size <= retained_prefix or not tail_size
            or retained_prefix + tail_size > 0xFFFFFFFFFFFFFFFF
            or metadata.size + header_size + tail_size != len(recipe)):
        raise ValueError("Invalid compatibility recipe header.")
    tail = metadata.size + header_size
    return recipe[metadata.size:tail], retained_prefix, recipe[tail:]


def patch_recipe_imports(recipe):
    """Read redirected imports. Legacy recipes leave named thunks in the source image."""
    header, retained, tail = validate_patch_recipe(recipe)
    try:
        layout = PeLayout(header, retained + len(tail))
    except struct.error as error:
        raise ValueError("Invalid PE headers in the compatibility recipe.") from error
    virtual, address, raw_size, raw = layout.sections[-1]
    if (len(header) != layout.headers_size
            or header[layout.new_section_offset - 40:layout.new_section_offset - 32] != b".xboxabi"
            or raw < retained or raw + raw_size != retained + len(tail) or not 0 < virtual <= raw_size):
        raise ValueError("Missing appended import section in the compatibility recipe.")
    data = tail[raw - retained:raw - retained + virtual]
    original_sections = layout.sections[:-1]

    def offset(rva, size):
        index = rva - address
        if not 0 <= index <= len(data) - size:
            raise ValueError("A redirected import points outside the appended import section.")
        return index

    def name_at(rva):
        index = offset(rva, 1)
        end = data.find(b"\0", index, index + 4096)
        if end <= index:
            raise ValueError("Missing import name in the compatibility recipe.")
        return data[index:end].decode("ascii")

    table, size = struct.unpack_from("<II", header, layout.directory(1))
    if not size or size % DESCRIPTOR.size:
        raise ValueError("Invalid import table size in the compatibility recipe.")
    start = offset(table, size)
    for entry in range(start, start + size, DESCRIPTOR.size):
        lookup, timestamp, chain, name, iat = DESCRIPTOR.unpack_from(data, entry)
        if not (lookup or timestamp or chain or name or iat):
            return
        if not name or not iat or timestamp:
            raise ValueError("Invalid import descriptor in the compatibility recipe.")
        layout.locate(iat, 8, original_sections)
        if not address <= name < address + virtual:
            layout.locate(name, 1, original_sections)
            continue  # Unchanged module name lives in the original image.
        module = name_at(name)
        if not DLL_NAME.fullmatch(module):
            raise ValueError(f"Invalid redirected DLL name {module!r}.")
        index = offset(lookup, 8)
        while index <= len(data) - 8:
            thunk = struct.unpack_from("<Q", data, index)[0]
            if not thunk:
                break
            if thunk & ORDINAL:
                if thunk & ~(ORDINAL | 0xffff):
                    raise ValueError("Invalid ordinal import in the compatibility recipe.")
                yield module, f"#{thunk & 0xffff}"
            elif address <= thunk < address + virtual:
                yield module, name_at(thunk + 2)
            else:
                layout.locate(thunk, 3, original_sections)
                yield module, None
            index += 8
        else:
            raise ValueError("Unterminated redirected lookup table in the compatibility recipe.")
    raise ValueError("Unterminated import table in the compatibility recipe.")


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("source", type=Path)
    cli.add_argument("bindings", type=Path, help="JSON mapping original DLL and function to replacement DLL")
    cli.add_argument("output", type=Path, help="new file outside the Steam install")
    cli.add_argument("--recipe", action="store_true", help="write a compact recipe for a signed runtime package")
    cli.add_argument("--signed-source-sha256", metavar="SHA256",
                     help="pin an original signed PE for an unsigned, locally staged compatibility copy")
    cli.add_argument("--omit-dwarf-mapping", action="store_true",
                     help="omit a verified trailing DWARF mapping while keeping the full original file in the recipe")
    cli.add_argument("--trim-dwarf", action="store_true",
                     help="omit a verified trailing DWARF mapping and its on-disk debug bytes")
    args = cli.parse_args()
    if args.signed_source_sha256 is not None:
        args.signed_source_sha256 = args.signed_source_sha256.lower()
        if not args.recipe or not re.fullmatch(r"[0-9a-f]{64}", args.signed_source_sha256):
            cli.error("--signed-source-sha256 requires --recipe and the original file's 64-digit SHA-256")
    try:
        rules = parse_rules(json.loads(args.bindings.read_text(encoding="utf-8")))
        if args.output.exists():
            raise FileExistsError(f"Use a new output path: {args.output}")
        with args.source.open("rb") as source:
            with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as image:
                candidate_image = bytes(image) if args.signed_source_sha256 is not None else image
                parts = PeImage(candidate_image, allow_signed_source=args.signed_source_sha256 is not None).redirect(
                    rules, omit_dwarf_mapping=args.omit_dwarf_mapping, trim_dwarf=args.trim_dwarf)
                recipe = (patch_recipe(candidate_image, *parts, signed_source_sha256=args.signed_source_sha256)
                          if args.recipe else None)
            with tempfile.TemporaryDirectory(dir=args.output.parent) as stage:
                candidate = Path(stage) / "candidate"
                with candidate.open("wb") as result:
                    if recipe is not None:
                        result.write(recipe)
                    else:
                        header, padding, section, retained_prefix = parts
                        result.write(header)
                        source.seek(len(header))
                        if args.trim_dwarf:
                            while source.tell() < retained_prefix:
                                chunk = source.read(min(1024 * 1024, retained_prefix - source.tell()))
                                if not chunk:
                                    raise ValueError("The PE source changed while copying retained bytes.")
                                result.write(chunk)
                        else:
                            shutil.copyfileobj(source, result, length=1024 * 1024)
                        result.write(padding)
                        result.write(section)
                os.link(candidate, args.output)
    except (OSError, UnicodeError, ValueError, TypeError, struct.error) as error:
        cli.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()
