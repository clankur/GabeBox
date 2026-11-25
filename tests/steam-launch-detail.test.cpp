#include "../runtime/steam/steam-launch-detail.h"
#include <cassert>

int main() {
    using namespace std::string_view_literals;
    assert(SteamProcessCreateError("\"Access is denied.\" (0x5)\0"sv) == 5);
    assert(SteamProcessCreateError("\"Failed to start (0x12).\" (0x2E4)\0ignored"sv) == 740);
    assert(SteamProcessCreateError("\"Process rejected.\" (0xC000007B)\0"sv) == 0xC000007B);
    assert(!SteamProcessCreateError("\"Missing terminator.\" (0x5)"sv));
    assert(!SteamProcessCreateError("\"Missing suffix.\"\0"sv));
    assert(!SteamProcessCreateError("\"Invalid suffix.\" (0x5G)\0"sv));
    assert(!SteamProcessCreateError("\"Too large.\" (0x100000000)\0"sv));
    assert(!SteamProcessCreateError("\"Unknown error.\" (0x0)\0"sv));
}
