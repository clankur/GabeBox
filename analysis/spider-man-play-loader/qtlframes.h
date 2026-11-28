/* Per-frame queue summary (diagnostic, GPU_PROF). Reads the last ~5 s of the queue timeline ring. A frame ends at
   each submission from XboxWin32Dxgi (the hosted Present's copy). For the game's batches between two presents:
     gap    = first game submission - previous present (pacing sleep, Present return, waits before recording)
     submit = last game submission - first game submission (render-thread recording/submission span)
     busy   = union of [max(submit, previous completion), completion] over the frame's batches (GPU queue busy)
     lag    = last completion - this present (GPU still working after Present when positive)
   Line: asset qtlframes s=N frames= int-avg/max gap-avg/max submit-avg/max busy-avg/max lag-avg/max over16=<frames
   with busy > 16.7 ms> (all in microseconds). */

#define QF_VALUES 8192u
static LONG64 qfSubmit[GPUQ_SLOTS][QF_VALUES], qfDone[GPUQ_SLOTS][QF_VALUES];
static UINT64 qfValue[GPUQ_SLOTS][QF_VALUES];

static void QtlFrames(unsigned sample) {
    static ULONG_PTR dxgiBase, dxgiEnd;
    if (!dxgiBase) {
        const BYTE *m = (const BYTE *)GetModuleHandleW(L"XboxWin32Dxgi.dll");
        if (!m) return;
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(m + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
        dxgiBase = (ULONG_PTR)m; dxgiEnd = dxgiBase + nt->OptionalHeader.SizeOfImage;
    }
    LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    const LONG64 from = now.QuadPart - f.QuadPart * 5;
    const ULONG last = (ULONG)qtlCursor;
    /* Pass 1: completions by queue and value. */
    for (ULONG k = 0; k < QTL_EVENTS; ++k) {
        const QtlEvent *e = &qtlRing[(last + 1 + k) % QTL_EVENTS];
        const LONG64 t = e->qpc;
        if (!t || t < from || e->kind != 'X' || e->queue < 0 || (unsigned)e->queue >= GPUQ_SLOTS) continue;
        qfDone[e->queue][e->value % QF_VALUES] = t; qfValue[e->queue][e->value % QF_VALUES] = e->value;
    }
    /* Pass 2: frames in submission order. */
    unsigned frames = 0, over16 = 0;
    LONG64 prevPresent = 0, firstE = 0, lastE = 0, lastX = 0, busy = 0, busyEnd = 0;
    LONG64 sum[5] = {0}, max[5] = {0};
    const LONG64 frame16 = f.QuadPart * 167 / 10000;
    for (ULONG k = 0; k < QTL_EVENTS; ++k) {
        const QtlEvent *e = &qtlRing[(last + 1 + k) % QTL_EVENTS];
        const LONG64 t = e->qpc;
        if (!t || t < from || e->kind != 'E' || e->queue < 0 || (unsigned)e->queue >= GPUQ_SLOTS) continue;
        if (e->caller >= dxgiBase && e->caller < dxgiEnd) {
            if (prevPresent && firstE) {
                const LONG64 v[5] = {t - prevPresent, firstE - prevPresent, lastE - firstE, busy, lastX > t ? lastX - t : 0};
                for (unsigned i = 0; i < 5; ++i) { sum[i] += v[i]; if (v[i] > max[i]) max[i] = v[i]; }
                if (busy > frame16) ++over16;
                ++frames;
            }
            prevPresent = t; firstE = lastE = lastX = busy = 0;
            continue;
        }
        const LONG64 done = qfValue[e->queue][e->value % QF_VALUES] == e->value ? qfDone[e->queue][e->value % QF_VALUES] : 0;
        if (!firstE) firstE = t;
        lastE = t;
        if (done > t) {
            const LONG64 begin = t > busyEnd ? t : busyEnd;
            if (done > begin) busy += done - begin;
            if (done > busyEnd) busyEnd = done;
            if (done > lastX) lastX = done;
        }
    }
    if (!frames) return;
    char line[400], *o = PerfDec(Text(line, "asset qtlframes s="), sample);
    o = PerfDec(Text(o, " frames="), frames);
    static const char *const names[5] = {" int=", " gap=", " submit=", " busy=", " lag="};
    for (unsigned i = 0; i < 5; ++i) {
        o = PerfDec(Text(o, names[i]), (unsigned long long)((sum[i] / frames) * 1000000 / f.QuadPart));
        o = PerfDec(Text(o, "/"), (unsigned long long)(max[i] * 1000000 / f.QuadPart));
    }
    o = PerfDec(Text(o, " over16="), over16);
    o = Text(o, "\r\n");
    AppendTrace(line, (DWORD)(o - line));
}
