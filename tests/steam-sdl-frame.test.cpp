#include "../probe/sdl-frame-copy.h"
#include <algorithm>
#include <cassert>
#include <vector>

int main() {
    for (std::size_t count : {0, 1, 3, 8, 31, 1280, 1921}) {
        std::vector<std::uint8_t> input(count * 4 + 8);
        for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<std::uint8_t>(37 * i);
        const auto original = input;
        std::vector<std::uint8_t> output(input.size(), 0x55);
        auto expected = output;
        std::copy_n(input.begin() + 1, count * 4, expected.begin() + 3);
        for (std::size_t i = 0; i < count; ++i) expected[3 + 4 * i + 3] = 255;
        CopyOpaqueSdlFrame(output.data() + 3, input.data() + 1, count);
        assert(output == expected);
        std::vector<std::uint16_t> encoded(2 * count);
        EncodeSdlRgb(encoded.data(), input.data() + 1, count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto low = encoded[2 * i], high = encoded[2 * i + 1];
            assert((low >> 12) == 1 && (high >> 12) == 1);
            const auto rgb = (low & 4095) | ((high & 4095) << 12);
            assert((rgb & 255) == input[1 + 4 * i]);
            assert(((rgb >> 8) & 255) == input[2 + 4 * i]);
            assert(((rgb >> 16) & 255) == input[3 + 4 * i]);
        }
        if (count) {
            const auto unchanged = EncodeSdlRgbDelta(encoded.data(), input.data(), input.data(), count);
            assert(unchanged == 1 && encoded.front() == 0x4000 + count - 1);
            auto changed = input;
            changed[0]++;
            auto n = EncodeSdlRgbDelta(encoded.data(), changed.data(), input.data(), count);
            assert(n >= 1 && encoded[0] == 0x2889);
            changed[0] = 255;
            n = EncodeSdlRgbDelta(encoded.data(), changed.data(), input.data(), count);
            assert(n >= 2 && (encoded[0] >> 12) == 1);
        }
        assert(input == original);
    }
}
