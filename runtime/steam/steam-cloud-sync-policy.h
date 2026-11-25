#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// Portable half of the launch and exit Steam Cloud sync for Win32 trial titles:
// the client's sync-state and callback contract, the conflict policy and the
// report format. The pinned client calls live in SteamCloudSync.h.
namespace steam_cloud_sync {

// IClientRemoteStorage::SynchronizeApp direction. CUserRemoteStorage::SynchronizeApp
// queues job command 0x8 for direction 1 or 3 and 0x4 for direction 2 or 3.
inline constexpr std::uint32_t Download = 1;
inline constexpr std::uint32_t Upload = 2;
// SynchronizeApp flags. Bits 1 and 2 add the AutoCloud launch (0x20) and exit
// (0x40) commands; bit 0 (0x80) ignores pending operations from other devices.
// None is requested: AutoCloud would resolve its roots against the host
// process, not the trial's redirected profile, and the other-device guard must
// stay on. ISteamRemoteStorage files (root 0) sync through the redirected
// userdata folder without them.
inline constexpr std::uint64_t NoFlags = 0;

// Completion callbacks posted for each direction: {AppId_t, EResult, int32 files}.
inline constexpr std::int32_t DownloadComplete = 1250001;
inline constexpr std::int32_t UploadComplete = 1250002;
inline constexpr std::int32_t ResultOk = 1;

struct Completion {
    std::uint32_t app;
    std::int32_t result;
    std::int32_t files;
};
static_assert(sizeof(Completion) == 12);

// ERemoteStorageSyncState as stored in localconfig.vdf last_sync_state.
enum class State : std::int32_t {
    Unknown = 1,
    Synchronized = 2,
    InProgress = 3,
    ChangesInCloud = 4,
    ChangesLocally = 5,
    ChangesInCloudAndLocally = 6,
    ConflictingChanges = 7,
    NotInitialized = 8,
};

inline const char* StateName(std::int32_t state) {
    switch (static_cast<State>(state)) {
    case State::Unknown: return "unknown";
    case State::Synchronized: return "synchronized";
    case State::InProgress: return "inprogress";
    case State::ChangesInCloud: return "changesincloud";
    case State::ChangesLocally: return "changeslocally";
    case State::ChangesInCloudAndLocally: return "changesincloudandlocally";
    case State::ConflictingChanges: return "conflictingchanges";
    case State::NotInitialized: return "notinitialized";
    }
    return "invalid";
}

inline bool IsConflict(std::int32_t state) {
    return state == static_cast<std::int32_t>(State::ConflictingChanges) ||
        state == static_cast<std::int32_t>(State::ChangesInCloudAndLocally);
}

// Local files match what the client last synced only in this state. Any other
// state may hold progress that exists nowhere else, so it is backed up before
// the client is allowed to replace files.
inline bool BackupRequired(std::int32_t stateBefore) {
    return stateBefore != static_cast<std::int32_t>(State::Synchronized);
}

// Every host callback drain records completions here, so a completion consumed
// by another poller of the shared pipe is not lost to the waiter.
class CompletionLog {
public:
    bool Observe(std::int32_t message, const void* data, std::int32_t length) {
        Entry* entry = message == DownloadComplete ? &download_ : message == UploadComplete ? &upload_ : nullptr;
        if (!entry && message / 1000 == DownloadComplete / 1000) {
            // Other remote-storage callbacks are kept for diagnosis only.
            other_ = message;
            otherSequence_ = ++sequence_;
            return false;
        }
        if (!entry || !data || length != static_cast<std::int32_t>(sizeof(Completion))) return false;
        std::memcpy(&entry->event, data, sizeof(Completion));
        entry->sequence = ++sequence_;
        return true;
    }

    std::uint64_t Mark() const { return sequence_; }

    // The completion for app in direction posted after mark, if any.
    bool Since(std::uint32_t direction, std::uint32_t app, std::uint64_t mark, Completion& out) const {
        const Entry& entry = direction == Download ? download_ : upload_;
        if (entry.sequence <= mark || entry.event.app != app) return false;
        out = entry.event;
        return true;
    }

    // The last other remote-storage callback posted after mark, or 0.
    std::int32_t OtherSince(std::uint64_t mark) const { return otherSequence_ > mark ? other_ : 0; }

private:
    struct Entry {
        Completion event{};
        std::uint64_t sequence = 0;
    };
    Entry download_;
    Entry upload_;
    std::int32_t other_ = 0;
    std::uint64_t otherSequence_ = 0;
    std::uint64_t sequence_ = 0;
};

struct WaitResult {
    bool observed = false;   // a completion callback for this app arrived
    bool timedOut = false;   // the bound elapsed with the job still running
    bool ran = false;        // the job was seen in progress or completed
    std::int32_t other = 0;  // another remote-storage callback seen meanwhile
    Completion completion{};
};

// The job is finished once its completion arrives, or once the client has
// stopped reporting it in progress (after it was seen running or after a start
// grace) for a settle period. The settle period lets a completion posted just
// after the job leaves the in-progress set reach the pipe. A job that ends
// without one, as the console's conflicting download did after 297 ms, finishes
// as complete-unobserved and is judged by the client's sync state alone.
inline bool WaitFinished(bool completionSeen, bool inProgress, bool seenInProgress, std::uint64_t elapsedMs,
                         std::uint64_t graceMs, std::uint64_t idleMs, std::uint64_t settleMs) {
    return completionSeen || (!inProgress && (seenInProgress || elapsedMs >= graceMs) && idleMs >= settleMs);
}

enum class Choice { None, Cloud, Local, Invalid };

// The side word of a one-shot conflict choice: exactly "cloud" or "local",
// case-insensitive, surrounding whitespace ignored.
inline Choice ParseChoice(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);
    if (text.empty()) return Choice::None;
    if (text.size() != 5) return Choice::Invalid;
    char lower[5];
    for (int i = 0; i < 5; ++i) lower[i] = static_cast<char>(text[i] >= 'A' && text[i] <= 'Z' ? text[i] + 32 : text[i]);
    const std::string_view word(lower, 5);
    if (word == "cloud") return Choice::Cloud;
    if (word == "local") return Choice::Local;
    return Choice::Invalid;
}

inline const char* ChoiceName(Choice choice) {
    switch (choice) {
    case Choice::None: return "none";
    case Choice::Cloud: return "cloud";
    case Choice::Local: return "local";
    case Choice::Invalid: return "invalid";
    }
    return "invalid";
}

// Conflict policy: the cloud copy wins unless the player chose local for this
// launch. An unreadable choice falls back to the policy.
inline bool AcceptLocal(Choice choice) {
    return choice == Choice::Local;
}

// Windows FILETIME ticks (100 ns since 1601) to Unix seconds.
inline constexpr std::uint64_t FileTimeUnixEpoch = 116444736000000000ull;
inline constexpr std::uint64_t FileTimeTicksPerSecond = 10000000ull;
inline std::int64_t UnixSeconds(std::uint64_t filetime) {
    return filetime < FileTimeUnixEpoch ? 0
                                        : static_cast<std::int64_t>((filetime - FileTimeUnixEpoch) / FileTimeTicksPerSecond);
}

// Days since 1970-01-01 for a proleptic Gregorian date (Hinnant's days_from_civil).
inline std::int64_t DaysFromCivil(std::int64_t year, unsigned month, unsigned day) {
    year -= month <= 2;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<std::int64_t>(dayOfEra) - 719468;
}

// Parses exactly YYYY-MM-DDTHH:MM:SSZ into Unix seconds.
inline bool ParseUtc(std::string_view text, std::int64_t& seconds) {
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
            text[16] != ':' || text[19] != 'Z')
        return false;
    auto number = [&](std::size_t at, std::size_t width, unsigned& out) {
        out = 0;
        for (std::size_t i = at; i < at + width; ++i) {
            if (text[i] < '0' || text[i] > '9') return false;
            out = out * 10 + static_cast<unsigned>(text[i] - '0');
        }
        return true;
    };
    unsigned year, month, day, hour, minute, second;
    if (!number(0, 4, year) || !number(5, 2, month) || !number(8, 2, day) || !number(11, 2, hour) ||
            !number(14, 2, minute) || !number(17, 2, second))
        return false;
    static constexpr unsigned monthDays[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > monthDays[month - 1] ||
            (month == 2 && day == 29 && !leap) || hour > 23 || minute > 59 || second > 59)
        return false;
    seconds = DaysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
    return true;
}

// LocalState\steam-cloud-resolve-<app>.txt holds one line:
//   <cloud|local> account=<id32> expires=<YYYY-MM-DDTHH:MM:SSZ>
// It applies only to that Steam account's conflict and only before the expiry,
// which may be at most MaxChoiceLifetimeSeconds away. Any other file is ignored
// and removed at the next launch, whether or not that launch has a conflict.
inline constexpr std::int64_t MaxChoiceLifetimeSeconds = 7 * 86400;

enum class ChoiceStatus {
    Absent,        // no file
    Pending,       // valid, this account, not expired: applies to this launch's conflict
    Expired,       // the expiry has passed
    OtherAccount,  // left for a different Steam account
    Unbound,       // a bare side word without account and expiry (the old format)
    TooFar,        // expiry further away than MaxChoiceLifetimeSeconds
    Invalid,       // unreadable or malformed
    Used,          // this host process already applied this exact file
};

inline const char* ChoiceStatusName(ChoiceStatus status) {
    switch (status) {
    case ChoiceStatus::Absent: return "absent";
    case ChoiceStatus::Pending: return "pending";
    case ChoiceStatus::Expired: return "expired";
    case ChoiceStatus::OtherAccount: return "other-account";
    case ChoiceStatus::Unbound: return "unbound";
    case ChoiceStatus::TooFar: return "expiry-too-far";
    case ChoiceStatus::Invalid: return "invalid";
    case ChoiceStatus::Used: return "used";
    }
    return "invalid";
}

struct ChoiceFile {
    ChoiceStatus status = ChoiceStatus::Absent;
    Choice choice = Choice::None;  // the side written in the file, when it parsed
    std::uint32_t account = 0;
    std::int64_t expires = 0;

    // The side this launch may apply to a conflict.
    Choice Effective() const { return status == ChoiceStatus::Pending ? choice : Choice::None; }
};

// Every file that is present but not pending is removed at launch, before any sync.
inline bool DiscardAtLaunch(ChoiceStatus status) {
    return status != ChoiceStatus::Absent && status != ChoiceStatus::Pending;
}

inline ChoiceFile EvaluateChoice(std::string_view text, std::uint32_t account, std::int64_t now) {
    ChoiceFile file;
    file.status = ChoiceStatus::Invalid;
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::string_view tokens[3];
    std::size_t count = 0;
    for (std::size_t at = 0; at < text.size();) {
        if (space(text[at])) {
            ++at;
            continue;
        }
        std::size_t end = at;
        while (end < text.size() && !space(text[end])) ++end;
        if (count == 3) return file;
        tokens[count++] = text.substr(at, end - at);
        at = end;
    }
    if (count == 0) return file;
    file.choice = ParseChoice(tokens[0]);
    if (file.choice != Choice::Cloud && file.choice != Choice::Local) {
        file.choice = Choice::Invalid;
        return file;
    }
    if (count == 1) {
        file.status = ChoiceStatus::Unbound;
        return file;
    }
    if (count != 3 || tokens[1].substr(0, 8) != "account=" || tokens[2].substr(0, 8) != "expires=") return file;
    const auto id = tokens[1].substr(8);
    if (id.empty() || id.size() > 10 || id[0] == '0') return file;
    std::uint64_t value = 0;
    for (char c : id) {
        if (c < '0' || c > '9') return file;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (value > 0xFFFFFFFFull || !ParseUtc(tokens[2].substr(8), file.expires)) return file;
    file.account = static_cast<std::uint32_t>(value);
    if (file.account != account) file.status = ChoiceStatus::OtherAccount;
    else if (file.expires <= now) file.status = ChoiceStatus::Expired;
    else if (file.expires - now > MaxChoiceLifetimeSeconds) file.status = ChoiceStatus::TooFar;
    else file.status = ChoiceStatus::Pending;
    return file;
}

enum class Decision {
    Synchronized,   // local and cloud agree
    LocalChanges,   // cloud unchanged since the last sync; local changes upload at exit
    HeldConflict,   // both sides changed and no resolution was confirmed; local kept active
    ResolvedCloudAuto,  // conflict policy took the cloud copy; local backed up first
    ResolvedCloud,  // the player chose the cloud copy; local backed up first
    ResolvedLocal,  // the player chose the local copy; local backed up first
    Unconfirmed,    // the download did not finish with a known result
    Skipped,        // no sync was attempted
};

inline const char* DecisionName(Decision decision) {
    switch (decision) {
    case Decision::Synchronized: return "synchronized";
    case Decision::LocalChanges: return "local-changes";
    case Decision::HeldConflict: return "held-conflict";
    case Decision::ResolvedCloudAuto: return "resolved-cloud-auto";
    case Decision::ResolvedCloud: return "resolved-cloud";
    case Decision::ResolvedLocal: return "resolved-local";
    case Decision::Unconfirmed: return "unconfirmed";
    case Decision::Skipped: return "skipped";
    }
    return "invalid";
}

// Classification after the launch-time download. The client only replaces
// local files when the local side is unchanged since the last sync
// (ChangesInCloud) and only uploads when the cloud side is unchanged
// (ChangesLocally). A conflict is resolved separately, after a backup.
//
// GetLastKnownSyncState is the client's last recorded state, so "synchronized"
// after the request proves nothing unless the download job was seen to run (in
// progress or completed) and finished within the bound. A first launch whose job
// was never seen is therefore unconfirmed, and the exit upload stays blocked.
//
// One narrow exception: the state was already synchronized before the request,
// the client accepted the request, and the wait ended without a timeout. A job
// that was never seen then counts as nothing to sync. The client can finish or
// skip a no-op job between two polls; treating that as unconfirmed would block
// the exit upload on every launch of an up-to-date title. In that state the
// pre-download backup is not needed, because nothing on this side was unsynced.
// A failed completion is never accepted.
inline Decision Classify(std::int32_t stateBefore, std::int32_t stateAfter, bool requestAccepted,
                         WaitResult const& wait) {
    if (IsConflict(stateAfter)) return Decision::HeldConflict;
    if (!requestAccepted || wait.timedOut || (wait.observed && wait.completion.result != ResultOk))
        return Decision::Unconfirmed;
    constexpr auto synchronized = static_cast<std::int32_t>(State::Synchronized);
    if (stateAfter == synchronized && (wait.ran || stateBefore == synchronized)) return Decision::Synchronized;
    if (stateAfter == static_cast<std::int32_t>(State::ChangesLocally) && wait.observed) return Decision::LocalChanges;
    return Decision::Unconfirmed;
}

// How a synchronized decision was reached, for the report.
inline const char* SynchronizedBy(WaitResult const& wait) {
    return wait.ran ? "job" : "already-synchronized";
}

// The exit upload may only run when this launch established that the cloud has
// nothing this device has not seen; otherwise it could replace another
// device's newer files.
inline bool UploadAllowed(Decision decision) {
    return decision == Decision::Synchronized || decision == Decision::LocalChanges ||
        decision == Decision::ResolvedCloudAuto || decision == Decision::ResolvedCloud ||
        decision == Decision::ResolvedLocal;
}

inline bool IsResolved(Decision decision) {
    return decision == Decision::ResolvedCloudAuto || decision == Decision::ResolvedCloud ||
        decision == Decision::ResolvedLocal;
}

inline const char* UploadBlockReason(Decision decision) {
    switch (decision) {
    case Decision::HeldConflict: return "conflict-held";
    case Decision::Unconfirmed: return "download-unconfirmed";
    case Decision::Skipped: return "no-launch-sync";
    default: return "";
    }
}

// A resolution counts only when the client accepted it, its follow-up job was
// seen to run and finished within the bound, and the client then reports the
// sides synchronized. A completion callback alone never confirms it, and a
// missing one does not either. Anything less keeps the conflict held.
inline Decision AfterResolution(Choice choice, bool resolveAccepted, std::int32_t stateAfter, WaitResult const& wait) {
    if (!resolveAccepted || wait.timedOut || !wait.ran ||
            stateAfter != static_cast<std::int32_t>(State::Synchronized) ||
            (wait.observed && wait.completion.result != ResultOk))
        return Decision::HeldConflict;
    if (choice == Choice::Local) return Decision::ResolvedLocal;
    return choice == Choice::Cloud ? Decision::ResolvedCloud : Decision::ResolvedCloudAuto;
}

// Backups live in LocalState\SteamCloudSync\<account>\<app>\<run>, where run is
// the launch's FILETIME. After a launch takes a complete backup, older ones are
// pruned: the newest KeepBackups and anything younger than KeepBackupTicks stay,
// and the backup this launch just took is never removed. Folders whose name is
// not a run number are never touched.
inline constexpr std::size_t KeepBackups = 5;
inline constexpr std::uint64_t KeepBackupTicks = 7ull * 86400 * FileTimeTicksPerSecond;

inline bool ParseRun(std::wstring_view name, std::uint64_t& run) {
    if (name.empty() || name.size() > 19 || name[0] == L'0') return false;
    run = 0;
    for (wchar_t c : name) {
        if (c < L'0' || c > L'9') return false;
        run = run * 10 + static_cast<std::uint64_t>(c - L'0');
    }
    return true;
}

inline std::vector<std::uint64_t> BackupsToPrune(std::vector<std::uint64_t> runs, std::uint64_t currentRun,
                                                 std::uint64_t now) {
    std::sort(runs.begin(), runs.end(), [](std::uint64_t a, std::uint64_t b) { return a > b; });
    runs.erase(std::unique(runs.begin(), runs.end()), runs.end());
    std::vector<std::uint64_t> prune;
    for (std::size_t i = KeepBackups; i < runs.size(); ++i) {
        const auto run = runs[i];
        if (run == currentRun || run >= now || now - run < KeepBackupTicks) continue;
        prune.push_back(run);
    }
    return prune;
}

// key=value lines in the style of the host's other LocalState reports.
class Report {
public:
    Report& Add(std::string_view key, std::string_view value) {
        text_ += "steam_cloud_sync_";
        text_ += key;
        text_ += '=';
        for (char c : value) text_ += (c >= ' ' && c <= '~') ? c : '?';
        text_ += "\r\n";
        return *this;
    }
    Report& Number(std::string_view key, std::int64_t value) { return Add(key, std::to_string(value)); }
    Report& Flag(std::string_view key, bool value) { return Add(key, value ? "yes" : "no"); }
    Report& StateField(std::string_view key, std::int32_t state) {
        return Add(key, std::string(StateName(state)) + " (" + std::to_string(state) + ")");
    }
    Report& Wait(std::string_view key, WaitResult const& wait, std::uint64_t elapsedMs) {
        const std::string prefix(key);
        Add(prefix + "_wait", wait.timedOut ? "timeout" : wait.observed ? "complete" : "complete-unobserved");
        Number(prefix + "_wait_ms", static_cast<std::int64_t>(elapsedMs));
        Flag(prefix + "_ran", wait.ran);
        if (wait.observed) {
            Number(prefix + "_result", wait.completion.result);
            Number(prefix + "_files", wait.completion.files);
        }
        if (wait.other) Number(prefix + "_other_callback", wait.other);
        return *this;
    }
    std::string const& Text() const { return text_; }

private:
    std::string text_;
};

}  // namespace steam_cloud_sync
