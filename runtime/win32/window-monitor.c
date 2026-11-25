#include "window.h"
#include "hosted-display.h"

#define XBOX_MONITOR ((HMONITOR)(ULONG_PTR)0x58424D000001ULL)

static BOOL SameDisplayName(LPCWSTR name) {
    return name && CompareStringOrdinal(name, -1, hostedDisplayName, -1, TRUE) == CSTR_EQUAL;
}

RECT XboxDesktopBounds(void) {
    return HostedDesktopBounds();
}

HMONITOR WINAPI XboxMonitorFromWindow(HWND handle, DWORD flags) {
    if (handle && XboxFindWindow(handle)) return XBOX_MONITOR;
    return (flags & (MONITOR_DEFAULTTOPRIMARY | MONITOR_DEFAULTTONEAREST)) ? XBOX_MONITOR : NULL;
}

HMONITOR WINAPI XboxMonitorFromPoint(POINT point, DWORD flags) {
    const RECT bounds = XboxDesktopBounds();
    const BOOL within = point.x >= bounds.left && point.x < bounds.right && point.y >= bounds.top && point.y < bounds.bottom;
    return within || (flags & (MONITOR_DEFAULTTOPRIMARY | MONITOR_DEFAULTTONEAREST)) ? XBOX_MONITOR : NULL;
}

HMONITOR WINAPI XboxMonitorFromRect(const RECT *area, DWORD flags) {
    if (!area || (flags != MONITOR_DEFAULTTONULL && flags != MONITOR_DEFAULTTOPRIMARY &&
            flags != MONITOR_DEFAULTTONEAREST)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (flags != MONITOR_DEFAULTTONULL) return XBOX_MONITOR;
    const RECT bounds = XboxDesktopBounds();
    return area->left < area->right && area->top < area->bottom &&
        area->right > bounds.left && area->bottom > bounds.top &&
        area->left < bounds.right && area->top < bounds.bottom ? XBOX_MONITOR : NULL;
}

BOOL WINAPI XboxEnumDisplayDevicesW(LPCWSTR adapter, DWORD index, DISPLAY_DEVICEW *device, DWORD flags) {
    if (!device || device->cb != sizeof(*device)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (index || flags || (adapter && !SameDisplayName(adapter))) return FALSE;
    CopyDisplayText(device->DeviceName, ARRAYSIZE(device->DeviceName), adapter ? L"\\\\.\\DISPLAY1\\Monitor0" : hostedDisplayName);
    CopyDisplayText(device->DeviceString, ARRAYSIZE(device->DeviceString), adapter ? hostedMonitorName : L"Xbox display adapter");
    CopyDisplayText(device->DeviceID, ARRAYSIZE(device->DeviceID), L"");
    CopyDisplayText(device->DeviceKey, ARRAYSIZE(device->DeviceKey), L"");
    device->StateFlags = adapter ? DISPLAY_DEVICE_ACTIVE | DISPLAY_DEVICE_ATTACHED :
        DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
    return TRUE;
}

int WINAPI XboxGetSystemMetrics(int index) {
    const RECT bounds = XboxDesktopBounds();
    switch (index) {
    case SM_CXSCREEN: case SM_CXVIRTUALSCREEN: return bounds.right;
    case SM_CYSCREEN: case SM_CYVIRTUALSCREEN: return bounds.bottom;
    case SM_CMONITORS: return 1;
    default: return 0;
    }
}

HDC WINAPI XboxCreateDCW(LPCWSTR driver, LPCWSTR device, LPCWSTR port, const DEVMODEW *mode) {
    if ((!driver || CompareStringOrdinal(driver, -1, L"DISPLAY", -1, TRUE) == CSTR_EQUAL || SameDisplayName(driver)) &&
            (!device || SameDisplayName(device)) && !port && !mode) return XBOX_DISPLAY_DC;
    SetLastError(ERROR_NOT_SUPPORTED);
    return NULL;
}

BOOL WINAPI XboxDeleteDC(HDC dc) {
    if (dc == XBOX_DISPLAY_DC) return TRUE;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

int WINAPI XboxGetDeviceCaps(HDC dc, int index) {
    if (dc != XBOX_DESKTOP_DC && dc != XBOX_DISPLAY_DC && !XboxFindWindow((HWND)dc)) return 0;
    const RECT bounds = XboxDesktopBounds();
    switch (index) {
    case TECHNOLOGY: return DT_RASDISPLAY;
    case HORZRES: case DESKTOPHORZRES: return bounds.right;
    case VERTRES: case DESKTOPVERTRES: return bounds.bottom;
    case LOGPIXELSX: case LOGPIXELSY: return 96;
    case BITSPIXEL: return 32;
    case PLANES: return 1;
    case VREFRESH: return hostedRefreshRate;
    default: return 0;
    }
}

BOOL WINAPI XboxEnumDisplayMonitors(HDC dc, const RECT *clip, MONITORENUMPROC callback, LPARAM data) {
    if (dc || !callback) {
        SetLastError(dc ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    RECT bounds = XboxDesktopBounds();
    if (clip && (clip->right <= bounds.left || clip->bottom <= bounds.top ||
                 clip->left >= bounds.right || clip->top >= bounds.bottom)) return TRUE;
    return callback(XBOX_MONITOR, NULL, &bounds, data);
}

static BOOL MonitorInfo(HMONITOR monitor, MONITORINFO *info) {
    if (!monitor || !info || info->cbSize < sizeof(*info)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    RECT bounds;
    if (monitor == XBOX_MONITOR) {
        bounds = XboxDesktopBounds();
    } else {
        // Native DXGI enumerates outputs even when the game uses a hosted
        // window. Translate only a monitor identity verified by that factory.
        typedef BOOL (WINAPI *GetOutputMonitorRect)(HMONITOR, RECT *);
        const HMODULE dxgi = GetModuleHandleW(L"XboxWin32Dxgi.dll");
        const GetOutputMonitorRect query = dxgi ?
            (GetOutputMonitorRect)(void *)GetProcAddress(dxgi, "XboxGetOutputMonitorRect") : NULL;
        if (!query || !query(monitor, &bounds)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top)
            bounds = XboxDesktopBounds();
    }
    info->rcMonitor = bounds;
    info->rcWork = info->rcMonitor;
    info->dwFlags = MONITORINFOF_PRIMARY;
    return TRUE;
}

BOOL WINAPI XboxGetMonitorInfoW(HMONITOR monitor, MONITORINFO *info) {
    if (!MonitorInfo(monitor, info)) return FALSE;
    if (info->cbSize >= sizeof(MONITORINFOEXW)) {
        MONITORINFOEXW *extended = (MONITORINFOEXW *)info;
        CopyDisplayText(extended->szDevice, ARRAYSIZE(extended->szDevice), hostedDisplayName);
    }
    return TRUE;
}

BOOL WINAPI XboxGetMonitorInfoA(HMONITOR monitor, MONITORINFO *info) {
    if (!MonitorInfo(monitor, info)) return FALSE;
    if (info->cbSize >= sizeof(MONITORINFOEXA)) {
        const char name[] = "\\\\.\\DISPLAY1";
        MONITORINFOEXA *extended = (MONITORINFOEXA *)info;
        for (UINT i = 0; i < ARRAYSIZE(name); ++i) extended->szDevice[i] = name[i];
    }
    return TRUE;
}

BOOL WINAPI XboxEnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW *settings) {
    if (!settings || settings->dmSize < sizeof(*settings) ||
            (device && !SameDisplayName(device))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (mode != 0 && mode != ENUM_CURRENT_SETTINGS && mode != ENUM_REGISTRY_SETTINGS) return FALSE;
    const RECT bounds = XboxDesktopBounds();
    CopyDisplayText(settings->dmDeviceName, ARRAYSIZE(settings->dmDeviceName), hostedDisplayName);
    settings->dmFields = DM_POSITION | DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
    settings->dmPosition.x = 0;
    settings->dmPosition.y = 0;
    settings->dmBitsPerPel = 32;
    settings->dmPelsWidth = bounds.right;
    settings->dmPelsHeight = bounds.bottom;
    settings->dmDisplayFlags = 0;
    settings->dmDisplayFrequency = hostedRefreshRate;
    return TRUE;
}
