#ifndef XBOX_STEAM_LAUNCH_DETAIL_H
#define XBOX_STEAM_LAUNCH_DETAIL_H

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

// Steam formats the system's process creation error as a message followed
// by a parenthesized hexadecimal Win32 code. Preserve only the numeric code.
inline std::optional<std::uint32_t> SteamProcessCreateError(std::string_view buffer) {
    const auto terminator = buffer.find('\0');
    if (terminator == std::string_view::npos) return std::nullopt;
    const auto detail = buffer.substr(0, terminator);
    if (!detail.ends_with(')')) return std::nullopt;
    const auto opening = detail.rfind(" (0x");
    if (opening == std::string_view::npos) return std::nullopt;
    const auto hex = detail.substr(opening + 4, detail.size() - opening - 5);
    if (hex.empty() || hex.size() > 8) return std::nullopt;
    std::uint32_t code{};
    const auto end = hex.data() + hex.size();
    const auto parsed = std::from_chars(hex.data(), end, code, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !code) return std::nullopt;
    return code;
}

#endif
