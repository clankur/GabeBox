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


class PeImportRedirectTests(unittest.TestCase):
    def test_split_lookup_tables_keep_every_original_iat_slot(self):
        image = bytes(fixture())
        bindings = tool["parse_rules"]({"ole32.dll": {"GetErrorInfo": "oleaut32.dll"}})
        header, padding, added_section = tool["PeImage"](image).redirect(bindings)
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
        header, padding, added_section = tool["PeImage"](image).redirect(rules)
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
        header, padding, section = parts
        recipe = tool["patch_recipe"](image, *parts)
        metadata = tool["PATCH_HEADER"]
        magic, original_size, header_size, trailer_size, digest = metadata.unpack_from(recipe)
        self.assertEqual((magic, original_size, digest), (b"XWPEV1\r\n", len(image), hashlib.sha256(image).digest()))
        self.assertEqual(metadata.size + header_size + trailer_size, len(recipe))
        self.assertEqual(recipe[metadata.size:metadata.size + header_size] + image[header_size:]
                         + recipe[metadata.size + header_size:], header + image[len(header):] + padding + section)
