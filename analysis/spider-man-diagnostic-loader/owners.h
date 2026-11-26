/* Attributes committed CPU memory and D3D12 heap bytes to the game code that requested them, so a memory
   census can name the owners of the 5 GiB process commit. Included by trace.c before XboxTraceVirtualAlloc.
   Lines start with "asset owner-". Decommits are not hooked, so commit totals are cumulative. */

typedef struct OwnerEntry {
    ULONG_PTR key;          /* kind 0: allocation base; kind 1: heap type | flags << 8 | size bucket << 40 */
    DWORD frame[3];
    DWORD kind;
    volatile LONG64 bytes;
    volatile LONG count;
} OwnerEntry;

static OwnerEntry ownerTable[1024];
static SRWLOCK ownerLock = SRWLOCK_INIT;
static ULONG_PTR ownerImage, ownerImageEnd;
static volatile LONG ownerDropped;

static unsigned OwnerBucket(UINT64 bytes) {
    unsigned bucket = 0;
    while (bucket < 15 && (bytes >> (16 + bucket)) > 1) ++bucket; /* 0: <=128 KiB ... doubling */
    return bucket;
}

__declspec(noinline) static void OwnerRecord(DWORD kind, ULONG_PTR key, UINT64 bytes) {
    if (!ownerImage) {
        const ULONG_PTR image = (ULONG_PTR)GetModuleHandleW(NULL);
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)image;
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(image + dos->e_lfanew);
        ownerImageEnd = image + nt->OptionalHeader.SizeOfImage;
        ownerImage = image;
    }
    PVOID stack[20];
    const USHORT count = RtlCaptureStackBackTrace(1, ARRAYSIZE(stack), stack, NULL);
    DWORD frame[3] = {0, 0, 0};
    unsigned found = 0;
    for (USHORT index = 0; index < count && found < ARRAYSIZE(frame); ++index) {
        const ULONG_PTR address = (ULONG_PTR)stack[index];
        if (address < ownerImage || address >= ownerImageEnd) continue;
        const DWORD rva = (DWORD)(address - ownerImage);
        /* Skip the engine's virtual-memory wrappers so the owner is the pool or heap that grew. */
        if (kind == 0 && rva >= 0x01C64000u && rva < 0x01C66000u) continue;
        frame[found++] = rva;
    }
    ULONG_PTR hash = key ^ ((ULONG_PTR)frame[0] * 0x9E3779B1u) ^ ((ULONG_PTR)frame[1] << 7) ^ kind;
    hash ^= hash >> 17;
    AcquireSRWLockExclusive(&ownerLock);
    for (unsigned probe = 0; probe < ARRAYSIZE(ownerTable); ++probe) {
        OwnerEntry *entry = &ownerTable[(hash + probe) % ARRAYSIZE(ownerTable)];
        if (entry->count && (entry->kind != kind || entry->key != key || entry->frame[0] != frame[0] ||
                entry->frame[1] != frame[1] || entry->frame[2] != frame[2])) continue;
        if (!entry->count) {
            entry->kind = kind;
            entry->key = key;
            entry->frame[0] = frame[0];
            entry->frame[1] = frame[1];
            entry->frame[2] = frame[2];
        }
        entry->bytes += (LONG64)bytes;
        entry->count += 1;
        ReleaseSRWLockExclusive(&ownerLock);
        return;
    }
    ReleaseSRWLockExclusive(&ownerLock);
    InterlockedIncrement(&ownerDropped);
}

static void OwnerCommit(LPVOID allocation, SIZE_T bytes) {
    MEMORY_BASIC_INFORMATION region;
    if (VirtualQuery(allocation, &region, sizeof(region)) != sizeof(region)) return;
    OwnerRecord(0, (ULONG_PTR)region.AllocationBase, bytes);
}

static void OwnerHeap(UINT type, UINT flags, UINT64 bytes) {
    OwnerRecord(1, (ULONG_PTR)type | ((ULONG_PTR)(flags & 0xFFFFu) << 8) | ((ULONG_PTR)OwnerBucket(bytes) << 40), bytes);
}

/* Emits every entry holding at least 2 MiB, largest first, at most 160 lines. */
static void OwnerEmit(unsigned sample) {
    static OwnerEntry snapshot[ARRAYSIZE(ownerTable)];
    unsigned used = 0;
    AcquireSRWLockShared(&ownerLock);
    for (unsigned index = 0; index < ARRAYSIZE(ownerTable); ++index)
        if (ownerTable[index].count && ownerTable[index].bytes >= (2ll << 20)) snapshot[used++] = ownerTable[index];
    ReleaseSRWLockShared(&ownerLock);
    char line[200], *out;
    for (unsigned emitted = 0; emitted < used && emitted < 160; ++emitted) {
        unsigned best = emitted;
        for (unsigned index = emitted + 1; index < used; ++index)
            if (snapshot[index].bytes > snapshot[best].bytes) best = index;
        const OwnerEntry pick = snapshot[best];
        snapshot[best] = snapshot[emitted];
        out = Hex(Text(line, "asset owner sample=0x"), sample, 4);
        out = Hex(Text(out, " kind=0x"), pick.kind, 2);
        out = Hex(Text(out, " key=0x"), (DWORD)((UINT64)pick.key >> 32), 8);
        out = Hex(out, (DWORD)pick.key, 8);
        out = Hex(Text(out, " kib=0x"), (DWORD)((UINT64)pick.bytes >> 10), 8);
        out = Hex(Text(out, " n=0x"), (DWORD)pick.count, 8);
        out = Hex(Text(out, " f0=0x"), pick.frame[0], 8);
        out = Hex(Text(out, " f1=0x"), pick.frame[1], 8);
        out = Hex(Text(out, " f2=0x"), pick.frame[2], 8);
        out = Text(out, "\r\n");
        AppendTrace(line, (DWORD)(out - line));
    }
    out = Hex(Text(line, "asset owner-done sample=0x"), sample, 4);
    out = Hex(Text(out, " entries=0x"), used, 4);
    out = Hex(Text(out, " dropped=0x"), (DWORD)ownerDropped, 8);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}
