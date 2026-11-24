#include "../runtime/steam/SteamCloudManifest.h"

#include <cassert>

using namespace steam_cloud;

static void Number(std::string &message, unsigned field, std::uint64_t value) {
    AppendVarint(message, field << 3);
    AppendVarint(message, value);
}

static void Bytes(std::string &message, unsigned field, std::string_view value) {
    AppendVarint(message, (field << 3) | 2);
    AppendVarint(message, value.size());
    message.append(value);
}

static std::string Save(std::uint32_t app, std::string_view path, std::uint32_t size) {
    std::string file;
    Number(file, 1, app);
    Bytes(file, 3, path);
    Number(file, 5, size);
    Bytes(file, 6, "https://cdn.steamusercontent.com/download?temporary=TEST");
    Bytes(file, 10, "AABBCCDDEEFF00112233445566778899AABBCCDD");
    Number(file, 11, size);
    return file;
}

int main() {
    constexpr std::uint32_t app = 646570;
    const auto request = ListRequest(app);
    std::string_view data = request;
    Field field{};
    assert(ReadField(data, field) && field.number == 1 && field.numberValue == app);
    assert(ReadField(data, field) && field.number == 2 && field.numberValue == 1);
    assert(ReadField(data, field) && field.number == 3 && field.numberValue == maxFiles && data.empty());

    auto preference = Save(app, "%GameInstall%preferences/STSPlayer", 64);
    auto autosave = Save(app, "%GameInstall%saves/IRONCLAD.autosave", 4096);
    std::string reply;
    Bytes(reply, 1, preference);
    Bytes(reply, 1, autosave);
    Number(reply, 2, 2);
    std::vector<File> files;
    assert(ParseManifest(reply, app, files) == ManifestResult::ready);
    assert(files.size() == 2);
    assert(files[0].path == "%GameInstall%preferences/STSPlayer");
    assert(files[0].sha1 == "aabbccddeeff00112233445566778899aabbccdd");
    assert(files[1].bytes == 4096);

    files.clear();
    assert(ParseManifest(reply, app + 1, files) == ManifestResult::malformed && files.empty());
    assert(ParseManifest(std::string_view(reply).substr(0, reply.size() - 1), app, files) == ManifestResult::malformed);

    files.clear();
    assert(ParseManifest({}, app, files) == ManifestResult::ready && files.empty());
    Number(reply, 2, 3);
    assert(ParseManifest(reply, app, files) == ManifestResult::incomplete);

    files.clear();
    reply.clear();
    Bytes(reply, 1, preference);
    Bytes(reply, 1, preference);
    Number(reply, 2, 2);
    assert(ParseManifest(reply, app, files) == ManifestResult::malformed);

    files.clear();
    reply.clear();
    Number(reply, 2, maxFiles + 1);
    assert(ParseManifest(reply, app, files) == ManifestResult::limit);

    files.clear();
    reply.clear();
    Bytes(reply, 1, Save(app, "%GameInstall%saves/oversized", maxFileBytes + 1));
    Number(reply, 2, 1);
    assert(ParseManifest(reply, app, files) == ManifestResult::malformed);
}
