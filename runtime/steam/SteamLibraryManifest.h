#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace steam_library_storage {

struct DownloadProgress {
    std::uint64_t downloaded = 0;
    std::uint64_t downloadTotal = 0;
    std::uint64_t staged = 0;
    std::uint64_t stagingTotal = 0;
};

struct Manifest {
    std::uint32_t id = 0;
    std::uint32_t state = 0;
    std::uint64_t sizeOnDisk = 0;
    std::string name;
    std::string installDir;
    std::string buildId;
    DownloadProgress download;

    bool IsInstalled() const { return (state & 0x7000a6) == 4; }
};

template<typename Number>
inline bool ParseNumber(std::string_view text, Number &value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

inline bool IsSafeName(std::wstring_view name) {
    if (name.empty() || name.size() > 255 || name.back() == L'.' || name.back() == L' ' ||
            name.find_first_of(L"<>:\"/\\|?*") != name.npos)
        return false;
    for (auto character : name) {
        if (character < L' ') return false;
    }
    auto stem = std::wstring(name.substr(0, name.find(L'.')));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    for (auto &character : stem) {
        if (character >= L'a' && character <= L'z') character -= L'a' - L'A';
    }
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
            stem == L"CONIN$" || stem == L"CONOUT$")
        return false;
    return !(stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
        std::wstring_view(L"0123456789\u00b9\u00b2\u00b3").find(stem[3]) != std::wstring_view::npos);
}

namespace manifest_detail {

enum class Kind { String, Open, Close, End, Invalid };

struct Token {
    Kind kind;
    std::string text;
};

inline Token ReadToken(std::string_view &input) {
    for (;;) {
        auto next = input.find_first_not_of(" \r\n\t");
        if (next == input.npos) {
            input = {};
            return {Kind::End, {}};
        }
        input.remove_prefix(next);
        if (!input.starts_with("//")) break;
        input.remove_prefix(input.find('\n') == input.npos ? input.size() : input.find('\n') + 1);
    }
    auto first = input.front();
    input.remove_prefix(1);
    if (first == '{') return {Kind::Open, {}};
    if (first == '}') return {Kind::Close, {}};
    if (first != '"') return {Kind::Invalid, {}};
    std::string text;
    while (!input.empty()) {
        auto character = input.front();
        input.remove_prefix(1);
        if (character == '"') return {Kind::String, std::move(text)};
        if (character == '\\' && !input.empty() && (input.front() == '"' || input.front() == '\\')) {
            character = input.front();
            input.remove_prefix(1);
        }
        if (character == '\0') return {Kind::Invalid, {}};
        text += character;
    }
    return {Kind::Invalid, {}};
}

inline std::string LowerAscii(std::string value) {
    for (auto &character : value) {
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    }
    return value;
}

} // namespace manifest_detail

inline std::optional<Manifest> ParseManifest(std::string_view text, std::uint32_t expectedId) {
    using namespace manifest_detail;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    auto root = ReadToken(text);
    if (root.kind != Kind::String || LowerAscii(std::move(root.text)) != "appstate" ||
            ReadToken(text).kind != Kind::Open)
        return std::nullopt;

    Manifest manifest;
    bool foundState = false;
    for (;;) {
        auto key = ReadToken(text);
        if (key.kind == Kind::Close) break;
        if (key.kind != Kind::String) return std::nullopt;
        auto value = ReadToken(text);
        if (value.kind == Kind::Open) {
            std::size_t depth = 1;
            while (depth) {
                switch (ReadToken(text).kind) {
                case Kind::Open: ++depth; break;
                case Kind::Close: --depth; break;
                case Kind::String: break;
                default: return std::nullopt;
                }
            }
            continue;
        }
        if (value.kind != Kind::String) return std::nullopt;
        key.text = LowerAscii(std::move(key.text));
        if (key.text == "appid" && !ParseNumber(value.text, manifest.id)) return std::nullopt;
        if (key.text == "stateflags") {
            if (!ParseNumber(value.text, manifest.state)) return std::nullopt;
            foundState = true;
        }
        if (key.text == "sizeondisk" && !ParseNumber(value.text, manifest.sizeOnDisk)) return std::nullopt;
        if (key.text == "name") manifest.name = std::move(value.text);
        if (key.text == "installdir") manifest.installDir = std::move(value.text);
        if (key.text == "buildid") manifest.buildId = std::move(value.text);
        if (key.text.starts_with("bytes")) {
            std::uint64_t count{};
            if (!ParseNumber(value.text, count)) continue;
            if (key.text == "bytesdownloaded") manifest.download.downloaded = count;
            if (key.text == "bytestodownload") manifest.download.downloadTotal = count;
            if (key.text == "bytesstaged") manifest.download.staged = count;
            if (key.text == "bytestostage") manifest.download.stagingTotal = count;
        }
    }
    if (!foundState || !manifest.id || manifest.id != expectedId || manifest.installDir.empty() ||
            ReadToken(text).kind != Kind::End)
        return std::nullopt;
    return manifest;
}

} // namespace steam_library_storage
