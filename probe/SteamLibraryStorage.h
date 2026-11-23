#pragma once

#include "SteamLibraryManifest.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace steam_library_storage {

enum class Drive { Developer, External, Local };

inline constexpr std::array<Drive, 3> FixedDrives() {
    return std::array{Drive::Developer, Drive::External, Drive::Local};
}

struct Space {
    std::uint64_t available;
    std::uint64_t capacity;
};

struct App {
    std::uint32_t id;
    std::wstring name;
    std::wstring installDir;
    std::wstring buildId;
    std::uint64_t sizeOnDisk;
    bool installed;
};

struct PendingMove {
    std::uint32_t id;
    Drive destination;
};

namespace storage_detail {

inline std::wstring Child(std::wstring_view parent, std::wstring_view name) {
    std::wstring result(parent);
    if (!result.empty() && result.back() != L'\\') result += L'\\';
    result += name;
    return result;
}

inline std::wstring Mount(Drive drive, std::wstring_view localState) {
    switch (drive) {
    case Drive::Developer: return L"D:\\DevelopmentFiles";
    case Drive::External: return L"E:\\";
    case Drive::Local: return std::wstring(localState);
    }
    throw std::invalid_argument("Unknown library drive");
}

inline wchar_t DriveToken(Drive drive) {
    switch (drive) {
    case Drive::Developer: return L'D';
    case Drive::External: return L'E';
    case Drive::Local: return L'L';
    }
    throw std::invalid_argument("Unknown library drive");
}

inline std::wstring ApiPath(std::wstring_view path) {
    // Preserve ordinary drive paths for the Xbox AppContainer. Long paths are
    // needed for nested game directories under the legacy package's LocalState.
    if (path.size() < MAX_PATH) return std::wstring(path);
    return L"\\\\?\\" + std::wstring(path);
}

[[noreturn]] inline void Fail(const char *operation, DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error ? error : ERROR_GEN_FAILURE),
        std::system_category(), operation);
}

inline bool Missing(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
           error == ERROR_INVALID_DRIVE || error == ERROR_NOT_READY ||
           error == ERROR_DEVICE_NOT_CONNECTED;
}

inline std::optional<DWORD> Attributes(std::wstring_view path) {
    const auto attributes = GetFileAttributesW(ApiPath(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (Missing(error)) return std::nullopt;
        Fail("Inspect Steam library path", error);
    }
    return attributes;
}

inline bool Exists(std::wstring_view path, bool directory) {
    const auto attributes = Attributes(path);
    if (!attributes) return false;
    if ((*attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            bool(*attributes & FILE_ATTRIBUTE_DIRECTORY) != directory)
        throw std::runtime_error("Unsafe file or directory in Steam library");
    return true;
}

inline void EnsureDirectory(std::wstring_view path) {
    if (Exists(path, true)) return;
    if (!CreateDirectoryW(ApiPath(path).c_str(), nullptr)) {
        const auto error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS || !Exists(path, true))
            Fail("Create Steam library directory", error);
    }
}

struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    File(std::wstring_view path, DWORD access, DWORD disposition) {
        CREATEFILE2_EXTENDED_PARAMETERS options{};
        options.dwSize = sizeof(options);
        options.dwFileFlags = FILE_FLAG_OPEN_REPARSE_POINT;
        handle = CreateFile2(ApiPath(path).c_str(), access, FILE_SHARE_READ, disposition, &options);
        if (handle == INVALID_HANDLE_VALUE) Fail("Open Steam library file");
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        const bool inspected = GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) != 0;
        if (!inspected || (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
            const auto error = inspected ? ERROR_ACCESS_DENIED : GetLastError();
            CloseHandle(handle);
            Fail("Verify Steam library file type", error);
        }
    }
    ~File() { CloseHandle(handle); }
    File(File const &) = delete;
    File &operator=(File const &) = delete;

    std::uint64_t Size() const {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle, &size)) Fail("Measure Steam library file");
        return static_cast<std::uint64_t>(size.QuadPart);
    }

    DWORD Read(char *buffer, DWORD length) const {
        DWORD count = 0;
        if (!ReadFile(handle, buffer, length, &count, nullptr)) Fail("Read Steam library file");
        return count;
    }

    void Write(const char *buffer, DWORD length) const {
        DWORD count = 0;
        if (!WriteFile(handle, buffer, length, &count, nullptr) || count != length)
            Fail("Write Steam library file");
    }
};

inline std::string ReadManifest(std::wstring_view path) {
    File input(path, GENERIC_READ, OPEN_EXISTING);
    const auto size = input.Size();
    if (size > 2 * 1024 * 1024) throw std::runtime_error("Steam manifest exceeds the size limit");
    std::string text(static_cast<std::size_t>(size), '\0');
    if (input.Read(text.data(), static_cast<DWORD>(text.size())) != text.size() || input.Size() != size)
        throw std::runtime_error("Steam manifest changed while it was read");
    return text;
}

inline std::wstring Decode(std::string_view utf8) {
    if (utf8.empty()) return {};
    const auto length = static_cast<int>(utf8.size());
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, nullptr, 0);
    if (!count) Fail("Decode Steam manifest text");
    std::wstring result(count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, result.data(), count))
        Fail("Decode Steam manifest text");
    return result;
}

inline std::wstring ManifestName(std::uint32_t app) {
    return L"appmanifest_" + std::to_wstring(app) + L".acf";
}

inline std::wstring RetiredName(std::uint32_t app, Drive destination) {
    return L".steam-xbox-retired-" + std::wstring(1, DriveToken(destination)) +
        L"-" + std::to_wstring(app) + L".acf";
}

inline std::optional<std::uint32_t> NameId(std::wstring_view name, std::wstring_view prefix) {
    if (!name.starts_with(prefix) || !name.ends_with(L".acf") || name.size() <= prefix.size() + 4)
        return std::nullopt;
    auto digits = name.substr(prefix.size(), name.size() - prefix.size() - 4);
    if (digits.size() > 8 || digits.front() == L'0') return std::nullopt;
    std::uint32_t id = 0;
    for (const auto digit : digits) {
        if (digit < L'0' || digit > L'9') return std::nullopt;
        id = id * 10 + static_cast<std::uint32_t>(digit - L'0');
    }
    return id && id <= 0x00ffffff ? std::optional(id) : std::nullopt;
}

template<typename Visitor>
inline void Visit(std::wstring_view folder, std::wstring_view pattern, Visitor visitor) {
    WIN32_FIND_DATAW entry{};
    const auto handle = FindFirstFileExW(ApiPath(Child(folder, pattern)).c_str(), FindExInfoBasic,
        &entry, FindExSearchNameMatch, nullptr, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) return;
        Fail("List Steam library directory", error);
    }
    struct Search {
        HANDLE handle;
        ~Search() { FindClose(handle); }
    } search{handle};

    do {
        const std::wstring_view name(entry.cFileName);
        if (name != L"." && name != L"..") visitor(name, entry);
    } while (FindNextFileW(handle, &entry));
    const auto error = GetLastError();
    if (error != ERROR_NO_MORE_FILES) Fail("List Steam library directory", error);
}

inline std::optional<App> ReadApp(std::wstring_view apps, std::wstring_view manifestPath, std::uint32_t id) {
    auto manifest = ParseManifest(ReadManifest(manifestPath), id);
    if (!manifest) return std::nullopt;
    auto installDir = Decode(manifest->installDir);
    if (!IsSafeName(installDir)) return std::nullopt;
    const auto common = Child(apps, L"common");
    const auto installed = manifest->IsInstalled() && Exists(common, true) &&
        Exists(Child(common, installDir), true);
    return App{manifest->id, Decode(manifest->name), std::move(installDir),
        Decode(manifest->buildId), manifest->sizeOnDisk, installed};
}

struct TreeEntry {
    std::wstring relative;
    std::uint64_t size;
    bool directory;
};

inline void InspectTree(std::wstring_view base, std::wstring_view relative, std::vector<TreeEntry> &entries) {
    const auto current = relative.empty() ? std::wstring(base) : Child(base, relative);
    Visit(current, L"*", [&](std::wstring_view name, WIN32_FIND_DATAW const &data) {
        if (!IsSafeName(name) || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Unsafe entry in Steam game files");
        const bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        auto entry = relative.empty() ? std::wstring(name) : Child(relative, name);
        const auto size = (std::uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        entries.push_back({entry, size, directory});
        if (directory) InspectTree(base, entry, entries);
    });
}

inline std::vector<TreeEntry> ReadTree(std::wstring_view root) {
    std::vector<TreeEntry> entries;
    if (Exists(root, true)) InspectTree(root, {}, entries);
    return entries;
}

struct Buffers {
    static constexpr DWORD BlockSize = 1024 * 1024;
    std::vector<char> source = std::vector<char>(BlockSize);
    std::vector<char> destination = std::vector<char>(BlockSize);
};

inline bool SameBytes(std::wstring_view source, std::wstring_view destination, Buffers &buffers) {
    File input(source, GENERIC_READ, OPEN_EXISTING);
    File output(destination, GENERIC_READ, OPEN_EXISTING);
    if (input.Size() != output.Size()) return false;
    for (;;) {
        const auto count = input.Read(buffers.source.data(), Buffers::BlockSize);
        if (output.Read(buffers.destination.data(), Buffers::BlockSize) != count) return false;
        if (!count) return true;
        if (std::memcmp(buffers.source.data(), buffers.destination.data(), count) != 0) return false;
    }
}

inline void RemoveFile(std::wstring_view path) {
    if (Exists(path, false) && !DeleteFileW(ApiPath(path).c_str())) Fail("Remove Steam library file");
}

inline void DeleteEmptyFolder(std::wstring_view path) {
    if (Exists(path, true) && !RemoveDirectoryW(ApiPath(path).c_str())) Fail("Remove Steam library directory");
}

inline void RemoveUnpublishedTree(std::wstring_view root) {
    auto entries = ReadTree(root);
    for (auto item = entries.rbegin(); item != entries.rend(); ++item) {
        auto path = Child(root, item->relative);
        if (item->directory) DeleteEmptyFolder(path);
        else RemoveFile(path);
    }
    DeleteEmptyFolder(root);
}

inline void Rename(std::wstring_view from, std::wstring_view to) {
    // Never replace an unrelated target or copy implicitly across volumes.
    if (!MoveFileExW(ApiPath(from).c_str(), ApiPath(to).c_str(), 0))
        Fail("Rename Steam library path");
}

inline void CopyVerified(std::wstring_view source, std::wstring_view destination,
        std::wstring_view temporary, Buffers &buffers,
        std::optional<std::uint64_t> expectedSize = std::nullopt) {
    if (Exists(destination, false)) {
        if (!SameBytes(source, destination, buffers))
            throw std::runtime_error("Existing Steam destination file differs from the source");
        return;
    }
    // Only the private transaction directory owns this disposable filename.
    RemoveFile(temporary);
    {
        File input(source, GENERIC_READ, OPEN_EXISTING);
        const auto size = input.Size();
        if (expectedSize && size != *expectedSize)
            throw std::runtime_error("Steam source file changed before copying");
        File output(temporary, GENERIC_WRITE, CREATE_NEW);
        std::uint64_t copied = 0;
        for (DWORD count = 0; (count = input.Read(buffers.source.data(), Buffers::BlockSize));) {
            if (count > size - copied)
                throw std::runtime_error("Steam source file changed during the copy");
            output.Write(buffers.source.data(), count);
            copied += count;
        }
        if (copied != size || size != input.Size())
            throw std::runtime_error("Steam source file changed during the copy");
        FILETIME lastWrite{};
        if (!GetFileTime(input.handle, nullptr, nullptr, &lastWrite) ||
                !SetFileTime(output.handle, nullptr, nullptr, &lastWrite) ||
                !FlushFileBuffers(output.handle))
            Fail("Finish Steam library copy");
    }
    if (!SameBytes(source, temporary, buffers))
        throw std::runtime_error("Steam library copy failed byte verification");
    Rename(temporary, destination);
}

inline void VerifyTree(std::vector<TreeEntry> const &sourceEntries, std::wstring_view source,
        std::wstring_view destination, Buffers &buffers) {
    const auto destinationEntries = ReadTree(destination);
    if (destinationEntries.size() != sourceEntries.size())
        throw std::runtime_error("Steam destination has a different directory structure");
    for (const auto &entry : sourceEntries) {
        const auto to = Child(destination, entry.relative);
        if (!Exists(to, entry.directory) ||
                (!entry.directory && !SameBytes(Child(source, entry.relative), to, buffers)))
            throw std::runtime_error("Steam destination game files failed byte verification");
    }
}

inline void RemoveVerifiedSource(std::wstring_view source, std::wstring_view destination, Buffers &buffers) {
    auto entries = ReadTree(source);
    // Files may already be retired after an interruption. Every remaining
    // source file must have a verified copy before it can be deleted.
    for (const auto &entry : entries) {
        auto from = Child(source, entry.relative);
        auto to = Child(destination, entry.relative);
        if (!Exists(to, entry.directory) || (!entry.directory && !SameBytes(from, to, buffers)))
            throw std::runtime_error("Preserved Steam source files because the destination differs");
    }
    for (auto item = entries.rbegin(); item != entries.rend(); ++item) {
        auto from = Child(source, item->relative);
        if (item->directory) DeleteEmptyFolder(from);
        else RemoveFile(from);
    }
    DeleteEmptyFolder(source);
}

} // namespace storage_detail

inline std::wstring LibraryPath(Drive drive, std::wstring_view localState) {
    return storage_detail::Child(storage_detail::Mount(drive, localState), L"SteamLibrary");
}

inline Space GetSpace(Drive drive, std::wstring_view localState) {
    const auto mount = storage_detail::Mount(drive, localState);
    if (!storage_detail::Exists(mount, true))
        throw std::runtime_error("Steam storage drive is unavailable");
    ULARGE_INTEGER available{}, capacity{};
    if (!GetDiskFreeSpaceExW(mount.c_str(), &available, &capacity, nullptr))
        storage_detail::Fail("Measure Steam storage drive");
    return {available.QuadPart, capacity.QuadPart};
}

inline bool IsAvailable(Drive drive, std::wstring_view localState) {
    try {
        GetSpace(drive, localState);
        return true;
    } catch (std::runtime_error const &) {
        return false;
    }
}

inline std::wstring OpenLibrary(Drive drive, std::wstring_view localState) {
    GetSpace(drive, localState);
    auto root = LibraryPath(drive, localState);
    storage_detail::EnsureDirectory(root);
    return root;
}

inline std::optional<DownloadProgress> ReadDownloadProgress(std::wstring_view library, std::uint32_t id) {
    using namespace storage_detail;
    const auto apps = Child(library, L"steamapps");
    if (!Exists(library, true) || !Exists(apps, true)) return std::nullopt;
    const auto path = Child(apps, ManifestName(id));
    if (!Exists(path, false)) return std::nullopt;
    const auto manifest = ParseManifest(ReadManifest(path), id);
    return manifest ? std::optional(manifest->download) : std::nullopt;
}

inline std::optional<App> FindApp(Drive drive, std::wstring_view localState, std::uint32_t id) {
    using namespace storage_detail;
    if (!IsAvailable(drive, localState)) return std::nullopt;
    auto root = LibraryPath(drive, localState);
    if (!Exists(root, true)) return std::nullopt;
    auto apps = Child(root, L"steamapps");
    if (!Exists(apps, true)) return std::nullopt;
    auto path = Child(apps, ManifestName(id));
    return Exists(path, false) ? ReadApp(apps, path, id) : std::nullopt;
}

inline std::vector<App> ListApps(Drive drive, std::wstring_view localState) {
    using namespace storage_detail;
    std::vector<App> result;
    if (!IsAvailable(drive, localState)) return result;
    auto root = LibraryPath(drive, localState);
    if (!Exists(root, true)) return result;
    auto apps = Child(root, L"steamapps");
    if (!Exists(apps, true)) return result;
    Visit(apps, L"appmanifest_*.acf", [&](std::wstring_view name, WIN32_FIND_DATAW const &entry) {
        const auto id = NameId(name, L"appmanifest_");
        if (!id || (entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return;
        try {
            if (auto app = ReadApp(apps, Child(apps, name), *id)) result.push_back(std::move(*app));
        } catch (std::runtime_error const &) {
            // Steam can replace a manifest while this inventory is being read.
            // An unreadable entry must not hide the other games on this drive.
        }
    });
    std::sort(result.begin(), result.end(), [](App const &a, App const &b) { return a.id < b.id; });
    return result;
}

inline std::vector<PendingMove> PendingMoves(Drive drive, std::wstring_view localState) {
    using namespace storage_detail;
    std::vector<PendingMove> result;
    if (!IsAvailable(drive, localState)) return result;
    auto root = LibraryPath(drive, localState);
    if (!Exists(root, true)) return result;
    auto apps = Child(root, L"steamapps");
    if (!Exists(apps, true)) return result;
    for (auto target : FixedDrives()) {
        if (target == drive) continue;
        const auto prefix = L".steam-xbox-retired-" + std::wstring(1, DriveToken(target)) + L"-";
        Visit(apps, prefix + L"*.acf", [&](std::wstring_view name, WIN32_FIND_DATAW const &entry) {
            const auto id = NameId(name, prefix);
            if (id && !(entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
                result.push_back({*id, target});
        });
    }
    return result;
}

inline std::wstring MoveDirectory(std::wstring_view targetApps, Drive source, std::uint32_t id) {
    using namespace storage_detail;
    return Child(targetApps, L".steam-xbox-move-" + std::wstring(1, DriveToken(source)) +
        L"-" + std::to_wstring(id));
}

inline bool HasMoveTransaction(std::wstring_view localState, Drive source, Drive destination, std::uint32_t id) {
    using namespace storage_detail;
    const auto target = LibraryPath(destination, localState);
    const auto apps = Child(target, L"steamapps");
    const auto staging = MoveDirectory(apps, source, id);
    return Exists(target, true) && Exists(apps, true) && Exists(staging, true) &&
        Exists(Child(staging, L"original.acf"), false);
}

inline void MoveApp(std::wstring_view localState, Drive source, Drive destination, std::uint32_t id) {
    using namespace storage_detail;
    if (source == destination || !id || id > 0x00ffffff)
        throw std::invalid_argument("Invalid Steam library move");
    const auto sourceRoot = LibraryPath(source, localState);
    if (!IsAvailable(source, localState) || !Exists(sourceRoot, true))
        throw std::runtime_error("Steam source library is unavailable");
    const auto sourceApps = Child(sourceRoot, L"steamapps");
    if (!Exists(sourceApps, true))
        throw std::runtime_error("Steam source library is empty");
    const auto sourceCommon = Child(sourceApps, L"common");
    if (!Exists(sourceCommon, true))
        throw std::runtime_error("Steam source game directory is unavailable");
    const auto sourceManifest = Child(sourceApps, ManifestName(id));
    const auto retired = Child(sourceApps, RetiredName(id, destination));
    const bool finishing = Exists(retired, false);
    if (finishing == Exists(sourceManifest, false))
        throw std::runtime_error("Steam source manifest is missing or ambiguous");

    const auto origin = finishing ? retired : sourceManifest;
    const auto game = ReadApp(sourceApps, origin, id);
    if (!game || (!finishing && !game->installed))
        throw std::runtime_error("Only completed Steam installations can be moved");
    const auto sourceGame = Child(sourceCommon, game->installDir);
    for (const auto &other : ListApps(source, localState)) {
        if (other.id != id && CompareStringOrdinal(other.installDir.c_str(), -1,
                game->installDir.c_str(), -1, TRUE) == CSTR_EQUAL)
            throw std::runtime_error("Another Steam app shares this source game directory");
    }
    const auto targetApps = Child(OpenLibrary(destination, localState), L"steamapps");
    EnsureDirectory(targetApps);
    const auto targetCommon = Child(targetApps, L"common");
    EnsureDirectory(targetCommon);
    const auto targetGame = Child(targetCommon, game->installDir);
    const auto targetManifest = Child(targetApps, ManifestName(id));
    const auto staging = MoveDirectory(targetApps, source, id);
    const auto original = Child(staging, L"original.acf");
    const auto payload = Child(staging, L"game");
    const auto temporary = Child(staging, L"copy.pending");
    const auto manifestForPublish = Child(staging, L"publish.acf");

    const bool hasTargetGame = Exists(targetGame, true);
    const bool hasTargetManifest = Exists(targetManifest, false);
    const bool hasTransaction = Exists(staging, true) && Exists(original, false);
    if ((hasTargetGame || hasTargetManifest || finishing) && !hasTransaction)
        throw std::runtime_error("Steam destination already contains this game");
    if (hasTargetManifest && !hasTargetGame)
        throw std::runtime_error("Steam destination manifest has no game directory");
    if (finishing && !hasTargetManifest)
        throw std::runtime_error("Steam destination is incomplete after retiring the source manifest");
    if (hasTargetGame && Exists(payload, true))
        throw std::runtime_error("Steam destination directory conflicts with a pending copy");

    Buffers buffers;
    EnsureDirectory(staging);
    CopyVerified(origin, original, temporary, buffers);
    RemoveFile(temporary);

    if (!finishing) {
        const auto sourceEntries = ReadTree(sourceGame);
        if (!hasTargetGame) {
            EnsureDirectory(payload);
            std::uint64_t remaining = 0;
            for (const auto &entry : sourceEntries) {
                if (!entry.directory && !Exists(Child(payload, entry.relative), false)) {
                    if (entry.size > (std::numeric_limits<std::uint64_t>::max)() - remaining)
                        throw std::runtime_error("Steam game files exceed the supported size");
                    remaining += entry.size;
                }
            }
            // Include one copy of the manifest and conservative directory metadata overhead.
            const auto available = GetSpace(destination, localState).available;
            constexpr std::uint64_t allowance = 4 * 1024 * 1024;
            if (remaining > available || available - remaining < allowance)
                throw std::runtime_error("Steam destination has insufficient space");
            for (const auto &entry : sourceEntries) {
                const auto to = Child(payload, entry.relative);
                if (entry.directory) EnsureDirectory(to);
                else CopyVerified(Child(sourceGame, entry.relative), to, temporary, buffers, entry.size);
            }
            VerifyTree(ReadTree(sourceGame), sourceGame, payload, buffers);
            Rename(payload, targetGame);
        } else {
            VerifyTree(ReadTree(sourceGame), sourceGame, targetGame, buffers);
        }
        if (!SameBytes(sourceManifest, original, buffers))
            throw std::runtime_error("Steam source manifest changed during the move");
        if (!hasTargetManifest) {
            CopyVerified(original, manifestForPublish, temporary, buffers);
            Rename(manifestForPublish, targetManifest);
        }
        if (!SameBytes(original, targetManifest, buffers))
            throw std::runtime_error("Steam destination manifest failed byte verification");
        // Publishing the destination first preserves at least one visible install.
        // Retiring the source to a marker makes source cleanup restartable.
        Rename(sourceManifest, retired);
    } else if (!SameBytes(original, targetManifest, buffers)) {
        throw std::runtime_error("Preserved Steam source files because the destination manifest differs");
    }

    RemoveVerifiedSource(sourceGame, targetGame, buffers);
    RemoveFile(retired);
    RemoveFile(manifestForPublish);
    RemoveFile(temporary);
    RemoveFile(original);
    DeleteEmptyFolder(staging);
}

inline std::wstring StageDosApp(Drive drive, std::wstring_view localState,
        std::wstring_view temporaryRoot, std::uint32_t appId) {
    using namespace storage_detail;
    const auto app = FindApp(drive, localState, appId);
    if (!app || !app->installed)
        throw std::runtime_error("The DOS game is not completely installed");
    const auto common = Child(Child(LibraryPath(drive, localState), L"steamapps"), L"common");
    const auto source = Child(common, app->installDir);
    const auto entries = ReadTree(source);
    constexpr std::uint64_t maximum = 256 * 1024 * 1024;
    std::uint64_t bytes = 0;
    if (entries.size() > 8192) throw std::runtime_error("DOS game has too many files to stage");
    for (const auto &entry : entries) {
        if (!entry.directory) {
            if (entry.size > maximum - bytes)
                throw std::runtime_error("The DOS game exceeds the temporary copy limit");
            bytes += entry.size;
        }
    }
    const auto temp = std::wstring(temporaryRoot);
    if (!Exists(temp, true))
        throw std::runtime_error("The temporary Steam library folder is unavailable");
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(temp.c_str(), &available, nullptr, nullptr))
        Fail("Measure DOS staging drive");
    constexpr std::uint64_t allowance = 4 * 1024 * 1024;
    if (bytes + allowance > available.QuadPart)
        throw std::runtime_error("The DOS staging drive has insufficient space");

    const auto prefix = L"SteamDos-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    for (unsigned attempt = 0; attempt != 32; ++attempt) {
        const auto name = prefix + L"-" + std::to_wstring(attempt);
        const auto destination = Child(temp, name);
        const auto staging = Child(temp, name + L".staging");
        if (!CreateDirectoryW(ApiPath(staging).c_str(), nullptr)) {
            if (GetLastError() == ERROR_ALREADY_EXISTS) continue;
            Fail("Create DOS staging folder");
        }
        try {
            const auto payload = Child(staging, L"game");
            const auto temporary = Child(staging, L"copy.pending");
            EnsureDirectory(payload);
            Buffers buffers;
            for (const auto &entry : entries) {
                const auto target = Child(payload, entry.relative);
                if (entry.directory) EnsureDirectory(target);
                else CopyVerified(Child(source, entry.relative), target, temporary, buffers, entry.size);
            }
            VerifyTree(ReadTree(source), source, payload, buffers);
            Rename(payload, destination);
            // A published and verified game remains usable if empty-folder cleanup fails.
            RemoveDirectoryW(ApiPath(staging).c_str());
            return name;
        } catch (...) {
            // Only the uniquely allocated unpublished tree belongs to this call.
            try { RemoveUnpublishedTree(staging); } catch (...) {}
            throw;
        }
    }
    throw std::runtime_error("Could not allocate a unique DOS staging folder");
}

inline void MigrateLocalLibrary(std::wstring_view localState, std::uint32_t &moved) {
    std::exception_ptr failure;
    auto migrate = [&](std::uint32_t id) {
        try {
            MoveApp(localState, Drive::Local, Drive::Developer, id);
            ++moved;
        } catch (std::runtime_error const &) {
            if (!failure) failure = std::current_exception();
        }
    };
    for (const auto &move : PendingMoves(Drive::Local, localState)) {
        if (move.destination == Drive::Developer) migrate(move.id);
    }
    for (const auto &app : ListApps(Drive::Local, localState)) {
        if (app.installed) migrate(app.id);
    }
    if (failure) std::rethrow_exception(failure);
}

} // namespace steam_library_storage
