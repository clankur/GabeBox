#include <cassert>
#include <string>
#include "../probe/SteamLibraryManifest.h"

int main() {
    using namespace steam_library_storage;

    constexpr auto acf = R"VDF(
        "AppState" {
            "appid"       "1817070"
            "name"        "Marvel's \"Spider-Man\" Remastered"
            "StateFlags"  "4"
            "installdir"  "Marvel's Spider-Man Remastered"
            "SizeOnDisk"  "74532011400"
            "buildid"     "15232166"
            "BytesToDownload" "32000000000"
            "BytesDownloaded" "8000000000"
            "BytesToStage" "74532011400"
            "BytesStaged" "unknown"
            "InstalledDepots" {
                "1817071" { "manifest" "1821234567890" "size" "74532011400" }
                "appid" "500"
                "StateFlags" "0"
                "BytesDownloaded" "0"
            }
        }
    )VDF";
    const auto app = ParseManifest(acf, 1817070);
    assert(app && app->id == 1817070 && app->IsInstalled());
    assert(app->name == "Marvel's \"Spider-Man\" Remastered");
    assert(app->installDir == "Marvel's Spider-Man Remastered");
    assert(app->sizeOnDisk == 74532011400ULL && app->buildId == "15232166");
    assert(app->download.downloadTotal == 32000000000ULL && app->download.downloaded == 8000000000ULL);
    assert(app->download.stagingTotal == 74532011400ULL && app->download.staged == 0);
    assert(!ParseManifest(acf, 1817071));
    assert(!ParseManifest(std::string_view(acf).substr(0, 200), 1817070));

    auto downloading = std::string(acf);
    downloading.replace(downloading.find("\"4\""), 3, "\"1048578\"");
    const auto partial = ParseManifest(downloading, 1817070);
    assert(partial && !partial->IsInstalled());

    const auto minimalist = ParseManifest(
        "\xEF\xBB\xBF\"AppState\" { // metadata\n"
        "\"appid\" \"1\" \"StateFlags\" \"4\" \"installdir\" \"Game folder\" }",
        1);
    assert(minimalist && minimalist->IsInstalled());
    assert(!ParseManifest("\"AppState\" {\"appid\" \"1\" \"installdir\" \"Game\"}", 1));
    assert(!ParseManifest("\"AppState\" {\"appid\" \"1\" \"StateFlags\" \"4x\" \"installdir\" \"Game\"}", 1));
    assert(!ParseManifest("\"AppState\" {\"appid\" \"4294967296\" \"StateFlags\" \"4\" \"installdir\" \"Game\"}", 1));

    assert(IsSafeName(L"Marvel's Spider-Man Remastered"));
    assert(IsSafeName(L"Game.Folder"));
    assert(!IsSafeName(L""));
    assert(!IsSafeName(L"."));
    assert(!IsSafeName(L".."));
    assert(!IsSafeName(L"C:\\Temp"));
    assert(!IsSafeName(L"foo/bar"));
    assert(!IsSafeName(L"file:alternate_stream"));
    assert(!IsSafeName(L"bad "));
    assert(!IsSafeName(L"bad."));
    assert(!IsSafeName(L"cOn.txt"));
    assert(!IsSafeName(L"COM\u00b9.vdf"));
    assert(!IsSafeName(L"aux .acf"));
}
