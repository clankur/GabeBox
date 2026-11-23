#define WIN32_LEAN_AND_MEAN
#include "loader-trace-output.h"

typedef LONG (WINAPI *GetDisplaySizes)(UINT32, UINT32 *, UINT32 *);

enum { displaySampleLimit = 8 };
static volatile LONG displaySamples;

static char *AppendText(char *out, const char *text) {
    while (*text) *out++ = *text++;
    return out;
}

static char *AppendHex(char *out, UINT32 value) {
    static const char digits[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) *out++ = digits[(value >> shift) & 15];
    return out;
}

static void RecordDisplaySizes(UINT32 flags, LONG result, const char *state, const UINT32 *paths, const UINT32 *modes) {
    if (displaySamples >= displaySampleLimit || InterlockedIncrement(&displaySamples) > displaySampleLimit) return;
    char line[128];
    char *out = AppendText(line, "asset display-sizes=");
    out = AppendText(out, state);
    out = AppendHex(AppendText(out, " flags=0x"), flags);
    out = AppendHex(AppendText(out, " native=0x"), (UINT32)result);
    if (paths) {
        out = AppendHex(AppendText(out, " paths=0x"), *paths);
        out = AppendHex(AppendText(out, " modes=0x"), *modes);
    }
    out = AppendText(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

LONG WINAPI XboxGetDisplayConfigBufferSizes(UINT32 flags, UINT32 *pathCount, UINT32 *modeCount) {
    const DWORD previous = GetLastError();
    HMODULE user = GetModuleHandleW(L"user32.dll");
    GetDisplaySizes getSizes = user ? (GetDisplaySizes)(void *)GetProcAddress(user, "GetDisplayConfigBufferSizes") : NULL;
    SetLastError(previous);
    if (!getSizes) {
        RecordDisplaySizes(flags, ERROR_PROC_NOT_FOUND, "export-unavailable", NULL, NULL);
        return ERROR_NOT_SUPPORTED;
    }
    if (!pathCount || !modeCount || pathCount == modeCount) return getSizes(flags, pathCount, modeCount);

    UINT32 paths = MAXDWORD;
    UINT32 modes = MAXDWORD - 1;
    LONG result = getSizes(flags, &paths, &modes);
    if (result == ERROR_SUCCESS && (paths == MAXDWORD || modes == MAXDWORD - 1)) {
        // Try different initial values to distinguish unwritten outputs from valid counts.
        const UINT32 pathInitial = MAXDWORD - 2;
        const UINT32 modeInitial = MAXDWORD - 3;
        paths = pathInitial;
        modes = modeInitial;
        result = getSizes(flags, &paths, &modes);
        if (result == ERROR_SUCCESS && (paths == pathInitial || modes == modeInitial)) {
            const char *state = paths == pathInitial && modes == modeInitial ? "counts-unwritten" :
                (paths == pathInitial ? "paths-unwritten" : "modes-unwritten");
            RecordDisplaySizes(flags, result, state, NULL, NULL);
            return ERROR_NOT_SUPPORTED;
        }
    }
    if (result == ERROR_SUCCESS) {
        *pathCount = paths;
        *modeCount = modes;
        RecordDisplaySizes(flags, result, "complete", &paths, &modes);
    } else {
        RecordDisplaySizes(flags, result, "native-failure", NULL, NULL);
    }
    return result;
}
