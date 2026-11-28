/* Demand-committed engine memory for the Spider-Man play loader, built on the shared
   runtime/win32/demand-commit.h. The selection is pinned to the audited executable (build 23986256):
   - kind 0: the AssetHeap pool (size announced by the pool-size hook) and every pool-constructor
     commit (return address exe+0x01C64D22) of 16 MiB or more;
   - kind 1 (LAZY_GENERAL): every VM::Commit (return address exe+0x01C64DD2) of LAZY_GENERAL_MIN or
     more. VM::Commit backs the general heap's large blocks (Scaleform 96 MiB, skinning, streaming and
     loader buffers); in the world about 320 MiB of them were committed and never touched. The engine
     frees them with VM::Decommit (VirtualFree MEM_DECOMMIT), which the wrapped import tracks.
   Lines start with "asset lazy-". Included by trace.c before XboxTraceVirtualAlloc. */
#include "demand-commit.h"

#ifndef LAZY_GENERAL_MIN
#define LAZY_GENERAL_MIN (1u << 20)
#endif
#define LAZY_SLOTS XDC_SLOTS
#define lazyRanges (xdcState.ranges)
#define lazyCount (xdcState.count)

static volatile LONG64 lazyPendingBytes;
static volatile LONG lazyGeneralTraced;

static void LazyExpect(UINT64 bytes) {
    InterlockedExchange64(&lazyPendingBytes, (LONG64)bytes);
}

static LPVOID LazyTakeCommit(LPVOID address, SIZE_T bytes, DWORD type, DWORD protection, BOOL pool, BOOL general) {
    if (!(type & MEM_COMMIT) || !address || protection != PAGE_READWRITE) return NULL;
    const BOOL expected = lazyPendingBytes && (LONG64)bytes == lazyPendingBytes &&
        InterlockedCompareExchange64(&lazyPendingBytes, 0, (LONG64)bytes) == (LONG64)bytes;
    DWORD kind = 2;
    if (expected || (pool && bytes >= (16u << 20))) kind = 0;
#ifdef LAZY_GENERAL
    else if (general && bytes >= LAZY_GENERAL_MIN) kind = 1;
#else
    (void)general;
#endif
    if (kind == 2) return NULL;
#ifdef LAZY_GENERAL
    if (kind == 1) {
        static volatile LONG wrapped;
        if (!InterlockedExchange(&wrapped, 1)) {
            const DWORD patched = XboxDemandCommitWrapImports(GetModuleHandleW(NULL), 0);
            char line[80], *out = Hex(Text(line, "asset lazy-image-hooks patched=0x"), patched, 2);
            out = Text(out, "\r\n");
            AppendTrace(line, (DWORD)(out - line));
        }
    }
#endif
    const LPVOID taken = XboxDemandCommitTake(address, bytes, type, protection, kind, AppendTrace);
    if (!taken) return NULL;
    XdcRange *range = NULL;
    AcquireSRWLockShared(&xdcState.lock);
    range = XdcFindLocked((ULONG_PTR)taken);
    if (range && kind == 0) XdcTrace("asset lazy-range", range, range->base);
    else if (range && InterlockedIncrement(&lazyGeneralTraced) <= 48) XdcTrace("asset lazy-general", range, range->base);
    ReleaseSRWLockShared(&xdcState.lock);
    return taken;
}

static void LazyPrepareWrite(LPVOID buffer, DWORD length) {
    XboxDemandCommitPrepareWrite(buffer, length);
}

static void LazySummary(unsigned sample) {
    char prefix[48], *out = Hex(Text(prefix, "asset lazy-summary sample=0x"), sample, 4);
    *out = 0;
    XboxDemandCommitSummary(prefix, 0, 0);
}
