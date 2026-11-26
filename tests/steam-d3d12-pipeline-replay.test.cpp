// Host tests for the Direct3D 12 pipeline record-and-replay format: desc
// serialization by content, the rebuilt graph's key, validation and policy.
#include "../runtime/win32/pipeline-replay-format.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

// A desc with the shapes the adapter serializes: a root-signature object, a
// shader by pointer and length, a null shader, an array of elements whose
// names are strings, and scalars, laid out as D3D12's are on x64.
struct Bytecode {
    const void *code;
    size_t length;
};
struct Element {
    const char *name;
    uint32_t index, format, slot, offset, classification, rate;
};
struct Layout {
    const Element *elements;
    uint32_t count;
};
struct Desc {
    void *root;
    Bytecode vs, ps, gs;
    Layout layout;
    uint32_t topology, flags;
    uint64_t sampleMask;
};
static_assert(sizeof(Element) == 32 && offsetof(Desc, vs) == 8 && offsetof(Desc, layout) == 56, "x64 layout");

// Root signatures by object, as the adapter's RememberRoot table keeps them.
static std::map<const void *, std::vector<uint8_t>> rootHashes;

static void RememberRoot(const void *object, uint32_t nodeMask, const std::string &blob) {
    std::vector<uint8_t> digest(32);
    XboxPsoReplayRootHash(nodeMask, blob.data(), blob.size(), digest.data());
    rootHashes[object] = digest;
}

// The adapter's key scheme: pointers by content, the root by its blob's hash,
// a null pointer distinct from an empty buffer.
static std::vector<uint8_t> Key(const Desc &desc) {
    XboxSha256 hash;
    XboxSha256Init(&hash);
    XboxPsoKeyU32(&hash, XBOX_PSO_CACHE_KEY_VERSION);
    XboxPsoKeyU32(&hash, 'G');
    if (desc.root) {
        XboxPsoKeyU32(&hash, 1);
        XboxSha256Update(&hash, rootHashes.at(desc.root).data(), 32);
    } else {
        XboxPsoKeyU32(&hash, 0);
    }
    for (const Bytecode *shader : {&desc.vs, &desc.ps, &desc.gs}) XboxPsoKeyBytes(&hash, shader->code, shader->length);
    XboxPsoKeyU32(&hash, desc.layout.count);
    for (uint32_t i = 0; i < desc.layout.count; ++i) {
        const Element &element = desc.layout.elements[i];
        XboxPsoKeyString(&hash, element.name);
        XboxSha256Update(&hash, &element.index, 6 * sizeof(uint32_t));
    }
    XboxPsoKeyU32(&hash, desc.topology);
    XboxPsoKeyU32(&hash, desc.flags);
    XboxPsoKeyU64(&hash, desc.sampleMask);
    std::vector<uint8_t> digest(32);
    XboxSha256Final(&hash, digest.data());
    return digest;
}

static void Shader(XboxPsoReplayBuilder *builder, uint32_t chunk, size_t at, const Bytecode &shader) {
    XboxPsoReplayAddFixup(builder, chunk, at, shader.code ? XboxPsoReplayAddChunk(builder, shader.code, shader.length) :
        XBOX_PSO_REPLAY_FIXUP_NULL);
}

// The adapter's walk for this desc shape (WalkGraphics, WalkInputLayout).
static void Walk(XboxPsoReplayBuilder *builder, const Desc &desc) {
    const uint32_t top = XboxPsoReplayAddChunk(builder, &desc, sizeof(desc));
    XboxPsoReplayAddFixup(builder, top, offsetof(Desc, root), desc.root ? XBOX_PSO_REPLAY_FIXUP_ROOT : XBOX_PSO_REPLAY_FIXUP_NULL);
    Shader(builder, top, offsetof(Desc, vs), desc.vs);
    Shader(builder, top, offsetof(Desc, ps), desc.ps);
    Shader(builder, top, offsetof(Desc, gs), desc.gs);
    const uint32_t elements = XboxPsoReplayAddChunk(builder, desc.layout.elements, desc.layout.count * sizeof(Element));
    XboxPsoReplayAddFixup(builder, top, offsetof(Desc, layout), elements);
    for (uint32_t i = 0; i < desc.layout.count; ++i) {
        const char *name = desc.layout.elements[i].name;
        XboxPsoReplayAddFixup(builder, elements, i * sizeof(Element) + offsetof(Element, name),
            name ? XboxPsoReplayAddChunk(builder, name, std::strlen(name) + 1) : XBOX_PSO_REPLAY_FIXUP_NULL);
    }
}

static std::vector<uint8_t> Serialize(const Desc &desc, const XboxPsoReplayMeta &meta) {
    auto builder = std::make_unique<XboxPsoReplayBuilder>();
    XboxPsoReplayBuilderInit(builder.get());
    Walk(builder.get(), desc);
    const uint32_t body = XboxPsoReplayPipelineBodySize(builder.get());
    assert(body);
    std::vector<uint8_t> entry(XboxPsoReplayEntrySize(body), 0xCC);
    const std::vector<uint8_t> key = Key(desc);
    const uint8_t *root = desc.root ? rootHashes.at(desc.root).data() : nullptr;
    assert(XboxPsoReplayWritePipeline(entry.data(), builder.get(), key.data(), root, &meta) == entry.size());
    return entry;
}

static bool Contains(const std::vector<uint8_t> &bytes, const void *pointer) {
    uint8_t value[sizeof(pointer)];
    std::memcpy(value, &pointer, sizeof(pointer));
    for (size_t i = 0; i + sizeof(value) <= bytes.size(); ++i)
        if (std::memcmp(bytes.data() + i, value, sizeof(value)) == 0) return true;
    return false;
}

struct Fixture {
    int titleRoot = 0, replayRoot = 0;
    std::string rootBlob = std::string("DXBC root signature v1.1\0\x01\x02", 28);
    std::vector<uint8_t> vs = std::vector<uint8_t>(5000, 0x11);
    std::vector<uint8_t> ps = std::vector<uint8_t>(777, 0x22);
    Element elements[2] = {{"POSITION", 0, 6, 0, 0, 0, 0}, {"TEXCOORD", 3, 16, 1, 12, 1, 1}};
    Desc desc{};

    Fixture() {
        RememberRoot(&titleRoot, 0, rootBlob);
        // The replay's own object from the same blob hashes the same.
        RememberRoot(&replayRoot, 0, rootBlob);
        desc.root = &titleRoot;
        desc.vs = {vs.data(), vs.size()};
        desc.ps = {ps.data(), ps.size()};
        desc.gs = {nullptr, 0};
        desc.layout = {elements, 2};
        desc.topology = 3;
        desc.flags = 0;
        desc.sampleMask = 0xFFFFFFFFull;
    }
};

static void TestRoundTrip() {
    Fixture fixture;
    const XboxPsoReplayMeta meta = {'G', 17, 105000, 61000, 2, 0};
    const auto entry = Serialize(fixture.desc, meta);

    // No process address reaches the file.
    for (const void *pointer : {static_cast<const void *>(&fixture.titleRoot), static_cast<const void *>(fixture.vs.data()),
                                static_cast<const void *>(fixture.ps.data()), static_cast<const void *>(fixture.elements),
                                static_cast<const void *>(fixture.elements[0].name)})
        assert(!Contains(entry, pointer));

    uint64_t offset = 0;
    uint32_t tag = 0, bodySize = 0;
    const uint8_t *body = nullptr;
    assert(XboxPsoReplayNextEntry(entry.data(), entry.size(), &offset, &tag, &body, &bodySize) == 1);
    assert(tag == XBOX_PSO_REPLAY_TAG_PIPELINE && offset == entry.size());
    XboxPsoReplayPipeline pipeline{};
    assert(XboxPsoReplayParsePipeline(body, bodySize, &pipeline));
    assert(pipeline.meta.kind == 'G' && pipeline.meta.order == 17 && pipeline.meta.createUs == 105000);
    assert(pipeline.meta.atMs == 61000 && pipeline.meta.phase == 2 && pipeline.meta.age == 0);
    assert(XboxPsoReplayHasRoot(&pipeline));
    assert(std::memcmp(pipeline.rootHash, rootHashes.at(&fixture.titleRoot).data(), 32) == 0);
    assert(XboxPsoReplayChunkSize(&pipeline, 0) == sizeof(Desc));

    // Rebuilt with another root object for the same blob, in a fresh arena:
    // the same content and the same key.
    // Unsigned-char storage from new[] is aligned for any object and may hold the rebuilt desc.
    std::unique_ptr<uint8_t[]> arena(new uint8_t[XboxPsoReplayArenaSize(&pipeline)]);
    const Desc *rebuilt = static_cast<const Desc *>(XboxPsoReplayBuild(&pipeline, arena.get(), &fixture.replayRoot));
    assert(rebuilt->root == &fixture.replayRoot);
    assert(rebuilt->vs.code != fixture.vs.data() && rebuilt->vs.length == fixture.vs.size());
    assert(std::memcmp(rebuilt->vs.code, fixture.vs.data(), fixture.vs.size()) == 0);
    assert(std::memcmp(rebuilt->ps.code, fixture.ps.data(), fixture.ps.size()) == 0);
    assert(rebuilt->gs.code == nullptr && rebuilt->gs.length == 0);
    assert(rebuilt->layout.count == 2 && std::string(rebuilt->layout.elements[1].name) == "TEXCOORD");
    assert(rebuilt->layout.elements[1].offset == 12 && rebuilt->sampleMask == 0xFFFFFFFFull);
    assert(Key(*rebuilt) == Key(fixture.desc));
    assert(std::memcmp(pipeline.key, Key(fixture.desc).data(), 32) == 0);
    // Pointer slots are 8-aligned in the arena.
    assert(reinterpret_cast<uintptr_t>(&rebuilt->layout.elements[1].name) % 8 == 0);

    // A different blob for the root is a different key: the identity rule
    // never confuses root signatures.
    int otherRoot = 0;
    RememberRoot(&otherRoot, 0, "another root");
    Desc other = fixture.desc;
    other.root = &otherRoot;
    assert(Key(other) != Key(fixture.desc));
    RememberRoot(&otherRoot, 1, fixture.rootBlob);  // node mask is part of the hash
    assert(Key(other) != Key(fixture.desc));
}

static void TestNullAndEmpty() {
    Fixture fixture;
    // A non-null empty shader stays non-null; a null root stays null.
    static const uint8_t none[1] = {0};
    fixture.desc.gs = {none, 0};
    fixture.desc.root = nullptr;
    const XboxPsoReplayMeta meta = {'G', 0, 1000, 0, 0, 0};
    const auto entry = Serialize(fixture.desc, meta);
    uint64_t offset = 0;
    uint32_t tag, bodySize;
    const uint8_t *body;
    assert(XboxPsoReplayNextEntry(entry.data(), entry.size(), &offset, &tag, &body, &bodySize) == 1);
    XboxPsoReplayPipeline pipeline{};
    assert(XboxPsoReplayParsePipeline(body, bodySize, &pipeline));
    assert(!XboxPsoReplayHasRoot(&pipeline));
    std::unique_ptr<uint8_t[]> arena(new uint8_t[XboxPsoReplayArenaSize(&pipeline)]);
    int unused = 0;
    const Desc *rebuilt = static_cast<const Desc *>(XboxPsoReplayBuild(&pipeline, arena.get(), &unused));
    assert(rebuilt->root == nullptr && rebuilt->gs.code != nullptr && rebuilt->gs.length == 0);
    assert(Key(*rebuilt) == Key(fixture.desc));
}

// Copies an entry and applies an edit to its body.
template <typename Edit>
static bool ParsesAfter(const std::vector<uint8_t> &entry, Edit edit) {
    std::vector<uint8_t> copy = entry;
    edit(copy.data() + 8);
    XboxPsoReplayPipeline pipeline{};
    return XboxPsoReplayParsePipeline(copy.data() + 8, XboxPsoCacheGet32(copy.data() + 4), &pipeline);
}

static void TestMalformedPipelines() {
    Fixture fixture;
    const XboxPsoReplayMeta meta = {'G', 1, 5000, 10, 0, 0};
    const auto entry = Serialize(fixture.desc, meta);
    const uint32_t chunks = XboxPsoCacheGet32(entry.data() + 8 + 88);
    const size_t fixups = XBOX_PSO_REPLAY_PIPELINE_HEADER + 8 * chunks;
    assert(ParsesAfter(entry, [](uint8_t *) {}));
    assert(!ParsesAfter(entry, [](uint8_t *body) { XboxPsoCachePut32(body + 64, 'X'); }));
    assert(!ParsesAfter(entry, [](uint8_t *body) { XboxPsoCachePut32(body + 88, 0); }));
    assert(!ParsesAfter(entry, [](uint8_t *body) { XboxPsoCachePut32(body + 88, XBOX_PSO_REPLAY_MAX_CHUNKS + 1); }));
    // A chunk outside the data area, or misaligned.
    assert(!ParsesAfter(entry, [](uint8_t *body) { XboxPsoCachePut32(body + XBOX_PSO_REPLAY_PIPELINE_HEADER + 4, 1u << 30); }));
    assert(!ParsesAfter(entry, [](uint8_t *body) { XboxPsoCachePut32(body + XBOX_PSO_REPLAY_PIPELINE_HEADER, 4); }));
    // A fixup into another chunk's range, misaligned, past its chunk, or to a missing chunk.
    assert(!ParsesAfter(entry, [&](uint8_t *body) { XboxPsoCachePut32(body + fixups, chunks); }));
    assert(!ParsesAfter(entry, [&](uint8_t *body) { XboxPsoCachePut32(body + fixups + 4, 4); }));
    assert(!ParsesAfter(entry, [&](uint8_t *body) { XboxPsoCachePut32(body + fixups + 4, sizeof(Desc)); }));
    assert(!ParsesAfter(entry, [&](uint8_t *body) { XboxPsoCachePut32(body + fixups + 8, chunks); }));
    // A root fixup without a root hash.
    assert(!ParsesAfter(entry, [](uint8_t *body) { std::memset(body + 32, 0, 32); }));
    // Nonzero padding between the tables and the data.
    const size_t tables = XboxPsoReplayTablesSize(chunks, XboxPsoCacheGet32(entry.data() + 8 + 92));
    const size_t tableEnd = fixups + 12 * XboxPsoCacheGet32(entry.data() + 8 + 92);
    if (tableEnd < tables) assert(!ParsesAfter(entry, [&](uint8_t *body) { body[tableEnd] = 1; }));

    // Entry framing: nonzero padding after the body, and truncation.
    std::vector<uint8_t> padded = entry;
    const uint32_t bodySize = XboxPsoCacheGet32(entry.data() + 4);
    if (XboxPsoReplayAlign8(bodySize) != bodySize) {
        padded[8 + bodySize] = 1;
        uint64_t offset = 0;
        uint32_t tag, size;
        const uint8_t *body;
        assert(XboxPsoReplayNextEntry(padded.data(), padded.size(), &offset, &tag, &body, &size) == -1);
    }
    uint64_t offset = 0;
    uint32_t tag, size;
    const uint8_t *body;
    assert(XboxPsoReplayNextEntry(entry.data(), entry.size() - 8, &offset, &tag, &body, &size) == -1);
    offset = 4;
    assert(XboxPsoReplayNextEntry(entry.data(), entry.size(), &offset, &tag, &body, &size) == -1);
}

static void TestBuilderBounds() {
    auto builder = std::make_unique<XboxPsoReplayBuilder>();
    XboxPsoReplayBuilderInit(builder.get());
    static const uint8_t byte = 0;
    assert(XboxPsoReplayPipelineBodySize(builder.get()) == 0);  // no chunk 0
    for (uint32_t i = 0; i < XBOX_PSO_REPLAY_MAX_CHUNKS; ++i) assert(XboxPsoReplayAddChunk(builder.get(), &byte, 1) == i);
    assert(XboxPsoReplayPipelineBodySize(builder.get()) != 0);
    assert(XboxPsoReplayAddChunk(builder.get(), &byte, 1) == XBOX_PSO_REPLAY_NO_CHUNK);
    assert(XboxPsoReplayPipelineBodySize(builder.get()) == 0);  // overflow: not recorded

    XboxPsoReplayBuilderInit(builder.get());
    uint64_t slot = 0;
    const uint32_t top = XboxPsoReplayAddChunk(builder.get(), &slot, sizeof(slot));
    XboxPsoReplayAddFixup(builder.get(), top, 4, XBOX_PSO_REPLAY_FIXUP_NULL);  // misaligned
    assert(builder->overflow);
    XboxPsoReplayBuilderInit(builder.get());
    XboxPsoReplayAddChunk(builder.get(), &slot, sizeof(slot));
    XboxPsoReplayAddFixup(builder.get(), 0, 0, 1);  // a chunk that does not exist
    assert(builder->overflow);
    XboxPsoReplayBuilderInit(builder.get());
    XboxPsoReplayAddChunk(builder.get(), &slot, 4);
    XboxPsoReplayAddFixup(builder.get(), 0, 0, XBOX_PSO_REPLAY_FIXUP_NULL);  // the slot overruns the chunk
    assert(builder->overflow);
}

static void TestPayloadAndFile() {
    Fixture fixture;
    std::vector<uint8_t> payload(XboxPsoReplayEntrySize(XBOX_PSO_REPLAY_ROOT_HEADER + fixture.rootBlob.size()));
    const uint8_t *rootHash = rootHashes.at(&fixture.titleRoot).data();
    assert(XboxPsoReplayWriteRoot(payload.data(), rootHash, 0, fixture.rootBlob.data(),
        static_cast<uint32_t>(fixture.rootBlob.size())) == payload.size());
    for (uint32_t order = 0; order < 3; ++order) {
        fixture.desc.topology = order + 1;  // three distinct pipelines
        const XboxPsoReplayMeta meta = {'G', order, 2000 + order, order * 100, 0, 0};
        const auto entry = Serialize(fixture.desc, meta);
        payload.insert(payload.end(), entry.begin(), entry.end());
    }
    uint32_t pipelines = 0, roots = 0;
    assert(XboxPsoReplayValidatePayload(payload.data(), payload.size(), &pipelines, &roots) && pipelines == 3 && roots == 1);

    uint64_t offset = 0;
    uint32_t tag, bodySize;
    const uint8_t *body;
    assert(XboxPsoReplayNextEntry(payload.data(), payload.size(), &offset, &tag, &body, &bodySize) == 1);
    XboxPsoReplayRoot root{};
    assert(tag == XBOX_PSO_REPLAY_TAG_ROOT && XboxPsoReplayParseRoot(body, bodySize, &root));
    assert(root.size == fixture.rootBlob.size() && std::memcmp(root.blob, fixture.rootBlob.data(), root.size) == 0);

    // A root blob that no longer matches its hash, an unknown tag, a truncated payload.
    auto bad = payload;
    bad[8 + XBOX_PSO_REPLAY_ROOT_HEADER] ^= 1;
    assert(!XboxPsoReplayValidatePayload(bad.data(), bad.size(), &pipelines, &roots));
    bad = payload;
    XboxPsoCachePut32(bad.data(), 0x58585858u);
    assert(!XboxPsoReplayValidatePayload(bad.data(), bad.size(), &pipelines, &roots));
    assert(!XboxPsoReplayValidatePayload(payload.data(), payload.size() - 8, &pipelines, &roots));

    // The cache container carries it as mode 3 with the usual identity and checksum.
    uint8_t identity[32];
    XboxSha256Digest("identity", 8, identity);
    std::vector<uint8_t> file(XBOX_PSO_CACHE_HEADER_SIZE + payload.size());
    std::memcpy(file.data() + XBOX_PSO_CACHE_HEADER_SIZE, payload.data(), payload.size());
    XboxPsoCacheWriteHeader(file.data(), XBOX_PSO_CACHE_MODE_REPLAY, identity, payload.size(), 3);
    XboxPsoCacheView view{};
    assert(XboxPsoCacheValidate(file.data(), file.size(), identity, &view) == XBOX_PSO_CACHE_OK);
    assert(view.mode == XBOX_PSO_CACHE_MODE_REPLAY && view.entries == 3 && view.payloadSize == payload.size());
    file.back() ^= 1;
    assert(XboxPsoCacheValidate(file.data(), file.size(), identity, &view) == XBOX_PSO_CACHE_HASH_MISMATCH);

    // Rewriting the meta in place (age, this launch's order) keeps the entry valid.
    offset = 0;
    XboxPsoReplayNextEntry(payload.data(), payload.size(), &offset, &tag, &body, &bodySize);
    uint8_t *second = payload.data() + offset;
    const XboxPsoReplayMeta updated = {'G', 40, 2000, 5, 1, 3};
    XboxPsoReplayPutMeta(second + 8, &updated);
    XboxPsoReplayPipeline pipeline{};
    assert(XboxPsoReplayParsePipeline(second + 8, XboxPsoCacheGet32(second + 4), &pipeline));
    assert(pipeline.meta.order == 40 && pipeline.meta.phase == 1 && pipeline.meta.age == 3);
    assert(XboxPsoReplayValidatePayload(payload.data(), payload.size(), &pipelines, &roots));
}

static void TestPolicy() {
    // The title's recorded order: by phase, then time, then sequence.
    const XboxPsoReplayMeta metas[] = {
        {'G', 0, 6000000, 0, 0, 0},    // the 6 s startup pipeline
        {'G', 1, 40000, 900, 0, 0},
        {'G', 30, 105000, 61000, 2, 0}, // first combat
        {'G', 20, 38000, 30000, 1, 0},  // main menu
        {'G', 21, 68000, 30000, 1, 0},
    };
    uint32_t order[] = {0, 1, 2, 3, 4};
    XboxPsoReplaySort(order, 5, metas);
    const uint32_t expected[] = {0, 1, 3, 4, 2};
    for (int i = 0; i < 5; ++i) assert(order[i] == expected[i]);

    assert(XboxPsoReplayNextAge(5, 1) == 0);
    assert(XboxPsoReplayNextAge(0, 0) == 1);
    assert(XboxPsoReplayNextAge(XBOX_PSO_REPLAY_MAX_AGE - 2, 0) == static_cast<int>(XBOX_PSO_REPLAY_MAX_AGE - 1));
    assert(XboxPsoReplayNextAge(XBOX_PSO_REPLAY_MAX_AGE - 1, 0) == -1);

    assert(!XboxPsoReplayNewPhase(100000, 1));
    assert(!XboxPsoReplayNewPhase(XBOX_PSO_REPLAY_IDLE_MS - 1, 0));
    assert(XboxPsoReplayNewPhase(XBOX_PSO_REPLAY_IDLE_MS, 0));
    // Measured first-turn stalls were 38 to 105 ms pipelines; the record floor is far below.
    assert(XBOX_PSO_REPLAY_RECORD_US <= 2000u);
}

static void TestRootHash() {
    // The same digest as the adapter's RememberRoot: node mask, then the blob with its length.
    const std::string blob = "root";
    uint8_t digest[32], manual[32];
    XboxPsoReplayRootHash(0, blob.data(), blob.size(), digest);
    XboxSha256 hash;
    XboxSha256Init(&hash);
    XboxPsoKeyU32(&hash, 0);
    XboxPsoKeyBytes(&hash, blob.data(), blob.size());
    XboxSha256Final(&hash, manual);
    assert(std::memcmp(digest, manual, 32) == 0);
    XboxPsoReplayRootHash(1, blob.data(), blob.size(), manual);
    assert(std::memcmp(digest, manual, 32) != 0);
}

int main() {
    TestRoundTrip();
    TestNullAndEmpty();
    TestMalformedPipelines();
    TestBuilderBounds();
    TestPayloadAndFile();
    TestPolicy();
    TestRootHash();
    return 0;
}
