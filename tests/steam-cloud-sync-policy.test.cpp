#include "../runtime/steam/steam-cloud-sync-policy.h"
#include <cassert>
#include <string>

using namespace steam_cloud_sync;

static WaitResult Finished(std::int32_t result, std::int32_t files = 0) {
    WaitResult wait;
    wait.observed = true;
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

    // A job is done on its callback, or when it stops running after it started
    // or after the start grace; it is not done while still running.
    assert(WaitFinished(true, true, true, 0, 2000));
    assert(!WaitFinished(false, true, true, 9000, 2000));
    assert(WaitFinished(false, false, true, 10, 2000));
    assert(!WaitFinished(false, false, false, 10, 2000));
    assert(WaitFinished(false, false, false, 2000, 2000));

    assert(ParseChoice("cloud") == Choice::Cloud);
    assert(ParseChoice(" Local\r\n") == Choice::Local);
    assert(ParseChoice("CLOUD\n") == Choice::Cloud);
    assert(ParseChoice("") == Choice::None);
    assert(ParseChoice("\r\n") == Choice::None);
    assert(ParseChoice("both") == Choice::Invalid);
    assert(ParseChoice("cloud local") == Choice::Invalid);
    assert(ParseChoice("clouds") == Choice::Invalid);

    // Conflicts are held whatever the download reported.
    assert(Classify(conflict, Finished(ResultOk, 3)) == Decision::HeldConflict);
    assert(Classify(both, Finished(ResultOk)) == Decision::HeldConflict);
    assert(Classify(conflict, WaitResult{}) == Decision::HeldConflict);
    // A synchronized result needs no callback.
    assert(Classify(synchronized, WaitResult{}) == Decision::Synchronized);
    // Local-only changes count only with a successful completion.
    assert(Classify(local, Finished(ResultOk)) == Decision::LocalChanges);
    assert(Classify(local, Finished(2)) == Decision::Unconfirmed);
    assert(Classify(local, WaitResult{}) == Decision::Unconfirmed);
    WaitResult timedOut = Finished(ResultOk);
    timedOut.timedOut = true;
    assert(Classify(local, timedOut) == Decision::Unconfirmed);
    // Cloud changes still pending after the download are not a settled sync.
    assert(Classify(cloud, Finished(ResultOk)) == Decision::Unconfirmed);
    assert(Classify(1, Finished(ResultOk)) == Decision::Unconfirmed);

    // The exit upload never runs on an unsettled or held launch sync.
    assert(UploadAllowed(Decision::Synchronized));
    assert(UploadAllowed(Decision::LocalChanges));
    assert(UploadAllowed(Decision::ResolvedCloud));
    assert(UploadAllowed(Decision::ResolvedLocal));
    assert(!UploadAllowed(Decision::HeldConflict));
    assert(!UploadAllowed(Decision::Unconfirmed));
    assert(!UploadAllowed(Decision::Skipped));
    assert(std::string(UploadBlockReason(Decision::HeldConflict)) == "conflict-held");

    // A player's choice resolves only when the follow-up sync settles.
    assert(AfterResolution(Choice::Cloud, true, synchronized, WaitResult{}) == Decision::ResolvedCloud);
    assert(AfterResolution(Choice::Local, true, local, Finished(ResultOk)) == Decision::ResolvedLocal);
    assert(AfterResolution(Choice::Cloud, false, synchronized, Finished(ResultOk)) == Decision::HeldConflict);
    assert(AfterResolution(Choice::Cloud, true, conflict, Finished(ResultOk)) == Decision::HeldConflict);
    assert(AfterResolution(Choice::Local, true, local, WaitResult{}) == Decision::Unconfirmed);

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
           "steam_cloud_sync_download_result=1\r\n"
           "steam_cloud_sync_download_files=3\r\n");
}
