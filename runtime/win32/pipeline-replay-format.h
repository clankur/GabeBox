/* Record-and-replay pipeline pre-warm: the payload of a mode-3 cache file,
   pipeline records, and the replay policy. Shared by the Direct3D 12
   pipeline cache adapter and its host-side unit tests.

   Platform-neutral C (also valid C++) with no C runtime calls, like
   pipeline-cache-format.h, whose header, identity and checksum a replay file
   reuses (mode XBOX_PSO_CACHE_MODE_REPLAY).

   The payload is a sequence of entries, each 8-byte aligned:

     u32 tag ("ROOT" or "PPSO"), u32 body size, body, zero padding to 8

   A root entry is a root signature as the title created it:

     hash[32]  XboxPsoReplayRootHash(nodeMask, blob)
     u32 nodeMask, u32 reserved (zero), blob

   A pipeline entry is a pipeline desc serialized by content as a graph of
   chunks. Chunk 0 is the top-level desc (the D3D12_GRAPHICS_ or
   D3D12_COMPUTE_PIPELINE_STATE_DESC, or the subobject stream itself); every
   pointer field in any chunk has a fixup that names the chunk it points to,
   the root signature, or null. Pointer slots are stored as zero, so the file
   holds no process address.

     0   key[32]       the cache key of the desc (SHA-256 over its content)
     32  rootHash[32]  the root signature's hash, zero without one
     64  u32 kind      'G', 'C' or 'S'
     68  u32 order     the title's creation sequence number
     72  u32 createUs  the title's creation time when it compiled it
     76  u32 atMs      ms after the title's first creation in that process
     80  u32 phase     loading bursts seen before it (0: the startup burst)
     84  u32 age       launches since the title last asked for it
     88  u32 chunkCount
     92  u32 fixupCount
     96  chunkCount x {u32 offset, u32 size}: offsets into the data area, 8-aligned
         fixupCount x {u32 chunk, u32 offset, u32 target}: an 8-aligned pointer
             slot in a chunk; target is a chunk index, FIXUP_ROOT or FIXUP_NULL
         zero padding to 8, then the data area */
#pragma once
#include "pipeline-cache-format.h"

#define XBOX_PSO_REPLAY_TAG_ROOT 0x544F4F52u     /* "ROOT" */
#define XBOX_PSO_REPLAY_TAG_PIPELINE 0x4F535050u /* "PPSO" */
#define XBOX_PSO_REPLAY_ROOT_HEADER 40u
#define XBOX_PSO_REPLAY_PIPELINE_HEADER 96u
#define XBOX_PSO_REPLAY_FIXUP_NULL 0xFFFFFFFEu
#define XBOX_PSO_REPLAY_FIXUP_ROOT 0xFFFFFFFFu
#define XBOX_PSO_REPLAY_NO_CHUNK 0xFFFFFFFFu

/* Bounds. A desc with more chunks or fixups is not recorded. */
#define XBOX_PSO_REPLAY_MAX_CHUNKS 512u
#define XBOX_PSO_REPLAY_MAX_FIXUPS 1024u
#define XBOX_PSO_REPLAY_MAX_PAYLOAD (64ull * 1024 * 1024)
#define XBOX_PSO_REPLAY_MAX_PIPELINES 2048u
#define XBOX_PSO_REPLAY_MAX_ROOTS 512u
#define XBOX_PSO_REPLAY_MAX_BODY (32u * 1024 * 1024)

/* Policy. Only creations of at least RECORD_US are recorded: cheaper ones
   cannot stall a frame. A record the title has not asked for in MAX_AGE
   launches is dropped. IDLE_MS without a creation ends a loading phase.
   Pre-created pipelines the title has not asked for are released HOLD_MS
   after replay starts. */
#define XBOX_PSO_REPLAY_RECORD_US 1000u
#define XBOX_PSO_REPLAY_MAX_AGE 8u
#define XBOX_PSO_REPLAY_IDLE_MS 2000u
#define XBOX_PSO_REPLAY_HOLD_MS 600000u
/* replay-idle waits for the first idle period, at most this long. */
#define XBOX_PSO_REPLAY_IDLE_WAIT_MS 120000u

static inline uint32_t XboxPsoReplayAlign8(uint64_t value) {
    return (uint32_t)((value + 7u) & ~(uint64_t)7u);
}

static inline uint64_t XboxPsoReplayEntrySize(uint32_t bodySize) {
    return 8u + (uint64_t)XboxPsoReplayAlign8(bodySize);
}

/* The root-signature hash the cache keys pipelines with: the node mask, then
   the blob with its length (XboxPsoKeyBytes). */
static inline void XboxPsoReplayRootHash(uint32_t nodeMask, const void *blob, uint64_t size, uint8_t digest[32]) {
    XboxSha256 hash;
    XboxSha256Init(&hash);
    XboxPsoKeyU32(&hash, nodeMask);
    XboxPsoKeyBytes(&hash, blob, size);
    XboxSha256Final(&hash, digest);
}

/* --------------------------------------------------------------- writing */

typedef struct XboxPsoReplayBuilder {
    uint32_t chunkCount, fixupCount;
    int overflow;
    const void *chunkData[XBOX_PSO_REPLAY_MAX_CHUNKS];
    uint32_t chunkSize[XBOX_PSO_REPLAY_MAX_CHUNKS];
    uint32_t fixupChunk[XBOX_PSO_REPLAY_MAX_FIXUPS];
    uint32_t fixupOffset[XBOX_PSO_REPLAY_MAX_FIXUPS];
    uint32_t fixupTarget[XBOX_PSO_REPLAY_MAX_FIXUPS];
} XboxPsoReplayBuilder;

typedef struct XboxPsoReplayMeta {
    uint32_t kind, order, createUs, atMs, phase, age;
} XboxPsoReplayMeta;

static inline void XboxPsoReplayBuilderInit(XboxPsoReplayBuilder *builder) {
    builder->chunkCount = builder->fixupCount = 0;
    builder->overflow = 0;
}

/* The chunk's index, or XBOX_PSO_REPLAY_NO_CHUNK (and overflow) when full.
   The bytes are copied only by XboxPsoReplayWritePipeline. */
static inline uint32_t XboxPsoReplayAddChunk(XboxPsoReplayBuilder *builder, const void *data, uint64_t size) {
    if (builder->chunkCount >= XBOX_PSO_REPLAY_MAX_CHUNKS || size > XBOX_PSO_REPLAY_MAX_BODY) {
        builder->overflow = 1;
        return XBOX_PSO_REPLAY_NO_CHUNK;
    }
    builder->chunkData[builder->chunkCount] = data;
    builder->chunkSize[builder->chunkCount] = (uint32_t)size;
    return builder->chunkCount++;
}

/* A pointer slot at offset in chunk: target is a chunk index, FIXUP_ROOT or
   FIXUP_NULL. */
static inline void XboxPsoReplayAddFixup(XboxPsoReplayBuilder *builder, uint32_t chunk, uint64_t offset,
        uint32_t target) {
    if (builder->fixupCount >= XBOX_PSO_REPLAY_MAX_FIXUPS || chunk >= builder->chunkCount || (offset & 7) ||
            offset + 8 > builder->chunkSize[chunk] || (target != XBOX_PSO_REPLAY_FIXUP_NULL &&
            target != XBOX_PSO_REPLAY_FIXUP_ROOT && target >= builder->chunkCount)) {
        builder->overflow = 1;
        return;
    }
    builder->fixupChunk[builder->fixupCount] = chunk;
    builder->fixupOffset[builder->fixupCount] = (uint32_t)offset;
    builder->fixupTarget[builder->fixupCount] = target;
    ++builder->fixupCount;
}

static inline uint64_t XboxPsoReplayTablesSize(uint32_t chunks, uint32_t fixups) {
    return XboxPsoReplayAlign8(XBOX_PSO_REPLAY_PIPELINE_HEADER + 8ull * chunks + 12ull * fixups);
}

/* The body size of a pipeline entry, or 0 when the builder overflowed or the
   body would exceed MAX_BODY. */
static inline uint32_t XboxPsoReplayPipelineBodySize(const XboxPsoReplayBuilder *builder) {
    if (builder->overflow || !builder->chunkCount) return 0;
    uint64_t size = XboxPsoReplayTablesSize(builder->chunkCount, builder->fixupCount);
    for (uint32_t i = 0; i < builder->chunkCount; ++i) size += XboxPsoReplayAlign8(builder->chunkSize[i]);
    return size > XBOX_PSO_REPLAY_MAX_BODY ? 0 : (uint32_t)size;
}

static inline void XboxPsoReplayPutMeta(uint8_t *body, const XboxPsoReplayMeta *meta) {
    XboxPsoCachePut32(body + 64, meta->kind);
    XboxPsoCachePut32(body + 68, meta->order);
    XboxPsoCachePut32(body + 72, meta->createUs);
    XboxPsoCachePut32(body + 76, meta->atMs);
    XboxPsoCachePut32(body + 80, meta->phase);
    XboxPsoCachePut32(body + 84, meta->age);
}

/* Writes a whole pipeline entry (tag, size, body, padding) and returns its
   size; out holds XboxPsoReplayEntrySize(XboxPsoReplayPipelineBodySize()). */
static inline uint64_t XboxPsoReplayWritePipeline(uint8_t *out, const XboxPsoReplayBuilder *builder,
        const uint8_t key[32], const uint8_t rootHash[32], const XboxPsoReplayMeta *meta) {
    const uint32_t bodySize = XboxPsoReplayPipelineBodySize(builder);
    if (!bodySize) return 0;
    XboxPsoCachePut32(out, XBOX_PSO_REPLAY_TAG_PIPELINE);
    XboxPsoCachePut32(out + 4, bodySize);
    uint8_t *body = out + 8;
    XboxPsoCacheCopy(body, key, 32);
    if (rootHash) XboxPsoCacheCopy(body + 32, rootHash, 32);
    else XboxPsoCacheFill(body + 32, 0, 32);
    XboxPsoReplayPutMeta(body, meta);
    XboxPsoCachePut32(body + 88, builder->chunkCount);
    XboxPsoCachePut32(body + 92, builder->fixupCount);
    const uint64_t tables = XboxPsoReplayTablesSize(builder->chunkCount, builder->fixupCount);
    uint8_t *data = body + tables;
    uint32_t offset = 0;
    for (uint32_t i = 0; i < builder->chunkCount; ++i) {
        XboxPsoCachePut32(body + XBOX_PSO_REPLAY_PIPELINE_HEADER + 8 * i, offset);
        XboxPsoCachePut32(body + XBOX_PSO_REPLAY_PIPELINE_HEADER + 8 * i + 4, builder->chunkSize[i]);
        const uint32_t padded = XboxPsoReplayAlign8(builder->chunkSize[i]);
        if (builder->chunkSize[i])
            XboxPsoCacheCopy(data + offset, (const uint8_t *)builder->chunkData[i], builder->chunkSize[i]);
        XboxPsoCacheFill(data + offset + builder->chunkSize[i], 0, padded - builder->chunkSize[i]);
        offset += padded;
    }
    uint8_t *fixups = body + XBOX_PSO_REPLAY_PIPELINE_HEADER + 8 * builder->chunkCount;
    uint32_t chunkStart[XBOX_PSO_REPLAY_MAX_CHUNKS];
    for (uint32_t i = 0, at = 0; i < builder->chunkCount; ++i) {
        chunkStart[i] = at;
        at += XboxPsoReplayAlign8(builder->chunkSize[i]);
    }
    for (uint32_t i = 0; i < builder->fixupCount; ++i) {
        XboxPsoCachePut32(fixups + 12 * i, builder->fixupChunk[i]);
        XboxPsoCachePut32(fixups + 12 * i + 4, builder->fixupOffset[i]);
        XboxPsoCachePut32(fixups + 12 * i + 8, builder->fixupTarget[i]);
        /* No process address reaches the file. */
        XboxPsoCacheFill(data + chunkStart[builder->fixupChunk[i]] + builder->fixupOffset[i], 0, 8);
    }
    uint8_t *tableEnd = fixups + 12 * builder->fixupCount;
    XboxPsoCacheFill(tableEnd, 0, (size_t)(data - tableEnd));
    XboxPsoCacheFill(body + bodySize, 0, XboxPsoReplayAlign8(bodySize) - bodySize);
    return XboxPsoReplayEntrySize(bodySize);
}

/* Writes a root entry and returns its size. */
static inline uint64_t XboxPsoReplayWriteRoot(uint8_t *out, const uint8_t hash[32], uint32_t nodeMask,
        const void *blob, uint32_t size) {
    const uint32_t bodySize = XBOX_PSO_REPLAY_ROOT_HEADER + size;
    XboxPsoCachePut32(out, XBOX_PSO_REPLAY_TAG_ROOT);
    XboxPsoCachePut32(out + 4, bodySize);
    XboxPsoCacheCopy(out + 8, hash, 32);
    XboxPsoCachePut32(out + 40, nodeMask);
    XboxPsoCachePut32(out + 44, 0);
    XboxPsoCacheCopy(out + 48, (const uint8_t *)blob, size);
    XboxPsoCacheFill(out + 8 + bodySize, 0, XboxPsoReplayAlign8(bodySize) - bodySize);
    return XboxPsoReplayEntrySize(bodySize);
}

/* --------------------------------------------------------------- reading */

/* The entry at *offset: 1 and advance, 0 at the exact end, -1 malformed. */
static inline int XboxPsoReplayNextEntry(const uint8_t *payload, uint64_t size, uint64_t *offset, uint32_t *tag,
        const uint8_t **body, uint32_t *bodySize) {
    if (*offset == size) return 0;
    if (*offset > size || size - *offset < 8 || (*offset & 7)) return -1;
    const uint8_t *start = payload + *offset;
    const uint32_t length = XboxPsoCacheGet32(start + 4);
    const uint64_t total = XboxPsoReplayEntrySize(length);
    if (length > XBOX_PSO_REPLAY_MAX_BODY || total > size - *offset) return -1;
    for (uint64_t i = 8u + length; i < total; ++i)
        if (start[i]) return -1;
    *tag = XboxPsoCacheGet32(start);
    *body = start + 8;
    *bodySize = length;
    *offset += total;
    return 1;
}

typedef struct XboxPsoReplayRoot {
    const uint8_t *hash;
    uint32_t nodeMask;
    const uint8_t *blob;
    uint32_t size;
} XboxPsoReplayRoot;

/* 1 for a well-formed root entry whose hash matches its blob. */
static inline int XboxPsoReplayParseRoot(const uint8_t *body, uint32_t bodySize, XboxPsoReplayRoot *root) {
    if (bodySize <= XBOX_PSO_REPLAY_ROOT_HEADER || XboxPsoCacheGet32(body + 36)) return 0;
    root->hash = body;
    root->nodeMask = XboxPsoCacheGet32(body + 32);
    root->blob = body + XBOX_PSO_REPLAY_ROOT_HEADER;
    root->size = bodySize - XBOX_PSO_REPLAY_ROOT_HEADER;
    uint8_t digest[32];
    XboxPsoReplayRootHash(root->nodeMask, root->blob, root->size, digest);
    return XboxPsoCacheSameBytes(digest, root->hash, 32);
}

typedef struct XboxPsoReplayPipeline {
    const uint8_t *key;
    const uint8_t *rootHash;
    XboxPsoReplayMeta meta;
    uint32_t chunkCount, fixupCount;
    const uint8_t *chunks;  /* chunkCount x {offset, size} */
    const uint8_t *fixups;  /* fixupCount x {chunk, offset, target} */
    const uint8_t *data;
    uint32_t dataSize;
} XboxPsoReplayPipeline;

static inline int XboxPsoReplayHasRoot(const XboxPsoReplayPipeline *pipeline) {
    static const uint8_t none[32] = {0};
    return !XboxPsoCacheSameBytes(pipeline->rootHash, none, 32);
}

/* 1 for a well-formed pipeline entry: every chunk inside the data area,
   every fixup an aligned pointer slot inside its chunk with a valid target. */
static inline int XboxPsoReplayParsePipeline(const uint8_t *body, uint32_t bodySize, XboxPsoReplayPipeline *pipeline) {
    if (bodySize < XBOX_PSO_REPLAY_PIPELINE_HEADER) return 0;
    pipeline->key = body;
    pipeline->rootHash = body + 32;
    pipeline->meta.kind = XboxPsoCacheGet32(body + 64);
    pipeline->meta.order = XboxPsoCacheGet32(body + 68);
    pipeline->meta.createUs = XboxPsoCacheGet32(body + 72);
    pipeline->meta.atMs = XboxPsoCacheGet32(body + 76);
    pipeline->meta.phase = XboxPsoCacheGet32(body + 80);
    pipeline->meta.age = XboxPsoCacheGet32(body + 84);
    pipeline->chunkCount = XboxPsoCacheGet32(body + 88);
    pipeline->fixupCount = XboxPsoCacheGet32(body + 92);
    const uint32_t kind = pipeline->meta.kind;
    if ((kind != 'G' && kind != 'C' && kind != 'S') || !pipeline->chunkCount ||
            pipeline->chunkCount > XBOX_PSO_REPLAY_MAX_CHUNKS || pipeline->fixupCount > XBOX_PSO_REPLAY_MAX_FIXUPS)
        return 0;
    const uint64_t tables = XboxPsoReplayTablesSize(pipeline->chunkCount, pipeline->fixupCount);
    if (tables > bodySize) return 0;
    pipeline->chunks = body + XBOX_PSO_REPLAY_PIPELINE_HEADER;
    pipeline->fixups = pipeline->chunks + 8 * pipeline->chunkCount;
    pipeline->data = body + tables;
    pipeline->dataSize = (uint32_t)(bodySize - tables);
    for (const uint8_t *pad = pipeline->fixups + 12 * pipeline->fixupCount; pad < pipeline->data; ++pad)
        if (*pad) return 0;
    for (uint32_t i = 0; i < pipeline->chunkCount; ++i) {
        const uint32_t offset = XboxPsoCacheGet32(pipeline->chunks + 8 * i);
        const uint32_t size = XboxPsoCacheGet32(pipeline->chunks + 8 * i + 4);
        if ((offset & 7) || offset > pipeline->dataSize || size > pipeline->dataSize - offset) return 0;
    }
    for (uint32_t i = 0; i < pipeline->fixupCount; ++i) {
        const uint32_t chunk = XboxPsoCacheGet32(pipeline->fixups + 12 * i);
        const uint32_t offset = XboxPsoCacheGet32(pipeline->fixups + 12 * i + 4);
        const uint32_t target = XboxPsoCacheGet32(pipeline->fixups + 12 * i + 8);
        if (chunk >= pipeline->chunkCount || (offset & 7) ||
                (uint64_t)offset + 8 > XboxPsoCacheGet32(pipeline->chunks + 8 * chunk + 4))
            return 0;
        if (target != XBOX_PSO_REPLAY_FIXUP_NULL && target != XBOX_PSO_REPLAY_FIXUP_ROOT &&
                target >= pipeline->chunkCount)
            return 0;
        if (target == XBOX_PSO_REPLAY_FIXUP_ROOT && !XboxPsoReplayHasRoot(pipeline)) return 0;
    }
    return 1;
}

static inline uint32_t XboxPsoReplayChunkSize(const XboxPsoReplayPipeline *pipeline, uint32_t chunk) {
    return XboxPsoCacheGet32(pipeline->chunks + 8 * chunk + 4);
}

/* The arena XboxPsoReplayBuild needs, in bytes; the arena must be 8-aligned. */
static inline uint64_t XboxPsoReplayArenaSize(const XboxPsoReplayPipeline *pipeline) {
    uint64_t size = 0;
    for (uint32_t i = 0; i < pipeline->chunkCount; ++i) size += XboxPsoReplayAlign8(XboxPsoReplayChunkSize(pipeline, i));
    return size ? size : 8;
}

/* Rebuilds the desc graph in arena with live pointers; the root fixup gets
   root. Returns chunk 0. The pipeline must have passed ParsePipeline. */
static inline void *XboxPsoReplayBuild(const XboxPsoReplayPipeline *pipeline, uint8_t *arena, void *root) {
    uint32_t start[XBOX_PSO_REPLAY_MAX_CHUNKS];
    uint32_t at = 0;
    for (uint32_t i = 0; i < pipeline->chunkCount; ++i) {
        const uint32_t offset = XboxPsoCacheGet32(pipeline->chunks + 8 * i);
        const uint32_t size = XboxPsoCacheGet32(pipeline->chunks + 8 * i + 4);
        start[i] = at;
        if (size) XboxPsoCacheCopy(arena + at, pipeline->data + offset, size);
        XboxPsoCacheFill(arena + at + size, 0, XboxPsoReplayAlign8(size) - size);
        at += XboxPsoReplayAlign8(size);
    }
    for (uint32_t i = 0; i < pipeline->fixupCount; ++i) {
        const uint32_t chunk = XboxPsoCacheGet32(pipeline->fixups + 12 * i);
        const uint32_t offset = XboxPsoCacheGet32(pipeline->fixups + 12 * i + 4);
        const uint32_t target = XboxPsoCacheGet32(pipeline->fixups + 12 * i + 8);
        void *value = target == XBOX_PSO_REPLAY_FIXUP_NULL ? (void *)0 :
            target == XBOX_PSO_REPLAY_FIXUP_ROOT ? root : (void *)(arena + start[target]);
        /* Slots are 8-aligned in an 8-aligned arena. */
        *(void **)(void *)(arena + start[chunk] + offset) = value;
    }
    return arena;
}

/* 1 for a payload of well-formed root and pipeline entries within the
   bounds; *pipelines and *roots receive the counts. */
static inline int XboxPsoReplayValidatePayload(const uint8_t *payload, uint64_t size, uint32_t *pipelines,
        uint32_t *roots) {
    *pipelines = *roots = 0;
    if (size > XBOX_PSO_REPLAY_MAX_PAYLOAD) return 0;
    uint64_t offset = 0;
    uint32_t tag, bodySize;
    const uint8_t *body;
    int next;
    while ((next = XboxPsoReplayNextEntry(payload, size, &offset, &tag, &body, &bodySize)) > 0) {
        if (tag == XBOX_PSO_REPLAY_TAG_ROOT) {
            XboxPsoReplayRoot root;
            if (!XboxPsoReplayParseRoot(body, bodySize, &root) || ++*roots > XBOX_PSO_REPLAY_MAX_ROOTS) return 0;
        } else if (tag == XBOX_PSO_REPLAY_TAG_PIPELINE) {
            XboxPsoReplayPipeline pipeline;
            if (!XboxPsoReplayParsePipeline(body, bodySize, &pipeline) || ++*pipelines > XBOX_PSO_REPLAY_MAX_PIPELINES)
                return 0;
        } else {
            return 0;
        }
    }
    return next == 0;
}

/* ---------------------------------------------------------------- policy */

/* Replay order: the order the title created them in. Loading phase first
   (records from different launches keep their coarse place: the startup
   burst, the menu, the first combat), then time since the title's first
   creation, then sequence. The replay starts at the title's first root
   signature, so it runs ahead of the title through the startup burst: while
   Mesa translates the next shader on the title's thread, the driver compiles
   it on the replay's, and the title waits at most for the rest of a creation
   already under way. Later phases follow well before they are reached. */
static inline int XboxPsoReplayBefore(const XboxPsoReplayMeta *a, const XboxPsoReplayMeta *b) {
    if (a->phase != b->phase) return a->phase < b->phase;
    if (a->atMs != b->atMs) return a->atMs < b->atMs;
    return a->order < b->order;
}

/* Sorts indices by XboxPsoReplayBefore (insertion sort: at most
   MAX_PIPELINES, once per launch, on a background thread). */
static inline void XboxPsoReplaySort(uint32_t *indices, uint32_t count, const XboxPsoReplayMeta *metas) {
    for (uint32_t i = 1; i < count; ++i) {
        const uint32_t value = indices[i];
        uint32_t j = i;
        while (j && XboxPsoReplayBefore(&metas[value], &metas[indices[j - 1]])) {
            indices[j] = indices[j - 1];
            --j;
        }
        indices[j] = value;
    }
}

/* The age a record is written with, or -1 when it is dropped. */
static inline int XboxPsoReplayNextAge(uint32_t age, int requested) {
    if (requested) return 0;
    return age + 1 >= XBOX_PSO_REPLAY_MAX_AGE ? -1 : (int)(age + 1);
}

/* Whether a creation starts a new loading phase. */
static inline int XboxPsoReplayNewPhase(uint64_t msSinceLastCreation, int first) {
    return !first && msSinceLastCreation >= XBOX_PSO_REPLAY_IDLE_MS;
}
