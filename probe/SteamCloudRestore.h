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

// Reject junctions and type mismatches before renaming or copying a save tree.
static bool SlayCloudDirectoryExists(std::wstring const& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &info)) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
        winrt::throw_hresult(HRESULT_FROM_WIN32(error));
    }
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw winrt::hresult_invalid_argument();
    return true;
}

static void SlayCloudMove(std::wstring const& from, std::wstring const& to) {
    if (!MoveFileFromAppW(from.c_str(), to.c_str())) winrt::throw_last_error();
}

static std::wstring SlayCloudArchiveName(std::wstring_view prefix) {
    using winrt::Windows::Security::Cryptography::CryptographicBuffer;
    return std::wstring(prefix) +
        std::wstring(CryptographicBuffer::EncodeToHexString(CryptographicBuffer::GenerateRandom(12)));
}

static void RollBackSlayCloud(std::wstring const& cloudRoot, std::wstring const& game) {
    const auto pending = SlayCloudChild(cloudRoot, slayCloudPending);
    if (!SlayCloudDirectoryExists(pending)) return;
    const auto planned = SlayCloudChild(pending, L"planned");
    const auto next = SlayCloudChild(pending, L"next");
    const auto backup = SlayCloudChild(pending, L"backup");
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

static winrt::Windows::Foundation::IAsyncOperation<std::uint32_t> RestoreSlayCloudFiles(
        winrt::Windows::Storage::StorageFolder cloudRoot,
        winrt::Windows::Storage::StorageFolder stage,
        winrt::Windows::Storage::StorageFolder game,
        winrt::Windows::Data::Json::JsonArray manifest) {
    using namespace winrt::Windows::Data::Json;
    using namespace winrt::Windows::Storage;
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
    if (!count) co_return 0;

    auto transaction = co_await cloudRoot.CreateFolderAsync(L"restore-stage", CreationCollisionOption::GenerateUniqueName);
    auto planned = co_await transaction.CreateFolderAsync(L"planned");
    auto next = co_await transaction.CreateFolderAsync(L"next");
    co_await transaction.CreateFolderAsync(L"backup");
    std::wstring gamePath(game.Path());
    std::wstring cloudPath(cloudRoot.Path());

    for (size_t i = 0; i < mapped.size(); ++i) {
        if (mapped[i].empty()) continue;
        auto root = winrt::hstring(slayCloudRoots[i]);
        co_await planned.CreateFolderAsync(root);
        auto destination = co_await next.CreateFolderAsync(root);
        if (SlayCloudDirectoryExists(SlayCloudChild(gamePath, root))) {
            auto existing = co_await game.GetFolderAsync(root);
            auto items = co_await existing.GetItemsAsync();
            for (const auto &item : items) {
                auto file = item.try_as<StorageFile>();
                WIN32_FILE_ATTRIBUTE_DATA info{};
                if (!file || !GetFileAttributesExFromAppW(file.Path().c_str(), GetFileExInfoStandard, &info) ||
                        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    throw winrt::hresult_invalid_argument();
                co_await file.CopyAsync(destination);
            }
        }
        for (const auto &file : mapped[i]) {
            auto downloaded = co_await stage.GetFileAsync(file.staged);
            co_await downloaded.MoveAsync(destination, winrt::hstring(file.name), NameCollisionOption::ReplaceExisting);
        }
    }

    // The folder rename publishes a prepared journal. Play checks for this
    // journal so it cannot start with half a restore after a crash.
    const auto pending = SlayCloudChild(cloudPath, slayCloudPending);
    SlayCloudMove(std::wstring(transaction.Path()), pending);
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
