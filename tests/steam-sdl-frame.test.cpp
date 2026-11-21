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
        assert(input == original);
    }
}
