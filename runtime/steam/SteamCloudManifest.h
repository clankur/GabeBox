#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace steam_cloud {

inline constexpr std::uint32_t maxFiles = 256;
inline constexpr std::uint32_t maxFileBytes = 32 * 1024 * 1024;
inline constexpr std::uint32_t maxTotalBytes = 256 * 1024 * 1024;
inline constexpr std::uint32_t maxListBytes = 4 * 1024 * 1024;

inline void AppendVarint(std::string &buffer, std::uint64_t value) {
    while (value > 127) {
        buffer.push_back(static_cast<char>((value & 127) | 128));
        value >>= 7;
    }
    buffer.push_back(static_cast<char>(value));
}

inline std::string ListRequest(std::uint32_t app) {
    // CCloud_EnumerateUserFiles_Request: appid, extended_details, count.
    std::string buffer;
    AppendVarint(buffer, 8);
    AppendVarint(buffer, app);
    AppendVarint(buffer, 16);
    AppendVarint(buffer, 1);
    AppendVarint(buffer, 24);
    AppendVarint(buffer, maxFiles);
    return buffer;
}

struct Field {
    unsigned number;
    unsigned wire;
    std::uint64_t numberValue = 0;
    std::string_view bytes;
};

inline bool ReadVarint(std::string_view &input, std::uint64_t &value) {
    value = 0;
    for (unsigned shift = 0; shift < 64 && !input.empty(); shift += 7) {
        auto byte = static_cast<std::uint8_t>(input.front());
        input.remove_prefix(1);
        if (shift == 63 && byte > 1) return false;
        value |= static_cast<std::uint64_t>(byte & 127) << shift;
        if (!(byte & 128)) return true;
    }
    return false;
}

inline bool ReadField(std::string_view &input, Field &field) {
    std::uint64_t key = 0;
    if (!ReadVarint(input, key) || key < 8 || key > UINT32_MAX) return false;
    field = Field{static_cast<unsigned>(key >> 3), static_cast<unsigned>(key & 7), 0, {}};
    std::uint64_t size = 0;
    switch (field.wire) {
    case 0: return ReadVarint(input, field.numberValue);
    case 1: size = 8; break;
    case 2: if (!ReadVarint(input, size)) return false; break;
    case 5: size = 4; break;
    default: return false;
    }
    if (size > input.size()) return false;
    field.bytes = input.substr(0, static_cast<std::size_t>(size));
    input.remove_prefix(static_cast<std::size_t>(size));
    return true;
}

struct File {
    std::string path;
    std::string url;
    std::string sha1;
    std::uint32_t bytes = 0;
};

inline bool IsPrintable(std::string_view text) {
    return std::none_of(text.begin(), text.end(), [](unsigned char character) {
        return character < 32 || character == 127;
    });
}

inline bool ParseFile(std::string_view input, std::uint32_t expectedApp, File &file) {
    std::uint64_t app = 0;
    std::uint64_t size = 0;
    std::string_view path, url, sha;
    while (!input.empty()) {
        Field field{};
        if (!ReadField(input, field)) return false;
        if (field.number == 1 && field.wire == 0) app = field.numberValue;
        if (field.number == 3 && field.wire == 2) path = field.bytes;
        if (field.number == 5 && field.wire == 0) size = field.numberValue;
        if (field.number == 6 && field.wire == 2) url = field.bytes;
        if (field.number == 10 && field.wire == 2) sha = field.bytes;
    }
    if (app != expectedApp || size > maxFileBytes || path.empty() || path.size() > 1024 || !IsPrintable(path) ||
            url.empty() || url.size() > 8192 || !IsPrintable(url) || sha.size() != 40) return false;
    file.path = path;
    file.url = url;
    file.bytes = static_cast<std::uint32_t>(size);
    for (unsigned char c : sha) {
        if (c >= 'A' && c <= 'F') c += 'a' - 'A';
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) return false;
        file.sha1.push_back(static_cast<char>(c));
    }
    return true;
}

enum class ManifestResult { ready, malformed, limit, incomplete };

inline ManifestResult ParseManifest(std::string_view input, std::uint32_t app, std::vector<File> &files) {
    if (input.size() > maxListBytes) return ManifestResult::limit;
    std::uint64_t totalFiles = 0;
    std::uint64_t totalBytes = 0;
    while (!input.empty()) {
        Field field{};
        if (!ReadField(input, field)) return ManifestResult::malformed;
        if (field.number == 2 && field.wire == 0) {
            totalFiles = field.numberValue;
            if (totalFiles > maxFiles) return ManifestResult::limit;
        }
        if (field.number != 1 || field.wire != 2) continue;
        if (files.size() >= maxFiles) return ManifestResult::limit;
        File file;
        if (!ParseFile(field.bytes, app, file) ||
            std::any_of(files.begin(), files.end(), [&](auto const& old) { return old.path == file.path; })) {
            return ManifestResult::malformed;
        }
        totalBytes += file.bytes;
        if (totalBytes > maxTotalBytes) return ManifestResult::limit;
        files.push_back(std::move(file));
    }
    return totalFiles == files.size() ? ManifestResult::ready : ManifestResult::incomplete;
}

} // namespace steam_cloud
