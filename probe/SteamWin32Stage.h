#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

static IAsyncAction ApplyRecipe(StorageFile recipe, std::wstring source, StorageFolder stage) {
    constexpr std::array<std::uint8_t, 8> magic{'X', 'W', 'P', 'E', 'V', '1', '\r', '\n'};
    constexpr std::uint32_t metadataSize = 56;
    auto length = (co_await recipe.GetBasicPropertiesAsync()).Size();
    if (length <= metadataSize || length > chunkSize) throw winrt::hresult_invalid_argument();
    const auto patch = co_await FileIO::ReadBufferAsync(recipe);
    const std::span<const std::uint8_t> bytes(patch.data(), patch.Length());
    if (bytes.size() != length || !std::equal(magic.begin(), magic.end(), bytes.begin()))
        throw winrt::hresult_invalid_argument();
    const auto sourceLength = ReadLittleEndian(bytes.subspan(8, 8));
    const auto headerLength = ReadLittleEndian(bytes.subspan(16, 4));
    const auto tailLength = ReadLittleEndian(bytes.subspan(20, 4));
    if (headerLength < 64 || headerLength > sourceLength ||
            !tailLength || sourceLength > (std::numeric_limits<std::uint64_t>::max)() - tailLength ||
            headerLength + tailLength != bytes.size() - metadataSize)
        throw winrt::hresult_invalid_argument();

    auto from = OpenNativeFile(source);
    LARGE_INTEGER actualSize{};
    if (!GetFileSizeEx(from.get(), &actualSize)) winrt::throw_last_error();
    if (static_cast<std::uint64_t>(actualSize.QuadPart) != sourceLength)
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    ULARGE_INTEGER callerAvailable{};
    if (GetDiskFreeSpaceExW(stage.Path().c_str(), &callerAvailable, nullptr, nullptr) &&
            callerAvailable.QuadPart < sourceLength + tailLength)
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
        const auto boundary = offset < headerLength ? headerLength : sourceLength;
        const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(chunkSize, boundary - offset));
        DWORD read = 0;
        if (!ReadFile(from.get(), buffer.data(), count, &read, nullptr)) winrt::throw_last_error();
        if (read != count) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_READ_FAULT));
        auto block = createBuffer(std::span(buffer.data(), static_cast<size_t>(read)));
        digest.Append(block);
        if (offset >= headerLength) co_await Write(to, block);
        offset += count;
    }
    if (!CryptographicBuffer::Compare(digest.GetValueAndReset(), createBuffer(bytes.subspan(metadataSize - 32, 32))))
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    co_await Write(to, createBuffer(bytes.last(static_cast<size_t>(tailLength))));
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

static bool IsImageName(std::wstring_view name, std::wstring_view extension) {
    return name.size() > extension.size() && name.size() <= 100 && name.ends_with(extension) &&
        !name.starts_with(L'.') && name.find_first_not_of(
            L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ._-") == name.npos;
}

static IAsyncAction CopyCompanions(std::wstring source, StorageFolder stage, StorageFile list,
        StorageFolder package, std::wstring recipePrefix) {
    if ((co_await list.GetBasicPropertiesAsync()).Size() > 64 * 1024)
        throw winrt::hresult_invalid_argument();
    std::map<std::wstring, StorageFolder> directories;
    std::wistringstream entries(std::wstring(co_await FileIO::ReadTextAsync(list)));
    unsigned count = 0;
    for (std::wstring relative; std::getline(entries, relative);) {
        if (relative.ends_with(L'\r')) relative.pop_back();
        std::wstring recipeKey;
        const auto tab = relative.find(L'\t');
        if (tab != relative.npos) {
            recipeKey = relative.substr(tab + 1);
            relative.resize(tab);
            if (recipeKey.empty() || recipeKey.size() > 24 ||
                    recipeKey.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789-") != recipeKey.npos)
                throw winrt::hresult_invalid_argument();
        }
        if (relative.empty() || ++count > 1024 || relative.size() > 260)
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
        } else {
            if (!IsImageName(parts.back(), L".exe") && !IsImageName(parts.back(), L".dll"))
                throw winrt::hresult_invalid_argument();
            auto recipe = co_await package.GetFileAsync(recipePrefix + L'-' + recipeKey + L".xwp");
            co_await ApplyRecipe(recipe, companion, to);
        }
    }
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
        if ((co_await trace.GetBasicPropertiesAsync()).Size() > 32 * 1024)
            co_return winrt::hstring(L"steam_shadow_loader_trace=too-large\r\n");
        std::wistringstream records(std::wstring(co_await FileIO::ReadTextAsync(trace)));
        std::wstring result;
        for (std::wstring line; std::getline(records, line);) {
            if (line.ends_with(L'\r')) line.pop_back();
            if (line.size() > 180 || (!line.starts_with(L"load ") && !line.starts_with(L"export ") &&
                    !line.starts_with(L"fault ") && !line.starts_with(L"dxgi ") && !line.starts_with(L"asset ") &&
                    !line.starts_with(L"input ")) ||
                    line.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ._?@$#=-") != line.npos)
                continue;
            result += L"steam_shadow_loader=" + line + L"\r\n";
        }
        co_return winrt::hstring(result);
    } catch (winrt::hresult_error const&) {
        co_return winrt::hstring(L"steam_shadow_loader_trace=read-error\r\n");
    }
}

} // namespace steam_win32_stage
