import struct
import hashlib
from pathlib import Path
import runpy
import unittest


tool = runpy.run_path(str(Path(__file__).resolve().parents[1] / "probe" / "redirect-pe-imports.py"))


def fixture():
    image = bytearray(0x600)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 0x3c, 0x80)
    image[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", image, 0x84, 0x8664, 1)
    struct.pack_into("<H", image, 0x94, 240)
    optional = 0x98
    struct.pack_into("<H", image, optional, 0x20b)
    struct.pack_into("<II", image, optional + 32, 0x1000, 0x200)
    struct.pack_into("<II", image, optional + 56, 0x2000, 0x200)
    struct.pack_into("<I", image, optional + 108, 16)
    struct.pack_into("<II", image, optional + 120, 0x1000, 40)
    struct.pack_into("<8sIIIIIIHHI", image, optional + 240, b".idata", 0x400, 0x1000,
                     0x400, 0x200, 0, 0, 0, 0, 0x40000040)
    struct.pack_into("<IIIII", image, 0x200, 0x1040, 0, 0, 0x1080, 0x1060)
    for file_offset in (0x240, 0x260):
        struct.pack_into("<4Q", image, file_offset, 0x1090, 0x10a0, 0x10b0, 0)
    image[0x280:0x28a] = b"ole32.dll\0"
    image[0x292:0x297] = b"Init\0"
    image[0x2a2:0x2af] = b"GetErrorInfo\0"
    image[0x2b2:0x2b7] = b"Quit\0"
    return image


def dwarf_fixture():
    base = fixture()
    image = base[:0x200] + bytearray(0x200) + base[0x200:] + bytearray(0x400)
    optional = 0x98
    sections = optional + 240
    struct.pack_into("<H", image, 0x86, 3)
    struct.pack_into("<II", image, 0x8c, len(image), 0)
    struct.pack_into("<I", image, optional + 16, 0x1000)
    struct.pack_into("<II", image, optional + 56, 0x4000, 0x400)
    struct.pack_into("<II", image, optional + 112 + 8 * 12, 0x1060, 32)
    struct.pack_into("<8sIIIIIIHHI", image, sections, b".text", 0x400, 0x1000,
                     0x400, 0x400, 0, 0, 0, 0, 0x60000020)
    struct.pack_into("<8sIIIIIIHHI", image, sections + 40, b"/4", 0x180, 0x2000,
                     0x200, 0x800, 0, 0, 0, 0, 0x42000040)
    struct.pack_into("<8sIIIIIIHHI", image, sections + 80, b"/16", 0x130, 0x3000,
                     0x200, 0xa00, 0, 0, 0, 0, 0x42000040)
    names = b".debug_info\0.debug_line\0"
    image += struct.pack("<I", len(names) + 4) + names
    return image


def recipe_fixture(target="oleaut32.dll", *, trimmed=False, ordinal=None):
    source = dwarf_fixture() if trimmed else fixture()
    symbol = "GetErrorInfo"
    if ordinal is not None:
        symbol = f"#{ordinal}"
        base = 0x200 if trimmed else 0
        for slot in (0x248, 0x268):
            struct.pack_into("<Q", source, slot + base, tool["ORDINAL"] | ordinal)
    rules = tool["parse_rules"]({"ole32.dll": {symbol: target}})
    return tool["patch_recipe"](source, *tool["PeImage"](source).redirect(rules, trim_dwarf=trimmed))


def export_fixture(symbol="GetErrorInfo", ordinal=1, *, aliases=()):
    image = fixture()
    names = sorted(((symbol,) if symbol else ()) + aliases)
    struct.pack_into("<II", image, 0x98 + 112, 0x1100, 0x90)
    struct.pack_into("<6I", image, 0x310, ordinal, 1, len(names), 0x1140, 0x1150, 0x1160)
    struct.pack_into("<I", image, 0x340, 0x1000)
    offset = 0x370
    for i, name in enumerate(names):
        struct.pack_into("<I", image, 0x350 + i * 4, offset + 0xe00)
        text = name.encode("ascii") + b"\0"
        image[offset:offset + len(text)] = text
        offset += len(text)
    return image


class PeImportRedirectTests(unittest.TestCase):
    def test_split_lookup_tables_keep_every_original_iat_slot(self):
        image = bytes(fixture())
        bindings = tool["parse_rules"]({"ole32.dll": {"GetErrorInfo": "oleaut32.dll"}})
        header, padding, added_section, _ = tool["PeImage"](image).redirect(bindings)
        result = bytes(header) + image[len(header):] + padding + added_section
        patched = tool["PeImage"](result)
        bindings = []
        for lookup, name, iat, _ in patched.import_descriptors():
            for index, _, symbol in patched.imported_symbols(lookup, iat):
                bindings.append((patched.string(name), iat + index * 8, symbol))
        self.assertEqual(bindings, [
            ("ole32.dll", 0x1060, "Init"), ("oleaut32.dll", 0x1068, "GetErrorInfo"),
            ("ole32.dll", 0x1070, "Quit"),
        ])
        self.assertEqual(result[0x200:len(image)], image[0x200:])

    def test_missing_rule_and_authenticode_are_rejected(self):
        image = fixture()
        with self.assertRaisesRegex(ValueError, "does not import"):
            tool["PeImage"](image).redirect(tool["parse_rules"]({"ole32.dll": {"Other": "oleaut32.dll"}}))
        struct.pack_into("<II", image, 0x98 + 112 + 8 * 4, 0x600, 64)
        with self.assertRaisesRegex(ValueError, "unsigned"):
            tool["PeImage"](image)

    def test_hash_pinned_signed_source_keeps_payload_and_covers_certificate_bytes(self):
        image = fixture()
        prefix = len(image)
        certificate = struct.pack("<IHH", 24, 0x0200, 2) + b"certificate-data"
        struct.pack_into("<II", image, 0x98 + 112 + 8 * 4, prefix, len(certificate))
        image.extend(certificate)
        pin = hashlib.sha256(image).hexdigest()
        source = tool["PeImage"](image, allow_signed_source=True)
        rules = tool["parse_rules"]({"ole32.dll": {"GetErrorInfo": "oleaut32.dll"}})
        parts = source.redirect(rules)
        header, padding, added_section, retained = parts
        self.assertEqual(retained, prefix)
        self.assertEqual(struct.unpack_from("<II", header, source.directory(4)), (0, 0))
        staged = bytes(header) + image[len(header):retained] + padding + added_section
        self.assertEqual(staged[len(header):retained], image[len(header):retained])
        self.assertNotIn(certificate, staged)
        tool["PeImage"](staged)
        recipe = tool["patch_recipe"](image, *parts, signed_source_sha256=pin)
        metadata = tool["TRIMMED_PATCH_HEADER"].unpack_from(recipe)
        self.assertEqual(metadata[:3], (b"XWPEV2\r\n", len(image), prefix))
        self.assertEqual(metadata[-1].hex(), pin)
        image[-1] ^= 1
        with self.assertRaisesRegex(ValueError, "requested SHA-256"):
            tool["patch_recipe"](image, *parts, signed_source_sha256=pin)

    def test_signed_source_requires_a_valid_suffix_and_honors_image_integrity(self):
        image = fixture()
        prefix = len(image)
        struct.pack_into("<II", image, 0x98 + 112 + 8 * 4, prefix, 24)
        image.extend(struct.pack("<IHH", 24, 0x0200, 2) + bytes(16))
        with self.assertRaisesRegex(ValueError, "separate trailing region"):
            tool["PeImage"](image + bytes(8), allow_signed_source=True)
        malformed = bytearray(image)
        struct.pack_into("<I", malformed, prefix, 32)
        with self.assertRaisesRegex(ValueError, "certificate record"):
            tool["PeImage"](malformed, allow_signed_source=True)
        struct.pack_into("<H", image, 0x98 + 70, 0x0080)
        with self.assertRaisesRegex(ValueError, "publisher requires image integrity"):
            tool["PeImage"](image, allow_signed_source=True)

    def test_hosted_window_creation_keeps_class_lifecycle_in_one_adapter(self):
        image = fixture()
        image[0x280:0x28b] = b"user32.dll\0"
        names = (b"RegisterClassW", b"UnregisterClassW", b"CreateWindowExW", b"GetClassNameW")
        addresses = range(0x10a0, 0x1120, 0x20)
        for rva, name in zip(addresses, names):
            offset = rva - 0xe00
            image[offset:offset + len(name) + 3] = b"\0\0" + name + b"\0"
        struct.pack_into("<I", image, 0x210, 0x1140)
        for offset in (0x240, 0x340):
            struct.pack_into("<5Q", image, offset, *addresses, 0)
        target = "XboxWin32Window.dll"
        rules = tool["parse_rules"]({"user32.dll": {"CreateWindowExW": target}})
        header, padding, added_section, _ = tool["PeImage"](image).redirect(rules)
        patched = tool["PeImage"](bytes(header) + image[len(header):] + padding + added_section)
        bindings = [(iat + index * 8, symbol, patched.string(name))
                    for lookup, name, iat, _ in patched.import_descriptors()
                    for index, _, symbol in patched.imported_symbols(lookup, iat)]
        self.assertEqual(bindings, [(0x1140 + index * 8, name.decode("ascii"),
                                     target if index != 3 else "user32.dll")
                                    for index, name in enumerate(names)])
        rules["user32.dll", "UnregisterClassW"] = "Other.dll"
        with self.assertRaisesRegex(ValueError, "hosted window adapter"):
            tool["PeImage"](image).redirect(rules)

    def test_compact_recipe_pins_the_original_and_reconstructs_the_same_pe(self):
        image = bytes(fixture())
        rules = tool["parse_rules"]({"ole32.dll": {"GetErrorInfo": "oleaut32.dll"}})
        parts = tool["PeImage"](image).redirect(rules)
        header, padding, section, _ = parts
        recipe = tool["patch_recipe"](image, *parts)
        metadata = tool["PATCH_HEADER"]
        magic, original_size, header_size, trailer_size, digest = metadata.unpack_from(recipe)
        self.assertEqual((magic, original_size, digest), (b"XWPEV1\r\n", len(image), hashlib.sha256(image).digest()))
        self.assertEqual(metadata.size + header_size + trailer_size, len(recipe))
        self.assertEqual(recipe[metadata.size:metadata.size + header_size] + image[header_size:]
                         + recipe[metadata.size + header_size:], header + image[len(header):] + padding + section)

    def test_appended_imports_can_be_verified_without_the_original_image(self):
        imports = tool["patch_recipe_imports"]
        self.assertEqual(list(imports(recipe_fixture())), [("oleaut32.dll", "GetErrorInfo")])
        self.assertEqual(list(imports(recipe_fixture(trimmed=True))), [("oleaut32.dll", "GetErrorInfo")])
        self.assertEqual(list(imports(recipe_fixture(ordinal=189))), [("oleaut32.dll", "#189")])

        # A V1 lookup entry from before the change points to the original hint/name table.
        legacy = bytearray(recipe_fixture())
        header, prefix, tail = tool["validate_patch_recipe"](legacy)
        pe = tool["PeLayout"](header, prefix + len(tail))
        virtual_size, rva, _, raw = pe.sections[-1]
        section = len(legacy) - len(tail) + raw - prefix
        entry = section + pe.word(pe.directory(1)) - rva + tool["DESCRIPTOR"].size
        lookup = struct.unpack_from("<I", legacy, entry)[0]
        struct.pack_into("<Q", legacy, section + lookup - rva, 0x10a0)
        self.assertEqual(list(imports(legacy)), [("oleaut32.dll", None)])

        struct.pack_into("<Q", legacy, section + lookup - rva, rva + virtual_size)
        with self.assertRaisesRegex(ValueError, "outside the file-backed sections"):
            list(imports(legacy))
        struct.pack_into("<I", legacy, entry, rva - 8)
        with self.assertRaisesRegex(ValueError, "outside the appended import section"):
            list(imports(legacy))

    def test_export_inspection_reads_names_and_noname_ordinals(self):
        inspector = tool["PeLayout"]
        self.assertEqual(inspector(export_fixture()).exported_symbols(), {"GetErrorInfo", "#1"})
        self.assertEqual(inspector(export_fixture(None, ordinal=189)).exported_symbols(), {"#189"})
        malformed = export_fixture(ordinal=189)
        struct.pack_into("<I", malformed, 0x340, 0)
        with self.assertRaisesRegex(ValueError, "refers to a missing entry"):
            inspector(malformed).exported_symbols()
        struct.pack_into("<I", malformed, 0x340, 0xfffffffc)
        with self.assertRaisesRegex(ValueError, "outside the mapped image"):
            inspector(malformed).exported_symbols()

    def test_dwarf_omission_compacts_only_the_opt_in_mapped_section_table(self):
        image = bytes(dwarf_fixture())
        rules = tool["parse_rules"]({"ole32.dll": {"GetErrorInfo": "oleaut32.dll"}})
        source = tool["PeImage"](image)
        default_header, _, _, _ = source.redirect(rules)
        self.assertEqual(struct.unpack_from("<I", default_header, 0x98 + 56)[0], 0x5000)

        parts = source.redirect(rules, omit_dwarf_mapping=True)
        header, padding, section, _ = parts
        recipe = tool["patch_recipe"](image, *parts)
        magic, original_size, header_size, _, digest = tool["PATCH_HEADER"].unpack_from(recipe)
        self.assertEqual((magic, original_size, digest), (b"XWPEV1\r\n", len(image), hashlib.sha256(image).digest()))
        result = bytes(header) + image[header_size:] + padding + section
        patched = tool["PeImage"](result)
        self.assertEqual(patched.word(0x98 + 56), 0x3000)
        self.assertEqual(patched.sections[:-1], source.sections[:1])
        self.assertEqual(patched.sections[-1][1:], (0x2000, len(section), len(image) + len(padding)))
        self.assertEqual(patched.word(patched.directory(12)), source.word(source.directory(12)))
        self.assertEqual(patched.locate(0x1060, 32), source.locate(0x1060, 32))
        self.assertEqual(result[header_size:len(image)], image[header_size:])
        self.assertEqual(result[source.section_table + 80:source.section_table + 120], bytes(40))
        imports = [(iat + index * 8, symbol, patched.string(name))
                   for lookup, name, iat, _ in patched.import_descriptors()
                   for index, _, symbol in patched.imported_symbols(lookup, iat)]
        self.assertEqual(imports, [(0x1060, "Init", "ole32.dll"), (0x1068, "GetErrorInfo", "oleaut32.dll"),
                                   (0x1070, "Quit", "ole32.dll")])

    def test_trimmed_recipe_keeps_the_complete_original_hash_and_only_copies_retained_bytes(self):
        image = bytes(dwarf_fixture())
        rules = tool["parse_rules"]({"ole32.dll": {"GetErrorInfo": "oleaut32.dll"}})
        source = tool["PeImage"](image)
        parts = source.redirect(rules, trim_dwarf=True)
        header, padding, section, retained = parts
        recipe = tool["patch_recipe"](image, *parts)
        tool["validate_patch_recipe"](recipe)
        metadata = tool["TRIMMED_PATCH_HEADER"]
        magic, source_size, prefix, header_size, tail_size, digest = metadata.unpack_from(recipe)
        self.assertEqual((magic, source_size, prefix, digest),
                         (b"XWPEV2\r\n", len(image), 0x800, hashlib.sha256(image).digest()))
        self.assertEqual(retained, prefix)
        self.assertEqual(tail_size, len(padding) + len(section))
        self.assertNotEqual(digest, hashlib.sha256(image[:prefix]).digest())
        patched = tool["PeImage"](header + image[header_size:prefix] + padding + section)
        self.assertEqual(patched.word(source.optional - 12), 0)
        self.assertEqual(patched.image[header_size:prefix], image[header_size:prefix])
        self.assertEqual(patched.sections[-1][3], prefix)
        self.assertEqual(patched.sections[:-1], source.sections[:1])

        malformed = bytearray(recipe)
        for bogus_prefix in (header_size - 1, source_size):
            struct.pack_into("<Q", malformed, 16, bogus_prefix)
            with self.assertRaisesRegex(ValueError, "compatibility recipe"):
                tool["validate_patch_recipe"](malformed)

    def test_trimmed_recipe_rejects_an_overlay_but_mapping_only_compaction_keeps_it(self):
        image = bytes(dwarf_fixture()) + b"game data"
        rules = tool["parse_rules"]({"ole32.dll": {"GetErrorInfo": "oleaut32.dll"}})
        source = tool["PeImage"](image)
        self.assertEqual(source.redirect(rules, omit_dwarf_mapping=True)[-1], len(image))
        with self.assertRaisesRegex(ValueError, "overlay"):
            source.redirect(rules, trim_dwarf=True)

    def test_dwarf_omission_rejects_ambiguous_and_referenced_sections(self):
        optional = 0x98
        sections = optional + 240
        rules = tool["parse_rules"]({"ole32.dll": {"GetErrorInfo": "oleaut32.dll"}})
        changes = [
            (sections + 80 + 36, 0x40000040, "discardable"),
            (sections + 40 + 20, 0x600, "non-overlapping"),
            (optional + 16, 0x2000, "runtime reference"),
            (optional + 112 + 8 * 3, 0x3000, "runtime reference"),
            (0x410, 0x2000, "runtime reference"),
        ]
        for offset, value, message in changes:
            with self.subTest(offset=offset):
                image = dwarf_fixture()
                struct.pack_into("<I", image, offset, value)
                if offset == optional + 112 + 8 * 3:
                    struct.pack_into("<I", image, offset + 4, 12)
                with self.assertRaisesRegex(ValueError, message):
                    tool["PeImage"](image).redirect(rules, omit_dwarf_mapping=True)


if __name__ == "__main__":
    unittest.main()
