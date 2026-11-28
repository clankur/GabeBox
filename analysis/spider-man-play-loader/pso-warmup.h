/* PSO_SKIP_WARMUP=1: start without the game's own pipeline warm-up list.

   At boot the game reads its pipeline cache file (`xbox0.pso` in the Documents folder, opened with
   _wfopen "rb") and queues every listed pipeline on its six pipeline-creation threads. The queue is
   first-in first-out (exe+0x018CF540 appends, the threads at exe+0x018CD4E0 pop in order), so the
   pipelines the boot screens, the menu and the world need wait behind the whole list. On Xbox each
   of those 2,500+ pipelines is a full compile, because the runtime has no pipeline cache.

   With this define the loader answers the read open with ERROR_FILE_NOT_FOUND, the same result the
   game gets on a first launch ("File ... is missing, no pipelines read."), so it creates pipelines
   only when something asks for one. The game appends each new pipeline to the file (_wfopen "ab+");
   those opens go to `<file>.session`, truncated on the first open of each run, so the player's
   accumulated list stays unchanged and removing the define restores the warm-up. Precompiling is
   left to the pipeline replay database, whose threads run only while no title compile waits.

   Records: "asset pso-warmup hidden" for each hidden open or attribute query (first 8), and
   "asset pso-warmup session-open ok|error=0x..." for each redirected write open (first 8).

   Console result (run skip1): the lobby is ready 17.6 s after Play, but without the list the
   render thread creates each world pipeline itself at first draw, and Continue to gameplay took
   196 s. Use it only to record a session list for PSO_WARMUP_LIST.

   PSO_WARMUP_LIST: keep the warm-up, but put the session list first. A PSO_SKIP_WARMUP run leaves
   `<file>.session` holding, in the order the game created them, only the pipelines that run asked
   for: about 1,470 for boot, menu and the first world area, against 2,600+ in the player's list.
   The file is an 8-byte header {1, 4} followed by 0x588-byte records (an 8-byte hash and 0x580
   bytes of description), as the game's write thread (exe+0x018CF9E0) appends them, and the
   reader counts and skips duplicate hashes.
   - 1 (prune): serve the session list alone.
   - 2 (merge): serve the session list, then every record of the player's list; the reader drops
     the duplicates, so the warm-up keeps every pipeline the player ever used.
   At the first read open of the list the loader writes `<file>.served` from whole, validated
   records and answers the open with it; the run's new pipelines are appended to `<file>.session`.
   Without a valid session list nothing is redirected and the game uses the player's file as
   before. The player's file is only read while the session list is served.

   Records: "asset pso-warmup served=prune|merge records=N session=S original=O" (or
   "served=original reason=...") for each read open (first 8), and "asset pso-warmup session-open"
   for each redirected write. */
#if PSO_SKIP_WARMUP && defined(PSO_WARMUP_LIST) && PSO_WARMUP_LIST
#error "PSO_SKIP_WARMUP records the session list and PSO_WARMUP_LIST serves it; choose one"
#endif
#ifndef PSO_WARMUP_LEAF
#define PSO_WARMUP_LEAF L"xbox0.pso"
#endif

static volatile LONG warmupHiddenCount, warmupSessionCount, warmupSessionStarted, warmupServedCount, warmupServingSession;

static BOOL WarmupCacheFile(LPCWSTR path) {
    if (!path) return FALSE;
    const WCHAR *leaf = path;
    UINT length = 0;
    for (; length < 1024 && path[length]; ++length)
        if (path[length] == L'/' || path[length] == L'\\') leaf = path + length + 1;
    return length < 1024 && CompareStringOrdinal(leaf, -1, PSO_WARMUP_LEAF, -1, TRUE) == CSTR_EQUAL;
}

static void WarmupTrace(const char *event, BOOL success, DWORD error, volatile LONG *counter) {
    if (InterlockedIncrement(counter) > 8) return;
    char line[96], *out = Text(Text(line, "asset pso-warmup "), event);
    if (success) out = Text(out, " ok");
    else if (error) out = Hex(Text(out, " error=0x"), error, 8);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

/* For a CreateFileW of the warm-up file: returns TRUE when the open must fail as not found (a
   read-only open of an existing file). Otherwise, when the open writes, *session receives the
   redirected path and *creation becomes CREATE_ALWAYS on the run's first open. */
static BOOL WarmupRouteOpen(LPCWSTR path, DWORD access, DWORD *creation, WCHAR *session, DWORD capacity,
        LPCWSTR *routed) {
    *routed = NULL;
    if (!WarmupCacheFile(path)) return FALSE;
    const DWORD writes = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE;
    if (*creation == OPEN_EXISTING && !(access & writes)) return TRUE;
    static const WCHAR suffix[] = L".session";
    DWORD length = 0;
    while (path[length]) ++length;
    if (length + ARRAYSIZE(suffix) > capacity) return TRUE; /* never touch the player's file */
    for (DWORD i = 0; i < length; ++i) session[i] = path[i];
    for (DWORD i = 0; i < ARRAYSIZE(suffix); ++i) session[length + i] = suffix[i];
    if ((*creation == OPEN_ALWAYS || *creation == CREATE_NEW) && !InterlockedExchange(&warmupSessionStarted, 1))
        *creation = CREATE_ALWAYS;
    *routed = session;
    return FALSE;
}

/* PSO_WARMUP_LIST: <path><suffix>, or NULL when it does not fit. */
static LPCWSTR WarmupSibling(LPCWSTR path, const WCHAR *suffix, WCHAR *out, DWORD capacity) {
    DWORD length = 0, extra = 0;
    while (path[length]) ++length;
    while (suffix[extra]) ++extra;
    if (length + extra + 1 > capacity) return NULL;
    for (DWORD i = 0; i < length; ++i) out[i] = path[i];
    for (DWORD i = 0; i <= extra; ++i) out[length + i] = suffix[i];
    return out;
}

#define WARMUP_RECORD 0x588u

/* Reads a whole warm-up list. Returns the heap buffer and the number of whole records after a
   valid {1, 4} header, or NULL. A partial last record (an interrupted append) is left out. */
static BYTE *WarmupReadList(LPCWSTR path, DWORD *records) {
    *records = 0;
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    BYTE *data = NULL;
    if (GetFileSizeEx(file, &size) && size.QuadPart >= 8 && size.QuadPart <= (64ll << 20)) {
        data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart);
        DWORD read = 0;
        if (data && (!ReadFile(file, data, (DWORD)size.QuadPart, &read, NULL) || read != (DWORD)size.QuadPart ||
                ((const UINT32 *)data)[0] != 1 || ((const UINT32 *)data)[1] != 4)) {
            HeapFree(GetProcessHeap(), 0, data);
            data = NULL;
        }
        if (data) *records = (DWORD)((size.QuadPart - 8) / WARMUP_RECORD);
    }
    CloseHandle(file);
    return data;
}

static void WarmupServedTrace(const char *mode, DWORD records, DWORD session, DWORD original) {
    if (InterlockedIncrement(&warmupServedCount) > 8) return;
    char line[160], *out = Text(Text(line, "asset pso-warmup served="), mode);
    out = Hex(Text(out, " records=0x"), records, 8);
    out = Hex(Text(out, " session=0x"), session, 8);
    out = Hex(Text(out, " original=0x"), original, 8);
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

/* Writes <path>.served from the session list (and, when merging, the player's list). */
static __attribute__((noinline)) BOOL WarmupBuildServed(LPCWSTR path, LPCWSTR served) {
    WCHAR session[520];
    if (!WarmupSibling(path, L".session", session, ARRAYSIZE(session))) return FALSE;
    DWORD sessionRecords, originalRecords = 0;
    BYTE *first = WarmupReadList(session, &sessionRecords);
    if (!first || !sessionRecords) {
        if (first) HeapFree(GetProcessHeap(), 0, first);
        WarmupTrace(first ? "served=original reason=empty-session" : "served=original reason=no-session", FALSE, 0,
            &warmupServedCount);
        return FALSE;
    }
    BYTE *second = PSO_WARMUP_LIST == 2 ? WarmupReadList(path, &originalRecords) : NULL;
    const HANDLE out = CreateFileW(served, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL ok = out != INVALID_HANDLE_VALUE;
    DWORD written = 0;
    if (ok) ok = WriteFile(out, first, 8 + sessionRecords * WARMUP_RECORD, &written, NULL);
    if (ok && second && originalRecords)
        ok = WriteFile(out, second + 8, originalRecords * WARMUP_RECORD, &written, NULL);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    HeapFree(GetProcessHeap(), 0, first);
    if (second) HeapFree(GetProcessHeap(), 0, second);
    if (ok) WarmupServedTrace(PSO_WARMUP_LIST == 2 ? "merge" : "prune", sessionRecords + originalRecords,
        sessionRecords, originalRecords);
    else WarmupTrace("served=original reason=write-failed", FALSE, GetLastError(), &warmupServedCount);
    return ok;
}

/* PSO_WARMUP_LIST: returns the handle to use with *handled TRUE, or *handled FALSE to open the
   requested path unchanged. */
static __attribute__((noinline)) HANDLE WarmupListOpen(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
        DWORD creation, DWORD attributes, HANDLE templateFile, BOOL *handled) {
    *handled = FALSE;
    if (!WarmupCacheFile(path)) return INVALID_HANDLE_VALUE;
    const DWORD writes = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE;
    WCHAR other[520];
    if (creation == OPEN_EXISTING && !(access & writes)) {
        if (!WarmupSibling(path, L".served", other, ARRAYSIZE(other))) return INVALID_HANDLE_VALUE;
        if (!WarmupBuildServed(path, other)) return INVALID_HANDLE_VALUE;
        const HANDLE file = CreateFileW(other, access, share, security, creation, attributes, templateFile);
        if (file == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
        InterlockedExchange(&warmupServingSession, 1);
        *handled = TRUE;
        return file;
    }
    if (!warmupServingSession) return INVALID_HANDLE_VALUE; /* no session list: the player's file as usual */
    if (!WarmupSibling(path, L".session", other, ARRAYSIZE(other))) return INVALID_HANDLE_VALUE;
    const HANDLE file = CreateFileW(other, access, share, security, creation, attributes, templateFile);
    const DWORD error = GetLastError();
    WarmupTrace("session-open", file != INVALID_HANDLE_VALUE, error, &warmupSessionCount);
    SetLastError(error);
    *handled = TRUE;
    return file;
}
