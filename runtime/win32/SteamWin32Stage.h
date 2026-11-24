#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <winrt/Windows.Data.Json.h>

namespace steam_win32_stage {

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Security::Cryptography;

static constexpr std::uint32_t chunkSize = 1024 * 1024;

static std::uint64_t ReadLittleEndian(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 0;
    for (size_t index = bytes.size(); index > 0; --index) value = (value << 8) | bytes[index - 1];
    return value;
}

static void CheckType(IStorageItem const& item, bool directory) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExFromAppW(item.Path().c_str(), GetFileExInfoStandard, &data))
        winrt::throw_last_error();
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            bool(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory)
        throw winrt::hresult_access_denied();
}

static void CheckNativeType(std::wstring const& path, bool directory) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) winrt::throw_last_error();
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            bool(attributes & FILE_ATTRIBUTE_DIRECTORY) != directory)
        throw winrt::hresult_access_denied();
}

static winrt::handle OpenNativeFile(std::wstring const& path) {
    CheckNativeType(path, false);
    CREATEFILE2_EXTENDED_PARAMETERS options{};
    options.dwSize = sizeof(options);
    options.dwFileFlags = FILE_FLAG_OPEN_REPARSE_POINT;
    const auto handle = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &options);
    if (handle == INVALID_HANDLE_VALUE) winrt::throw_last_error();
    winrt::handle file(handle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info)) winrt::throw_last_error();
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
        throw winrt::hresult_access_denied();
    return file;
}

static IAsyncAction Write(IRandomAccessStream destination, IBuffer bytes) {
    if (co_await destination.WriteAsync(bytes) != bytes.Length())
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
}

static auto ReadOverlays(std::span<const std::uint8_t> entries, std::uint64_t count,
        std::uint64_t headerLength, std::uint64_t retainedLength) {
    std::vector<std::pair<std::uint64_t, std::span<const std::uint8_t>>> overlays;
    auto end = headerLength;
    for (std::uint64_t index = 0; index < count; ++index) {
        if (entries.size() < 12) throw winrt::hresult_invalid_argument();
        const auto offset = ReadLittleEndian(entries.first(8));
        const auto length = ReadLittleEndian(entries.subspan(8, 4));
        entries = entries.subspan(12);
        if (offset < end || offset > retainedLength || !length ||
                length > retainedLength - offset || length > entries.size())
            throw winrt::hresult_invalid_argument();
        const auto size = static_cast<size_t>(length);
        overlays.emplace_back(offset, entries.first(size));
        entries = entries.subspan(size);
        end = offset + length;
    }
    if (!entries.empty()) throw winrt::hresult_invalid_argument();
    return overlays;
}

static IAsyncAction ApplyRecipe(StorageFile recipe, std::wstring source, StorageFolder stage) {
    constexpr std::array<std::uint8_t, 8> originalMagic{'X', 'W', 'P', 'E', 'V', '1', '\r', '\n'};
    constexpr std::array<std::uint8_t, 8> compactMagic{'X', 'W', 'P', 'E', 'V', '2', '\r', '\n'};
    constexpr std::array<std::uint8_t, 8> overlayMagic{'X', 'W', 'P', 'E', 'V', '3', '\r', '\n'};
    constexpr std::uint32_t originalMetadataSize = 56;
    constexpr std::uint32_t compactMetadataSize = 64;
    constexpr std::uint32_t overlayMetadataSize = 68;
    auto length = (co_await recipe.GetBasicPropertiesAsync()).Size();
    if (length <= originalMetadataSize || length > 16 * chunkSize) throw winrt::hresult_invalid_argument();
    const auto patch = co_await FileIO::ReadBufferAsync(recipe);
    const std::span<const std::uint8_t> bytes(patch.data(), patch.Length());
    if (bytes.size() != length) throw winrt::hresult_invalid_argument();
    const bool compact = std::equal(compactMagic.begin(), compactMagic.end(), bytes.begin());
    const bool overlay = std::equal(overlayMagic.begin(), overlayMagic.end(), bytes.begin());
    if (!compact && !overlay && !std::equal(originalMagic.begin(), originalMagic.end(), bytes.begin()))
        throw winrt::hresult_invalid_argument();
    const auto metadataSize = overlay ? overlayMetadataSize : compact ? compactMetadataSize : originalMetadataSize;
    if (bytes.size() <= metadataSize || (!overlay && length > chunkSize)) throw winrt::hresult_invalid_argument();
    const auto sourceLength = ReadLittleEndian(bytes.subspan(8, 8));
    const auto retainedLength = compact || overlay ? ReadLittleEndian(bytes.subspan(16, 8)) : sourceLength;
    const auto lengthsOffset = compact || overlay ? 24u : 16u;
    const auto headerLength = ReadLittleEndian(bytes.subspan(lengthsOffset, 4));
    const auto tailLength = ReadLittleEndian(bytes.subspan(lengthsOffset + 4, 4));
    const auto count = overlay ? ReadLittleEndian(bytes.subspan(32, 4)) : 0;
    if (headerLength < 64 || headerLength > retainedLength || retainedLength > sourceLength ||
            (compact && retainedLength == sourceLength) || (overlay && !count) ||
            !tailLength || retainedLength > (std::numeric_limits<std::uint64_t>::max)() - tailLength ||
            headerLength + tailLength > bytes.size() - metadataSize)
        throw winrt::hresult_invalid_argument();
    const auto tailOffset = metadataSize + static_cast<size_t>(headerLength);
    const auto tail = bytes.subspan(tailOffset, static_cast<size_t>(tailLength));
    const auto overlays = ReadOverlays(bytes.subspan(tailOffset + tail.size()), count, headerLength, retainedLength);

    auto from = OpenNativeFile(source);
    LARGE_INTEGER actualSize{};
    if (!GetFileSizeEx(from.get(), &actualSize)) winrt::throw_last_error();
    if (static_cast<std::uint64_t>(actualSize.QuadPart) != sourceLength)
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    ULARGE_INTEGER callerAvailable{};
    if (GetDiskFreeSpaceExW(stage.Path().c_str(), &callerAvailable, nullptr, nullptr) &&
            callerAvailable.QuadPart < retainedLength + tailLength)
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_DISK_FULL));
    auto name = source.substr(source.find_last_of(L'\\') + 1);
    auto target = co_await stage.CreateFileAsync(name, CreationCollisionOption::FailIfExists);
    auto to = co_await target.OpenAsync(FileAccessMode::ReadWrite);
    auto digest = Core::HashAlgorithmProvider::OpenAlgorithm(L"SHA256").CreateHash();
    std::vector<std::uint8_t> buffer(chunkSize);
    auto createBuffer = [](std::span<const std::uint8_t> data) {
        return CryptographicBuffer::CreateFromByteArray({data.data(), data.data() + data.size()});
    };
    co_await Write(to, createBuffer(bytes.subspan(metadataSize, static_cast<size_t>(headerLength))));
    std::uint64_t offset = 0;
    while (offset < sourceLength) {
        auto boundary = sourceLength;
        if (offset < headerLength) boundary = headerLength;
        else if (offset < retainedLength) boundary = retainedLength;
        const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(chunkSize, boundary - offset));
        DWORD read = 0;
        if (!ReadFile(from.get(), buffer.data(), count, &read, nullptr)) winrt::throw_last_error();
        if (read != count) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_READ_FAULT));
        auto block = createBuffer(std::span(buffer.data(), static_cast<size_t>(read)));
        // Hash the complete original, including any suffix excluded from staging.
        digest.Append(block);
        if (offset >= headerLength && offset < retainedLength) co_await Write(to, block);
        offset += count;
    }
    if (!CryptographicBuffer::Compare(digest.GetValueAndReset(), createBuffer(bytes.subspan(metadataSize - 32, 32))))
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    co_await Write(to, createBuffer(tail));
    for (const auto& [position, data] : overlays) {
        to.Seek(position);
        co_await Write(to, createBuffer(data));
    }
    if (!co_await to.FlushAsync()) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
    to.Close();
}

static std::vector<std::wstring> RelativeComponents(std::wstring_view path) {
    std::vector<std::wstring> parts;
    while (!path.empty()) {
        const auto slash = path.find(L'/');
        const auto part = path.substr(0, slash);
        if (part.empty() || part.size() > 255 || part == L"." || part == L".." ||
                part.back() == L'.' || part.back() == L' ' || part.find_first_of(L"<>:\"\\|?*") != part.npos ||
                std::any_of(part.begin(), part.end(), [](wchar_t character) { return character < L' '; }))
            throw winrt::hresult_invalid_argument();
        parts.emplace_back(part);
        if (slash == path.npos) return parts;
        path.remove_prefix(slash + 1);
    }
    throw winrt::hresult_invalid_argument();
}

static void DateRoamingDefault(StorageFile const& file) {
    // Games that reconcile settings by modification time should prefer an
    // existing cloud copy over this first-run default.
    constexpr std::uint64_t baseline = 119'600'064'000'000'000; // 1980-01-01 UTC as FILETIME ticks.
    const FILETIME modified{static_cast<DWORD>(baseline), static_cast<DWORD>(baseline >> 32)};
    CREATEFILE2_EXTENDED_PARAMETERS options{};
    options.dwSize = sizeof(options);
    options.dwFileFlags = FILE_FLAG_OPEN_REPARSE_POINT;
    const auto handle = CreateFile2(file.Path().c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, OPEN_EXISTING, &options);
    if (handle == INVALID_HANDLE_VALUE) winrt::throw_last_error();
    winrt::handle attributes(handle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(attributes.get(), &info)) winrt::throw_last_error();
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
        throw winrt::hresult_access_denied();
    if (!SetFileTime(attributes.get(), nullptr, nullptr, &modified)) winrt::throw_last_error();
}

static IAsyncAction PrepareRoamingFolders(StorageFolder root, StorageFile manifest) {
    const auto length = (co_await manifest.GetBasicPropertiesAsync()).Size();
    if (!length || length > 2048) throw winrt::hresult_invalid_argument();
    const auto bytes = co_await FileIO::ReadBufferAsync(manifest);
    if (bytes.Length() != length) throw winrt::hresult_invalid_argument();
    std::string_view text(reinterpret_cast<char const*>(bytes.data()), bytes.Length());
    std::vector<std::vector<std::wstring>> paths;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto path = text.substr(0, end);
        if (path.ends_with('\r')) path.remove_suffix(1);
        if (paths.size() == 16 || path.empty() || path.size() > 220 || path.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./ -") != path.npos)
            throw winrt::hresult_invalid_argument();
        paths.push_back(RelativeComponents(winrt::to_hstring(path)));
        text.remove_prefix(end == text.npos ? text.size() : end + 1);
    }
    CheckType(root, true);
    for (const auto& path : paths) {
        auto folder = root;
        for (const auto& part : path) {
            folder = co_await folder.CreateFolderAsync(part, CreationCollisionOption::OpenIfExists);
            CheckType(folder, true);
        }
    }
}

static IAsyncOperation<bool> SeedRoamingDefault(StorageFolder folder, StorageFile seed) {
    constexpr std::uint32_t maximumSize = 4096;
    const auto length = (co_await seed.GetBasicPropertiesAsync()).Size();
    if (!length || length > maximumSize) throw winrt::hresult_invalid_argument();
    const auto bytes = co_await FileIO::ReadBufferAsync(seed);
    if (bytes.Length() != length) throw winrt::hresult_invalid_argument();
    const std::string_view text(reinterpret_cast<char const*>(bytes.data()), bytes.Length());
    const auto line = text.find('\n');
    if (line == text.npos) throw winrt::hresult_invalid_argument();
    auto path = text.substr(0, line);
    if (path.ends_with('\r')) path.remove_suffix(1);
    if (path.size() > 220 || !path.ends_with(".ini") || path.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./ -") != path.npos)
        throw winrt::hresult_invalid_argument();
    const auto content = text.substr(line + 1);
    if (content.find_first_not_of(" \t\r\n") == content.npos ||
            std::any_of(content.begin(), content.end(), [](unsigned char ch) {
                return !(ch == '\t' || ch == '\r' || ch == '\n' || (ch >= ' ' && ch <= '~'));
            })) throw winrt::hresult_invalid_argument();
    const auto parts = RelativeComponents(winrt::to_hstring(path));
    CheckType(folder, true);
    for (size_t index = 0; index + 1 < parts.size(); ++index) {
        folder = co_await folder.CreateFolderAsync(parts[index], CreationCollisionOption::OpenIfExists);
        CheckType(folder, true);
    }
    const winrt::hstring filename(parts.back());
    if (co_await folder.TryGetItemAsync(filename)) co_return false;

    // Publish the complete file in one rename. Another launch or the game
    // may create its own configuration while this file is being written.
    auto temporary = co_await folder.CreateFileAsync(L"xbox-profile-default.tmp", CreationCollisionOption::GenerateUniqueName);
    winrt::hresult failure = S_OK;
    try {
        const auto begin = bytes.data() + line + 1;
        co_await FileIO::WriteBufferAsync(temporary, CryptographicBuffer::CreateFromByteArray({begin, begin + content.size()}));
        DateRoamingDefault(temporary);
        co_await temporary.RenameAsync(filename, NameCollisionOption::FailIfExists);
    } catch (winrt::hresult_error const& error) {
        failure = error.code();
    }
    if (failure == S_OK) co_return true;
    co_await temporary.DeleteAsync(StorageDeleteOption::PermanentDelete);
    if (failure == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) || failure == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS))
        co_return false;
    winrt::throw_hresult(failure);
}

static std::wstring OpenInstall(std::wstring root, std::wstring_view relative) {
    CheckNativeType(root, true);
    for (const auto& part : RelativeComponents(relative)) {
        root += L'\\' + part;
        CheckNativeType(root, true);
    }
    return root;
}

static std::wstring SourceFile(std::wstring const& root, std::wstring_view name) {
    if (RelativeComponents(name).size() != 1) throw winrt::hresult_invalid_argument();
    const auto path = root + L"\\" + std::wstring(name);
    CheckNativeType(path, false);
    return path;
}

static bool MatchesFingerprint(std::wstring const& path, winrt::hstring const& hash,
        std::optional<std::uint64_t> expectedSize = std::nullopt) {
    auto file = OpenNativeFile(path);
    LARGE_INTEGER actualSize{};
    if (!GetFileSizeEx(file.get(), &actualSize)) winrt::throw_last_error();
    const auto size = static_cast<std::uint64_t>(actualSize.QuadPart);
    if (expectedSize && *expectedSize != size) return false;
    auto digest = Core::HashAlgorithmProvider::OpenAlgorithm(L"SHA256").CreateHash();
    std::vector<std::uint8_t> bytes(chunkSize);
    for (std::uint64_t remaining = size; remaining;) {
        const auto length = static_cast<DWORD>(std::min<std::uint64_t>(remaining, bytes.size()));
        DWORD read{};
        if (!ReadFile(file.get(), bytes.data(), length, &read, nullptr)) winrt::throw_last_error();
        if (read != length) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_READ_FAULT));
        digest.Append(CryptographicBuffer::CreateFromByteArray({bytes.data(), bytes.data() + read}));
        remaining -= read;
    }
    return CryptographicBuffer::Compare(digest.GetValueAndReset(), CryptographicBuffer::DecodeFromHexString(hash));
}

static IAsyncOperation<winrt::hstring> CheckSourceProfile(StorageFile profile, std::wstring library,
        std::wstring relative, std::wstring build) {
    using winrt::Windows::Data::Json::JsonObject;
    co_await winrt::resume_background();
    try {
        if ((co_await profile.GetBasicPropertiesAsync()).Size() > 65536)
            co_return winrt::hstring(L"profile-error");
        const auto builds = JsonObject::Parse(co_await FileIO::ReadTextAsync(profile));
        if (!builds.HasKey(build)) co_return winrt::hstring(L"build-mismatch");
        const auto entries = builds.GetNamedArray(build);
        if (entries.Size() == 0) co_return winrt::hstring(L"profile-error");
        const auto install = OpenInstall(std::move(library), relative);
        for (auto const& value : entries) {
            const auto row = value.GetObject();
            const auto name = row.GetNamedString(L"path");
            const auto size = row.GetNamedNumber(L"size");
            const auto hash = row.GetNamedString(L"sha256");
            if (name.empty() || name.size() > 260 || std::wstring_view(name).find_first_not_of(
                    L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_. /") != std::wstring_view::npos ||
                    !(size > 0 && size < 9007199254740992.0) || size != static_cast<std::uint64_t>(size) ||
                    hash.size() != 64 || std::wstring_view(hash).find_first_not_of(L"0123456789abcdef") != std::wstring_view::npos)
                co_return winrt::hstring(L"profile-error");
            const auto parts = RelativeComponents(name);
            try {
                auto parent = install;
                for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
                    parent += L'\\' + parts[index];
                    CheckNativeType(parent, true);
                }
                if (!MatchesFingerprint(SourceFile(parent, parts.back()), hash, static_cast<std::uint64_t>(size)))
                    co_return winrt::hstring(std::wstring(L"file-mismatch\r\nsteam_source_file=") + name.c_str());
            } catch (winrt::hresult_error const&) {
                co_return winrt::hstring(std::wstring(L"file-unavailable\r\nsteam_source_file=") + name.c_str());
            }
        }
        co_return winrt::hstring(L"verified");
    } catch (winrt::hresult_error const&) {
        co_return winrt::hstring(L"profile-error");
    }
}

static bool IsImageName(std::wstring_view name, std::wstring_view extension) {
    return name.size() > extension.size() && name.size() <= 100 && name.ends_with(extension) &&
        !name.starts_with(L'.') && name.find_first_not_of(
            L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ._-") == name.npos;
}

static IAsyncOperation<bool> CopyCompanions(std::wstring source, StorageFolder stage, StorageFile list,
        StorageFolder package, std::wstring recipePrefix, std::wstring& systemCompanions) {
    if ((co_await list.GetBasicPropertiesAsync()).Size() > 64 * 1024)
        throw winrt::hresult_invalid_argument();
    std::map<std::wstring, StorageFolder> directories;
    std::wistringstream entries(std::wstring(co_await FileIO::ReadTextAsync(list)));
    co_await winrt::resume_background();
    unsigned count = 0;
    bool hasHashes = false;
    for (std::wstring relative; std::getline(entries, relative);) {
        if (relative.ends_with(L'\r')) relative.pop_back();
        std::wstring recipeKey;
        std::wstring hash;
        bool systemCompanion = false;
        const auto tab = relative.find(L'\t');
        if (tab != relative.npos) {
            recipeKey = relative.substr(tab + 1);
            relative.resize(tab);
            systemCompanion = recipeKey.starts_with(L"system-sha256:");
            if (systemCompanion || recipeKey.starts_with(L"sha256:")) {
                hash = recipeKey.substr(systemCompanion ? 14 : 7);
                recipeKey.clear();
                if (hash.size() != 64 || hash.find_first_not_of(L"0123456789abcdef") != hash.npos)
                    throw winrt::hresult_invalid_argument();
            } else if (recipeKey.empty() || recipeKey.size() > 24 ||
                    recipeKey.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789-") != recipeKey.npos)
                throw winrt::hresult_invalid_argument();
        }
        if (relative.empty() || ++count > 1024 || relative.size() > 260)
            throw winrt::hresult_invalid_argument();
        if (systemCompanion && (!IsImageName(relative, L".dll") || relative.find(L' ') != relative.npos ||
                systemCompanions.size() + relative.size() + 1 >= 1024))
            throw winrt::hresult_invalid_argument();
        const auto parts = RelativeComponents(relative);
        auto from = source;
        auto to = stage;
        std::wstring parent;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            parent += parts[i] + L'/';
            from += L'\\' + parts[i];
            CheckNativeType(from, true);
            auto found = directories.find(parent);
            if (found == directories.end()) {
                auto destination = co_await to.CreateFolderAsync(parts[i], CreationCollisionOption::OpenIfExists);
                found = directories.emplace(parent, destination).first;
            }
            to = found->second;
        }
        auto companion = SourceFile(from, parts.back());
        if (recipeKey.empty()) {
            const auto target = std::wstring(to.Path()) + L"\\" + parts.back();
            if (!CopyFileW(companion.c_str(), target.c_str(), TRUE)) winrt::throw_last_error();
            if (!hash.empty()) {
                if (!MatchesFingerprint(target, winrt::hstring(hash)))
                    winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
                hasHashes = true;
                if (systemCompanion) systemCompanions += relative + L'|';
            }
        } else {
            if (!IsImageName(parts.back(), L".exe") && !IsImageName(parts.back(), L".dll"))
                throw winrt::hresult_invalid_argument();
            auto recipe = co_await package.GetFileAsync(recipePrefix + L'-' + recipeKey + L".xwp");
            co_await ApplyRecipe(recipe, companion, to);
        }
    }
    co_return hasHashes;
}

static winrt::fire_and_forget RemoveStage(StorageFolder stage) {
    try {
        co_await stage.DeleteAsync(StorageDeleteOption::PermanentDelete);
    } catch (winrt::hresult_error const&) {}
}

static winrt::fire_and_forget RemoveStageAfterExit(StorageFolder stage, winrt::handle child) {
    if (child) co_await winrt::resume_on_signal(child.get());
    RemoveStage(stage);
}

struct StageFolder {
    StorageFolder folder{nullptr};
    ~StageFolder() {
        if (folder) RemoveStage(folder);
    }
};

static IAsyncOperation<winrt::hstring> ReadLoaderTrace(StorageFolder stage) {
    try {
        auto item = co_await stage.TryGetItemAsync(L"loader-trace.txt");
        auto trace = item.try_as<StorageFile>();
        if (!trace) co_return winrt::hstring(L"steam_shadow_loader_trace=no-output\r\n");
        CheckType(trace, false);
        auto stream = co_await trace.OpenReadAsync();
        const auto size = stream.Size();
        if (!size) co_return winrt::hstring(L"steam_shadow_loader_trace=no-output\r\n");
        constexpr std::uint32_t window = 28 * 1024;
        constexpr std::size_t resultBudget = 24 * 1024;
        constexpr std::wstring_view prefix = L"steam_shadow_loader=";
        constexpr std::wstring_view complete = L"steam_shadow_loader_trace=collected\r\n";
        const bool suffix = size > window;
        // Include the preceding byte to detect whether the suffix begins on a line boundary.
        auto input = stream.GetInputStreamAt(suffix ? size - window - 1 : 0);
        const auto length = suffix ? window + 1 : static_cast<std::uint32_t>(size);
        auto buffer = co_await input.ReadAsync(Buffer(length), length, InputStreamOptions::None);
        const std::string_view bytes(reinterpret_cast<char const*>(buffer.data()), buffer.Length());
        std::vector<std::string_view> records;
        auto budget = resultBudget - complete.size();
        for (auto end = bytes.rfind('\n'); end != bytes.npos;) {
            const auto preceding = end ? bytes.rfind('\n', end - 1) : bytes.npos;
            if (suffix && preceding == bytes.npos) break;
            const auto start = preceding == bytes.npos ? 0 : preceding + 1;
            auto line = bytes.substr(start, end - start);
            end = preceding;
            if (line.ends_with('\r')) line.remove_suffix(1);
            if (line.size() > 180 || (!line.starts_with("load ") && !line.starts_with("export ") &&
                    !line.starts_with("fault ") && !line.starts_with("dxgi ") && !line.starts_with("asset ") &&
                    !line.starts_with("input ")) ||
                    line.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ._?@$#=-") != line.npos)
                continue;
            const auto cost = prefix.size() + line.size() + 2;
            if (cost > budget) break;
            budget -= cost;
            records.push_back(line);
        }
        if (records.empty()) co_return winrt::hstring(L"steam_shadow_loader_trace=no-complete-records\r\n");
        std::wstring result;
        result.reserve(resultBudget - budget);
        for (auto record = records.rbegin(); record != records.rend(); ++record) {
            result += prefix;
            result.append(record->begin(), record->end());
            result += L"\r\n";
        }
        result += complete;
        co_return winrt::hstring(result);
    } catch (winrt::hresult_error const&) {
        co_return winrt::hstring(L"steam_shadow_loader_trace=read-error\r\n");
    }
}

} // namespace steam_win32_stage
