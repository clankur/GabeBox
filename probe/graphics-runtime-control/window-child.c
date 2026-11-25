#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include "../../runtime/shared/child-process.h"

int _fltused;

static unsigned int lifecycle;
static unsigned int activated, deactivated;
static unsigned int nonclientActivated, nonclientDeactivated;
static HWND activationRedirect;

static BOOL CALLBACK OnMonitor(HMONITOR monitor, HDC dc, RECT *bounds, LPARAM data) {
    if (dc || !monitor || bounds->right <= 0 || bounds->bottom <= 0) return FALSE;
    *(HMONITOR *)data = monitor;
    return TRUE;
}

static LRESULT CALLBACK OnMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        if (((const CREATESTRUCTW *)lparam)->lpCreateParams == &lifecycle) lifecycle |= 1;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)&lifecycle);
        break;
    case WM_APP:
        if (wparam == 7 && lparam == 42 && GetWindowLongPtrW(window, GWLP_USERDATA) == (LONG_PTR)&lifecycle)
            lifecycle |= 2;
        break;
    case WM_SIZE:
        if (LOWORD(lparam) == 80 && HIWORD(lparam) == 48) lifecycle |= 4;
        break;
    case WM_ACTIVATEAPP:
        if (wparam) ++activated;
        else ++deactivated;
        break;
    case WM_NCACTIVATE:
        if (wparam) ++nonclientActivated;
        else ++nonclientDeactivated;
        break;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE && activationRedirect) {
            HWND next = activationRedirect;
            activationRedirect = NULL;
            SetForegroundWindow(next);
        }
        break;
    case WM_DESTROY:
        lifecycle |= 8;
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static unsigned int TryGraphics(void) {
    typedef HGLRC (WINAPI *Create)(HDC);
    typedef BOOL (WINAPI *MakeCurrent)(HDC, HGLRC);
    typedef BOOL (WINAPI *Delete)(HGLRC);
    typedef PROC (WINAPI *Resolve)(LPCSTR);
    typedef HGLRC (WINAPI *CreateAttributes)(HDC, HGLRC, const int *);
    const HINSTANCE image = GetModuleHandleW(NULL);
    const WNDCLASSEXW windowClass = {sizeof(windowClass), CS_OWNDC, OnMessage, 0, 0, image, NULL, NULL,
        NULL, NULL, L"Win32GraphicsControl", NULL};
    const WNDCLASSW probeClass = {0, OnMessage, 0, 0, image, NULL, NULL, NULL, NULL, L"Win32ProbeClass"};
    if (!RegisterClassW(&probeClass) || !UnregisterClassW(probeClass.lpszClassName, image)) return 0x6e0e;
    if (!RegisterClassExW(&windowClass)) return 0x6e010000u | (GetLastError() & 0xffffu);
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"Win32 graphics control", 0,
        4, 8, 64, 64, NULL, NULL, NULL, &lifecycle);
    if (!window || lifecycle != 1) return 0x6e020000u | (GetLastError() & 0xffffu);
    MSG event;
    if (!PostMessageW(window, WM_APP, 7, 42) || !PeekMessageW(&event, window, WM_APP, WM_APP, PM_REMOVE)) return 0x6e03;
    DispatchMessageW(&event);
    if (!PostMessageW(window, WM_APP, 7, 42) || !PeekMessageA(&event, window, WM_APP, WM_APP, PM_REMOVE)) return 0x6e03;
    DispatchMessageA(&event);
    if (!SetWindowPos(window, NULL, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE) ||
            !IsWindowVisible(window) || activated || deactivated || nonclientActivated || nonclientDeactivated) return 0x6e12;
    ShowWindow(window, SW_SHOW);
    ShowWindow(window, SW_SHOW);
    if (activated != 1 || deactivated || nonclientActivated != 1 || nonclientDeactivated ||
            GetActiveWindow() != window) return 0x6e13;
    RECT area;
    if (lifecycle != 3 || !IsWindowVisible(window) || !GetClientRect(window, &area) || area.right != 64 ||
            !GetWindowRect(window, &area) || area.left != 4 || area.top != 8) return 0x6e04;
    POINT point = {-6, -10};
    SetLastError(ERROR_RETRY);
    if (!ClientToScreen(window, &point) || point.x != -2 || point.y != -2 ||
            !ScreenToClient(window, &point) || point.x != -6 || point.y != -10 ||
            GetLastError() != ERROR_RETRY) return 0x6e17;
    const UINT moveFlags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE;
    if (!SetWindowPos(window, NULL, -12, 23, 0, 0, moveFlags) ||
            !ClientToScreen(window, &point) || point.x != -18 || point.y != 13 ||
            !SetWindowPos(window, NULL, 4, 8, 0, 0, moveFlags)) return 0x6e18;
    point = (POINT){MAXLONG, 3};
    if (ClientToScreen(window, &point) || GetLastError() != ERROR_ARITHMETIC_OVERFLOW ||
            point.x != MAXLONG || point.y != 3) return 0x6e19;
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    HMONITOR enumerated = NULL;
    MONITORINFOEXW wide;
    MONITORINFOEXA ansi;
    DEVMODEW mode;
    wide.cbSize = sizeof(wide);
    ansi.cbSize = sizeof(ansi);
    mode.dmSize = sizeof(mode);
    if (!monitor || !EnumDisplayMonitors(NULL, NULL, OnMonitor, (LPARAM)&enumerated) || enumerated != monitor ||
            !GetMonitorInfoW(monitor, (MONITORINFO *)&wide) || !GetMonitorInfoA(monitor, (MONITORINFO *)&ansi) ||
            !EnumDisplaySettingsW(wide.szDevice, ENUM_CURRENT_SETTINGS, &mode) ||
            mode.dmPelsWidth != (DWORD)ansi.rcMonitor.right || mode.dmPelsHeight != (DWORD)ansi.rcMonitor.bottom ||
            mode.dmDisplayFrequency != 60) return 0x6e0f;

    DISPLAY_DEVICEW adapter, display;
    adapter.cb = sizeof(adapter);
    display.cb = sizeof(display);
    if (!EnumDisplayDevicesW(NULL, 0, &adapter, 0) || !(adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ||
            !EnumDisplayDevicesW(adapter.DeviceName, 0, &display, 0) || !(display.StateFlags & DISPLAY_DEVICE_ACTIVE) ||
            EnumDisplayDevicesW(adapter.DeviceName, 1, &display, 0) || EnumDisplayDevicesW(NULL, 1, &adapter, 0) ||
            MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTONULL) != monitor) return 0x6e10;
    const HDC displayDc = CreateDCW(L"DISPLAY", adapter.DeviceName, NULL, NULL);
    const HDC desktopDc = GetDC(NULL);
    if (!displayDc || !desktopDc || GetDeviceCaps(displayDc, HORZRES) != GetSystemMetrics(SM_CXSCREEN) ||
            GetDeviceCaps(displayDc, VERTRES) != GetSystemMetrics(SM_CYSCREEN) ||
            GetDeviceCaps(desktopDc, LOGPIXELSX) != 96 || GetDeviceCaps(desktopDc, VREFRESH) != 60 ||
            !DeleteDC(displayDc) || !ReleaseDC(NULL, desktopDc)) return 0x6e11;

    HDC dc = GetDC(window);
    const PIXELFORMATDESCRIPTOR format = {sizeof(format), 1, PFD_SUPPORT_OPENGL | PFD_DRAW_TO_WINDOW | PFD_DOUBLEBUFFER,
        PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 24, 8, 0, PFD_MAIN_PLANE, 0, 0, 0, 0};
    int index = dc ? ChoosePixelFormat(dc, &format) : 0;
    if (!index || !SetPixelFormat(dc, index, &format)) return 0x6e050000u | (GetLastError() & 0xffffu);
    HMODULE gl = LoadLibraryW(L"opengl32.dll");
    if (!gl) return 0x6e060000u | (GetLastError() & 0xffffu);
    Create create = (Create)(void *)GetProcAddress(gl, "wglCreateContext");
    MakeCurrent makeCurrent = (MakeCurrent)(void *)GetProcAddress(gl, "wglMakeCurrent");
    Delete destroy = (Delete)(void *)GetProcAddress(gl, "wglDeleteContext");
    Resolve resolve = (Resolve)(void *)GetProcAddress(gl, "wglGetProcAddress");
    if (!create || !makeCurrent || !destroy || !resolve) return 0x6e07;
    HGLRC legacy = create(dc);
    if (!legacy || !makeCurrent(dc, legacy)) return 0x6e080000u | (GetLastError() & 0xffffu);
    CreateAttributes createAttributes = (CreateAttributes)(void *)resolve("wglCreateContextAttribsARB");
    if (!createAttributes) return 0x6e09;
    const int attributes[] = {0x2091, 3, 0x2092, 3, 0x9126, 1, 0x2094, 2, 0};
    HGLRC modern = createAttributes(dc, NULL, attributes);
    if (!modern || !makeCurrent(NULL, NULL) || !destroy(legacy) || !makeCurrent(dc, modern))
        return 0x6e0a0000u | (GetLastError() & 0xffffu);
    if (!resolve("glBindFramebuffer") || !SetWindowPos(window, NULL, 0, 0, 80, 48, SWP_NOMOVE | SWP_NOZORDER) ||
            lifecycle != 7) return 0x6e0b;

    unsigned char pixel[4] = {0};
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(79, 47, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (glGetError() || pixel[0] != 255 || pixel[1] != 0 || pixel[2] != 255 || pixel[3] != 255 ||
            !SwapBuffers(dc)) return 0x6e0c;
    if (!makeCurrent(NULL, NULL) || !destroy(modern) || !ReleaseDC(window, dc)) return 0x6e0d;
    HWND next = CreateWindowExW(0, windowClass.lpszClassName, L"Second graphics window", 0,
        4, 8, 64, 64, NULL, NULL, NULL, NULL);
    if (!next) return 0x6e15;
    ShowWindow(next, SW_SHOWNA);
    activationRedirect = next;
    if (!DestroyWindow(window) || activationRedirect || activated != 1 || deactivated ||
            GetForegroundWindow() != next || GetActiveWindow() != next) return 0x6e16;
    if (ScreenToClient(window, &point) || GetLastError() != ERROR_INVALID_WINDOW_HANDLE ||
            point.x != MAXLONG || point.y != 3) return 0x6e1a;
    if (!DestroyWindow(next) || lifecycle != 15 ||
            !UnregisterClassW(windowClass.lpszClassName, image)) return 0x6e0d;
    if (activated != 1 || deactivated != 1 || nonclientActivated != 2 || nonclientDeactivated != 2) return 0x6e14;
    return PROBE_EXPECTED_CHILD_EXIT;
}

void WINAPI ChildEntry(void) {
    ExitProcess(TryGraphics());
}
