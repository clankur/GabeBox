/* Persistent pipeline-cache file format and keys, shared by the Direct3D 12
   pipeline cache adapter and its host-side unit tests.

   This header is platform-neutral C (also valid C++) with no C runtime calls,
   so the CRT-free Win32 adapters and a hosted g++ test can both include it.

   File layout, all integers little-endian:

     0   magic "XPSOCACH"
     8   u32 format version (1)
     12  u32 mode: 1 = ID3D12PipelineLibrary::Serialize output,
                   2 = records of ID3D12PipelineState::GetCachedBlob output
     16  identity[32]: SHA-256 of the app, game build, driver and OS identity
     48  u64 payload size
     56  payload[32]: SHA-256 of the payload
     88  u32 pipeline count (informational)
     92  u32 reserved, zero
     96  payload

   A mode-2 payload is a sequence of records: key[32], u32 kind, u32 size,
   size bytes of cached blob, then zero padding to an 8-byte boundary. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define XBOX_PSO_CACHE_VERSION 1u
#define XBOX_PSO_CACHE_HEADER_SIZE 96u
#define XBOX_PSO_CACHE_MODE_LIBRARY 1u
#define XBOX_PSO_CACHE_MODE_BLOBS 2u
/* A bounded cache: a full one stops accepting new pipelines. Single records
   are not limited below the total, so a pathological multi-second pipeline
   with a large compiled blob is kept like any other. */
#define XBOX_PSO_CACHE_MAX_PAYLOAD (256ull * 1024 * 1024)
#define XBOX_PSO_CACHE_MAX_RECORD 0xFFFFFFF8u
#define XBOX_PSO_CACHE_RECORD_HEADER 40u

/* Key version: bump when the desc serialization changes, so old keys miss. */
#define XBOX_PSO_CACHE_KEY_VERSION 1u

typedef enum XboxPsoCacheStatus {
    XBOX_PSO_CACHE_OK = 0,
    XBOX_PSO_CACHE_TOO_SMALL,
    XBOX_PSO_CACHE_BAD_MAGIC,
    XBOX_PSO_CACHE_BAD_VERSION,
    XBOX_PSO_CACHE_BAD_MODE,
    XBOX_PSO_CACHE_IDENTITY_MISMATCH,
    XBOX_PSO_CACHE_SIZE_MISMATCH,
    XBOX_PSO_CACHE_TOO_LARGE,
    XBOX_PSO_CACHE_HASH_MISMATCH,
    XBOX_PSO_CACHE_BAD_RECORDS,
} XboxPsoCacheStatus;

static inline const char *XboxPsoCacheStatusName(XboxPsoCacheStatus status) {
    switch (status) {
    case XBOX_PSO_CACHE_OK: return "valid";
    case XBOX_PSO_CACHE_TOO_SMALL: return "truncated";
    case XBOX_PSO_CACHE_BAD_MAGIC: return "bad-magic";
    case XBOX_PSO_CACHE_BAD_VERSION: return "old-version";
    case XBOX_PSO_CACHE_BAD_MODE: return "bad-mode";
    case XBOX_PSO_CACHE_IDENTITY_MISMATCH: return "identity-mismatch";
    case XBOX_PSO_CACHE_SIZE_MISMATCH: return "size-mismatch";
    case XBOX_PSO_CACHE_TOO_LARGE: return "too-large";
    case XBOX_PSO_CACHE_HASH_MISMATCH: return "checksum-mismatch";
    case XBOX_PSO_CACHE_BAD_RECORDS: return "bad-records";
    }
    return "unknown";
}

/* Byte copies and fills through volatile stores. The CRT-free adapters must
   not let the compiler turn these loops into memcpy or memset calls. */
static inline void XboxPsoCacheCopy(uint8_t *out, const uint8_t *in, size_t size) {
    volatile uint8_t *target = out;
    for (size_t i = 0; i < size; ++i) target[i] = in[i];
}

static inline void XboxPsoCacheFill(uint8_t *out, uint8_t value, size_t size) {
    volatile uint8_t *target = out;
    for (size_t i = 0; i < size; ++i) target[i] = value;
}

/* SHA-256 (FIPS 180-4). */
typedef struct XboxSha256 {
    uint32_t state[8];
    uint64_t length;
    uint8_t block[64];
    uint32_t used;
} XboxSha256;

static inline uint32_t XboxSha256Rotate(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}

static inline void XboxSha256Block(XboxSha256 *hash, const uint8_t *block) {
    static const uint32_t constants[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    uint32_t words[64];
    for (unsigned i = 0; i < 16; ++i)
        words[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
                   (uint32_t)block[i * 4 + 2] << 8 | (uint32_t)block[i * 4 + 3];
    for (unsigned i = 16; i < 64; ++i) {
        const uint32_t s0 = XboxSha256Rotate(words[i - 15], 7) ^ XboxSha256Rotate(words[i - 15], 18) ^ (words[i - 15] >> 3);
        const uint32_t s1 = XboxSha256Rotate(words[i - 2], 17) ^ XboxSha256Rotate(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    uint32_t a = hash->state[0], b = hash->state[1], c = hash->state[2], d = hash->state[3];
    uint32_t e = hash->state[4], f = hash->state[5], g = hash->state[6], h = hash->state[7];
    for (unsigned i = 0; i < 64; ++i) {
        const uint32_t s1 = XboxSha256Rotate(e, 6) ^ XboxSha256Rotate(e, 11) ^ XboxSha256Rotate(e, 25);
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t first = h + s1 + choose + constants[i] + words[i];
        const uint32_t s0 = XboxSha256Rotate(a, 2) ^ XboxSha256Rotate(a, 13) ^ XboxSha256Rotate(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t second = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    hash->state[0] += a;
    hash->state[1] += b;
    hash->state[2] += c;
    hash->state[3] += d;
    hash->state[4] += e;
    hash->state[5] += f;
    hash->state[6] += g;
    hash->state[7] += h;
}

static inline void XboxSha256Init(XboxSha256 *hash) {
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    for (unsigned i = 0; i < 8; ++i) hash->state[i] = initial[i];
    hash->length = 0;
    hash->used = 0;
}

static inline void XboxSha256Update(XboxSha256 *hash, const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    hash->length += size;
    if (hash->used) {
        const size_t take = size < 64 - hash->used ? size : 64 - hash->used;
        XboxPsoCacheCopy(hash->block + hash->used, bytes, take);
        hash->used += (uint32_t)take;
        bytes += take;
        size -= take;
        if (hash->used < 64) return;
        XboxSha256Block(hash, hash->block);
        hash->used = 0;
    }
    for (; size >= 64; size -= 64, bytes += 64) XboxSha256Block(hash, bytes);
    XboxPsoCacheCopy(hash->block, bytes, size);
    hash->used = (uint32_t)size;
}

static inline void XboxSha256Final(XboxSha256 *hash, uint8_t digest[32]) {
    const uint64_t bits = hash->length * 8;
    hash->block[hash->used++] = 0x80;
    if (hash->used > 56) {
        XboxPsoCacheFill(hash->block + hash->used, 0, 64 - hash->used);
        XboxSha256Block(hash, hash->block);
        hash->used = 0;
    }
    XboxPsoCacheFill(hash->block + hash->used, 0, 56 - hash->used);
    for (unsigned i = 0; i < 8; ++i) hash->block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    XboxSha256Block(hash, hash->block);
    for (unsigned i = 0; i < 8; ++i) {
        digest[i * 4] = (uint8_t)(hash->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(hash->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(hash->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)hash->state[i];
    }
}

static inline void XboxSha256Digest(const void *data, size_t size, uint8_t digest[32]) {
    XboxSha256 hash;
    XboxSha256Init(&hash);
    XboxSha256Update(&hash, data, size);
    XboxSha256Final(&hash, digest);
}

/* Desc serialization helpers. A field is hashed by its bytes; pointed-to
   content is hashed with its length first, so adjacent variable-length
   fields cannot alias, and a null pointer differs from an empty one. */
static inline void XboxPsoKeyU32(XboxSha256 *hash, uint32_t value) {
    uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    XboxSha256Update(hash, bytes, 4);
}

static inline void XboxPsoKeyU64(XboxSha256 *hash, uint64_t value) {
    XboxPsoKeyU32(hash, (uint32_t)value);
    XboxPsoKeyU32(hash, (uint32_t)(value >> 32));
}

static inline void XboxPsoKeyBytes(XboxSha256 *hash, const void *data, uint64_t size) {
    XboxPsoKeyU32(hash, data ? 1u : 0u);
    XboxPsoKeyU64(hash, size);
    if (data && size) XboxSha256Update(hash, data, (size_t)size);
}

static inline void XboxPsoKeyString(XboxSha256 *hash, const char *text) {
    size_t length = 0;
    if (text) {
        while (text[length] && length < 4096) ++length;
    }
    XboxPsoKeyBytes(hash, text, length);
}

static inline void XboxPsoCachePut32(uint8_t *out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (8 * i));
}

static inline void XboxPsoCachePut64(uint8_t *out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out[i] = (uint8_t)(value >> (8 * i));
}

static inline uint32_t XboxPsoCacheGet32(const uint8_t *in) {
    return (uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
}

static inline uint64_t XboxPsoCacheGet64(const uint8_t *in) {
    return (uint64_t)XboxPsoCacheGet32(in) | (uint64_t)XboxPsoCacheGet32(in + 4) << 32;
}

static inline int XboxPsoCacheSameBytes(const uint8_t *left, const uint8_t *right, size_t size) {
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) difference |= (uint8_t)(left[i] ^ right[i]);
    return difference == 0;
}

/* Fills the header in front of a payload that the caller has already placed
   at out + XBOX_PSO_CACHE_HEADER_SIZE. */
static inline void XboxPsoCacheWriteHeader(uint8_t *out, uint32_t mode, const uint8_t identity[32],
        uint64_t payloadSize, uint32_t entries) {
    static const char magic[8] = {'X', 'P', 'S', 'O', 'C', 'A', 'C', 'H'};
    XboxPsoCacheCopy(out, (const uint8_t *)magic, 8);
    XboxPsoCachePut32(out + 8, XBOX_PSO_CACHE_VERSION);
    XboxPsoCachePut32(out + 12, mode);
    XboxPsoCacheCopy(out + 16, identity, 32);
    XboxPsoCachePut64(out + 48, payloadSize);
    XboxSha256Digest(out + XBOX_PSO_CACHE_HEADER_SIZE, (size_t)payloadSize, out + 56);
    XboxPsoCachePut32(out + 88, entries);
    XboxPsoCachePut32(out + 92, 0);
}

static inline uint64_t XboxPsoCacheRecordSize(uint32_t blobSize) {
    return (XBOX_PSO_CACHE_RECORD_HEADER + (uint64_t)blobSize + 7u) & ~(uint64_t)7u;
}

/* Writes one record and returns its padded size. */
static inline uint64_t XboxPsoCacheWriteRecord(uint8_t *out, const uint8_t key[32], uint32_t kind,
        const void *blob, uint32_t blobSize) {
    const uint8_t *bytes = (const uint8_t *)blob;
    const uint64_t total = XboxPsoCacheRecordSize(blobSize);
    XboxPsoCacheCopy(out, key, 32);
    XboxPsoCachePut32(out + 32, kind);
    XboxPsoCachePut32(out + 36, blobSize);
    XboxPsoCacheCopy(out + XBOX_PSO_CACHE_RECORD_HEADER, bytes, blobSize);
    XboxPsoCacheFill(out + XBOX_PSO_CACHE_RECORD_HEADER + blobSize, 0,
        (size_t)(total - XBOX_PSO_CACHE_RECORD_HEADER - blobSize));
    return total;
}

typedef struct XboxPsoCacheRecord {
    const uint8_t *key;
    uint32_t kind;
    uint32_t size;
    const uint8_t *blob;
} XboxPsoCacheRecord;

/* Reads the record at *offset. Returns 1 and advances, 0 at the exact end,
   and -1 for a malformed payload. */
static inline int XboxPsoCacheNextRecord(const uint8_t *payload, uint64_t size, uint64_t *offset,
        XboxPsoCacheRecord *record) {
    if (*offset == size) return 0;
    if (*offset > size || size - *offset < XBOX_PSO_CACHE_RECORD_HEADER) return -1;
    const uint8_t *start = payload + *offset;
    const uint32_t blobSize = XboxPsoCacheGet32(start + 36);
    const uint64_t total = XboxPsoCacheRecordSize(blobSize);
    if (!blobSize || blobSize > XBOX_PSO_CACHE_MAX_RECORD || total > size - *offset) return -1;
    for (uint64_t i = XBOX_PSO_CACHE_RECORD_HEADER + blobSize; i < total; ++i)
        if (start[i]) return -1;
    record->key = start;
    record->kind = XboxPsoCacheGet32(start + 32);
    record->size = blobSize;
    record->blob = start + XBOX_PSO_CACHE_RECORD_HEADER;
    *offset += total;
    return 1;
}

typedef struct XboxPsoCacheView {
    uint32_t mode;
    uint32_t entries;
    const uint8_t *payload;
    uint64_t payloadSize;
} XboxPsoCacheView;

/* Validates a whole cache file against the expected identity. Any failure
   means the file must be ignored and replaced. */
static inline XboxPsoCacheStatus XboxPsoCacheValidate(const uint8_t *file, uint64_t size,
        const uint8_t identity[32], XboxPsoCacheView *view) {
    static const uint8_t magic[8] = {'X', 'P', 'S', 'O', 'C', 'A', 'C', 'H'};
    if (!file || size < XBOX_PSO_CACHE_HEADER_SIZE) return XBOX_PSO_CACHE_TOO_SMALL;
    if (!XboxPsoCacheSameBytes(file, magic, 8)) return XBOX_PSO_CACHE_BAD_MAGIC;
    if (XboxPsoCacheGet32(file + 8) != XBOX_PSO_CACHE_VERSION) return XBOX_PSO_CACHE_BAD_VERSION;
    const uint32_t mode = XboxPsoCacheGet32(file + 12);
    if ((mode != XBOX_PSO_CACHE_MODE_LIBRARY && mode != XBOX_PSO_CACHE_MODE_BLOBS) || XboxPsoCacheGet32(file + 92))
        return XBOX_PSO_CACHE_BAD_MODE;
    if (!XboxPsoCacheSameBytes(file + 16, identity, 32)) return XBOX_PSO_CACHE_IDENTITY_MISMATCH;
    const uint64_t payloadSize = XboxPsoCacheGet64(file + 48);
    if (payloadSize > XBOX_PSO_CACHE_MAX_PAYLOAD) return XBOX_PSO_CACHE_TOO_LARGE;
    if (payloadSize != size - XBOX_PSO_CACHE_HEADER_SIZE || !payloadSize) return XBOX_PSO_CACHE_SIZE_MISMATCH;
    uint8_t digest[32];
    XboxSha256Digest(file + XBOX_PSO_CACHE_HEADER_SIZE, (size_t)payloadSize, digest);
    if (!XboxPsoCacheSameBytes(digest, file + 56, 32)) return XBOX_PSO_CACHE_HASH_MISMATCH;
    const uint8_t *payload = file + XBOX_PSO_CACHE_HEADER_SIZE;
    if (mode == XBOX_PSO_CACHE_MODE_BLOBS) {
        uint64_t offset = 0;
        XboxPsoCacheRecord record;
        int next;
        while ((next = XboxPsoCacheNextRecord(payload, payloadSize, &offset, &record)) > 0) {}
        if (next < 0) return XBOX_PSO_CACHE_BAD_RECORDS;
    }
    view->mode = mode;
    view->entries = XboxPsoCacheGet32(file + 88);
    view->payload = payload;
    view->payloadSize = payloadSize;
    return XBOX_PSO_CACHE_OK;
}

/* Lowercase hex, NUL-terminated; out holds 2 * size + 1 characters. Pipeline
   library names are UTF-16, so a 16-bit variant fills the WCHAR name. */
static inline void XboxPsoCacheHex(const uint8_t *bytes, size_t size, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * size] = 0;
}

static inline void XboxPsoCacheHexWide(const uint8_t *bytes, size_t size, uint16_t *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        out[2 * i] = (uint16_t)digits[bytes[i] >> 4];
        out[2 * i + 1] = (uint16_t)digits[bytes[i] & 15];
    }
    out[2 * size] = 0;
}

/* Profile selection. The marker steam-win32-d3d12-pipeline-cache-APPID.txt is
   empty for passthrough (timing only: no cache call reaches the device), or one
   line naming a mode and the console control result that proved it:

     mode=library control=0x5CC0LBFF
     mode=blobs control=0x5CC0LBFF

   The control code must report outcome 1 (stored, reloaded in a second
   process and hit) for the selected path: L for the library, B for cached
   blobs. Returns the mode (0, 1 or 2), or -1 for a malformed marker. */
#define XBOX_PSO_CACHE_MODE_PASSTHROUGH 0u

static inline int XboxPsoCacheParseMarker(const char *text, size_t size, uint32_t *control) {
    if (control) *control = 0;
    if (!size) return (int)XBOX_PSO_CACHE_MODE_PASSTHROUGH;
    if (size >= 2 && text[size - 2] == '\r' && text[size - 1] == '\n') size -= 2;
    else if (text[size - 1] == '\n') size -= 1;
    static const char library[] = "mode=library control=0x";
    static const char blobs[] = "mode=blobs control=0x";
    const char *prefix = NULL;
    int mode = -1;
    if (size == sizeof(library) - 1 + 8) {
        prefix = library;
        mode = (int)XBOX_PSO_CACHE_MODE_LIBRARY;
    } else if (size == sizeof(blobs) - 1 + 8) {
        prefix = blobs;
        mode = (int)XBOX_PSO_CACHE_MODE_BLOBS;
    } else {
        return -1;
    }
    size_t at = 0;
    for (; prefix[at]; ++at)
        if (text[at] != prefix[at]) return -1;
    uint32_t code = 0;
    for (size_t i = 0; i < 8; ++i) {
        const char c = text[at + i];
        const uint32_t digit = c >= '0' && c <= '9' ? (uint32_t)(c - '0') : c >= 'A' && c <= 'F' ?
            (uint32_t)(c - 'A' + 10) : 16u;
        if (digit > 15) return -1;
        code = code << 4 | digit;
    }
    if ((code & 0xFFF00000u) != 0x5CC00000u) return -1;
    const uint32_t outcome = mode == (int)XBOX_PSO_CACHE_MODE_LIBRARY ? (code >> 12) & 15 : (code >> 8) & 15;
    if (outcome != 1) return -1;
    if (control) *control = code;
    return mode;
}

static inline const char *XboxPsoCacheModeName(uint32_t mode) {
    return mode == XBOX_PSO_CACHE_MODE_LIBRARY ? "library" : mode == XBOX_PSO_CACHE_MODE_BLOBS ? "blobs" :
        "passthrough";
}

/* Every call the cache makes into the D3D12 runtime on the title's device or
   its objects. A device-removed check follows each one, and the ID of the call
   that preceded a removal is recorded in the trace, the guard file and the
   control's exit code. */
typedef enum XboxPsoCacheCall {
    XBOX_PSO_CALL_NONE = 0,
    XBOX_PSO_CALL_ADAPTER_LUID = 1,     /* ID3D12Device::GetAdapterLuid */
    XBOX_PSO_CALL_CHECK_SUPPORT = 2,    /* CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE) */
    XBOX_PSO_CALL_QUERY_DEVICE1 = 3,    /* QueryInterface(ID3D12Device1) */
    XBOX_PSO_CALL_CREATE_LIBRARY = 4,   /* ID3D12Device1::CreatePipelineLibrary */
    XBOX_PSO_CALL_STORE = 5,            /* ID3D12PipelineLibrary::StorePipeline */
    XBOX_PSO_CALL_LOAD = 6,             /* ID3D12PipelineLibrary(1)::Load*Pipeline */
    XBOX_PSO_CALL_SERIALIZE = 7,        /* GetSerializedSize and Serialize */
    XBOX_PSO_CALL_CACHED_BLOB = 8,      /* ID3D12PipelineState::GetCachedBlob */
    XBOX_PSO_CALL_CACHED_PSO = 9,       /* pipeline creation with a CachedPSO blob */
    XBOX_PSO_CALL_QUERY_LIBRARY1 = 10,  /* QueryInterface(ID3D12PipelineLibrary1) */
    XBOX_PSO_CALL_GAME_CREATE = 14,     /* the title's own creation, unchanged */
    XBOX_PSO_CALL_UNKNOWN = 15,
} XboxPsoCacheCall;

static inline const char *XboxPsoCacheCallName(uint32_t call) {
    switch (call) {
    case XBOX_PSO_CALL_NONE: return "none";
    case XBOX_PSO_CALL_ADAPTER_LUID: return "adapter-luid";
    case XBOX_PSO_CALL_CHECK_SUPPORT: return "check-shader-cache";
    case XBOX_PSO_CALL_QUERY_DEVICE1: return "query-device1";
    case XBOX_PSO_CALL_CREATE_LIBRARY: return "create-library";
    case XBOX_PSO_CALL_STORE: return "store-pipeline";
    case XBOX_PSO_CALL_LOAD: return "load-pipeline";
    case XBOX_PSO_CALL_SERIALIZE: return "serialize-library";
    case XBOX_PSO_CALL_CACHED_BLOB: return "get-cached-blob";
    case XBOX_PSO_CALL_CACHED_PSO: return "create-cached-pso";
    case XBOX_PSO_CALL_QUERY_LIBRARY1: return "query-library1";
    case XBOX_PSO_CALL_GAME_CREATE: return "game-create";
    }
    return "unknown";
}

/* Guard file <prefix>.guard: written before the first call of each kind in a
   process and deleted once it returns with the device intact, so a process
   that dies inside a call leaves it behind. A guard for the same identity
   (driver, OS and build) selects passthrough on later launches:

     XPSOGUARD call=XX identity=HHHHHHHHHHHHHHHH name=NAME\r\n */
#define XBOX_PSO_CACHE_GUARD_MAX 80u

static inline size_t XboxPsoCacheFormatGuard(char *out, uint32_t call, const uint8_t identity[32]) {
    static const char digits[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const char *t = "XPSOGUARD call="; *t; ++t) out[used++] = *t;
    out[used++] = digits[(call >> 4) & 15];
    out[used++] = digits[call & 15];
    for (const char *t = " identity="; *t; ++t) out[used++] = *t;
    XboxPsoCacheHex(identity, 8, out + used);
    used += 16;
    for (const char *t = " name="; *t; ++t) out[used++] = *t;
    for (const char *t = XboxPsoCacheCallName(call); *t; ++t) out[used++] = *t;
    out[used++] = '\r';
    out[used++] = '\n';
    return used;
}

/* The call recorded in a guard (1..15), or 0 when malformed. *sameIdentity
   says whether it was written for this identity. */
static inline uint32_t XboxPsoCacheParseGuard(const char *text, size_t size, const uint8_t identity[32],
        int *sameIdentity) {
    char expected[XBOX_PSO_CACHE_GUARD_MAX];
    if (sameIdentity) *sameIdentity = 0;
    if (size < 44 || size > XBOX_PSO_CACHE_GUARD_MAX) return 0;
    for (size_t i = 0; i < 15; ++i)
        if (text[i] != "XPSOGUARD call="[i]) return 0;
    uint32_t call = 0;
    for (size_t i = 15; i < 17; ++i) {
        const char c = text[i];
        const uint32_t digit = c >= '0' && c <= '9' ? (uint32_t)(c - '0') : c >= 'A' && c <= 'F' ?
            (uint32_t)(c - 'A' + 10) : 16u;
        if (digit > 15) return 0;
        call = call << 4 | digit;
    }
    if (!call || call > 15) return 0;
    const size_t length = XboxPsoCacheFormatGuard(expected, call, identity);
    if (sameIdentity) *sameIdentity = length == size && XboxPsoCacheSameBytes((const uint8_t *)expected,
        (const uint8_t *)text, size);
    return call;
}

/* Control outcomes for one path (the L and B nibbles of 0x5CCXLBFF). A
   removal or a process that died is classified by its code: 7 to 10 name the
   removal reason, 11 a process that exited with any other code. */
#define XBOX_PSO_OUTCOME_HIT 1u
#define XBOX_PSO_OUTCOME_REMOVED 7u
#define XBOX_PSO_OUTCOME_HUNG 8u
#define XBOX_PSO_OUTCOME_RESET 9u
#define XBOX_PSO_OUTCOME_INTERNAL 10u
#define XBOX_PSO_OUTCOME_DIED 11u

static inline uint32_t XboxPsoCacheRemovalOutcome(uint32_t code) {
    switch (code) {
    case 0x887A0005u: return XBOX_PSO_OUTCOME_REMOVED;  /* DXGI_ERROR_DEVICE_REMOVED */
    case 0x887A0006u: return XBOX_PSO_OUTCOME_HUNG;     /* DXGI_ERROR_DEVICE_HUNG */
    case 0x887A0007u: return XBOX_PSO_OUTCOME_RESET;    /* DXGI_ERROR_DEVICE_RESET */
    case 0x887A0020u: return XBOX_PSO_OUTCOME_INTERNAL; /* DXGI_ERROR_DRIVER_INTERNAL_ERROR */
    }
    return XBOX_PSO_OUTCOME_DIED;
}

/* A CachedPSO subobject appended to a stream starts at the next 8-byte
   boundary, as every subobject does; returns its offset. */
static inline size_t XboxPsoCacheAppendOffset(size_t streamSize) {
    return (streamSize + 7) & ~(size_t)7;
}

/* Background serialization policy. Writing rewrites the whole file, so new
   pipelines are written once creation goes quiet, during a long burst only
   every so often, and always at exit (not after a device removal, which stops
   the cache). A pipeline whose
   creation took XBOX_PSO_CACHE_SLOW_US or longer is written immediately, so a
   crash or quit soon after a long compile still keeps it. */
#define XBOX_PSO_CACHE_IDLE_MS 3000u
#define XBOX_PSO_CACHE_BURST_COUNT 64u
#define XBOX_PSO_CACHE_BURST_MS 10000u
#define XBOX_PSO_CACHE_SLOW_US 500000u

static inline int XboxPsoCacheShouldWrite(uint32_t unsaved, uint64_t msSinceStore, uint64_t msSinceWrite, int final,
        int slowPending) {
    if (!unsaved) return 0;
    if (final || slowPending) return 1;
    if (msSinceStore >= XBOX_PSO_CACHE_IDLE_MS) return 1;
    return unsaved >= XBOX_PSO_CACHE_BURST_COUNT && msSinceWrite >= XBOX_PSO_CACHE_BURST_MS;
}
