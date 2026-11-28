from pathlib import Path
import runpy
import struct
import unittest


PROFILE = Path(__file__).resolve().parents[1] / 'profiles/1817070/23986256'
DISTANT = runpy.run_path(str(PROFILE / 'prepare-distant-overlay.py'))


def float_mean(a, b, c, d):
    """DirectXTex's 2:1 linear resize in float32 steps, rounded as the reference stores UNORM bytes."""
    pack = struct.Struct('<f')

    def f32(value):
        return pack.unpack(pack.pack(value))[0]

    top = f32(f32(f32(a / 255) * 0.5) + f32(f32(b / 255) * 0.5))
    bottom = f32(f32(f32(c / 255) * 0.5) + f32(f32(d / 255) * 0.5))
    value = f32(f32(f32(top * 0.5) + f32(bottom * 0.5)) * 255)
    return int(f32(value + 0.5) // 1)


class DistantOverlayTests(unittest.TestCase):
    def test_half_pixels_is_the_rounded_mean_of_each_channel_and_slice(self):
        width, height, slices, channels = 4, 2, 2, 4
        pixels = bytes((n * 37 + 11) % 256 for n in range(width * height * slices * channels))
        halved = DISTANT['half_pixels'](pixels, width, height, slices, channels)
        self.assertEqual(len(halved), len(pixels) // 4)
        row = width * channels
        for number in range(slices):
            for x in range(width // 2):
                for channel in range(channels):
                    at = number * row * height + 2 * x * channels + channel
                    quad = pixels[at], pixels[at + channels], pixels[at + row], pixels[at + row + channels]
                    got = halved[(number * (width // 2) + x) * channels + channel]
                    self.assertEqual(got, (sum(quad) + 2) >> 2)
                    self.assertEqual(got, float_mean(*quad))

    def test_integer_mean_matches_the_float_resize_at_every_tie(self):
        for total in range(0, 1021):
            quad = [min(255, total - 255 * k) if total > 255 * k else 0 for k in range(4)]
            quad[3] = total - sum(quad[:3])
            if 0 <= quad[3] <= 255:
                self.assertEqual((total + 2) >> 2, float_mean(*quad))

    def test_transform_halves_the_descriptor_and_rewrites_texture_sizes(self):
        descriptor = bytearray(164)
        struct.pack_into('<I', descriptor, 0, 0x567cc2f0)
        struct.pack_into('<II', descriptor, 116, 812, 1472)
        halved = DISTANT['transform'](bytes(descriptor))
        self.assertEqual(struct.unpack_from('<II', halved, 116), (406, 736))
        self.assertEqual(halved[:116] + halved[124:], bytes(descriptor[:116] + descriptor[124:]))

        old = 812 * 1472
        texture = bytearray(128)
        for field in (8, 20, 84):
            struct.pack_into('<I', texture, field, old)
        struct.pack_into('<HHHHH', texture, 96, 812, 1472, 1, 0, 61)
        texture += bytes(n % 251 for n in range(old))
        result = DISTANT['transform'](bytes(texture))
        new = 406 * 736
        self.assertEqual(len(result), 128 + new)
        self.assertEqual([struct.unpack_from('<I', result, field)[0] for field in (8, 20, 84)], [new] * 3)
        self.assertEqual(struct.unpack_from('<HHHH', result, 92), (406, 736, 406, 736))
        with self.assertRaises(ValueError):
            DISTANT['transform'](bytes(texture[:-1]))


if __name__ == '__main__':
    unittest.main()
