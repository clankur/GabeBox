#pragma once

// Slay the Spire's public Steam build reads these two flat directories from the
// installed game. The cloud transport's original paths never reach a file API.
static constexpr std::array<std::wstring_view, 2> slayCloudRoots{L"preferences", L"saves"};
static constexpr std::wstring_view slayCloudPending = L"restore-pending";

static std::wstring SlayCloudFileName(std::wstring_view cloudPath, std::wstring_view root) {
    constexpr std::wstring_view gameInstall = L"%GameInstall%";
    if (cloudPath.starts_with(gameInstall)) cloudPath.remove_prefix(gameInstall.size());
    if (cloudPath.starts_with(L"/")) cloudPath.remove_prefix(1);
    if (!cloudPath.starts_with(root) || cloudPath.size() <= root.size() ||
            cloudPath[root.size()] != L'/') return {};
    const auto file = cloudPath.substr(root.size() + 1);
    if (file.empty() || file.size() > 100 || file.front() == L'.' || file.back() == L'.') return {};
    std::wstring lower(file);
    for (auto &ch : lower) {
        if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
        if (!(ch >= L'a' && ch <= L'z') && !(ch >= L'0' && ch <= L'9') &&
                ch != L'.' && ch != L'-' && ch != L'_') return {};
    }
    const auto stem = std::wstring_view(lower).substr(0, lower.find(L'.'));
    if (stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul" ||
            (stem.size() == 4 && (stem.starts_with(L"com") || stem.starts_with(L"lpt")) &&
                stem[3] >= L'0' && stem[3] <= L'9') || lower == L"steam_autocloud.vdf") return {};
    return std::wstring(file);
}

static std::wstring SlayCloudChild(std::wstring_view parent, std::wstring_view name) {
    return std::wstring(parent) + L"\\" + std::wstring(name);
}

static std::wstring SlayCloudGamePath(std::wstring_view library, std::wstring_view installDir) {
    return SlayCloudChild(SlayCloudChild(library, L"steamapps\\common"), installDir);
}

static std::wstring SlayCloudJournalPath(std::wstring_view localState, std::wstring_view game,
        bool legacyVolume) {
    // A journal travels with a game moved between development and USB storage.
    if (!legacyVolume) return SlayCloudChild(game, L".steam-xbox-cloud-restore");
    return SlayCloudChild(localState, L"SteamCloud\\646570");
}

// Reject junctions and type mismatches before renaming or copying a save tree.
static bool SlayCloudDirectoryExists(std::wstring const& path) {
    using steam_library_storage::storage_detail::ApiPath;
    const auto attributes = GetFileAttributesW(ApiPath(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
        winrt::throw_hresult(HRESULT_FROM_WIN32(error));
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw winrt::hresult_invalid_argument();
    return true;
}

static void SlayCloudMove(std::wstring const& from, std::wstring const& to) {
    using steam_library_storage::storage_detail::ApiPath;
    if (!MoveFileExW(ApiPath(from).c_str(), ApiPath(to).c_str(), 0)) winrt::throw_last_error();
}

static std::wstring SlayCloudArchiveName(std::wstring_view prefix) {
    using winrt::Windows::Security::Cryptography::CryptographicBuffer;
    return std::wstring(prefix) +
        std::wstring(CryptographicBuffer::EncodeToHexString(CryptographicBuffer::GenerateRandom(12)));
}

static void RollBackSlayCloud(std::wstring const& cloudRoot, std::wstring const& game) {
    if (!SlayCloudDirectoryExists(cloudRoot)) return;
    const auto pending = SlayCloudChild(cloudRoot, slayCloudPending);
    if (!SlayCloudDirectoryExists(pending)) return;
    const auto planned = SlayCloudChild(pending, L"planned");
    const auto next = SlayCloudChild(pending, L"next");
    const auto backup = SlayCloudChild(pending, L"backup");
    if (!SlayCloudDirectoryExists(planned) || !SlayCloudDirectoryExists(next) || !SlayCloudDirectoryExists(backup))
        throw winrt::hresult_invalid_argument();
    for (auto root : slayCloudRoots) {
        if (!SlayCloudDirectoryExists(SlayCloudChild(planned, root))) continue;
        const auto installed = SlayCloudChild(game, root);
        const auto staged = SlayCloudChild(next, root);
        const auto old = SlayCloudChild(backup, root);
        if (!SlayCloudDirectoryExists(staged) && SlayCloudDirectoryExists(installed)) SlayCloudMove(installed, staged);
        if (SlayCloudDirectoryExists(old)) SlayCloudMove(old, installed);
    }
    SlayCloudMove(pending, SlayCloudChild(cloudRoot, SlayCloudArchiveName(L"interrupted-")));
}

static void RecoverLegacySlayCloudForMigration(std::wstring_view localState) {
    using namespace steam_library_storage;
    const auto entry = FindApp(Drive::Local, localState, 646570);
    if (!entry || !entry->installed) return;
    const auto game = SlayCloudGamePath(LibraryPath(Drive::Local, localState), entry->installDir);
    RollBackSlayCloud(SlayCloudJournalPath(localState, game, true), game);
}

static void CopySlayCloudFile(std::wstring const& from, std::wstring const& to, bool replace) {
    using namespace steam_library_storage::storage_detail;
    if (!Exists(from, false)) throw winrt::hresult_invalid_argument();
    const bool replaceExisting = replace && Exists(to, false);
    if (!CopyFileW(ApiPath(from).c_str(), ApiPath(to).c_str(), !replaceExisting)) winrt::throw_last_error();
}

static winrt::Windows::Foundation::IAsyncOperation<std::uint32_t> RestoreSlayCloudFiles(
        std::wstring cloudPath, std::wstring stagePath, std::wstring gamePath,
        winrt::Windows::Data::Json::JsonArray manifest) {
    using namespace steam_library_storage::storage_detail;
    struct File {
        std::wstring name;
        winrt::hstring staged;
        winrt::hstring sha1;
    };
    std::array<std::vector<File>, slayCloudRoots.size()> mapped;
    std::uint32_t count = 0;
    for (const auto &item : manifest) {
        auto entry = item.GetObject();
        auto path = entry.GetNamedString(L"path");
        for (size_t i = 0; i < slayCloudRoots.size(); ++i) {
            auto name = SlayCloudFileName(path, slayCloudRoots[i]);
            if (name.empty()) continue;
            auto staged = entry.GetNamedString(L"stagedName");
            auto sha1 = entry.GetNamedString(L"sha1");
            std::wstring_view stagedName(staged);
            if (stagedName.size() < 10 || stagedName.size() > 12 ||
                    !stagedName.starts_with(L"save-") || !stagedName.ends_with(L".bin") ||
                    stagedName.substr(5, stagedName.size() - 9).find_first_not_of(L"0123456789") != std::wstring_view::npos)
                throw winrt::hresult_invalid_argument();
            auto &files = mapped[i];
            auto duplicate = std::find_if(files.begin(), files.end(), [&](const File &file) {
                return CompareStringOrdinal(file.name.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
            if (duplicate != files.end()) {
                if (duplicate->sha1 != sha1) throw winrt::hresult_invalid_argument();
                continue;
            }
            files.push_back({std::move(name), std::move(staged), std::move(sha1)});
            ++count;
        }
    }
    co_await winrt::resume_background();
    if (!SlayCloudDirectoryExists(gamePath)) throw winrt::hresult_invalid_argument();
    RollBackSlayCloud(cloudPath, gamePath);
    if (!count) co_return 0;
    EnsureDirectory(cloudPath);
    const auto transaction = SlayCloudChild(cloudPath, SlayCloudArchiveName(L"restore-stage-"));
    if (!CreateDirectoryW(ApiPath(transaction).c_str(), nullptr)) winrt::throw_last_error();
    const auto planned = SlayCloudChild(transaction, L"planned");
    const auto next = SlayCloudChild(transaction, L"next");
    EnsureDirectory(planned);
    EnsureDirectory(next);
    EnsureDirectory(SlayCloudChild(transaction, L"backup"));

    for (size_t i = 0; i < mapped.size(); ++i) {
        if (mapped[i].empty()) continue;
        const auto root = slayCloudRoots[i];
        EnsureDirectory(SlayCloudChild(planned, root));
        const auto destination = SlayCloudChild(next, root);
        EnsureDirectory(destination);
        const auto existing = SlayCloudChild(gamePath, root);
        if (SlayCloudDirectoryExists(existing)) {
            Visit(existing, L"*", [&](std::wstring_view name, WIN32_FIND_DATAW const& entry) {
                if (entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                    throw winrt::hresult_invalid_argument();
                CopySlayCloudFile(SlayCloudChild(existing, name), SlayCloudChild(destination, name), false);
            });
        }
        for (const auto &file : mapped[i]) {
            CopySlayCloudFile(SlayCloudChild(stagePath, file.staged), SlayCloudChild(destination, file.name), true);
        }
    }

    // The folder rename publishes a prepared journal. Play checks for this
    // journal so it cannot start with half a restore after a crash.
    const auto pending = SlayCloudChild(cloudPath, slayCloudPending);
    SlayCloudMove(transaction, pending);
    try {
        for (size_t i = 0; i < mapped.size(); ++i) {
            if (mapped[i].empty()) continue;
            const auto root = slayCloudRoots[i];
            const auto installed = SlayCloudChild(gamePath, root);
            const auto old = SlayCloudChild(SlayCloudChild(pending, L"backup"), root);
            const auto staged = SlayCloudChild(SlayCloudChild(pending, L"next"), root);
            if (SlayCloudDirectoryExists(installed)) SlayCloudMove(installed, old);
            SlayCloudMove(staged, installed);
        }
        SlayCloudMove(pending, SlayCloudChild(cloudPath, SlayCloudArchiveName(L"backup-")));
    } catch (...) {
        RollBackSlayCloud(cloudPath, gamePath);
        throw;
    }
    co_return count;
}

static winrt::Windows::Foundation::IAsyncOperation<std::uint32_t> PreviewSlayCloudRestore(
        winrt::Windows::Storage::StorageFolder stage, winrt::Windows::Data::Json::JsonArray manifest) {
    using namespace winrt::Windows::Storage;
    auto sandbox = co_await stage.CreateFolderAsync(L"preview-game");
    auto journal = co_await stage.CreateFolderAsync(L"preview-journal");
    auto preferences = co_await sandbox.CreateFolderAsync(L"preferences");
    constexpr auto control = L"XboxCloudLocalControl.txt";
    auto local = co_await preferences.CreateFileAsync(control);
    co_await FileIO::WriteTextAsync(local, L"Preserve an unrelated local file.");
    auto count = co_await RestoreSlayCloudFiles(std::wstring(journal.Path()), std::wstring(stage.Path()),
        std::wstring(sandbox.Path()), manifest);
    if (!count) co_return 0;
    auto archives = co_await journal.GetFoldersAsync();
    if (archives.Size() != 1 || !std::wstring_view(archives.GetAt(0).Name()).starts_with(L"backup-"))
        throw winrt::hresult_invalid_argument();
    auto backup = co_await archives.GetAt(0).GetFolderAsync(L"backup");
    auto backedPreferences = co_await backup.GetFolderAsync(L"preferences");
    auto backedFile = co_await backedPreferences.GetFileAsync(control);
    auto activePreferences = co_await sandbox.GetFolderAsync(L"preferences");
    auto retained = co_await activePreferences.GetFileAsync(control);
    if ((co_await FileIO::ReadTextAsync(backedFile)) != (co_await FileIO::ReadTextAsync(retained)))
        throw winrt::hresult_invalid_argument();
    co_return count;
}
