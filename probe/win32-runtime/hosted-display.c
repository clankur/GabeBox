/* Read-only display configuration for the hosted desktop. */
#define WIN32_LEAN_AND_MEAN
#include "loader-trace-output.h"
#include "hosted-display.h"

static const LUID hostedAdapter = {0x58424f58u, 0x434344};
enum { sourceId = 0, targetId = 1, sourceIndex = 0, targetIndex = 1, pathCount = 1, modeCount = 2 };
static const DISPLAYCONFIG_RATIONAL hostedRefresh = {hostedRefreshRate, 1};
static volatile LONG traceEvents;

static BOOL SupportedQuery(UINT32 flags) {
    return flags == QDC_ALL_PATHS || flags == QDC_ONLY_ACTIVE_PATHS || flags == QDC_DATABASE_CURRENT;
}

static void TraceOnce(LONG event, const char *line, DWORD size) {
    if (!(InterlockedOr(&traceEvents, event) & event)) AppendTrace(line, size);
}

#define TRACE_DISPLAY(event, state) do { \
    static const char line[] = "asset hosted-display=" state "\r\n"; \
    TraceOnce(event, line, sizeof(line) - 1); \
} while (0)

LONG WINAPI XboxHostedDisplaySizes(UINT32 flags, UINT32 *paths, UINT32 *modes) {
    if (!SupportedQuery(flags) || !paths || !modes || paths == modes) return ERROR_INVALID_PARAMETER;
    *paths = pathCount;
    *modes = modeCount;
    TRACE_DISPLAY(1, "one-path-two-modes");
    return ERROR_SUCCESS;
}

LONG WINAPI XboxHostedQueryDisplay(UINT32 flags, UINT32 *paths, DISPLAYCONFIG_PATH_INFO *path,
    UINT32 *modes, DISPLAYCONFIG_MODE_INFO *mode, DISPLAYCONFIG_TOPOLOGY_ID *topology) {
    const BOOL database = flags == QDC_DATABASE_CURRENT;
    if (!SupportedQuery(flags) || !paths || !modes || paths == modes || !path || !mode ||
            database != (topology != NULL)) return ERROR_INVALID_PARAMETER;
    if (*paths < pathCount || *modes < modeCount) return ERROR_INSUFFICIENT_BUFFER;

    const DWORD previous = GetLastError();
    const RECT bounds = HostedDesktopBounds();
    SetLastError(previous);
    const UINT32 width = (UINT32)(bounds.right - bounds.left);
    const UINT32 height = (UINT32)(bounds.bottom - bounds.top);
    *path = (DISPLAYCONFIG_PATH_INFO){
        .sourceInfo = {.adapterId = hostedAdapter, .id = sourceId, .modeInfoIdx = sourceIndex,
            .statusFlags = DISPLAYCONFIG_SOURCE_IN_USE},
        .targetInfo = {.adapterId = hostedAdapter, .id = targetId, .modeInfoIdx = targetIndex,
            .outputTechnology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER, .rotation = DISPLAYCONFIG_ROTATION_IDENTITY,
            .scaling = DISPLAYCONFIG_SCALING_IDENTITY, .refreshRate = hostedRefresh,
            .scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE, .targetAvailable = TRUE,
            .statusFlags = DISPLAYCONFIG_TARGET_IN_USE},
        .flags = DISPLAYCONFIG_PATH_ACTIVE,
    };
    mode[sourceIndex] = (DISPLAYCONFIG_MODE_INFO){
        .infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE, .id = sourceId, .adapterId = hostedAdapter,
        .sourceMode = {.width = width, .height = height, .pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP,
            .position = {bounds.left, bounds.top}},
    };
    mode[targetIndex] = (DISPLAYCONFIG_MODE_INFO){
        .infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET, .id = targetId, .adapterId = hostedAdapter,
        .targetMode = {.targetVideoSignalInfo = {
            .pixelRate = (UINT64)width * height * hostedRefreshRate,
            .hSyncFreq = {height * hostedRefreshRate, 1}, .vSyncFreq = hostedRefresh,
            .activeSize = {width, height}, .totalSize = {database ? 0 : width, database ? 0 : height},
            .scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE,
        }},
    };
    if (database) {
        *topology = DISPLAYCONFIG_TOPOLOGY_EXTERNAL;
        TRACE_DISPLAY(2, "database-query");
    } else {
        TRACE_DISPLAY(4, "desktop-query");
    }
    *paths = pathCount;
    *modes = modeCount;
    return ERROR_SUCCESS;
}

LONG WINAPI XboxHostedDisplayDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_HEADER *header) {
    if (!header) return ERROR_INVALID_PARAMETER;
    if (header->size < sizeof(*header)) return ERROR_INSUFFICIENT_BUFFER;
    if (header->adapterId.LowPart != hostedAdapter.LowPart || header->adapterId.HighPart != hostedAdapter.HighPart)
        return ERROR_INVALID_PARAMETER;
    switch (header->type) {
    case DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME: {
        if (header->id != sourceId) return ERROR_INVALID_PARAMETER;
        if (header->size < sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME)) return ERROR_INSUFFICIENT_BUFFER;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME *name = (DISPLAYCONFIG_SOURCE_DEVICE_NAME *)header;
        CopyDisplayText(name->viewGdiDeviceName, ARRAYSIZE(name->viewGdiDeviceName), hostedDisplayName);
        TRACE_DISPLAY(8, "source-name");
        return ERROR_SUCCESS;
    }
    case DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME: {
        if (header->id != targetId) return ERROR_INVALID_PARAMETER;
        if (header->size < sizeof(DISPLAYCONFIG_TARGET_DEVICE_NAME)) return ERROR_INSUFFICIENT_BUFFER;
        DISPLAYCONFIG_TARGET_DEVICE_NAME *name = (DISPLAYCONFIG_TARGET_DEVICE_NAME *)header;
        name->flags.value = 0;
        name->outputTechnology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
        name->edidManufactureId = 0;
        name->edidProductCodeId = 0;
        name->connectorInstance = 0;
        CopyDisplayText(name->monitorFriendlyDeviceName, ARRAYSIZE(name->monitorFriendlyDeviceName), hostedMonitorName);
        CopyDisplayText(name->monitorDevicePath, ARRAYSIZE(name->monitorDevicePath), L"");
        TRACE_DISPLAY(16, "target-name");
        return ERROR_SUCCESS;
    }
    default:
        return ERROR_NOT_SUPPORTED;
    }
}
