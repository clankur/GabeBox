/* Inspect the native display contracts used by desktop engines in an AppContainer. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef PROBE_HOSTED_DISPLAY
#include <appmodel.h>
#endif
#include "../child-process.h"

typedef LONG (WINAPI *GetSizes)(UINT32, UINT32 *, UINT32 *);
typedef LONG (WINAPI *QueryConfig)(UINT32, UINT32 *, DISPLAYCONFIG_PATH_INFO *, UINT32 *,
    DISPLAYCONFIG_MODE_INFO *, DISPLAYCONFIG_TOPOLOGY_ID *);
typedef LONG (WINAPI *GetDeviceInfo)(DISPLAYCONFIG_DEVICE_INFO_HEADER *);

enum { maxPaths = 16, maxModes = 64 };
static DISPLAYCONFIG_PATH_INFO paths[maxPaths];
static DISPLAYCONFIG_MODE_INFO modes[maxModes];

static unsigned int Status(unsigned int stage, LONG error) {
    // 255 represents uncommon errors outside the byte-sized Win32 status range.
    return (stage << 8) | (error >= 0 && error < 255 ? (unsigned int)error : 255u);
}

#ifdef PROBE_HOSTED_DISPLAY
static HMODULE windowAdapter;

static BOOL SameLuid(LUID a, LUID b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static BOOL SameName(LPCWSTR a, LPCWSTR b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static BOOL MatchesLegacyDisplay(const DISPLAYCONFIG_SOURCE_DEVICE_NAME *source,
    const DISPLAYCONFIG_TARGET_DEVICE_NAME *target, UINT32 flags, UINT32 pathCount, UINT32 modeCount) {
    typedef HMONITOR (WINAPI *LookupMonitor)(const RECT *, DWORD);
    typedef BOOL (WINAPI *MonitorInfo)(HMONITOR, MONITORINFO *);
    typedef BOOL (WINAPI *ReadMode)(LPCWSTR, DWORD, DEVMODEW *);
    const LookupMonitor lookup = (LookupMonitor)(void *)GetProcAddress(windowAdapter, "MonitorFromRect");
    const MonitorInfo info = (MonitorInfo)(void *)GetProcAddress(windowAdapter, "GetMonitorInfoW");
    const ReadMode readMode = (ReadMode)(void *)GetProcAddress(windowAdapter, "EnumDisplaySettingsW");
    if (pathCount != 1 || modeCount != 2 || !lookup || !info || !readMode) return FALSE;

    const DISPLAYCONFIG_PATH_INFO *path = &paths[0];
    const UINT32 sourceIndex = path->sourceInfo.modeInfoIdx;
    const UINT32 targetIndex = path->targetInfo.modeInfoIdx;
    if (sourceIndex >= modeCount || targetIndex >= modeCount || !(path->flags & DISPLAYCONFIG_PATH_ACTIVE) ||
            !path->targetInfo.targetAvailable || !SameLuid(modes[sourceIndex].adapterId, path->sourceInfo.adapterId) ||
            !SameLuid(modes[targetIndex].adapterId, path->targetInfo.adapterId) ||
            modes[sourceIndex].id != path->sourceInfo.id || modes[targetIndex].id != path->targetInfo.id) return FALSE;
    const DISPLAYCONFIG_SOURCE_MODE *sourceMode = &modes[sourceIndex].sourceMode;
    const DISPLAYCONFIG_VIDEO_SIGNAL_INFO *signal = &modes[targetIndex].targetMode.targetVideoSignalInfo;
    const RECT area = {sourceMode->position.x, sourceMode->position.y,
        sourceMode->position.x + (LONG)sourceMode->width, sourceMode->position.y + (LONG)sourceMode->height};
    MONITORINFOEXW monitor;
    monitor.cbSize = sizeof(monitor);
    DEVMODEW legacy;
    legacy.dmSize = sizeof(legacy);
    if (!info(lookup(&area, MONITOR_DEFAULTTONULL), (MONITORINFO *)&monitor) ||
            !SameName(source->viewGdiDeviceName, monitor.szDevice) ||
            !readMode(monitor.szDevice, ENUM_CURRENT_SETTINGS, &legacy)) return FALSE;

    return monitor.rcMonitor.left == area.left && monitor.rcMonitor.top == area.top &&
        monitor.rcMonitor.right == area.right && monitor.rcMonitor.bottom == area.bottom &&
        sourceMode->width == 1280 && sourceMode->height == 720 &&
        legacy.dmPelsWidth == sourceMode->width && legacy.dmPelsHeight == sourceMode->height &&
        signal->activeSize.cx == sourceMode->width && signal->activeSize.cy == sourceMode->height &&
        signal->totalSize.cx == (flags == QDC_DATABASE_CURRENT ? 0 : sourceMode->width) &&
        signal->totalSize.cy == (flags == QDC_DATABASE_CURRENT ? 0 : sourceMode->height) &&
        signal->vSyncFreq.Numerator == legacy.dmDisplayFrequency && signal->vSyncFreq.Denominator == 1 &&
        path->targetInfo.refreshRate.Numerator == signal->vSyncFreq.Numerator &&
        path->targetInfo.refreshRate.Denominator == signal->vSyncFreq.Denominator &&
        target->monitorFriendlyDeviceName[0] && !target->monitorDevicePath[0] && !target->flags.value;
}
#endif

static unsigned int CheckDisplay(UINT32 flags, GetSizes getSizes, QueryConfig query, GetDeviceInfo deviceInfo) {
    if (!getSizes || !query || !deviceInfo)
        return Status(1, (!getSizes ? 1 : 0) | (!query ? 2 : 0) | (!deviceInfo ? 4 : 0));

    for (unsigned int attempt = 0; attempt < 2; ++attempt) {
        UINT32 pathCount = MAXDWORD;
        UINT32 modeCount = MAXDWORD - 1;
        LONG result = getSizes(flags, &pathCount, &modeCount);
        if (result != ERROR_SUCCESS) return Status(2, result);
        if (pathCount == MAXDWORD || modeCount == MAXDWORD - 1) {
            pathCount = MAXDWORD - 2;
            modeCount = MAXDWORD - 3;
            result = getSizes(flags, &pathCount, &modeCount);
            if (result != ERROR_SUCCESS) return Status(2, result);
            const unsigned int unwritten = (pathCount == MAXDWORD - 2 ? 1 : 0) |
                (modeCount == MAXDWORD - 3 ? 2 : 0);
            if (unwritten) return Status(3, unwritten);
        }
        if (pathCount > maxPaths || modeCount > maxModes)
            return Status(4, (pathCount > maxPaths ? 1 : 0) | (modeCount > maxModes ? 2 : 0));
        if (!pathCount) return Status(5, 0);

        const UINT32 pathCapacity = pathCount;
        const UINT32 modeCapacity = modeCount;
        DISPLAYCONFIG_TOPOLOGY_ID topology = (DISPLAYCONFIG_TOPOLOGY_ID)0;
        for (UINT32 i = 0; i < pathCount; ++i) {
            paths[i].sourceInfo.modeInfoIdx = MAXDWORD - 3;
            paths[i].targetInfo.modeInfoIdx = MAXDWORD - 4;
        }
        result = query(flags, &pathCount, paths, &modeCount, modes,
            flags == QDC_DATABASE_CURRENT ? &topology : NULL);
        if (result == ERROR_INSUFFICIENT_BUFFER && !attempt) continue;
        if (result != ERROR_SUCCESS) return Status(6, result);
        if (pathCount > pathCapacity || modeCount > modeCapacity) return Status(7, 1);
        if (!pathCount) return Status(5, 0);
        if (flags == QDC_DATABASE_CURRENT && !topology) return Status(7, 2);

        for (UINT32 i = 0; i < pathCount; ++i) {
            const UINT32 source = paths[i].sourceInfo.modeInfoIdx;
            const UINT32 target = paths[i].targetInfo.modeInfoIdx;
            if (flags == QDC_ONLY_ACTIVE_PATHS && source == DISPLAYCONFIG_PATH_MODE_IDX_INVALID)
                return Status(7, 4);
            if ((source != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
                    (source >= modeCount || modes[source].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)) ||
                (target != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
                    (target >= modeCount || modes[target].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)))
                return Status(7, 3);
        }
        static DISPLAYCONFIG_SOURCE_DEVICE_NAME source;
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[0].sourceInfo.adapterId;
        source.header.id = paths[0].sourceInfo.id;
        for (unsigned int i = 0; i < ARRAYSIZE(source.viewGdiDeviceName); ++i)
            source.viewGdiDeviceName[i] = (WCHAR)0xfdef;
        result = deviceInfo(&source.header);
        if (result != ERROR_SUCCESS) return Status(8, result);
        unsigned int end = 0;
        while (end < ARRAYSIZE(source.viewGdiDeviceName) && source.viewGdiDeviceName[end]) ++end;
        if (!end || end == ARRAYSIZE(source.viewGdiDeviceName)) return Status(7, 5);

        static DISPLAYCONFIG_TARGET_DEVICE_NAME target;
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = paths[0].targetInfo.adapterId;
        target.header.id = paths[0].targetInfo.id;
        result = deviceInfo(&target.header);
        if (result != ERROR_SUCCESS) return Status(9, result);
#ifdef PROBE_HOSTED_DISPLAY
        if (!MatchesLegacyDisplay(&source, &target, flags, pathCount, modeCount)) return Status(0xB, 1);
#endif
        return 0;
    }
    return Status(6, ERROR_INSUFFICIENT_BUFFER);
}

DECLSPEC_IMPORT DECLSPEC_NORETURN void WINAPI ExitProcess(UINT code);

void WINAPI ChildEntry(void) {
#ifdef PROBE_HOSTED_DISPLAY
    const HMODULE user = LoadPackagedLibrary(L"XboxWin32HostedDisplay.dll", 0);
    windowAdapter = LoadPackagedLibrary(L"XboxWin32Window.dll", 0);
    if (!windowAdapter || !SetEnvironmentVariableW(L"XBOX_SDL_RENDER_WIDTH", L"1280") ||
            !SetEnvironmentVariableW(L"XBOX_SDL_RENDER_HEIGHT", L"720")) ExitProcess(0x6EB02000u);
#else
    const HMODULE user = LoadLibraryExW(L"user32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
#endif
    if (!user) ExitProcess(0x6E000000u | (Status(0xA, GetLastError()) << 12));
    const GetSizes getSizes = (GetSizes)(void *)GetProcAddress(user, "GetDisplayConfigBufferSizes");
    const QueryConfig query = (QueryConfig)(void *)GetProcAddress(user, "QueryDisplayConfig");
    const GetDeviceInfo deviceInfo = (GetDeviceInfo)(void *)GetProcAddress(user, "DisplayConfigGetDeviceInfo");
#ifdef PROBE_HOSTED_DISPLAY
    if (query) {
        UINT32 shortPaths = 0, shortModes = 2;
        paths[0].flags = 0xCAFEu;
        if (query(QDC_ONLY_ACTIVE_PATHS, &shortPaths, paths, &shortModes, modes, NULL) != ERROR_INSUFFICIENT_BUFFER ||
                shortPaths != 0 || shortModes != 2 || paths[0].flags != 0xCAFEu) ExitProcess(0x6EB03000u);
    }
#endif
    const unsigned int active = CheckDisplay(QDC_ONLY_ACTIVE_PATHS, getSizes, query, deviceInfo);
    const unsigned int database = CheckDisplay(QDC_DATABASE_CURRENT, getSizes, query, deviceInfo);
    ExitProcess(active || database ? 0x6E000000u | (active << 12) | database : PROBE_EXPECTED_CHILD_EXIT);
}
