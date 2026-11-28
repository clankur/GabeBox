#pragma once
/* Persistent D3D12 pipeline replay for hosted Windows titles.

   Engines that compile pipeline state objects (PSOs) on first use stall a frame for as long as
   the compile takes. On Windows the driver's disk cache makes the second run cheap. The Xbox
   D3D12 runtime has no usable cache. D3D12_FEATURE_SHADER_CACHE reports flags 0x63, but
   ID3D12PipelineState::GetCachedBlob and ID3D12Device1::CreatePipelineLibrary both return
   DXGI_ERROR_DEVICE_REMOVED (replay controls 1.0.0.308 and 1.0.0.311, and inside Spider-Man on
   1.0.340.1, where GetDeviceRemovedReason still read S_OK and the title's render thread faulted
   about 20 s later). So every run pays for every compile again, and this header never calls
   either method.

   This header follows the same idea as Fossilize in Proton: it records the full description of
   every pipeline the title creates and replays it on the next run. Each record holds the shader
   bytecode, the serialized root signature and a canonical form of the fixed-function state.
   They are appended to a per-title, per-account database file. On the next launch a
   low-priority thread compiles the recorded pipelines ahead of the title. A request whose
   canonical description, shader bytes and root signature object match a precompiled entry gets
   that object at once. A request that races an in-progress replay compile waits for it, and any
   other request compiles normally. No pipeline is compiled twice.

   The canonical form never contains pointers or structure padding, so it is stable across runs.
   The key is a 64-bit hash of it, and a hit also compares the full record and every shader byte.
   Root signatures are matched by object: replay compiles with the title's own root signature
   object for that blob, and a hit requires the request to name the same object. Each root
   signature the title creates is kept alive by one extra reference, so a pointer cannot be
   reused. When the runtime returns one object for identical blobs (checked once per run; true on
   the Series X dev kit), replay creates a recorded root signature itself instead of waiting for
   the title to create it.

   The database is bound to the main executable's TimeDateStamp, SizeOfImage and CheckSum. It is
   reset when they change, so a Steam update starts a new recording. It is read through a
   read-only file mapping so the recorded bytecode is not charged to process commit. Records are
   appended one write each, and a truncated tail from an interrupted run is cut on the next open.

   The host supplies the compile callback, so it keeps its own limits, such as a bound on
   concurrent compiles, and the trace sink. Every failure disables replay and leaves pipeline
   creation untouched.

   By default one thread replays the database. XboxPipelineReplaySetThreads starts more; they walk
   the entries in the same order and claim each one under the lock, so no entry compiles twice and
   the database order is kept within the number of threads. A host that runs several should also
   bound them with the admission hook, which is called on each replay thread with that thread's
   own enter and leave. */
#include <windows.h>
#include <d3d12.h>

static const IID xprRootSignatureIid = {0xC54A6B66, 0x72DF, 0x4EE8, {0x8B,0xE5,0xA9,0x46,0xA1,0x42,0x92,0x14}};
static const IID xprPipelineStateIid = {0x765A30F3, 0xF624, 0x4C6F, {0xA8,0x28,0xAC,0xE9,0x48,0x62,0x24,0x45}};

typedef HRESULT (*XboxPipelineCompileFn)(ID3D12Device *device, BOOL graphics, const void *desc, REFIID iid,
    void **object);
typedef void (*XboxPipelineTraceFn)(const char *line, DWORD size);
/* Optional: called with TRUE before the replay thread claims an entry and FALSE after its compile,
   so a host can admit replay compiles at a lower priority. The entry is claimed only once the
   host admits it, so a title request never waits on a replay compile that has not started. */
typedef void (*XboxPipelineSlotFn)(BOOL enter);

enum {
    XboxPipelineReplayMissUncached = 0, /* not representable (stream output, cached blob, unknown root) */
    XboxPipelineReplayMissNew = 1,      /* compiled by the caller and recorded */
    XboxPipelineReplayMissClaimed = 2,  /* recorded, not yet replayed: compiled by the caller */
    XboxPipelineReplayMissRoot = 3,     /* replayed with a different root signature object */
    XboxPipelineReplayHit = 4,          /* precompiled object handed out */
    XboxPipelineReplayHitWaited = 5,    /* handed out after waiting for the replay compile */
};

#define XPR_MAGIC 0x31525058u
#define XPR_VERSION 1u
#define XPR_TAG_SHADER 1u
#define XPR_TAG_ROOT 2u
#define XPR_TAG_GRAPHICS 3u
#define XPR_TAG_COMPUTE 4u
#define XPR_MAX_ENTRIES 8192u
#define XPR_MAX_SHADERS 16384u
#define XPR_MAX_ROOTS 4096u
#define XPR_MAX_FILE (512ull << 20)
#define XPR_MAX_WORDS 1024u
/* 1: trace duplicate root-signature objects and replay root misses (diagnostic builds only;
   play builds compile none of it). */
#ifndef XPR_ROOT_TRACE
#define XPR_ROOT_TRACE 0
#endif

typedef struct XprHeader {
    UINT32 magic, version, imageStamp, imageSize, imageChecksum, reserved;
} XprHeader;

typedef struct XprRecordHead {
    UINT32 tag, size; /* payload bytes; payloads are 8-byte aligned */
} XprRecordHead;

typedef struct XprShader {
    UINT64 hash;
    const BYTE *bytes;
    UINT32 length;
} XprShader;

typedef struct XprRoot {
    ID3D12RootSignature *object; /* the title's object; one reference is ours */
    UINT64 hash;
    BYTE *blob;
    UINT32 size;
    BOOL replayMade;              /* created by replay before the title asked for it */
} XprRoot;

typedef struct XprEntry {
    UINT64 key;
    const UINT32 *words;          /* canonical form, in the mapped file */
    UINT32 wordCount;
    BOOL graphics;
    volatile LONG state;          /* XprPending, XprCompiling, XprReady, XprDone */
    ID3D12PipelineState *object;  /* while ready: our reference */
    ID3D12RootSignature *root;    /* object it was compiled with */
    volatile LONG reserved;       /* a replay thread is admitting it; others skip it */
} XprEntry;

enum { XprPending = 0, XprCompiling = 1, XprReady = 2, XprDone = 3 };

typedef struct XprState {
    BOOL enabled;
    ID3D12Device *device;
    XboxPipelineCompileFn compile;
    XboxPipelineTraceFn trace;
    XboxPipelineSlotFn slot;
    HANDLE file;
    const BYTE *view;
    UINT64 viewSize;
    UINT64 fileSize;
    XprEntry *entries;
    UINT32 entryCount;
    UINT32 *entryTable;           /* open addressing on key, index + 1 */
    UINT32 entryTableMask;
    XprShader *shaders;
    UINT32 shaderCount;
    UINT32 *shaderTable;          /* open addressing on hash, index + 1 */
    UINT32 shaderTableMask;
    UINT64 *storedShaders;        /* hashes present in the file, open addressing */
    UINT32 storedShaderMask;
    UINT64 *storedKeys;           /* pipeline keys present in the file */
    UINT32 storedKeyMask;
    UINT32 storedKeyCount, storedShaderCount;
    XprRoot roots[XPR_MAX_ROOTS];
    UINT32 rootCount;
    XprShader storedRoots[XPR_MAX_ROOTS]; /* root blobs in the mapped file */
    UINT32 storedRootCount;
    volatile LONG rootDedup;      /* 0 unknown, 1 identical blobs return one object, 2 they do not */
    SRWLOCK lock;                 /* entries, roots */
    CONDITION_VARIABLE changed;
    CRITICAL_SECTION writer;
    volatile LONG hits, waitedHits, misses, recorded, uncached, rootMisses;
    volatile LONG replayed, replayFailed;
#if XPR_ROOT_TRACE
    volatile LONG rootDuplicates;  /* root objects registered for a blob that already had one */
#endif
    UINT32 threadCount;           /* replay threads to start, 1 unless the host sets more */
    volatile LONG threadsLive;    /* the last thread to finish writes the summary */
    volatile LONG64 compileTicks; /* summed over the replay threads */
    volatile LONG64 startedQpc;
    UINT64 commitBeforeKib;
    PVOID createRootOriginal;
    PVOID createGraphicsOriginal;
    PVOID createComputeOriginal;
} XprState;

static XprState xprState;

/* Hosts may link no C runtime, so the header copies and compares by itself. */
static void XprCopy(void *to, const void *from, SIZE_T size) {
    BYTE *out = (BYTE *)to;
    const BYTE *in = (const BYTE *)from;
    for (; size >= 8; size -= 8, out += 8, in += 8) *(UINT64 UNALIGNED *)out = *(const UINT64 UNALIGNED *)in;
    while (size--) *out++ = *in++;
}

static BOOL XprEqual(const void *left, const void *right, SIZE_T size) {
    const BYTE *a = (const BYTE *)left, *b = (const BYTE *)right;
    for (; size >= 8; size -= 8, a += 8, b += 8)
        if (*(const UINT64 UNALIGNED *)a != *(const UINT64 UNALIGNED *)b) return FALSE;
    while (size--) if (*a++ != *b++) return FALSE;
    return TRUE;
}

static inline char *XprText(char *out, const char *text) {
    while (*text) *out++ = *text++;
    return out;
}

static inline char *XprDec(char *out, unsigned long long value) {
    char digits[24];
    int count = 0;
    do digits[count++] = (char)('0' + value % 10); while (value /= 10);
    while (count) *out++ = digits[--count];
    return out;
}

static inline char *XprHex(char *out, unsigned long long value, int digits) {
    static const char alphabet[] = "0123456789ABCDEF";
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) *out++ = alphabet[(value >> shift) & 15];
    return out;
}

static inline void XprEmit(char *line, char *end) {
    end = XprText(end, "\r\n");
    if (xprState.trace) xprState.trace(line, (DWORD)(end - line));
}

static inline UINT64 XprRotate(UINT64 value, int bits) {
    return (value << bits) | (value >> (64 - bits));
}

static inline UINT64 XprMix(UINT64 value) {
    value ^= value >> 33;
    value *= 0xFF51AFD7ED558CCDull;
    value ^= value >> 33;
    value *= 0xC4CEB9FE1A85EC53ull;
    return value ^ (value >> 33);
}

/* A Murmur3-style 64-bit hash. Every hit is also confirmed byte for byte. */
static UINT64 XprHash(const void *data, SIZE_T size) {
    const BYTE *bytes = (const BYTE *)data;
    UINT64 hash = 0x9E3779B97F4A7C15ull ^ ((UINT64)size * 0x87C37B91114253D5ull);
    SIZE_T index = 0;
    for (; index + 8 <= size; index += 8) {
        UINT64 word;
        word = *(const UINT64 UNALIGNED *)(bytes + index);
        word *= 0x87C37B91114253D5ull;
        word = XprRotate(word, 31);
        word *= 0x4CF5AD432745937Full;
        hash ^= word;
        hash = XprRotate(hash, 27) * 5 + 0x52DCE729;
    }
    UINT64 tail = 0;
    for (int shift = 0; index < size; ++index, shift += 8) tail |= (UINT64)bytes[index] << shift;
    hash ^= XprRotate(tail * 0x87C37B91114253D5ull, 31) * 0x4CF5AD432745937Full;
    return XprMix(hash);
}

static inline UINT32 XprSlot(UINT64 key, UINT32 mask) {
    return (UINT32)(XprMix(key) & mask);
}

static BOOL XprSetAdd(UINT64 *set, UINT32 mask, UINT32 *count, UINT64 key) {
    if (!key) key = 1;
    if (*count >= mask / 2) return FALSE;
    for (UINT32 slot = XprSlot(key, mask);; slot = (slot + 1) & mask) {
        if (set[slot] == key) return FALSE;
        if (!set[slot]) {
            set[slot] = key;
            ++*count;
            return TRUE;
        }
    }
}

static BOOL XprSetHas(const UINT64 *set, UINT32 mask, UINT64 key) {
    if (!key) key = 1;
    for (UINT32 slot = XprSlot(key, mask);; slot = (slot + 1) & mask) {
        if (set[slot] == key) return TRUE;
        if (!set[slot]) return FALSE;
    }
}

static const XprShader *XprFindShader(UINT64 hash, UINT32 length) {
    if (!xprState.shaderTable) return NULL;
    for (UINT32 slot = XprSlot(hash, xprState.shaderTableMask);; slot = (slot + 1) & xprState.shaderTableMask) {
        const UINT32 index = xprState.shaderTable[slot];
        if (!index) return NULL;
        const XprShader *shader = &xprState.shaders[index - 1];
        if (shader->hash == hash && shader->length == length) return shader;
    }
}

#define XPR_ROOT_TWEAK 0x524F4F5452544F31ull /* roots share the stored-hash set with shaders */

/* Called with the lock held. */
static XprRoot *XprRootByObject(const ID3D12RootSignature *object) {
    for (UINT32 i = 0; i < xprState.rootCount; ++i)
        if (xprState.roots[i].object == object) return &xprState.roots[i];
    return NULL;
}

static XprRoot *XprRootByHash(UINT64 hash) {
    XprRoot *found = NULL;
    for (UINT32 i = 0; i < xprState.rootCount; ++i)
        if (xprState.roots[i].hash == hash) found = &xprState.roots[i];
    return found; /* the most recently created object with this blob */
}

typedef struct XprWords {
    UINT32 *words;
    UINT32 count;
    BOOL overflow;
} XprWords;

static inline void XprPut(XprWords *out, UINT32 value) {
    if (out->count < XPR_MAX_WORDS) out->words[out->count++] = value;
    else out->overflow = TRUE;
}

static inline void XprPut64(XprWords *out, UINT64 value) {
    XprPut(out, (UINT32)value);
    XprPut(out, (UINT32)(value >> 32));
}

static inline UINT32 XprFloat(float value) {
    UINT32 bits;
    XprCopy(&bits, &value, 4);
    return bits;
}

static void XprPutShader(XprWords *out, D3D12_SHADER_BYTECODE shader) {
    XprPut64(out, shader.BytecodeLength && shader.pShaderBytecode ?
        XprHash(shader.pShaderBytecode, shader.BytecodeLength) : 0);
    XprPut(out, (UINT32)shader.BytecodeLength);
}

static void XprPutStencil(XprWords *out, const D3D12_DEPTH_STENCILOP_DESC *face) {
    XprPut(out, face->StencilFailOp);
    XprPut(out, face->StencilDepthFailOp);
    XprPut(out, face->StencilPassOp);
    XprPut(out, face->StencilFunc);
}

/* Canonical words for a graphics description. FALSE when it cannot be replayed. */
static BOOL XprCanonicalGraphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, UINT64 rootHash, XprWords *out) {
    if (desc->StreamOutput.NumEntries || desc->StreamOutput.NumStrides || desc->CachedPSO.CachedBlobSizeInBytes ||
        desc->InputLayout.NumElements > 64) return FALSE;
    XprPut(out, 1);
    XprPut64(out, rootHash);
    XprPutShader(out, desc->VS);
    XprPutShader(out, desc->PS);
    XprPutShader(out, desc->DS);
    XprPutShader(out, desc->HS);
    XprPutShader(out, desc->GS);
    XprPut(out, desc->BlendState.AlphaToCoverageEnable);
    XprPut(out, desc->BlendState.IndependentBlendEnable);
    for (UINT i = 0; i < 8; ++i) {
        const D3D12_RENDER_TARGET_BLEND_DESC *target = &desc->BlendState.RenderTarget[i];
        XprPut(out, target->BlendEnable);
        XprPut(out, target->LogicOpEnable);
        XprPut(out, target->SrcBlend);
        XprPut(out, target->DestBlend);
        XprPut(out, target->BlendOp);
        XprPut(out, target->SrcBlendAlpha);
        XprPut(out, target->DestBlendAlpha);
        XprPut(out, target->BlendOpAlpha);
        XprPut(out, target->LogicOp);
        XprPut(out, target->RenderTargetWriteMask);
    }
    XprPut(out, desc->SampleMask);
    const D3D12_RASTERIZER_DESC *raster = &desc->RasterizerState;
    XprPut(out, raster->FillMode);
    XprPut(out, raster->CullMode);
    XprPut(out, raster->FrontCounterClockwise);
    XprPut(out, (UINT32)raster->DepthBias);
    XprPut(out, XprFloat(raster->DepthBiasClamp));
    XprPut(out, XprFloat(raster->SlopeScaledDepthBias));
    XprPut(out, raster->DepthClipEnable);
    XprPut(out, raster->MultisampleEnable);
    XprPut(out, raster->AntialiasedLineEnable);
    XprPut(out, raster->ForcedSampleCount);
    XprPut(out, raster->ConservativeRaster);
    const D3D12_DEPTH_STENCIL_DESC *depth = &desc->DepthStencilState;
    XprPut(out, depth->DepthEnable);
    XprPut(out, depth->DepthWriteMask);
    XprPut(out, depth->DepthFunc);
    XprPut(out, depth->StencilEnable);
    XprPut(out, depth->StencilReadMask);
    XprPut(out, depth->StencilWriteMask);
    XprPutStencil(out, &depth->FrontFace);
    XprPutStencil(out, &depth->BackFace);
    XprPut(out, desc->InputLayout.NumElements);
    for (UINT i = 0; i < desc->InputLayout.NumElements; ++i) {
        const D3D12_INPUT_ELEMENT_DESC *element = &desc->InputLayout.pInputElementDescs[i];
        if (!desc->InputLayout.pInputElementDescs) return FALSE;
        const char *name = element->SemanticName ? element->SemanticName : "";
        UINT32 length = 0;
        while (name[length] && length < 200) ++length;
        if (name[length]) return FALSE;
        const UINT32 words = (length + 4) / 4; /* includes the terminator */
        XprPut(out, words);
        for (UINT32 w = 0; w < words; ++w) {
            UINT32 packed = 0;
            for (UINT32 b = 0; b < 4; ++b) {
                const UINT32 at = w * 4 + b;
                if (at < length) packed |= (UINT32)(BYTE)name[at] << (b * 8);
            }
            XprPut(out, packed);
        }
        XprPut(out, element->SemanticIndex);
        XprPut(out, element->Format);
        XprPut(out, element->InputSlot);
        XprPut(out, element->AlignedByteOffset);
        XprPut(out, element->InputSlotClass);
        XprPut(out, element->InstanceDataStepRate);
    }
    XprPut(out, desc->IBStripCutValue);
    XprPut(out, desc->PrimitiveTopologyType);
    XprPut(out, desc->NumRenderTargets);
    for (UINT i = 0; i < 8; ++i) XprPut(out, desc->RTVFormats[i]);
    XprPut(out, desc->DSVFormat);
    XprPut(out, desc->SampleDesc.Count);
    XprPut(out, desc->SampleDesc.Quality);
    XprPut(out, desc->NodeMask);
    XprPut(out, desc->Flags);
    return !out->overflow;
}

static BOOL XprCanonicalCompute(const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, UINT64 rootHash, XprWords *out) {
    if (desc->CachedPSO.CachedBlobSizeInBytes) return FALSE;
    XprPut(out, 2);
    XprPut64(out, rootHash);
    XprPutShader(out, desc->CS);
    XprPut(out, desc->NodeMask);
    XprPut(out, desc->Flags);
    return !out->overflow;
}

typedef struct XprReader {
    const UINT32 *words;
    UINT32 count, at;
    BOOL bad;
} XprReader;

static inline UINT32 XprGet(XprReader *in) {
    if (in->at < in->count) return in->words[in->at++];
    in->bad = TRUE;
    return 0;
}

static inline UINT64 XprGet64(XprReader *in) {
    const UINT64 low = XprGet(in);
    return low | ((UINT64)XprGet(in) << 32);
}

static D3D12_SHADER_BYTECODE XprGetShader(XprReader *in) {
    const UINT64 hash = XprGet64(in);
    const UINT32 length = XprGet(in);
    D3D12_SHADER_BYTECODE shader = {NULL, 0};
    if (!length) return shader;
    const XprShader *found = XprFindShader(hash, length);
    if (!found) in->bad = TRUE;
    else shader = (D3D12_SHADER_BYTECODE){found->bytes, found->length};
    return shader;
}

static void XprGetStencil(XprReader *in, D3D12_DEPTH_STENCILOP_DESC *face) {
    face->StencilFailOp = (D3D12_STENCIL_OP)XprGet(in);
    face->StencilDepthFailOp = (D3D12_STENCIL_OP)XprGet(in);
    face->StencilPassOp = (D3D12_STENCIL_OP)XprGet(in);
    face->StencilFunc = (D3D12_COMPARISON_FUNC)XprGet(in);
}

/* Rebuilds a description from canonical words. Element names point into the words. */
static BOOL XprDecodeGraphics(XprReader *in, D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
        D3D12_INPUT_ELEMENT_DESC *elements, UINT64 *rootHash) {
    *desc = (__typeof__(*desc)){0};
    if (XprGet(in) != 1) return FALSE;
    *rootHash = XprGet64(in);
    desc->VS = XprGetShader(in);
    desc->PS = XprGetShader(in);
    desc->DS = XprGetShader(in);
    desc->HS = XprGetShader(in);
    desc->GS = XprGetShader(in);
    desc->BlendState.AlphaToCoverageEnable = (BOOL)XprGet(in);
    desc->BlendState.IndependentBlendEnable = (BOOL)XprGet(in);
    for (UINT i = 0; i < 8; ++i) {
        D3D12_RENDER_TARGET_BLEND_DESC *target = &desc->BlendState.RenderTarget[i];
        target->BlendEnable = (BOOL)XprGet(in);
        target->LogicOpEnable = (BOOL)XprGet(in);
        target->SrcBlend = (D3D12_BLEND)XprGet(in);
        target->DestBlend = (D3D12_BLEND)XprGet(in);
        target->BlendOp = (D3D12_BLEND_OP)XprGet(in);
        target->SrcBlendAlpha = (D3D12_BLEND)XprGet(in);
        target->DestBlendAlpha = (D3D12_BLEND)XprGet(in);
        target->BlendOpAlpha = (D3D12_BLEND_OP)XprGet(in);
        target->LogicOp = (D3D12_LOGIC_OP)XprGet(in);
        target->RenderTargetWriteMask = (UINT8)XprGet(in);
    }
    desc->SampleMask = XprGet(in);
    D3D12_RASTERIZER_DESC *raster = &desc->RasterizerState;
    raster->FillMode = (D3D12_FILL_MODE)XprGet(in);
    raster->CullMode = (D3D12_CULL_MODE)XprGet(in);
    raster->FrontCounterClockwise = (BOOL)XprGet(in);
    raster->DepthBias = (INT)XprGet(in);
    UINT32 bits = XprGet(in);
    XprCopy(&raster->DepthBiasClamp, &bits, 4);
    bits = XprGet(in);
    XprCopy(&raster->SlopeScaledDepthBias, &bits, 4);
    raster->DepthClipEnable = (BOOL)XprGet(in);
    raster->MultisampleEnable = (BOOL)XprGet(in);
    raster->AntialiasedLineEnable = (BOOL)XprGet(in);
    raster->ForcedSampleCount = XprGet(in);
    raster->ConservativeRaster = (D3D12_CONSERVATIVE_RASTERIZATION_MODE)XprGet(in);
    D3D12_DEPTH_STENCIL_DESC *depth = &desc->DepthStencilState;
    depth->DepthEnable = (BOOL)XprGet(in);
    depth->DepthWriteMask = (D3D12_DEPTH_WRITE_MASK)XprGet(in);
    depth->DepthFunc = (D3D12_COMPARISON_FUNC)XprGet(in);
    depth->StencilEnable = (BOOL)XprGet(in);
    depth->StencilReadMask = (UINT8)XprGet(in);
    depth->StencilWriteMask = (UINT8)XprGet(in);
    XprGetStencil(in, &depth->FrontFace);
    XprGetStencil(in, &depth->BackFace);
    const UINT32 count = XprGet(in);
    if (count > 64) return FALSE;
    for (UINT32 i = 0; i < count && !in->bad; ++i) {
        const UINT32 words = XprGet(in);
        if (!words || words > 64 || in->at + words > in->count) return FALSE;
        elements[i].SemanticName = (const char *)&in->words[in->at];
        if (((const char *)&in->words[in->at])[words * 4 - 1]) return FALSE;
        in->at += words;
        elements[i].SemanticIndex = XprGet(in);
        elements[i].Format = (DXGI_FORMAT)XprGet(in);
        elements[i].InputSlot = XprGet(in);
        elements[i].AlignedByteOffset = XprGet(in);
        elements[i].InputSlotClass = (D3D12_INPUT_CLASSIFICATION)XprGet(in);
        elements[i].InstanceDataStepRate = XprGet(in);
    }
    desc->InputLayout = (D3D12_INPUT_LAYOUT_DESC){count ? elements : NULL, count};
    desc->IBStripCutValue = (D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)XprGet(in);
    desc->PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)XprGet(in);
    desc->NumRenderTargets = XprGet(in);
    for (UINT i = 0; i < 8; ++i) desc->RTVFormats[i] = (DXGI_FORMAT)XprGet(in);
    desc->DSVFormat = (DXGI_FORMAT)XprGet(in);
    desc->SampleDesc.Count = XprGet(in);
    desc->SampleDesc.Quality = XprGet(in);
    desc->NodeMask = XprGet(in);
    desc->Flags = (D3D12_PIPELINE_STATE_FLAGS)XprGet(in);
    return !in->bad && in->at == in->count;
}

static BOOL XprDecodeCompute(XprReader *in, D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, UINT64 *rootHash) {
    *desc = (__typeof__(*desc)){0};
    if (XprGet(in) != 2) return FALSE;
    *rootHash = XprGet64(in);
    desc->CS = XprGetShader(in);
    desc->NodeMask = XprGet(in);
    desc->Flags = (D3D12_PIPELINE_STATE_FLAGS)XprGet(in);
    return !in->bad && in->at == in->count && desc->CS.BytecodeLength;
}

static BOOL XprSameShader(D3D12_SHADER_BYTECODE request, D3D12_SHADER_BYTECODE stored) {
    if (request.BytecodeLength != stored.BytecodeLength) return FALSE;
    if (!request.BytecodeLength) return TRUE;
    return request.pShaderBytecode && stored.pShaderBytecode &&
        XprEqual(request.pShaderBytecode, stored.pShaderBytecode, request.BytecodeLength);
}

static void XprImageIdentity(XprHeader *header) {
    const BYTE *image = (const BYTE *)GetModuleHandleW(NULL);
    header->magic = XPR_MAGIC;
    header->version = XPR_VERSION;
    header->imageStamp = header->imageSize = header->imageChecksum = header->reserved = 0;
    if (!image || ((const IMAGE_DOS_HEADER *)image)->e_magic != IMAGE_DOS_SIGNATURE) return;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(image + ((const IMAGE_DOS_HEADER *)image)->e_lfanew);
    header->imageStamp = nt->FileHeader.TimeDateStamp;
    header->imageSize = nt->OptionalHeader.SizeOfImage;
    header->imageChecksum = nt->OptionalHeader.CheckSum;
}

static BOOL XprWriteAll(const void *data, DWORD size) {
    LARGE_INTEGER zero = {0}, end;
    DWORD written = 0;
    if (!SetFilePointerEx(xprState.file, zero, &end, FILE_END)) return FALSE;
    if ((UINT64)end.QuadPart + size > XPR_MAX_FILE) return FALSE;
    return WriteFile(xprState.file, data, size, &written, NULL) && written == size;
}

/* One record per write, so an interrupted run leaves at most one truncated tail record. */
static BOOL XprAppend(UINT32 tag, UINT64 first, const void *payload, UINT32 size) {
    const UINT32 body = 8 + size;
    const UINT32 padded = (body + 7) & ~7u;
    BYTE *record = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(XprRecordHead) + padded);
    if (!record) return FALSE;
    const XprRecordHead head = {tag, padded};
    XprCopy(record, &head, sizeof(head));
    XprCopy(record + sizeof(head), &first, 8);
    if (size) XprCopy(record + sizeof(head) + 8, payload, size);
    const BOOL ok = XprWriteAll(record, (DWORD)(sizeof(head) + padded));
    HeapFree(GetProcessHeap(), 0, record);
    return ok;
}

static void XprStoreShader(D3D12_SHADER_BYTECODE shader) {
    if (!shader.BytecodeLength || !shader.pShaderBytecode) return;
    const UINT64 hash = XprHash(shader.pShaderBytecode, shader.BytecodeLength);
    if (XprSetHas(xprState.storedShaders, xprState.storedShaderMask, hash)) return;
    /* the payload carries its own length so padding can be stripped on load */
    const UINT32 length = (UINT32)shader.BytecodeLength;
    BYTE *payload = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 4 + length);
    if (!payload) return;
    XprCopy(payload, &length, 4);
    XprCopy(payload + 4, shader.pShaderBytecode, length);
    if (XprAppend(XPR_TAG_SHADER, hash, payload, 4 + length))
        XprSetAdd(xprState.storedShaders, xprState.storedShaderMask, &xprState.storedShaderCount, hash);
    HeapFree(GetProcessHeap(), 0, payload);
}

/* Records a compiled pipeline that the database does not hold. */
static void XprRecord(BOOL graphics, const void *desc, const XprWords *canonical, UINT64 key, XprRoot *root) {
    EnterCriticalSection(&xprState.writer);
    if (!XprSetHas(xprState.storedKeys, xprState.storedKeyMask, key) && xprState.storedKeyCount < XPR_MAX_ENTRIES) {
        BOOL ok = TRUE;
        if (!XprSetHas(xprState.storedShaders, xprState.storedShaderMask, root->hash ^ XPR_ROOT_TWEAK)) {
            BYTE *payload = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 4 + root->size);
            ok = payload != NULL;
            if (ok) {
                XprCopy(payload, &root->size, 4);
                XprCopy(payload + 4, root->blob, root->size);
                ok = XprAppend(XPR_TAG_ROOT, root->hash, payload, 4 + root->size);
                HeapFree(GetProcessHeap(), 0, payload);
            }
            if (ok) XprSetAdd(xprState.storedShaders, xprState.storedShaderMask, &xprState.storedShaderCount,
                root->hash ^ XPR_ROOT_TWEAK);
        }
        if (ok && graphics) {
            const D3D12_GRAPHICS_PIPELINE_STATE_DESC *g = (const D3D12_GRAPHICS_PIPELINE_STATE_DESC *)desc;
            XprStoreShader(g->VS);
            XprStoreShader(g->PS);
            XprStoreShader(g->DS);
            XprStoreShader(g->HS);
            XprStoreShader(g->GS);
        } else if (ok) {
            XprStoreShader(((const D3D12_COMPUTE_PIPELINE_STATE_DESC *)desc)->CS);
        }
        UINT32 *payload = ok ? (UINT32 *)HeapAlloc(GetProcessHeap(), 0, (1 + canonical->count) * 4) : NULL;
        if (payload) {
            payload[0] = canonical->count;
            XprCopy(payload + 1, canonical->words, canonical->count * 4);
            if (XprAppend(graphics ? XPR_TAG_GRAPHICS : XPR_TAG_COMPUTE, key, payload, (1 + canonical->count) * 4)) {
                XprSetAdd(xprState.storedKeys, xprState.storedKeyMask, &xprState.storedKeyCount, key);
                InterlockedIncrement(&xprState.recorded);
            }
            HeapFree(GetProcessHeap(), 0, payload);
        }
    }
    LeaveCriticalSection(&xprState.writer);
}

static XprEntry *XprFindEntry(UINT64 key, const XprWords *canonical) {
    if (!xprState.entryTable) return NULL;
    for (UINT32 slot = XprSlot(key, xprState.entryTableMask);; slot = (slot + 1) & xprState.entryTableMask) {
        const UINT32 index = xprState.entryTable[slot];
        if (!index) return NULL;
        XprEntry *entry = &xprState.entries[index - 1];
        if (entry->key == key && entry->wordCount == canonical->count &&
            XprEqual(entry->words, canonical->words, canonical->count * 4)) return entry;
    }
}

/* Confirms that the request's shader bytes equal the recorded ones. */
static BOOL XprShadersMatch(const XprEntry *entry, BOOL graphics, const void *desc) {
    XprReader in = {entry->words, entry->wordCount, 0, FALSE};
    if (graphics) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC stored;
        D3D12_INPUT_ELEMENT_DESC elements[64];
        UINT64 rootHash;
        if (!XprDecodeGraphics(&in, &stored, elements, &rootHash)) return FALSE;
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *g = (const D3D12_GRAPHICS_PIPELINE_STATE_DESC *)desc;
        return XprSameShader(g->VS, stored.VS) && XprSameShader(g->PS, stored.PS) &&
            XprSameShader(g->DS, stored.DS) && XprSameShader(g->HS, stored.HS) && XprSameShader(g->GS, stored.GS);
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC stored;
    UINT64 rootHash;
    return XprDecodeCompute(&in, &stored, &rootHash) &&
        XprSameShader(((const D3D12_COMPUTE_PIPELINE_STATE_DESC *)desc)->CS, stored.CS);
}

/* A request in progress. Lives on the caller's stack between lookup and finish. */
typedef struct XboxPipelineTicket {
    int outcome;          /* one of the XboxPipelineReplay* values */
    BOOL record;          /* finish records the compiled pipeline */
    BOOL graphics;
    UINT64 key;
    XprRoot *root;
    XprWords canonical;   /* words are heap storage, released by finish */
} XboxPipelineTicket;

/* Returns TRUE with *result set when a precompiled pipeline satisfied the request. Otherwise the
   caller compiles as usual and then calls XboxPipelineReplayFinish with the same ticket. */
static BOOL XboxPipelineReplayLookup(ID3D12Device *device, BOOL graphics, const void *desc, REFIID iid,
        void **object, XboxPipelineTicket *ticket, HRESULT *result) {
    ticket->outcome = XboxPipelineReplayMissUncached;
    ticket->record = FALSE;
    ticket->graphics = graphics;
    ticket->root = NULL;
    ticket->canonical = (XprWords){NULL, 0, FALSE}; /* finish frees it: set before any early return */
    if (!xprState.enabled || device != xprState.device || !object) return FALSE;
    ID3D12RootSignature *rootObject = graphics ?
        ((const D3D12_GRAPHICS_PIPELINE_STATE_DESC *)desc)->pRootSignature :
        ((const D3D12_COMPUTE_PIPELINE_STATE_DESC *)desc)->pRootSignature;
    AcquireSRWLockShared(&xprState.lock);
    XprRoot *root = XprRootByObject(rootObject);
    const UINT64 rootHash = root ? root->hash : 0;
    ReleaseSRWLockShared(&xprState.lock);
    ticket->root = root;
    ticket->canonical = (XprWords){NULL, 0, FALSE};
    if (root) ticket->canonical.words = (UINT32 *)HeapAlloc(GetProcessHeap(), 0, XPR_MAX_WORDS * 4);
    const BOOL representable = root && ticket->canonical.words && (graphics ?
        XprCanonicalGraphics((const D3D12_GRAPHICS_PIPELINE_STATE_DESC *)desc, rootHash, &ticket->canonical) :
        XprCanonicalCompute((const D3D12_COMPUTE_PIPELINE_STATE_DESC *)desc, rootHash, &ticket->canonical));
    if (!representable) {
        InterlockedIncrement(&xprState.uncached);
        return FALSE;
    }
    ticket->key = XprHash(ticket->canonical.words, ticket->canonical.count * 4);
    XprEntry *entry = XprFindEntry(ticket->key, &ticket->canonical);
    if (entry && !XprShadersMatch(entry, graphics, desc)) entry = NULL;
    InterlockedIncrement(&xprState.misses); /* undone below on a hit */
    if (!entry) {
        ticket->outcome = XboxPipelineReplayMissNew;
        ticket->record = TRUE;
        return FALSE;
    }
    BOOL waited = FALSE;
    AcquireSRWLockExclusive(&xprState.lock);
    while (entry->state == XprCompiling) {
        waited = TRUE;
        SleepConditionVariableSRW(&xprState.changed, &xprState.lock, INFINITE, 0);
    }
    ID3D12PipelineState *ready = NULL;
    if (entry->state == XprReady && entry->root == rootObject) {
        ready = entry->object;
        entry->object = NULL;
        entry->state = XprDone;
    } else if (entry->state == XprReady) {
        ticket->outcome = XboxPipelineReplayMissRoot;
#if XPR_ROOT_TRACE
        const LONG misses = InterlockedIncrement(&xprState.rootMisses);
        if (misses <= 16) {
            const XprRoot *compiled = XprRootByObject(entry->root);
            char line[160], *out = XprText(line, "asset pso-replay miss-root compiled-with=");
            out = XprText(out, compiled && compiled->replayMade ? "replay-root" : "title-root");
            out = XprDec(XprText(out, " root-index="), compiled ? (unsigned long long)(compiled - xprState.roots) : 9999);
            out = XprDec(XprText(out, " request-index="), root ? (unsigned long long)(root - xprState.roots) : 9999);
            out = XprDec(XprText(out, " roots="), xprState.rootCount);
            XprEmit(line, out);
        }
#else
        InterlockedIncrement(&xprState.rootMisses);
#endif
    } else {
        entry->state = XprDone; /* pending: the caller compiles it and replay skips it */
        ticket->outcome = XboxPipelineReplayMissClaimed;
    }
    ReleaseSRWLockExclusive(&xprState.lock);
    if (!ready) return FALSE;
    HeapFree(GetProcessHeap(), 0, ticket->canonical.words);
    ticket->canonical.words = NULL;
    *result = iid ? ID3D12PipelineState_QueryInterface(ready, iid, object) : S_FALSE;
    ID3D12PipelineState_Release(ready);
    if (FAILED(*result)) {
        ticket->outcome = XboxPipelineReplayMissClaimed;
        return FALSE;
    }
    InterlockedDecrement(&xprState.misses);
    ticket->outcome = waited ? XboxPipelineReplayHitWaited : XboxPipelineReplayHit;
    InterlockedIncrement(waited ? &xprState.waitedHits : &xprState.hits);
    return TRUE;
}

static void XboxPipelineReplayFinish(XboxPipelineTicket *ticket, const void *desc, HRESULT result) {
    if (ticket->record && SUCCEEDED(result))
        XprRecord(ticket->graphics, desc, &ticket->canonical, ticket->key, ticket->root);
    if (ticket->canonical.words) HeapFree(GetProcessHeap(), 0, ticket->canonical.words);
    ticket->canonical.words = NULL;
}

static HRESULT XboxPipelineReplayCreate(ID3D12Device *device, BOOL graphics, const void *desc, REFIID iid,
        void **object, XboxPipelineCompileFn compile, int *outcome) {
    XboxPipelineTicket ticket;
    HRESULT result;
    if (!XboxPipelineReplayLookup(device, graphics, desc, iid, object, &ticket, &result)) {
        result = compile(device, graphics, desc, iid, object);
        XboxPipelineReplayFinish(&ticket, desc, result);
    }
    *outcome = ticket.outcome;
    return result;
}

typedef HRESULT (STDMETHODCALLTYPE *XprCreateRootFn)(ID3D12Device *, UINT, const void *, SIZE_T, REFIID, void **);

/* Keeps one reference to a root signature and remembers its blob. Returns FALSE when the title
   received a different object for a blob replay had already created, which means the runtime does
   not share root signatures by content here; replay then stops creating them. */
static BOOL XprRegisterRoot(ID3D12RootSignature *root, const void *blob, SIZE_T size, BOOL replayMade) {
    const UINT64 hash = XprHash(blob, size);
    BOOL shared = TRUE;
    AcquireSRWLockExclusive(&xprState.lock);
    XprRoot *known = XprRootByObject(root);
    if (!known && !replayMade) {
        const XprRoot *made = XprRootByHash(hash);
        if (made && made->replayMade) shared = FALSE;
    }
#if XPR_ROOT_TRACE
    /* A second object for one blob: replay's pipelines for it can only match one of them. */
    const XprRoot *prior = known ? NULL : XprRootByHash(hash);
    if (prior && InterlockedIncrement(&xprState.rootDuplicates) <= 16) {
        char line[160], *out = XprText(line, "asset pso-replay root-dup by=");
        out = XprText(out, replayMade ? "replay" : "title");
        out = XprText(XprText(out, " prior="), prior->replayMade ? "replay" : "title");
        out = XprHex(XprText(out, " hash=0x"), (unsigned long)(hash >> 32), 8);
        out = XprHex(out, (unsigned long)hash, 8);
        out = XprDec(XprText(out, " roots="), xprState.rootCount);
        XprEmit(line, out);
    }
#endif
    if (!known && xprState.rootCount < XPR_MAX_ROOTS) {
        BYTE *copy = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size);
        if (copy) {
            XprCopy(copy, blob, size);
            ID3D12RootSignature_AddRef(root);
            xprState.roots[xprState.rootCount++] = (XprRoot){root, hash, copy, (UINT32)size, replayMade};
        }
    }
    WakeAllConditionVariable(&xprState.changed);
    ReleaseSRWLockExclusive(&xprState.lock);
    return shared;
}

static HRESULT STDMETHODCALLTYPE XprCreateRootSignature(ID3D12Device *device, UINT node, const void *blob,
        SIZE_T size, REFIID iid, void **object) {
    typedef XprCreateRootFn CreateRoot;
    const HRESULT result = ((CreateRoot)xprState.createRootOriginal)(device, node, blob, size, iid, object);
    const DWORD error = GetLastError();
    if (SUCCEEDED(result) && object && *object && blob && size && size <= (1u << 20) && iid &&
            XprEqual(iid, &xprRootSignatureIid, sizeof(IID))) {
        if (!XprRegisterRoot((ID3D12RootSignature *)*object, blob, size, FALSE) &&
                InterlockedExchange(&xprState.rootDedup, 2) != 2) {
            char line[80], *out = XprText(line, "asset pso-replay root-dedup same=0 reason=title-object-differs");
            XprEmit(line, out);
        }
        /* Once: does the runtime return the same object for an identical blob? Replay may then
           create a recorded root signature itself and still match the title's later requests. */
        if (!xprState.rootDedup && !InterlockedCompareExchange(&xprState.rootDedup, 3, 0)) {
            /* Ask with a copy of the blob at another address, as replay's own requests (from the
               mapped database) are. With the title's own pointer the Xbox runtime returned the same
               object, but replay-created roots still differed from the title's: 631 replay=3
               misses in run skip1. */
            void *again = NULL;
            BYTE *copy = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size);
            HRESULT second = E_OUTOFMEMORY;
            if (copy) {
                XprCopy(copy, blob, size);
                second = ((CreateRoot)xprState.createRootOriginal)(device, node, copy, size, iid, &again);
                HeapFree(GetProcessHeap(), 0, copy);
            }
            const LONG dedup = SUCCEEDED(second) && again == *object ? 1 : 2;
            if (again) ((IUnknown *)again)->lpVtbl->Release((IUnknown *)again);
            InterlockedExchange(&xprState.rootDedup, dedup);
            char line[120], *out = XprText(line, "asset pso-replay root-dedup same=");
            out = XprDec(out, dedup == 1);
            out = XprHex(XprText(out, " hr=0x"), (unsigned long)second, 8);
            XprEmit(line, out);
        }
    }
    SetLastError(error);
    return result;
}

static HRESULT XprCompileOriginal(ID3D12Device *device, BOOL graphics, const void *desc, REFIID iid, void **object) {
    typedef HRESULT (STDMETHODCALLTYPE *CreateGraphics)(ID3D12Device *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *,
        REFIID, void **);
    typedef HRESULT (STDMETHODCALLTYPE *CreateCompute)(ID3D12Device *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *,
        REFIID, void **);
    return graphics ?
        ((CreateGraphics)xprState.createGraphicsOriginal)(device, (const D3D12_GRAPHICS_PIPELINE_STATE_DESC *)desc,
            iid, object) :
        ((CreateCompute)xprState.createComputeOriginal)(device, (const D3D12_COMPUTE_PIPELINE_STATE_DESC *)desc,
            iid, object);
}

static HRESULT STDMETHODCALLTYPE XprCreateGraphics(ID3D12Device *device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
        REFIID iid, void **object) {
    int outcome;
    return XboxPipelineReplayCreate(device, TRUE, desc, iid, object, XprCompileOriginal, &outcome);
}

static HRESULT STDMETHODCALLTYPE XprCreateCompute(ID3D12Device *device, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
        REFIID iid, void **object) {
    int outcome;
    return XboxPipelineReplayCreate(device, FALSE, desc, iid, object, XprCompileOriginal, &outcome);
}

/* Walks the mapped records. With index set, loads them; returns the length of the valid prefix. */
static UINT64 XprIndex(BOOL index) {
    const BYTE *view = xprState.view;
    UINT64 at = sizeof(XprHeader);
    while (at + sizeof(XprRecordHead) + 8 <= xprState.viewSize) {
        XprRecordHead head;
        XprCopy(&head, view + at, sizeof(head));
        if (head.size < 12 || (head.size & 7) || at + sizeof(head) + head.size > xprState.viewSize) break;
        const BYTE *payload = view + at + sizeof(head);
        UINT64 first;
        UINT32 length;
        XprCopy(&first, payload, 8);
        XprCopy(&length, payload + 8, 4);
        if (head.tag == XPR_TAG_SHADER || head.tag == XPR_TAG_ROOT) {
            if (length > head.size - 12) break;
            if (!index) {
            } else if (head.tag == XPR_TAG_SHADER) {
                if (xprState.shaderCount < XPR_MAX_SHADERS &&
                        XprSetAdd(xprState.storedShaders, xprState.storedShaderMask, &xprState.storedShaderCount, first))
                    xprState.shaders[xprState.shaderCount++] = (XprShader){first, payload + 12, length};
            } else if (XprSetAdd(xprState.storedShaders, xprState.storedShaderMask, &xprState.storedShaderCount,
                    first ^ XPR_ROOT_TWEAK) && xprState.storedRootCount < XPR_MAX_ROOTS) {
                xprState.storedRoots[xprState.storedRootCount++] = (XprShader){first, payload + 12, length};
            }
        } else if (head.tag == XPR_TAG_GRAPHICS || head.tag == XPR_TAG_COMPUTE) {
            if (!length || length > XPR_MAX_WORDS || (UINT64)length * 4 > head.size - 12) break;
            const UINT32 *words = (const UINT32 *)(payload + 12);
            if (index && xprState.entryCount < XPR_MAX_ENTRIES && XprHash(words, (SIZE_T)length * 4) == first &&
                    XprSetAdd(xprState.storedKeys, xprState.storedKeyMask, &xprState.storedKeyCount, first)) {
                xprState.entries[xprState.entryCount++] = (XprEntry){first, words, length,
                    head.tag == XPR_TAG_GRAPHICS, XprPending, NULL, NULL};
            }
        } else {
            break;
        }
        at += sizeof(head) + head.size;
    }
    if (!index) return at;
    UINT32 size = 64;
    while (size < xprState.entryCount * 2 + 2) size <<= 1;
    xprState.entryTable = (UINT32 *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size * sizeof(UINT32));
    if (xprState.entryTable) {
        xprState.entryTableMask = size - 1;
        for (UINT32 i = 0; i < xprState.entryCount; ++i) {
            UINT32 slot = XprSlot(xprState.entries[i].key, xprState.entryTableMask);
            while (xprState.entryTable[slot]) slot = (slot + 1) & xprState.entryTableMask;
            xprState.entryTable[slot] = i + 1;
        }
    }
    size = 64;
    while (size < xprState.shaderCount * 2 + 2) size <<= 1;
    xprState.shaderTable = (UINT32 *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size * sizeof(UINT32));
    if (xprState.shaderTable) {
        xprState.shaderTableMask = size - 1;
        for (UINT32 i = 0; i < xprState.shaderCount; ++i) {
            UINT32 slot = XprSlot(xprState.shaders[i].hash, xprState.shaderTableMask);
            while (xprState.shaderTable[slot]) slot = (slot + 1) & xprState.shaderTableMask;
            xprState.shaderTable[slot] = i + 1;
        }
    }
    return at;
}

/* Resolved at run time, like the CPU-set functions, so a missing export cannot fail the load. */
static FARPROC XprApi(const char *name) {
    HMODULE module = GetModuleHandleW(L"kernelbase.dll");
    FARPROC found = module ? GetProcAddress(module, name) : NULL;
    if (!found && (module = GetModuleHandleW(L"kernel32.dll")) != NULL) found = GetProcAddress(module, name);
    return found;
}

static BOOL xprViewHeap; /* the view is a heap copy, charged to commit */

static const BYTE *XprMap(UINT64 size) {
    typedef HANDLE (WINAPI *CreateMapping)(HANDLE, PSECURITY_ATTRIBUTES, ULONG, ULONG64, PCWSTR);
    typedef PVOID (WINAPI *MapView)(HANDLE, ULONG, ULONG64, SIZE_T);
    const CreateMapping createMapping = (CreateMapping)(void *)XprApi("CreateFileMappingFromApp");
    const MapView mapView = (MapView)(void *)XprApi("MapViewOfFileFromApp");
    HANDLE mapping = createMapping && mapView ? createMapping(xprState.file, NULL, PAGE_READONLY, size, NULL) : NULL;
    const BYTE *view = mapping ? (const BYTE *)mapView(mapping, FILE_MAP_READ, 0, (SIZE_T)size) : NULL;
    if (mapping) CloseHandle(mapping);
    xprViewHeap = FALSE;
    if (view || size > (64ull << 20)) return view;
    /* no file mapping: read a bounded database into memory instead */
    BYTE *copy = (BYTE *)VirtualAlloc(NULL, (SIZE_T)size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    LARGE_INTEGER zero = {0};
    DWORD read = 0;
    if (copy && SetFilePointerEx(xprState.file, zero, NULL, FILE_BEGIN) &&
            ReadFile(xprState.file, copy, (DWORD)size, &read, NULL) && read == size) {
        xprViewHeap = TRUE;
        return copy;
    }
    if (copy) VirtualFree(copy, 0, MEM_RELEASE);
    return NULL;
}

static void XprUnmap(const BYTE *view) {
    if (xprViewHeap) VirtualFree((void *)view, 0, MEM_RELEASE);
    else UnmapViewOfFile((void *)view);
}

static BOOL XprOpen(const WCHAR *path, char **status) {
    xprState.file = CreateFile2(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, OPEN_ALWAYS, NULL);
    if (xprState.file == INVALID_HANDLE_VALUE) {
        *status = "open-failed";
        return FALSE;
    }
    XprHeader expected, found = {0};
    XprImageIdentity(&expected);
    LARGE_INTEGER size;
    if (!GetFileSizeEx(xprState.file, &size)) size.QuadPart = 0;
    DWORD read = 0;
    BOOL valid = size.QuadPart >= (LONGLONG)sizeof(XprHeader) && size.QuadPart <= (LONGLONG)XPR_MAX_FILE &&
        ReadFile(xprState.file, &found, sizeof(found), &read, NULL) && read == sizeof(found) &&
        XprEqual(&found, &expected, sizeof(found));
    *status = valid ? "loaded" : size.QuadPart ? "reset" : "new";
    if (valid && size.QuadPart > (LONGLONG)sizeof(XprHeader)) {
        xprState.view = XprMap((UINT64)size.QuadPart);
        xprState.viewSize = xprState.view ? (UINT64)size.QuadPart : 0;
        const UINT64 prefix = xprState.view ? XprIndex(FALSE) : 0;
        if (xprState.view && prefix < xprState.viewSize) {
            /* an interrupted append: cut the partial record so later appends stay reachable */
            XprUnmap(xprState.view);
            LARGE_INTEGER cut = {.QuadPart = (LONGLONG)prefix};
            const BOOL cutDone = SetFilePointerEx(xprState.file, cut, NULL, FILE_BEGIN) && SetEndOfFile(xprState.file);
            xprState.view = cutDone && prefix > sizeof(XprHeader) ? XprMap(prefix) : NULL;
            xprState.viewSize = xprState.view ? prefix : 0;
            *status = "loaded-cut-tail";
            if (!cutDone) valid = FALSE;
        }
        if (!xprState.view && prefix > sizeof(XprHeader)) {
            *status = "map-failed";
            valid = FALSE;
        }
        if (valid && xprState.view) XprIndex(TRUE);
    }
    if (!valid) {
        LARGE_INTEGER zero = {0};
        if (!SetFilePointerEx(xprState.file, zero, NULL, FILE_BEGIN) || !SetEndOfFile(xprState.file)) {
            *status = "reset-failed";
            return FALSE;
        }
        DWORD written = 0;
        if (!WriteFile(xprState.file, &expected, sizeof(expected), &written, NULL) || written != sizeof(expected)) {
            *status = "header-failed";
            return FALSE;
        }
    }
    if (!GetFileSizeEx(xprState.file, &size)) size.QuadPart = 0;
    xprState.fileSize = (UINT64)size.QuadPart;
    return TRUE;
}

/* Creates a recorded root signature that the title has not created yet. Only used when identical
   blobs return one object, so the title's own later creation yields the same pointer. */
static void XprEnsureRoot(UINT64 hash) {
    AcquireSRWLockShared(&xprState.lock);
    const BOOL present = XprRootByHash(hash) != NULL;
    ReleaseSRWLockShared(&xprState.lock);
    if (present) return;
    for (UINT32 i = 0; i < xprState.storedRootCount; ++i) {
        if (xprState.storedRoots[i].hash != hash) continue;
        void *created = NULL;
        const HRESULT result = ((XprCreateRootFn)xprState.createRootOriginal)(xprState.device, 0,
            xprState.storedRoots[i].bytes, xprState.storedRoots[i].length, &xprRootSignatureIid, &created);
        if (SUCCEEDED(result) && created) {
            XprRegisterRoot((ID3D12RootSignature *)created, xprState.storedRoots[i].bytes,
                xprState.storedRoots[i].length, TRUE);
            ((IUnknown *)created)->lpVtbl->Release((IUnknown *)created);
        }
        return;
    }
}

static DWORD WINAPI XprReplayThread(void *unused) {
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    LARGE_INTEGER frequency, now;
    QueryPerformanceFrequency(&frequency);
    MEMORYSTATUSEX memory = {.dwLength = sizeof(memory)};
    UINT32 deferred = 0;
    for (UINT32 pass = 0; pass < 4800; ++pass) {
        deferred = 0;
        for (UINT32 i = 0; i < xprState.entryCount; ++i) {
            XprEntry *entry = &xprState.entries[i];
            if (entry->state != XprPending) continue;
            /* Each pending entry goes to one replay thread; the others move on instead of waiting
               for a compile slot only to find the entry taken. */
            if (InterlockedCompareExchange(&entry->reserved, 1, 0)) continue;
            XprReader in = {entry->words, entry->wordCount, 0, FALSE};
            D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics;
            D3D12_COMPUTE_PIPELINE_STATE_DESC compute;
            D3D12_INPUT_ELEMENT_DESC elements[64];
            UINT64 rootHash = 0;
            const BOOL decoded = entry->graphics ? XprDecodeGraphics(&in, &graphics, elements, &rootHash) :
                XprDecodeCompute(&in, &compute, &rootHash);
            if (decoded && xprState.rootDedup == 1) XprEnsureRoot(rootHash);
            if (decoded) {
                /* No root signature yet: retry on a later pass without taking a slot. */
                AcquireSRWLockShared(&xprState.lock);
                const BOOL rooted = XprRootByHash(rootHash) != NULL;
                ReleaseSRWLockShared(&xprState.lock);
                if (!rooted) {
                    ++deferred;
                    InterlockedExchange(&entry->reserved, 0);
                    continue;
                }
            }
            if (xprState.slot) xprState.slot(TRUE);
            AcquireSRWLockExclusive(&xprState.lock);
            XprRoot *root = decoded ? XprRootByHash(rootHash) : NULL;
            BOOL take = FALSE;
            if (entry->state == XprPending) {
                if (!decoded) entry->state = XprDone;
                else if (!root) ++deferred;
                else {
                    entry->state = XprCompiling;
                    entry->root = root->object;
                    take = TRUE;
                }
            }
            ReleaseSRWLockExclusive(&xprState.lock);
            if (!take) {
                if (xprState.slot) xprState.slot(FALSE);
                InterlockedExchange(&entry->reserved, 0);
                continue;
            }
            if (entry->graphics) graphics.pRootSignature = entry->root;
            else compute.pRootSignature = entry->root;
            ID3D12PipelineState *object = NULL;
            LARGE_INTEGER before, after;
            QueryPerformanceCounter(&before);
            const HRESULT result = xprState.compile(xprState.device, entry->graphics,
                entry->graphics ? (const void *)&graphics : (const void *)&compute, &xprPipelineStateIid,
                (void **)&object);
            QueryPerformanceCounter(&after);
            if (xprState.slot) xprState.slot(FALSE);
            InterlockedAdd64(&xprState.compileTicks, after.QuadPart - before.QuadPart);
            AcquireSRWLockExclusive(&xprState.lock);
            if (SUCCEEDED(result) && object) {
                entry->object = object;
                entry->state = XprReady;
                if (!(InterlockedIncrement(&xprState.replayed) & 255)) {
                    MEMORYSTATUSEX progress = {.dwLength = sizeof(progress)};
                    GlobalMemoryStatusEx(&progress);
                    char line[160], *out = XprText(line, "asset pso-replay progress replayed=");
                    out = XprDec(out, (unsigned long long)xprState.replayed);
                    out = XprDec(XprText(out, " hits="), (unsigned long long)xprState.hits);
                    out = XprDec(XprText(out, " commit-avail-kib="), (unsigned long long)(progress.ullAvailPageFile >> 10));
                    XprEmit(line, out);
                }
            } else {
                entry->state = XprDone;
                InterlockedIncrement(&xprState.replayFailed);
            }
            WakeAllConditionVariable(&xprState.changed);
            ReleaseSRWLockExclusive(&xprState.lock);
            if (FAILED(result) && xprState.replayFailed <= 8) {
                char line[120], *out = XprText(line, "asset pso-replay compile-failed hr=0x");
                out = XprHex(out, (unsigned long)result, 8);
                out = XprDec(XprText(out, " entry="), i);
                XprEmit(line, out);
            }
        }
        if (!deferred) break;
        Sleep(250);
    }
    if (InterlockedDecrement(&xprState.threadsLive) > 0) return 0;
    QueryPerformanceCounter(&now);
    GlobalMemoryStatusEx(&memory);
    const unsigned long long compileTicks = (unsigned long long)xprState.compileTicks;
    char line[XPR_ROOT_TRACE ? 400 : 340], *out = XprText(line, "asset pso-replay done replayed=");
    out = XprDec(out, (unsigned long long)xprState.replayed);
    out = XprDec(XprText(out, " failed="), (unsigned long long)xprState.replayFailed);
    out = XprDec(XprText(out, " deferred="), deferred);
    out = XprDec(XprText(out, " hits="), (unsigned long long)xprState.hits);
    out = XprDec(XprText(out, " waited-hits="), (unsigned long long)xprState.waitedHits);
    out = XprDec(XprText(out, " misses="), (unsigned long long)xprState.misses);
    out = XprDec(XprText(out, " compile-ms="), frequency.QuadPart ? compileTicks * 1000 / (unsigned long long)frequency.QuadPart : 0);
    out = XprDec(XprText(out, " wall-ms="), frequency.QuadPart ?
        (unsigned long long)(now.QuadPart - xprState.startedQpc) * 1000 / (unsigned long long)frequency.QuadPart : 0);
    out = XprDec(XprText(out, " threads="), xprState.threadCount);
#if XPR_ROOT_TRACE
    out = XprDec(XprText(out, " root-misses="), (unsigned long long)xprState.rootMisses);
    out = XprDec(XprText(out, " root-dups="), (unsigned long long)xprState.rootDuplicates);
#endif
    out = XprDec(XprText(out, " commit-avail-kib-before="), xprState.commitBeforeKib);
    out = XprDec(XprText(out, " after="), (unsigned long long)(memory.ullAvailPageFile >> 10));
    out = XprDec(XprText(out, " qpc="), (unsigned long long)now.QuadPart);
    XprEmit(line, out);
    return 0;
}

static BOOL XprPatch(void **vtable, unsigned slot, void *replacement, PVOID *original) {
    ULONG protection = 0, ignored = 0;
    if (!VirtualProtectFromApp(&vtable[slot], sizeof(void *), PAGE_READWRITE, &protection)) return FALSE;
    *original = InterlockedExchangePointer(&vtable[slot], replacement);
    VirtualProtectFromApp(&vtable[slot], sizeof(void *), protection, &ignored);
    return TRUE;
}

/* Reads the shader-cache support the device advertises. Safe on every device. */
static void XboxPipelineCacheSupport(ID3D12Device *device) {
    D3D12_FEATURE_DATA_SHADER_CACHE cache = {0};
    const HRESULT result = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_SHADER_CACHE, &cache, sizeof(cache));
    char line[120], *out = XprText(line, "asset pso-cache-support hr=0x");
    out = XprHex(out, (unsigned long)result, 8);
    out = XprHex(XprText(out, " flags=0x"), cache.SupportFlags, 8);
    XprEmit(line, out);
}

/* Installs replay on the title's device. path is the database file. The root-signature method
   is always hooked; hookPipelines also routes the device's own PSO methods through replay, for
   hosts that do not intercept those calls themselves. compile is used by the replay thread (and
   by the hooked methods' misses when hookPipelines is set and compile is NULL). */
static BOOL XboxPipelineReplayInstall(ID3D12Device *device, const WCHAR *path, XboxPipelineCompileFn compile,
        BOOL hookPipelines, XboxPipelineTraceFn trace) {
    if (xprState.device || !device || !path) return FALSE;
    xprState.trace = trace;
    InitializeSRWLock(&xprState.lock);
    InitializeConditionVariable(&xprState.changed);
    InitializeCriticalSection(&xprState.writer);
    XboxPipelineCacheSupport(device);
    xprState.storedShaderMask = XPR_MAX_SHADERS * 2 - 1;
    xprState.storedKeyMask = XPR_MAX_ENTRIES * 2 - 1;
    xprState.storedShaders = (UINT64 *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (xprState.storedShaderMask + 1) * sizeof(UINT64));
    xprState.storedKeys = (UINT64 *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (xprState.storedKeyMask + 1) * sizeof(UINT64));
    xprState.shaders = (XprShader *)HeapAlloc(GetProcessHeap(), 0, XPR_MAX_SHADERS * sizeof(XprShader));
    xprState.entries = (XprEntry *)HeapAlloc(GetProcessHeap(), 0, XPR_MAX_ENTRIES * sizeof(XprEntry));
    char *status = "allocation-failed";
    BOOL ok = xprState.storedShaders && xprState.storedKeys && xprState.shaders && xprState.entries &&
        XprOpen(path, &status);
    void **const vtable = *(void ***)device;
    xprState.createGraphicsOriginal = vtable[10];
    xprState.createComputeOriginal = vtable[11];
    xprState.compile = compile ? compile : XprCompileOriginal;
    if (ok) ok = XprPatch(vtable, 16, (void *)XprCreateRootSignature, &xprState.createRootOriginal);
    if (ok && hookPipelines) {
        PVOID ignored;
        ok = XprPatch(vtable, 10, (void *)XprCreateGraphics, &ignored) &&
            XprPatch(vtable, 11, (void *)XprCreateCompute, &ignored);
    }
    if (!ok && status[0] == 'l') status = "hook-failed";
    xprState.device = device;
    xprState.enabled = ok;
    char line[240], *out = XprText(line, "asset pso-replay open status=");
    out = XprText(out, status);
    out = XprDec(XprText(out, " pipelines="), xprState.entryCount);
    out = XprDec(XprText(out, " shaders="), xprState.shaderCount);
    out = XprDec(XprText(out, " bytes="), xprState.fileSize);
    out = XprText(out, xprViewHeap ? " view=heap" : " view=mapped");
    out = XprDec(XprText(out, " error="), GetLastError());
    XprEmit(line, out);
    if (ok && xprState.entryCount) {
        LARGE_INTEGER started;
        QueryPerformanceCounter(&started);
        xprState.startedQpc = started.QuadPart;
        MEMORYSTATUSEX memory = {.dwLength = sizeof(memory)};
        GlobalMemoryStatusEx(&memory);
        xprState.commitBeforeKib = memory.ullAvailPageFile >> 10;
        const UINT32 threads = xprState.threadCount ? xprState.threadCount : 1;
        xprState.threadCount = threads;
        xprState.threadsLive = (LONG)threads;
        for (UINT32 i = 0; i < threads; ++i) {
            HANDLE thread = CreateThread(NULL, 0, XprReplayThread, NULL, 0, NULL);
            if (thread) CloseHandle(thread);
            else if (InterlockedDecrement(&xprState.threadsLive) == 0) xprState.threadCount = 0;
        }
    }
    return ok;
}

/* Sets the replay thread's admission hook. Call before XboxPipelineReplayInstall. */
static inline void XboxPipelineReplaySetSlot(XboxPipelineSlotFn slot) {
    xprState.slot = slot;
}

/* Number of replay threads (1-16, default 1). Call before XboxPipelineReplayInstall. */
static inline void XboxPipelineReplaySetThreads(UINT32 threads) {
    xprState.threadCount = threads < 1 ? 1 : threads > 16 ? 16 : threads;
}

/* Default location: beside the hosted title's Documents folder, which the host scopes to the
   Steam account and AppID. */
static BOOL XboxPipelineReplayDefaultPath(WCHAR *path, DWORD capacity) {
    const WCHAR name[] = L"\\d3d12-pipelines.xpr";
    DWORD length = GetEnvironmentVariableW(L"XBOX_WIN32_DOCUMENTS", path, capacity);
    if (!length || length >= capacity) return FALSE;
    while (length && (path[length - 1] == L'\\' || path[length - 1] == L'/')) --length;
    while (length && path[length - 1] != L'\\' && path[length - 1] != L'/') --length;
    if (!length || length + ARRAYSIZE(name) > capacity) return FALSE;
    --length;
    for (DWORD i = 0; i < ARRAYSIZE(name); ++i) path[length + i] = name[i];
    return TRUE;
}
