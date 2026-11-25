#pragma once

// Steam Cloud sync around a Win32 trial title. The desktop client syncs an app
// inside IClientAppManager::LaunchApp: it downloads the app's cloud files before
// the game starts and uploads them after it exits. The host starts a trial with
// CreateProcessW from a staged copy instead, so neither step ran: STS2's
// remotecache.vdf kept "ChangeNumber" "0" and "remotetime" "0" on every file,
// and localconfig.vdf kept last_sync_state "changeslocally". This drives the same
// client job through the pinned client's IClientRemoteStorage.
//
// Included by ProbeNative.cpp after SteamSignInHandles, liveSteamSession,
// steamSessionMutex, FindPinnedSteamInterface and the userdata redirect.

#include "steam-cloud-sync-policy.h"

namespace steam_cloud_sync {

// steamclient64.dll SHA-256 5237b350..., Valve client manifest 1794536281. Sign-in
// refuses any other image (IsPinnedSteamImage), and Open() checks the engine
// getter, the interface vtable and every slot below before calling one. Slot
// names are the method names the image's own IPC proxies log.
inline constexpr unsigned EngineGetterSlot = 24;             // IClientEngine::GetIClientRemoteStorage
inline constexpr std::uintptr_t EngineGetterRva = 0x9a9970;  // returns the per-user field at +0x68
inline constexpr std::uintptr_t VtableRva = 0x13624c8;       // IClientRemoteStorage IPC proxy

struct Slot {
    unsigned index;
    std::uintptr_t rva;
};
inline constexpr Slot IsCloudEnabledForAccount{23, 0x86d5f0};  // bool (this)
inline constexpr Slot IsCloudEnabledForApp{24, 0x86d770};      // bool (this, AppId_t)
inline constexpr Slot GetLastKnownSyncState{72, 0x7a1d10};     // int32 (this, AppId_t)
inline constexpr Slot ResolveSyncConflict{77, 0x889c10};       // bool (this, AppId_t, bool acceptLocal)
inline constexpr Slot SynchronizeApp{78, 0x8b36e0};            // bool (this, AppId_t, uint32 direction, uint64 flags)
inline constexpr Slot IsAppSyncInProgress{79, 0x86cc60};       // bool (this, AppId_t)

inline constexpr std::uint64_t DownloadTimeoutMs = 20000;
inline constexpr std::uint64_t UploadTimeoutMs = 30000;
inline constexpr std::uint64_t PreviousUploadWaitMs = 30000;
inline constexpr std::uint64_t StartGraceMs = 2000;
inline constexpr std::uint64_t SettleMs = 1000;
inline constexpr std::uint32_t MaxBackupFiles = 4096;
inline constexpr std::uint64_t MaxBackupBytes = 512ull * 1024 * 1024;

struct RemoteStorage {
    void* object = nullptr;
    std::uintptr_t const* methods = nullptr;

    explicit operator bool() const { return object && methods; }

    bool AccountEnabled() const {
        return reinterpret_cast<bool(__cdecl*)(void*)>(methods[IsCloudEnabledForAccount.index])(object);
    }
    bool AppEnabled(std::uint32_t app) const {
        return reinterpret_cast<bool(__cdecl*)(void*, std::uint32_t)>(methods[IsCloudEnabledForApp.index])(object, app);
    }
    std::int32_t LastKnownState(std::uint32_t app) const {
        return reinterpret_cast<std::int32_t(__cdecl*)(void*, std::uint32_t)>(
            methods[GetLastKnownSyncState.index])(object, app);
    }
    bool Request(std::uint32_t app, std::uint32_t direction) const {
        return reinterpret_cast<bool(__cdecl*)(void*, std::uint32_t, std::uint32_t, std::uint64_t)>(
            methods[SynchronizeApp.index])(object, app, direction, NoFlags);
    }
    bool InProgress(std::uint32_t app) const {
        return reinterpret_cast<bool(__cdecl*)(void*, std::uint32_t)>(methods[IsAppSyncInProgress.index])(object, app);
    }
    bool Resolve(std::uint32_t app, bool acceptLocal) const {
        return reinterpret_cast<bool(__cdecl*)(void*, std::uint32_t, bool)>(
            methods[ResolveSyncConflict.index])(object, app, acceptLocal);
    }
};

// Returns the session's IClientRemoteStorage only when it is the pinned image's.
static RemoteStorage Open(SteamSignInHandles& session) {
    auto object = FindPinnedSteamInterface(session.module, session.factory, session.user, session.pipe,
        EngineGetterSlot, EngineGetterRva);
    auto methods = SteamMethods(object);
    const auto base = reinterpret_cast<std::uintptr_t>(session.module);
    if (!object || reinterpret_cast<std::uintptr_t>(methods) != base + VtableRva) return {};
    for (auto slot : {IsCloudEnabledForAccount, IsCloudEnabledForApp, GetLastKnownSyncState, ResolveSyncConflict,
                      SynchronizeApp, IsAppSyncInProgress}) {
        if (methods[slot.index] != base + slot.rva) return {};
    }
    return {object, methods};
}

struct Plan {
    std::uint32_t app = 0;
    std::uint64_t runId = 0;
    std::uint64_t account = 0;
    SteamSignInHandles* session = nullptr;
    RemoteStorage storage;
    Decision decision = Decision::Skipped;
    std::string detail = "not-started";
    std::wstring reportPath;
};

// Whether the exit upload may run, and the reason it may not.
inline std::string UploadPolicy(Plan const& plan) {
    if (UploadAllowed(plan.decision)) return "allowed";
    return std::string("blocked:") + (plan.decision == Decision::Skipped ? plan.detail : UploadBlockReason(plan.decision));
}

inline std::wstring Wide(std::string_view ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

inline std::wstring LaunchSummary(Plan const& plan) {
    return L"steam_cloud_sync_run=" + std::to_wstring(plan.runId) + L"\r\nsteam_cloud_sync=" +
        Wide(DecisionName(plan.decision)) + L"\r\nsteam_cloud_sync_upload_policy=" + Wide(UploadPolicy(plan)) + L"\r\n";
}

// One upload at a time; a launch waits for the previous title's exit upload.
inline std::atomic<bool> exitSyncActive{false};

inline std::string HexText(DWORD value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string text = "0x00000000";
    for (unsigned i = 0; i < 8; ++i, value >>= 4) text[9 - i] = digits[value & 0xF];
    return text;
}

inline bool WriteReport(std::wstring const& path, std::string const& text, bool append) noexcept {
    auto opened = CreateFile2(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ,
        append ? OPEN_ALWAYS : CREATE_ALWAYS, nullptr);
    if (opened == INVALID_HANDLE_VALUE) return false;
    winrt::handle file{opened};
    DWORD written{};
    return WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
        written == text.size();
}

// Reads a one-shot conflict choice the player left for this app.
inline Choice ReadChoice(std::wstring const& path) {
    auto opened = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (opened == INVALID_HANDLE_VALUE) return Choice::None;
    winrt::handle file{opened};
    char buffer[64];
    DWORD read{};
    if (!ReadFile(file.get(), buffer, sizeof(buffer), &read, nullptr)) return Choice::Invalid;
    return read == sizeof(buffer) ? Choice::Invalid : ParseChoice(std::string_view(buffer, read));
}

struct Backup {
    const char* status = "not-required";
    std::uint32_t files = 0;
    std::uint64_t bytes = 0;
    DWORD error = 0;
    bool ok = true;
};

inline bool Fail(Backup& backup, const char* status, DWORD error = 0) {
    backup.ok = false;
    backup.status = status;
    backup.error = error;
    return false;
}

inline bool MakeDirectory(std::wstring const& path, Backup& backup, bool mustBeNew) {
    if (CreateDirectoryFromAppW(path.c_str(), nullptr)) return true;
    const DWORD error = GetLastError();
    if (!mustBeNew && error == ERROR_ALREADY_EXISTS) return true;
    return Fail(backup, "create-directory-error", error);
}

// Copies a save tree without following reparse points. A partial copy is kept
// and reported; the caller does not sync when the copy is incomplete.
inline bool CopyTree(std::wstring const& from, std::wstring const& to, unsigned depth, Backup& backup) {
    if (depth > 16) return Fail(backup, "too-deep");
    if (!MakeDirectory(to, backup, depth == 0)) return false;
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileExFromAppW((from + L"\\*").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch,
        nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? true : Fail(backup, "enumerate-error", error);
    }
    struct Closer {
        HANDLE handle;
        ~Closer() { FindClose(handle); }
    } closer{find};
    do {
        const std::wstring_view name(entry.cFileName);
        if (name == L"." || name == L"..") continue;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return Fail(backup, "reparse-point");
        const auto source = from + L"\\" + std::wstring(name);
        const auto target = to + L"\\" + std::wstring(name);
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!CopyTree(source, target, depth + 1, backup)) return false;
            continue;
        }
        backup.bytes += (static_cast<std::uint64_t>(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow;
        if (++backup.files > MaxBackupFiles || backup.bytes > MaxBackupBytes) return Fail(backup, "too-large");
        if (!CopyFileFromAppW(source.c_str(), target.c_str(), TRUE)) return Fail(backup, "copy-error", GetLastError());
    } while (FindNextFileW(find, &entry));
    const DWORD error = GetLastError();
    return error == ERROR_NO_MORE_FILES ? true : Fail(backup, "enumerate-error", error);
}

// Copies <userdata>\<account>\<app> (the app's remote folder and
// remotecache.vdf) to LocalState\SteamCloudSync\<account>\<app>\<run>.
inline Backup BackupApp(std::wstring const& localState, std::uint64_t account, std::uint32_t app, std::uint64_t run,
                        std::wstring& destination) {
    Backup backup;
    std::wstring userdata;
    {
        auto& redirect = steam_userdata::State();
        std::lock_guard guard(redirect.lock);
        userdata = redirect.target;
    }
    if (userdata.empty()) {
        Fail(backup, "userdata-redirect-missing");
        return backup;
    }
    const auto accountFolder = std::to_wstring(static_cast<std::uint32_t>(account));
    const auto source = userdata + L"\\" + accountFolder + L"\\" + std::to_wstring(app);
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExFromAppW(source.c_str(), GetFileExInfoStandard, &attributes)) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) backup.status = "no-local-files";
        else Fail(backup, "source-error", error);
        return backup;
    }
    if (!(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        Fail(backup, "source-type");
        return backup;
    }
    auto parent = localState + L"\\SteamCloudSync";
    for (auto const& part : {accountFolder, std::to_wstring(app)}) {
        if (!MakeDirectory(parent, backup, false)) return backup;
        parent += L"\\" + part;
    }
    if (!MakeDirectory(parent, backup, false)) return backup;
    destination = parent + L"\\" + std::to_wstring(run);
    if (CopyTree(source, destination, 0, backup)) backup.status = "copied";
    return backup;
}

// Under steamSessionMutex: the session this plan started with, if it is still live.
inline SteamSignInHandles* Current(Plan const& plan) {
    auto session = liveSteamSession;
    if (!session || session != plan.session || session->accountId != plan.account || !session->OwnsSession())
        return nullptr;
    return session;
}

struct JobWait {
    WaitResult result;
    std::uint64_t elapsedMs = 0;
    bool sessionLost = false;
};

// Polls the client's job for app, releasing the session lock between polls.
static winrt::Windows::Foundation::IAsyncAction WaitForJob(std::shared_ptr<Plan> plan, std::uint32_t direction,
        std::uint64_t mark, std::uint64_t timeoutMs, bool alreadyRunning, std::shared_ptr<JobWait> out) {
    const auto started = GetTickCount64();
    bool seenRunning = alreadyRunning;
    std::uint64_t idleSince = 0;
    for (;;) {
        const auto now = GetTickCount64();
        const auto elapsed = now - started;
        out->elapsedMs = elapsed;
        bool done = false;
        {
            std::lock_guard lock(steamSessionMutex);
            auto session = Current(*plan);
            if (!session) {
                out->sessionLost = true;
                co_return;
            }
            session->PumpCallbacks();
            Completion event{};
            const bool seen = session->cloudCompletions.Since(direction, plan->app, mark, event);
            if (seen) {
                out->result.observed = true;
                out->result.completion = event;
            }
            if (const auto other = session->cloudCompletions.OtherSince(mark)) out->result.other = other;
            const bool running = !seen && plan->storage.InProgress(plan->app);
            seenRunning |= running;
            out->result.ran = seen || seenRunning;
            if (running || !idleSince) idleSince = running ? 0 : now;
            done = WaitFinished(seen, running, seenRunning, elapsed, StartGraceMs, running ? 0 : now - idleSince,
                SettleMs);
        }
        if (done) co_return;
        if (elapsed >= timeoutMs) {
            out->result.timedOut = true;
            co_return;
        }
        co_await winrt::resume_after(std::chrono::milliseconds(100));
    }
}

// Runs before CreateProcessW for every Win32 trial title. Fills plan and writes
// LocalState\steam-cloud-sync-report.txt; never blocks the launch past its bounds.
static winrt::Windows::Foundation::IAsyncAction PrepareLaunch(std::shared_ptr<Plan> plan, std::wstring localState) {
    co_await winrt::resume_background();
    Report report;
    plan->reportPath = localState + L"\\steam-cloud-sync-report.txt";
    report.Number("run_id", static_cast<std::int64_t>(plan->runId)).Number("app", plan->app)
        .Add("client", "steamclient64-5237b350").Add("flags", "none");
    auto skip = [&](const char* reason) {
        plan->decision = Decision::Skipped;
        plan->detail = reason;
        report.Add("decision", DecisionName(plan->decision)).Add("skip_reason", reason)
            .Add("upload_policy", UploadPolicy(*plan)).Add("launch", "complete");
        WriteReport(plan->reportPath, report.Text(), false);
    };

    const auto waitStarted = GetTickCount64();
    while (exitSyncActive.load() && GetTickCount64() - waitStarted < PreviousUploadWaitMs)
        co_await winrt::resume_after(std::chrono::milliseconds(100));
    if (exitSyncActive.load()) co_return skip("previous-upload-pending");

    std::int32_t stateBefore = 0;
    bool running = false;
    {
        std::lock_guard lock(steamSessionMutex);
        auto session = liveSteamSession;
        if (!session || !session->OwnsSession()) co_return skip("session-lost");
        plan->session = session;
        plan->account = session->accountId;
        plan->storage = Open(*session);
        if (!plan->storage) co_return skip("interface-mismatch");
        const bool accountEnabled = plan->storage.AccountEnabled();
        const bool appEnabled = plan->storage.AppEnabled(plan->app);
        stateBefore = plan->storage.LastKnownState(plan->app);
        running = plan->storage.InProgress(plan->app);
        report.Flag("account_enabled", accountEnabled).Flag("app_enabled", appEnabled)
            .StateField("state_before", stateBefore).Flag("in_progress_before", running);
        if (!accountEnabled) co_return skip("account-cloud-disabled");
        if (!appEnabled) co_return skip("app-cloud-disabled");
    }
    if (running) {
        auto wait = std::make_shared<JobWait>();
        co_await WaitForJob(plan, Download, ~0ull, DownloadTimeoutMs, true, wait);
        if (wait->sessionLost) co_return skip("session-lost");
        if (wait->result.timedOut) co_return skip("sync-already-running");
    }

    // Never let the client replace files that may exist nowhere else.
    bool backedUp = false;
    auto backupLocal = [&]() {
        std::wstring destination;
        const auto backup = BackupApp(localState, plan->account, plan->app, plan->runId, destination);
        report.Add("backup", backup.status).Number("backup_files", backup.files)
            .Number("backup_bytes", static_cast<std::int64_t>(backup.bytes));
        if (backup.error) report.Add("backup_error", HexText(backup.error));
        if (!destination.empty() && backup.ok)
            report.Add("backup_path", winrt::to_string(std::wstring_view(destination).substr(localState.size() + 1)));
        backedUp = backup.ok;
        return backup.ok;
    };
    if (BackupRequired(stateBefore) && !backupLocal()) co_return skip("backup-failed");

    std::uint64_t mark = 0;
    {
        std::lock_guard lock(steamSessionMutex);
        auto session = Current(*plan);
        if (!session) co_return skip("session-lost");
        session->PumpCallbacks();
        mark = session->cloudCompletions.Mark();
        const bool accepted = plan->storage.Request(plan->app, Download);
        report.Add("download", accepted ? "requested" : "refused");
        if (!accepted) {
            plan->decision = Decision::Unconfirmed;
            plan->detail = "download-refused";
        }
    }
    std::int32_t stateAfter = 0;
    if (plan->detail != "download-refused") {
        auto wait = std::make_shared<JobWait>();
        co_await WaitForJob(plan, Download, mark, DownloadTimeoutMs, false, wait);
        if (wait->sessionLost) co_return skip("session-lost");
        report.Wait("download", wait->result, wait->elapsedMs);
        {
            std::lock_guard lock(steamSessionMutex);
            if (!Current(*plan)) co_return skip("session-lost");
            stateAfter = plan->storage.LastKnownState(plan->app);
        }
        report.StateField("state_after_download", stateAfter);
        plan->decision = Classify(stateAfter, wait->result);
        plan->detail = DecisionName(plan->decision);
    }

    // Conflict policy: the cloud copy wins, unless the player left a one-shot
    // "local" choice for this launch. Nothing is resolved without a complete
    // backup of the local files, and only a confirmed synchronized state counts.
    if (plan->decision == Decision::HeldConflict) {
        const auto choicePath = localState + L"\\steam-cloud-resolve-" + std::to_wstring(plan->app) + L".txt";
        const auto choice = ReadChoice(choicePath);
        const bool acceptLocal = AcceptLocal(choice);
        report.Add("choice", ChoiceName(choice)).Add("resolve_side", acceptLocal ? "local" : "cloud");
        if (!(backedUp || backupLocal())) {
            report.Add("resolve", "skipped-backup-failed");
        } else {
            // One-shot: a later conflict follows the policy again.
            if (choice != Choice::None) {
                const auto applied = choicePath + L".applied-" + std::to_wstring(plan->runId);
                report.Flag("choice_consumed", MoveFileFromAppW(choicePath.c_str(), applied.c_str()) != FALSE);
            }
            const auto direction = acceptLocal ? Upload : Download;
            bool accepted = false;
            {
                std::lock_guard lock(steamSessionMutex);
                auto session = Current(*plan);
                if (!session) co_return skip("session-lost");
                session->PumpCallbacks();
                mark = session->cloudCompletions.Mark();
                accepted = plan->storage.Resolve(plan->app, acceptLocal);
                report.Flag("resolve_accepted", accepted);
                // The resolve may start the job itself; a refused duplicate request is
                // then expected, and the state after the wait decides.
                if (accepted)
                    report.Add("resolve_sync", plan->storage.Request(plan->app, direction) ? "requested" : "refused");
            }
            auto wait = std::make_shared<JobWait>();
            if (accepted) {
                co_await WaitForJob(plan, direction, mark, direction == Download ? DownloadTimeoutMs : UploadTimeoutMs,
                    false, wait);
                if (wait->sessionLost) co_return skip("session-lost");
                report.Wait("resolve", wait->result, wait->elapsedMs);
            }
            {
                std::lock_guard lock(steamSessionMutex);
                if (!Current(*plan)) co_return skip("session-lost");
                stateAfter = plan->storage.LastKnownState(plan->app);
            }
            report.StateField("state_after_resolve", stateAfter);
            plan->decision = AfterResolution(choice, accepted, stateAfter, wait->result);
            plan->detail = DecisionName(plan->decision);
            if (plan->decision == Decision::HeldConflict) report.Add("resolve", "unconfirmed");
        }
    }
    report.Add("decision", DecisionName(plan->decision)).Add("upload_policy", UploadPolicy(*plan))
        .Add("launch", "complete");
    WriteReport(plan->reportPath, report.Text(), false);
}

// Uploads after the trial's process exits, whoever reaps it.
static winrt::fire_and_forget UploadAfterExit(winrt::handle process, std::shared_ptr<Plan> plan) {
    exitSyncActive.store(true);
    struct Clear {
        ~Clear() { exitSyncActive.store(false); }
    } clear;
    Report report;
    try {
        co_await winrt::resume_background();
        report.Number("exit_run_id", static_cast<std::int64_t>(plan->runId));
        if (!co_await winrt::resume_on_signal(process.get())) {
            report.Add("upload", "skipped").Add("upload_reason", "child-wait-error");
            WriteReport(plan->reportPath, report.Text(), true);
            co_return;
        }
        DWORD code = 0;
        report.Add("child_exit", GetExitCodeProcess(process.get(), &code) ? HexText(code) : std::string("unavailable"));
        auto finish = [&](const char* reason) {
            report.Add("upload", "skipped").Add("upload_reason", reason).Add("exit", "complete");
            WriteReport(plan->reportPath, report.Text(), true);
        };
        if (!UploadAllowed(plan->decision)) co_return finish(UploadPolicy(*plan).c_str());

        std::uint64_t mark = 0;
        {
            std::lock_guard lock(steamSessionMutex);
            auto session = Current(*plan);
            if (!session) co_return finish("session-lost");
            if (!plan->storage.AccountEnabled() || !plan->storage.AppEnabled(plan->app))
                co_return finish("cloud-disabled");
            const auto state = plan->storage.LastKnownState(plan->app);
            report.StateField("state_before_upload", state);
            if (IsConflict(state)) co_return finish("conflict-at-exit");
            session->PumpCallbacks();
            mark = session->cloudCompletions.Mark();
            const bool accepted = plan->storage.Request(plan->app, Upload);
            report.Add("upload", accepted ? "requested" : "refused");
            if (!accepted) {
                report.Add("exit", "complete");
                WriteReport(plan->reportPath, report.Text(), true);
                co_return;
            }
        }
        auto wait = std::make_shared<JobWait>();
        co_await WaitForJob(plan, Upload, mark, UploadTimeoutMs, false, wait);
        if (wait->sessionLost) co_return finish("session-lost");
        report.Wait("upload", wait->result, wait->elapsedMs);
        {
            std::lock_guard lock(steamSessionMutex);
            if (Current(*plan)) report.StateField("state_after", plan->storage.LastKnownState(plan->app));
        }
        report.Add("exit", "complete");
        WriteReport(plan->reportPath, report.Text(), true);
    } catch (...) {
        report.Add("exit", "error");
        WriteReport(plan->reportPath, report.Text(), true);
    }
}

}  // namespace steam_cloud_sync
