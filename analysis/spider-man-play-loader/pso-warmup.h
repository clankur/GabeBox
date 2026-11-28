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
   "asset pso-warmup session-open ok|error=0x..." for each redirected write open (first 8). */
#ifndef PSO_WARMUP_LEAF
#define PSO_WARMUP_LEAF L"xbox0.pso"
#endif

static volatile LONG warmupHiddenCount, warmupSessionCount, warmupSessionStarted;

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
