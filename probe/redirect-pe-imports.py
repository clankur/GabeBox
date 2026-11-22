#!/usr/bin/env python3
"""Stage an unsigned x64 PE with per-function import bindings for an Xbox trial."""

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
MAX_PATCH_BYTES = 1024 * 1024
WINDOW_CLASS_IMPORTS = {"RegisterClassW", "RegisterClassExW", "UnregisterClassW"}
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


class PeImage:
    def __init__(self, image):
        self.image = image
        pe = self.word(0x3c)
        if image[:2] != b"MZ" or image[pe:pe + 4] != b"PE\0\0" or self.short(pe + 4) != 0x8664:
            raise ValueError("Use a Windows x64 PE executable or DLL.")
        self.section_count_offset = pe + 6
        count = self.short(self.section_count_offset)
        self.optional = pe + 24
        optional_size = self.short(pe + 20)
        self.section_table = self.optional + optional_size
        if (self.short(self.optional) != 0x20b or optional_size < 240 or self.word(self.optional + 108) < 16
                or not 1 <= count < 96):
            raise ValueError("Unsupported PE header layout.")
        self.headers_size = self.word(self.optional + 60)
        self.file_alignment = self.word(self.optional + 36)
        self.section_alignment = self.word(self.optional + 32)
        self.sections = []
        for index in range(count):
            offset = self.section_table + index * 40
            virtual_size, address, raw_size, raw = struct.unpack_from("<IIII", image, offset + 8)
            if raw_size and (raw < self.headers_size or raw + raw_size > len(image)):
                raise ValueError("Use a complete PE file with its original sections.")
            self.sections.append((virtual_size, address, raw_size, raw))
        self.new_section_offset = self.section_table + count * 40
        if self.new_section_offset + 40 > min(self.headers_size, len(image)):
            raise ValueError("The PE headers have no room for another section.")
        for directory in (4, 11):  # Authenticode and bound imports
            if self.word(self.directory(directory)) or self.word(self.directory(directory) + 4):
                raise ValueError("Use an unsigned PE without a bound import table.")

    def short(self, offset):
        return struct.unpack_from("<H", self.image, offset)[0]

    def word(self, offset):
        return struct.unpack_from("<I", self.image, offset)[0]

    def directory(self, index):
        return self.optional + 112 + 8 * index

    def locate(self, rva, size):
        for _, address, raw_size, raw in self.sections:
            if address <= rva and rva - address + size <= raw_size:
                return raw + rva - address
        raise ValueError(f"Import RVA 0x{rva:x} is outside the file-backed sections.")

    def string(self, rva):
        offset = self.locate(rva, 1)
        end = self.image.find(b"\0", offset, offset + 4096)
        if end < 0:
            raise ValueError("Unterminated PE import name.")
        self.locate(rva, end - offset + 1)
        return self.image[offset:end].decode("ascii")

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

    def redirect(self, rules):
        last_rva = max(address + max(virtual, raw_size) for virtual, address, raw_size, _ in self.sections)
        section_rva = align(max(last_rva, self.word(self.optional + 56)), self.section_alignment)
        section_file = align(len(self.image), self.file_alignment)
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
                entries.append((target, index, thunk))
            if not entries or all(entry[0] == module for entry in entries):
                descriptors.append((lookup, 0, chain, name, iat))
                continue
            for target, items in groupby(entries, key=lambda entry: entry[0]):
                run = list(items)
                # Keep each IAT cell at its original address. The loader reads
                # the relocated lookup table to determine the run's length.
                table = struct.pack(f"<{len(run) + 1}Q", *(item[2] for item in run), 0)
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
        struct.pack_into("<H", header, self.section_count_offset, len(self.sections) + 1)
        struct.pack_into("<I", header, self.optional + 8, self.word(self.optional + 8) + raw_size)
        struct.pack_into("<II", header, self.optional + 56, align(section_rva + len(data), self.section_alignment),
                         self.headers_size)
        struct.pack_into("<I", header, self.optional + 64, 0)  # Recalculate if a signer adds a checksum.
        struct.pack_into("<II", header, self.directory(1), directory_rva, len(directory))
        struct.pack_into("<8sIIIIIIHHI", header, self.new_section_offset, b".xboxabi", len(data), section_rva,
                         raw_size, section_file, 0, 0, 0, 0, 0x40000040)
        return header, bytes(section_file - len(self.image)), data + bytes(raw_size - len(data))


def patch_recipe(image, header, padding, section):
    trailer_size = len(padding) + len(section)
    if not 64 <= len(header) <= len(image) or not trailer_size:
        raise ValueError("Invalid compatibility recipe ranges.")
    if len(header) + trailer_size > MAX_PATCH_BYTES - PATCH_HEADER.size:
        raise ValueError("The compatibility patch exceeds the package recipe size limit.")
    metadata = PATCH_HEADER.pack(PATCH_MAGIC, len(image), len(header), trailer_size, hashlib.sha256(image).digest())
    return metadata + header + padding + section


def validate_patch_recipe(recipe):
    if not PATCH_HEADER.size < len(recipe) <= MAX_PATCH_BYTES:
        raise ValueError("Invalid compatibility recipe size.")
    magic, source_size, header_size, tail_size, _ = PATCH_HEADER.unpack_from(recipe)
    if magic != PATCH_MAGIC:
        raise ValueError("Invalid compatibility recipe format.")
    if (not 64 <= header_size <= source_size or not tail_size
            or source_size + tail_size > 0xFFFFFFFFFFFFFFFF
            or PATCH_HEADER.size + header_size + tail_size != len(recipe)):
        raise ValueError("Invalid compatibility recipe header.")


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("source", type=Path)
    cli.add_argument("bindings", type=Path, help="JSON mapping original DLL and function to replacement DLL")
    cli.add_argument("output", type=Path, help="new file outside the Steam install")
    cli.add_argument("--recipe", action="store_true", help="write a compact recipe for a signed runtime package")
    args = cli.parse_args()
    try:
        rules = parse_rules(json.loads(args.bindings.read_text(encoding="utf-8")))
        if args.output.exists():
            raise FileExistsError(f"Use a new output path: {args.output}")
        with args.source.open("rb") as source:
            with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as image:
                parts = PeImage(image).redirect(rules)
                recipe = patch_recipe(image, *parts) if args.recipe else None
            with tempfile.TemporaryDirectory(dir=args.output.parent) as stage:
                candidate = Path(stage) / "candidate"
                with candidate.open("wb") as result:
                    if recipe is not None:
                        result.write(recipe)
                    else:
                        header, padding, section = parts
                        result.write(header)
                        source.seek(len(header))
                        shutil.copyfileobj(source, result, length=1024 * 1024)
                        result.write(padding)
                        result.write(section)
                os.link(candidate, args.output)
    except (OSError, UnicodeError, ValueError, TypeError, struct.error) as error:
        cli.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()
