#include "../runtime/steam/steam-cloud-sync-policy.h"
#include <cassert>
#include <string>

using namespace steam_cloud_sync;

static WaitResult Finished(std::int32_t result, std::int32_t files = 0) {
    WaitResult wait;
    wait.observed = true;
    wait.ran = true;
    wait.completion = {2868840, result, files};
    return wait;
}

int main() {
    constexpr auto synchronized = static_cast<std::int32_t>(State::Synchronized);
    constexpr auto local = static_cast<std::int32_t>(State::ChangesLocally);
    constexpr auto cloud = static_cast<std::int32_t>(State::ChangesInCloud);
    constexpr auto both = static_cast<std::int32_t>(State::ChangesInCloudAndLocally);
    constexpr auto conflict = static_cast<std::int32_t>(State::ConflictingChanges);

    // State names match the strings the client writes to localconfig.vdf.
    assert(std::string(StateName(local)) == "changeslocally");
    assert(std::string(StateName(conflict)) == "conflictingchanges");
    assert(std::string(StateName(0)) == "invalid");

    // Only a synchronized device can skip the pre-download backup.
    assert(!BackupRequired(synchronized));
    for (auto state : {1, 3, local, cloud, both, conflict, 8, 0}) assert(BackupRequired(state));

    // Completions are matched by direction, app and arrival after the mark.
    CompletionLog log;
    const Completion downloaded{2868840, ResultOk, 4};
    const Completion other{646570, ResultOk, 1};
    Completion seen{};
    auto mark = log.Mark();
    assert(!log.Since(Download, 2868840, mark, seen));
    assert(!log.Observe(1250003, &downloaded, sizeof(downloaded)));
    assert(!log.Observe(DownloadComplete, &downloaded, 8));
    assert(!log.Observe(DownloadComplete, nullptr, sizeof(downloaded)));
    assert(log.Observe(DownloadComplete, &downloaded, sizeof(downloaded)));
    assert(log.Since(Download, 2868840, mark, seen) && seen.files == 4 && seen.result == ResultOk);
    assert(!log.Since(Upload, 2868840, mark, seen));
    assert(!log.Since(Download, 2868840, log.Mark(), seen));
    mark = log.Mark();
    assert(log.Observe(DownloadComplete, &other, sizeof(other)));
    assert(!log.Since(Download, 2868840, mark, seen));
    assert(log.Observe(UploadComplete, &downloaded, sizeof(downloaded)));
    assert(log.Since(Upload, 2868840, mark, seen));
    // Other remote-storage callbacks are kept for diagnosis, other ranges ignored.
    mark = log.Mark();
    assert(log.OtherSince(mark) == 0);
    assert(!log.Observe(1250017, nullptr, 0));
    assert(log.OtherSince(mark) == 1250017);
    assert(!log.Observe(1270027, nullptr, 0));
    assert(log.OtherSince(mark) == 1250017);
    assert(log.OtherSince(log.Mark()) == 0);

    // A job is done on its callback, or once it has stopped running (after it
    // started or after the start grace) for the settle period. Stopping alone
    // does not end the wait: the console's conflicting download stopped within
    // 297 ms, before a completion could be seen.
    assert(WaitFinished(true, true, true, 0, 2000, 0, 1000));
    assert(!WaitFinished(false, true, true, 9000, 2000, 0, 1000));
    assert(!WaitFinished(false, false, true, 297, 2000, 0, 1000));
    assert(!WaitFinished(false, false, true, 1200, 2000, 999, 1000));
    assert(WaitFinished(false, false, true, 1300, 2000, 1000, 1000));
    assert(!WaitFinished(false, false, false, 1500, 2000, 1500, 1000));
    assert(WaitFinished(false, false, false, 2000, 2000, 2000, 1000));

    assert(ParseChoice("cloud") == Choice::Cloud);
    assert(ParseChoice(" Local\r\n") == Choice::Local);
    assert(ParseChoice("CLOUD\n") == Choice::Cloud);
    assert(ParseChoice("") == Choice::None);
    assert(ParseChoice("\r\n") == Choice::None);
    assert(ParseChoice("both") == Choice::Invalid);
    assert(ParseChoice("cloud local") == Choice::Invalid);
    assert(ParseChoice("clouds") == Choice::Invalid);
    assert(std::string(ChoiceName(Choice::None)) == "none");

    // The cloud wins a conflict unless the player chose local for this launch.
    assert(!AcceptLocal(Choice::None));
    assert(!AcceptLocal(Choice::Cloud));
    assert(!AcceptLocal(Choice::Invalid));
    assert(AcceptLocal(Choice::Local));

    // Conflicts are held whatever the download reported.
    assert(Classify(local, conflict, true, Finished(ResultOk, 3)) == Decision::HeldConflict);
    assert(Classify(synchronized, both, true, Finished(ResultOk)) == Decision::HeldConflict);
    assert(Classify(conflict, conflict, true, WaitResult{}) == Decision::HeldConflict);
    // Synchronized counts once the download job was seen to run and finished in time,
    // with or without a callback.
    WaitResult ranOnly;
    ranOnly.ran = true;
    assert(Classify(local, synchronized, true, ranOnly) == Decision::Synchronized);
    assert(Classify(cloud, synchronized, true, Finished(ResultOk, 2)) == Decision::Synchronized);
    assert(std::string(SynchronizedBy(ranOnly)) == "job");
    // A first launch whose job was never seen is not proof: the client's state is cached.
    for (auto before : {1, local, cloud, 8, 0}) assert(Classify(before, synchronized, true, WaitResult{}) == Decision::Unconfirmed);
    // Narrow exception: synchronized before and after, request accepted, no timeout.
    assert(Classify(synchronized, synchronized, true, WaitResult{}) == Decision::Synchronized);
    assert(std::string(SynchronizedBy(WaitResult{})) == "already-synchronized");
    WaitResult idleTimedOut;
    idleTimedOut.timedOut = true;
    assert(Classify(synchronized, synchronized, true, idleTimedOut) == Decision::Unconfirmed);
    assert(Classify(synchronized, synchronized, false, WaitResult{}) == Decision::Unconfirmed);
    WaitResult ranTimedOut = ranOnly;
    ranTimedOut.timedOut = true;
    assert(Classify(local, synchronized, true, ranTimedOut) == Decision::Unconfirmed);
    // A failed completion is never accepted, even with a synchronized state.
    assert(Classify(synchronized, synchronized, true, Finished(2)) == Decision::Unconfirmed);
    // Local-only changes count only with a successful completion.
    assert(Classify(local, local, true, Finished(ResultOk)) == Decision::LocalChanges);
    assert(Classify(local, local, true, Finished(2)) == Decision::Unconfirmed);
    assert(Classify(local, local, true, WaitResult{}) == Decision::Unconfirmed);
    assert(Classify(local, local, true, ranOnly) == Decision::Unconfirmed);
    WaitResult timedOut = Finished(ResultOk);
    timedOut.timedOut = true;
    assert(Classify(local, local, true, timedOut) == Decision::Unconfirmed);
    // Cloud changes still pending after the download are not a settled sync.
    assert(Classify(cloud, cloud, true, Finished(ResultOk)) == Decision::Unconfirmed);
    assert(Classify(1, 1, true, Finished(ResultOk)) == Decision::Unconfirmed);

    // The exit upload never runs on an unsettled or held launch sync.
    assert(UploadAllowed(Decision::Synchronized));
    assert(UploadAllowed(Decision::LocalChanges));
    assert(UploadAllowed(Decision::ResolvedCloudAuto));
    assert(UploadAllowed(Decision::ResolvedCloud));
    assert(UploadAllowed(Decision::ResolvedLocal));
    assert(!UploadAllowed(Decision::HeldConflict));
    assert(!UploadAllowed(Decision::Unconfirmed));
    assert(!UploadAllowed(Decision::Skipped));
    assert(std::string(UploadBlockReason(Decision::HeldConflict)) == "conflict-held");

    // A resolution counts only when its job ran, finished in time and the
    // client then reports synchronized. The decision names who chose the side.
    WaitResult ranUnobserved;
    ranUnobserved.ran = true;
    assert(std::string(DecisionName(Decision::ResolvedCloudAuto)) == "resolved-cloud-auto");
    assert(AfterResolution(Choice::None, true, synchronized, ranUnobserved) == Decision::ResolvedCloudAuto);
    assert(AfterResolution(Choice::Invalid, true, synchronized, Finished(ResultOk)) == Decision::ResolvedCloudAuto);
    assert(AfterResolution(Choice::Cloud, true, synchronized, Finished(ResultOk, 4)) == Decision::ResolvedCloud);
    assert(AfterResolution(Choice::Local, true, synchronized, Finished(ResultOk)) == Decision::ResolvedLocal);
    // Not confirmed: refused, never seen running, timed out, failed or not synchronized.
    assert(AfterResolution(Choice::None, false, synchronized, Finished(ResultOk)) == Decision::HeldConflict);
    assert(AfterResolution(Choice::None, true, synchronized, WaitResult{}) == Decision::HeldConflict);
    WaitResult resolveTimedOut = ranUnobserved;
    resolveTimedOut.timedOut = true;
    assert(AfterResolution(Choice::None, true, synchronized, resolveTimedOut) == Decision::HeldConflict);
    assert(AfterResolution(Choice::None, true, synchronized, Finished(2)) == Decision::HeldConflict);
    assert(AfterResolution(Choice::None, true, conflict, Finished(ResultOk)) == Decision::HeldConflict);
    assert(AfterResolution(Choice::None, true, both, ranUnobserved) == Decision::HeldConflict);
    assert(AfterResolution(Choice::Local, true, local, Finished(ResultOk)) == Decision::HeldConflict);
    assert(AfterResolution(Choice::Cloud, true, cloud, Finished(ResultOk)) == Decision::HeldConflict);
    // Only a resolved decision consumes the one-shot choice.
    assert(IsResolved(Decision::ResolvedCloudAuto) && IsResolved(Decision::ResolvedCloud) &&
           IsResolved(Decision::ResolvedLocal));
    assert(!IsResolved(Decision::HeldConflict) && !IsResolved(Decision::Synchronized) &&
           !IsResolved(Decision::Unconfirmed));

    // The choice file is bound to one account and an expiry.
    std::int64_t when = 0;
    assert(ParseUtc("1970-01-01T00:00:00Z", when) && when == 0);
    assert(ParseUtc("2026-11-26T21:13:45Z", when) && when == 1795727625);
    assert(ParseUtc("2028-02-29T00:00:00Z", when) && when == 1835395200);
    for (auto bad : {"2027-02-29T00:00:00Z", "2026-13-01T00:00:00Z", "2026-11-26T24:00:00Z", "2026-11-26 21:13:45Z",
                     "2026-11-26T21:13:45", "2026-11-26T21:13:45+00:00", "1969-12-31T23:59:59Z", "2026-1a-26T21:13:45Z"})
        assert(!ParseUtc(bad, when));
    assert(UnixSeconds(FileTimeUnixEpoch) == 0);
    assert(UnixSeconds(134401124177058934ull) == 1795638817);
    constexpr std::uint32_t account = 22202;
    const std::int64_t now = 1795727625 - 3600;  // an hour before the expiry below
    const auto pending = EvaluateChoice("local account=22202 expires=2026-11-26T21:13:45Z\n", account, now);
    assert(pending.status == ChoiceStatus::Pending && pending.choice == Choice::Local &&
           pending.Effective() == Choice::Local && pending.account == account && !DiscardAtLaunch(pending.status));
    assert(EvaluateChoice(" CLOUD  account=22202\texpires=2026-11-26T21:13:45Z\r\n", account, now).Effective() ==
           Choice::Cloud);
    const auto otherAccount = EvaluateChoice("local account=22203 expires=2026-11-26T21:13:45Z\n", account, now);
    assert(otherAccount.status == ChoiceStatus::OtherAccount && otherAccount.Effective() == Choice::None &&
           DiscardAtLaunch(otherAccount.status));
    const auto expired = EvaluateChoice("local account=22202 expires=2026-11-26T21:13:45Z\n", account, 1795727625);
    assert(expired.status == ChoiceStatus::Expired && expired.Effective() == Choice::None);
    const auto tooFar = EvaluateChoice("local account=22202 expires=2026-12-04T21:13:46Z\n", account, now + 3600);
    assert(tooFar.status == ChoiceStatus::TooFar && tooFar.Effective() == Choice::None);
    assert(EvaluateChoice("local account=22202 expires=2026-12-03T21:13:45Z\n", account, now + 3600).status ==
           ChoiceStatus::Pending);
    // The old bare format no longer applies anywhere.
    assert(EvaluateChoice("local\n", account, now).status == ChoiceStatus::Unbound);
    assert(EvaluateChoice("cloud", account, now).Effective() == Choice::None);
    for (auto bad : {"", "\r\n", "both account=22202 expires=2026-11-26T21:13:45Z",
                     "local expires=2026-11-26T21:13:45Z account=22202", "local account=22202",
                     "local account=022202 expires=2026-11-26T21:13:45Z", "local account=0 expires=2026-11-26T21:13:45Z",
                     "local account=4294967296 expires=2026-11-26T21:13:45Z",
                     "local account=22202 expires=2026-11-26T21:13:45Z extra",
                     "local account=22202 expires=1795727625", "local account=2x202 expires=2026-11-26T21:13:45Z"}) {
        const auto invalid = EvaluateChoice(bad, account, now);
        assert(invalid.status == ChoiceStatus::Invalid && invalid.Effective() == Choice::None);
        assert(DiscardAtLaunch(invalid.status));
    }
    assert(!DiscardAtLaunch(ChoiceStatus::Absent) && DiscardAtLaunch(ChoiceStatus::Used));
    assert(std::string(ChoiceStatusName(ChoiceStatus::OtherAccount)) == "other-account");
    assert(std::string(ChoiceStatusName(ChoiceStatus::TooFar)) == "expiry-too-far");

    // Backups: keep the newest five and anything under a week old, never the current run.
    std::uint64_t run = 0;
    assert(ParseRun(L"134401124177058934", run) && run == 134401124177058934ull);
    for (auto bad : {L"", L"0", L"0134401124177058934", L"1344011241770589x", L"12345678901234567890", L"-1"})
        assert(!ParseRun(bad, run));
    constexpr std::uint64_t day = 86400ull * FileTimeTicksPerSecond;
    const std::uint64_t today = 134401124177058934ull;
    assert(BackupsToPrune({}, today, today).empty());
    // Eight backups, all over a week old, plus today's: today's and four more stay.
    std::vector<std::uint64_t> runs{today};
    for (std::uint64_t i = 1; i <= 8; ++i) runs.push_back(today - (7 + i) * day);
    auto pruned = BackupsToPrune(runs, today, today);
    assert((pruned == std::vector<std::uint64_t>{today - 12 * day, today - 13 * day, today - 14 * day, today - 15 * day}));
    // Recent backups beyond the newest five stay.
    std::vector<std::uint64_t> recent;
    for (std::uint64_t i = 0; i < 9; ++i) recent.push_back(today - i * day / 2);
    assert(BackupsToPrune(recent, today, today).empty());
    // The current run is never pruned, even when it is old by the clock and not among the newest.
    std::vector<std::uint64_t> skewed{today - 30 * day};
    for (std::uint64_t i = 0; i < 6; ++i) skewed.push_back(today - (8 + i) * day);
    pruned = BackupsToPrune(skewed, today - 30 * day, today);
    assert((pruned == std::vector<std::uint64_t>{today - 13 * day}));
    // A clock behind the backups prunes nothing that looks newer than now.
    assert(BackupsToPrune(runs, today, today - 20 * day).empty());

    Report report;
    report.Add("app", "2868840").Flag("account_enabled", true).StateField("state_before", local)
        .Number("backup_files", 5).Add("note", "a\nb").Wait("download", Finished(ResultOk, 3), 1250);
    assert(report.Text() ==
           "steam_cloud_sync_app=2868840\r\n"
           "steam_cloud_sync_account_enabled=yes\r\n"
           "steam_cloud_sync_state_before=changeslocally (5)\r\n"
           "steam_cloud_sync_backup_files=5\r\n"
           "steam_cloud_sync_note=a?b\r\n"
           "steam_cloud_sync_download_wait=complete\r\n"
           "steam_cloud_sync_download_wait_ms=1250\r\n"
           "steam_cloud_sync_download_ran=yes\r\n"
           "steam_cloud_sync_download_result=1\r\n"
           "steam_cloud_sync_download_files=3\r\n");
    WaitResult diagnosed = ranUnobserved;
    diagnosed.other = 1250017;
    Report unobserved;
    unobserved.Wait("download", diagnosed, 297);
    assert(unobserved.Text() ==
           "steam_cloud_sync_download_wait=complete-unobserved\r\n"
           "steam_cloud_sync_download_wait_ms=297\r\n"
           "steam_cloud_sync_download_ran=yes\r\n"
           "steam_cloud_sync_download_other_callback=1250017\r\n");
}
