#include "../runtime/steam/steam-session-cache-policy.h"
#include <array>
#include <cassert>
#include <cstring>

int main() {
    using namespace steam_session_cache;
    constexpr std::array<std::uint32_t, 4> corruptOrMissing{
        0x80070057u, 0x80090005u, 0x8007000du, 0x80070002u
    };
    for (auto code : corruptOrMissing) {
        assert(invalidRecordRead(false, code));
        assert(!invalidRecordRead(true, code));
    }
    assert(!invalidRecordRead(false, 0x80070020u)); // sharing violation
    assert(!invalidRecordRead(false, 0x80070005u)); // temporarily inaccessible profile
    assert(!invalidRecordRead(true, 0x800705b4u));  // catalog/backend timeout

    std::array<std::uint8_t, 8> callback{};
    for (std::int32_t result : {5, 18, 19, 26, 27, 43}) {
        std::memcpy(callback.data(), &result, sizeof(result));
        callback[4] = 0;
        assert(credentialRejected(7, 7, 102, callback.data(), 8));
        assert(!credentialRejected(7, 8, 102, callback.data(), 8));
        assert(!credentialRejected(7, 7, 103, callback.data(), 8));
        assert(!credentialRejected(7, 7, 102, callback.data(), 4));
        assert(!credentialRejected(7, 7, 102, nullptr, 8));
        callback[4] = 1;
        assert(!credentialRejected(7, 7, 102, callback.data(), 8));
    }
    for (std::int32_t result : {3, 16, 20, 22, 84}) {
        std::memcpy(callback.data(), &result, sizeof(result));
        callback[4] = 0;
        assert(!credentialRejected(7, 7, 102, callback.data(), 8));
    }
}
