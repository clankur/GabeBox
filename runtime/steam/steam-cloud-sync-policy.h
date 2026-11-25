#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

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

private:
    struct Entry {
        Completion event{};
        std::uint64_t sequence = 0;
    };
    Entry download_;
    Entry upload_;
    std::uint64_t sequence_ = 0;
};

struct WaitResult {
    bool observed = false;   // a completion callback for this app arrived
    bool timedOut = false;   // the bound elapsed with the job still running
    Completion completion{};
};

// The job is finished once its completion arrives, or once the client stops
// reporting it in progress after it was seen running or after a start grace.
inline bool WaitFinished(bool completionSeen, bool inProgress, bool seenInProgress, std::uint64_t elapsedMs,
                         std::uint64_t graceMs) {
    return completionSeen || (!inProgress && (seenInProgress || elapsedMs >= graceMs));
}

enum class Choice { None, Cloud, Local, Invalid };

// A one-shot player choice for a held conflict: exactly "cloud" or "local",
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

enum class Decision {
    Synchronized,   // local and cloud agree
    LocalChanges,   // cloud unchanged since the last sync; local changes upload at exit
    HeldConflict,   // both sides changed; local kept active, cloud untouched
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
    case Decision::ResolvedCloud: return "resolved-cloud";
    case Decision::ResolvedLocal: return "resolved-local";
    case Decision::Unconfirmed: return "unconfirmed";
    case Decision::Skipped: return "skipped";
    }
    return "invalid";
}

// Policy after the launch-time download. The client only replaces local files
// when the local side is unchanged since the last sync (ChangesInCloud) and
// only uploads when the cloud side is unchanged (ChangesLocally). When both
// sides changed nothing is resolved automatically.
inline Decision Classify(std::int32_t stateAfter, WaitResult const& wait) {
    if (IsConflict(stateAfter)) return Decision::HeldConflict;
    if (stateAfter == static_cast<std::int32_t>(State::Synchronized)) return Decision::Synchronized;
    if (stateAfter == static_cast<std::int32_t>(State::ChangesLocally) && wait.observed && !wait.timedOut &&
            wait.completion.result == ResultOk)
        return Decision::LocalChanges;
    return Decision::Unconfirmed;
}

// The exit upload may only run when this launch established that the cloud has
// nothing this device has not seen; otherwise it could replace another
// device's newer files.
inline bool UploadAllowed(Decision decision) {
    return decision == Decision::Synchronized || decision == Decision::LocalChanges ||
        decision == Decision::ResolvedCloud || decision == Decision::ResolvedLocal;
}

inline const char* UploadBlockReason(Decision decision) {
    switch (decision) {
    case Decision::HeldConflict: return "conflict-held";
    case Decision::Unconfirmed: return "download-unconfirmed";
    case Decision::Skipped: return "no-launch-sync";
    default: return "";
    }
}

// A resolution only counts when the follow-up sync leaves the sides in agreement.
inline Decision AfterResolution(Choice choice, bool resolveAccepted, std::int32_t stateAfter, WaitResult const& wait) {
    if (!resolveAccepted || IsConflict(stateAfter)) return Decision::HeldConflict;
    const bool settled = stateAfter == static_cast<std::int32_t>(State::Synchronized) ||
        (wait.observed && !wait.timedOut && wait.completion.result == ResultOk);
    if (!settled) return Decision::Unconfirmed;
    return choice == Choice::Cloud ? Decision::ResolvedCloud : Decision::ResolvedLocal;
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
        if (wait.observed) {
            Number(prefix + "_result", wait.completion.result);
            Number(prefix + "_files", wait.completion.files);
        }
        return *this;
    }
    std::string const& Text() const { return text_; }

private:
    std::string text_;
};

}  // namespace steam_cloud_sync
