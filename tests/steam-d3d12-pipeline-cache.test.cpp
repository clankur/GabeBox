// Host tests for the Direct3D 12 pipeline cache file format, keys and policy.
#include "../runtime/win32/pipeline-cache-format.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

static std::string Hex(const uint8_t *bytes, size_t size) {
    std::string text(size * 2, '\0');
    XboxPsoCacheHex(bytes, size, text.data());
    return text;
}

static std::string Sha(const std::string &text) {
    uint8_t digest[32];
    XboxSha256Digest(text.data(), text.size(), digest);
    return Hex(digest, 32);
}

static std::vector<uint8_t> MakeFile(uint32_t mode, const uint8_t identity[32], const std::vector<uint8_t> &payload,
        uint32_t entries) {
    std::vector<uint8_t> file(XBOX_PSO_CACHE_HEADER_SIZE + payload.size());
    std::memcpy(file.data() + XBOX_PSO_CACHE_HEADER_SIZE, payload.data(), payload.size());
    XboxPsoCacheWriteHeader(file.data(), mode, identity, payload.size(), entries);
    return file;
}

static void TestSha256() {
    assert(Sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(Sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(Sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // Feeding in uneven pieces matches a single update across block boundaries.
    std::string million(1000000, 'a');
    XboxSha256 hash;
    XboxSha256Init(&hash);
    for (size_t offset = 0, step = 1; offset < million.size(); offset += step, step = step % 97 + 1)
        XboxSha256Update(&hash, million.data() + offset, std::min(step, million.size() - offset));
    uint8_t digest[32];
    XboxSha256Final(&hash, digest);
    assert(Hex(digest, 32) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void TestKeys() {
    // Length prefixes keep adjacent variable-length fields from aliasing, and
    // a null pointer differs from an empty buffer.
    auto key = [](auto fill) {
        XboxSha256 hash;
        XboxSha256Init(&hash);
        fill(&hash);
        uint8_t digest[32];
        XboxSha256Final(&hash, digest);
        return Hex(digest, 32);
    };
    const std::string ab = key([](XboxSha256 *h) { XboxPsoKeyString(h, "a"); XboxPsoKeyString(h, "b"); });
    const std::string a_b = key([](XboxSha256 *h) { XboxPsoKeyString(h, "ab"); XboxPsoKeyString(h, ""); });
    assert(ab != a_b);
    const std::string null = key([](XboxSha256 *h) { XboxPsoKeyBytes(h, nullptr, 0); });
    const char empty[1] = {0};
    const std::string none = key([&](XboxSha256 *h) { XboxPsoKeyBytes(h, empty, 0); });
    assert(null != none);
    assert(key([](XboxSha256 *h) { XboxPsoKeyU32(h, 1); }) != key([](XboxSha256 *h) { XboxPsoKeyU32(h, 256); }));
    // Byte order is fixed, independent of the host.
    assert(key([](XboxSha256 *h) { XboxPsoKeyU32(h, 0x04030201u); }) ==
           key([](XboxSha256 *h) { const uint8_t b[4] = {1, 2, 3, 4}; XboxSha256Update(h, b, 4); }));

    uint8_t digest[32];
    for (int i = 0; i < 32; ++i) digest[i] = static_cast<uint8_t>(i * 7);
    uint16_t wide[65];
    XboxPsoCacheHexWide(digest, 32, wide);
    const std::string narrow = Hex(digest, 32);
    for (int i = 0; i < 64; ++i) assert(wide[i] == static_cast<uint16_t>(narrow[i]));
    assert(wide[64] == 0);
}

static void TestLibraryFile() {
    uint8_t identity[32], other[32];
    XboxSha256Digest("identity", 8, identity);
    XboxSha256Digest("other", 5, other);
    std::vector<uint8_t> payload(1000);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 13);
    auto file = MakeFile(XBOX_PSO_CACHE_MODE_LIBRARY, identity, payload, 7);
    assert(std::memcmp(file.data(), "XPSOCACH", 8) == 0);

    XboxPsoCacheView view{};
    assert(XboxPsoCacheValidate(file.data(), file.size(), identity, &view) == XBOX_PSO_CACHE_OK);
    assert(view.mode == XBOX_PSO_CACHE_MODE_LIBRARY && view.entries == 7);
    assert(view.payloadSize == payload.size() && view.payload == file.data() + XBOX_PSO_CACHE_HEADER_SIZE);

    assert(XboxPsoCacheValidate(file.data(), file.size(), other, &view) == XBOX_PSO_CACHE_IDENTITY_MISMATCH);
    assert(XboxPsoCacheValidate(file.data(), 10, identity, &view) == XBOX_PSO_CACHE_TOO_SMALL);
    assert(XboxPsoCacheValidate(nullptr, 0, identity, &view) == XBOX_PSO_CACHE_TOO_SMALL);
    assert(XboxPsoCacheValidate(file.data(), file.size() - 1, identity, &view) == XBOX_PSO_CACHE_SIZE_MISMATCH);

    auto corrupt = file;
    corrupt.back() ^= 1;
    assert(XboxPsoCacheValidate(corrupt.data(), corrupt.size(), identity, &view) == XBOX_PSO_CACHE_HASH_MISMATCH);
    corrupt = file;
    corrupt[0] = 'Y';
    assert(XboxPsoCacheValidate(corrupt.data(), corrupt.size(), identity, &view) == XBOX_PSO_CACHE_BAD_MAGIC);
    corrupt = file;
    XboxPsoCachePut32(corrupt.data() + 8, 2);
    assert(XboxPsoCacheValidate(corrupt.data(), corrupt.size(), identity, &view) == XBOX_PSO_CACHE_BAD_VERSION);
    corrupt = file;
    XboxPsoCachePut32(corrupt.data() + 12, 3);
    assert(XboxPsoCacheValidate(corrupt.data(), corrupt.size(), identity, &view) == XBOX_PSO_CACHE_BAD_MODE);
    corrupt = file;
    XboxPsoCachePut32(corrupt.data() + 92, 1);
    assert(XboxPsoCacheValidate(corrupt.data(), corrupt.size(), identity, &view) == XBOX_PSO_CACHE_BAD_MODE);
    corrupt = file;
    XboxPsoCachePut64(corrupt.data() + 48, XBOX_PSO_CACHE_MAX_PAYLOAD + 1);
    assert(XboxPsoCacheValidate(corrupt.data(), corrupt.size(), identity, &view) == XBOX_PSO_CACHE_TOO_LARGE);

    // An empty library payload is never written, so it is not a valid file.
    auto empty = MakeFile(XBOX_PSO_CACHE_MODE_LIBRARY, identity, {}, 0);
    assert(XboxPsoCacheValidate(empty.data(), empty.size(), identity, &view) == XBOX_PSO_CACHE_SIZE_MISMATCH);
}

static void TestBlobRecords() {
    uint8_t identity[32];
    XboxSha256Digest("identity", 8, identity);
    uint8_t first[32], second[32];
    XboxSha256Digest("first", 5, first);
    XboxSha256Digest("second", 6, second);
    const std::vector<uint8_t> blobA = {1, 2, 3};
    std::vector<uint8_t> blobB(64, 9);

    assert(XboxPsoCacheRecordSize(3) == 48 && XboxPsoCacheRecordSize(8) == 48 && XboxPsoCacheRecordSize(9) == 56);
    std::vector<uint8_t> payload(XboxPsoCacheRecordSize(3) + XboxPsoCacheRecordSize(64), 0xCC);
    uint64_t used = XboxPsoCacheWriteRecord(payload.data(), first, 'G', blobA.data(), 3);
    used += XboxPsoCacheWriteRecord(payload.data() + used, second, 'C', blobB.data(), 64);
    assert(used == payload.size());

    auto file = MakeFile(XBOX_PSO_CACHE_MODE_BLOBS, identity, payload, 2);
    XboxPsoCacheView view{};
    assert(XboxPsoCacheValidate(file.data(), file.size(), identity, &view) == XBOX_PSO_CACHE_OK);
    assert(view.mode == XBOX_PSO_CACHE_MODE_BLOBS);
    uint64_t offset = 0;
    XboxPsoCacheRecord record{};
    assert(XboxPsoCacheNextRecord(view.payload, view.payloadSize, &offset, &record) == 1);
    assert(record.kind == 'G' && record.size == 3 && std::memcmp(record.key, first, 32) == 0);
    assert(std::memcmp(record.blob, blobA.data(), 3) == 0);
    assert(XboxPsoCacheNextRecord(view.payload, view.payloadSize, &offset, &record) == 1);
    assert(record.kind == 'C' && record.size == 64 && std::memcmp(record.key, second, 32) == 0);
    assert(XboxPsoCacheNextRecord(view.payload, view.payloadSize, &offset, &record) == 0);

    // Nonzero padding, a zero-length blob and a record running past the end
    // are malformed; a rewritten header checksum does not hide them.
    auto padded = payload;
    padded[XBOX_PSO_CACHE_RECORD_HEADER + 3] = 1;
    auto bad = MakeFile(XBOX_PSO_CACHE_MODE_BLOBS, identity, padded, 2);
    assert(XboxPsoCacheValidate(bad.data(), bad.size(), identity, &view) == XBOX_PSO_CACHE_BAD_RECORDS);
    auto zero = payload;
    XboxPsoCachePut32(zero.data() + 36, 0);
    bad = MakeFile(XBOX_PSO_CACHE_MODE_BLOBS, identity, zero, 2);
    assert(XboxPsoCacheValidate(bad.data(), bad.size(), identity, &view) == XBOX_PSO_CACHE_BAD_RECORDS);
    auto overrun = payload;
    XboxPsoCachePut32(overrun.data() + 36, 4096);
    bad = MakeFile(XBOX_PSO_CACHE_MODE_BLOBS, identity, overrun, 2);
    assert(XboxPsoCacheValidate(bad.data(), bad.size(), identity, &view) == XBOX_PSO_CACHE_BAD_RECORDS);
    // A large record (a multi-megabyte compiled pipeline) is kept.
    std::vector<uint8_t> large(XboxPsoCacheRecordSize(20u << 20));
    std::vector<uint8_t> big(20u << 20, 0x5A);
    XboxPsoCacheWriteRecord(large.data(), first, 'G', big.data(), static_cast<uint32_t>(big.size()));
    auto largeFile = MakeFile(XBOX_PSO_CACHE_MODE_BLOBS, identity, large, 1);
    assert(XboxPsoCacheValidate(largeFile.data(), largeFile.size(), identity, &view) == XBOX_PSO_CACHE_OK);
    std::vector<uint8_t> truncated(payload.begin(), payload.end() - 8);
    bad = MakeFile(XBOX_PSO_CACHE_MODE_BLOBS, identity, truncated, 2);
    assert(XboxPsoCacheValidate(bad.data(), bad.size(), identity, &view) == XBOX_PSO_CACHE_BAD_RECORDS);
}

static void TestWritePolicy() {
    assert(!XboxPsoCacheShouldWrite(0, 100000, 100000, 1, 1));
    assert(XboxPsoCacheShouldWrite(1, 0, 0, 1, 0));
    assert(!XboxPsoCacheShouldWrite(5, 100, 100000, 0, 0));
    assert(XboxPsoCacheShouldWrite(5, XBOX_PSO_CACHE_IDLE_MS, 0, 0, 0));
    assert(!XboxPsoCacheShouldWrite(XBOX_PSO_CACHE_BURST_COUNT, 10, XBOX_PSO_CACHE_BURST_MS - 1, 0, 0));
    assert(XboxPsoCacheShouldWrite(XBOX_PSO_CACHE_BURST_COUNT, 10, XBOX_PSO_CACHE_BURST_MS, 0, 0));
    assert(!XboxPsoCacheShouldWrite(XBOX_PSO_CACHE_BURST_COUNT - 1, 10, 1000000, 0, 0));
    // A slow compile is written at once, during a burst and right after the last write.
    assert(XboxPsoCacheShouldWrite(1, 0, 0, 0, 1));
    // 4.16 s pipelines were measured on the console; the threshold is well below.
    assert(XBOX_PSO_CACHE_SLOW_US <= 500000u);
}

static void TestStatusNames() {
    for (int status = XBOX_PSO_CACHE_OK; status <= XBOX_PSO_CACHE_BAD_RECORDS; ++status) {
        const std::string name = XboxPsoCacheStatusName(static_cast<XboxPsoCacheStatus>(status));
        assert(!name.empty() && name != "unknown");
        // Diagnostics use the loader-trace character set.
        for (char c : name) assert((c >= 'a' && c <= 'z') || c == '-');
    }
}

int main() {
    TestSha256();
    TestKeys();
    TestLibraryFile();
    TestBlobRecords();
    TestWritePolicy();
    TestStatusNames();
    return 0;
}
