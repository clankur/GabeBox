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
    case WM_APP + 1:
        return wparam == GetCurrentThreadId() && lparam == (LPARAM)&lifecycle ? 0x4e11 : 0;
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
        PostQuitMessage(-9);
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static BOOL VerifyCoordinates(HWND window, LONG x, LONG y) {
    POINT point = {-7, 93};
    SetLastError(ERROR_BUSY);
    return ClientToScreen(window, &point) && point.x == x - 7 && point.y == y + 93 &&
        ScreenToClient(window, &point) && point.x == -7 && point.y == 93 && GetLastError() == ERROR_BUSY;
}

typedef struct SendControl {
    HWND window;
    DWORD owner;
    HANDLE completed;
    LRESULT result;
    BOOL coordinates;
} SendControl;

static void CALLBACK SendFromWorker(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK work) {
    (void)instance;
    (void)work;
    SendControl *send = context;
    RECT client, screen;
    send->coordinates = GetClientRect(send->window, &client) && client.left == 0 && client.top == 0 &&
        client.right == 64 && client.bottom == 64 && GetWindowRect(send->window, &screen) &&
        screen.left == 4 && screen.top == 8 && screen.right == 68 && screen.bottom == 72 &&
        VerifyCoordinates(send->window, 4, 8);
    send->result = SendMessageW(send->window, WM_APP + 1, send->owner, (LPARAM)&lifecycle);
    SetEvent(send->completed);
}

static BOOL VerifySend(HWND window) {
    const LONG style = GetWindowLongA(window, GWL_STYLE);
    const LONG resizing = style | WS_THICKFRAME;
    if (GetParent(window) || SetWindowLongA(window, GWL_STYLE, resizing) != style ||
            GetWindowLongPtrW(window, GWL_STYLE) != resizing || SetWindowLongA(window, GWL_STYLE, style) != resizing)
        return FALSE;
    const LPARAM icon = 0x1234;
    if (SendMessageW(window, WM_SETICON, ICON_SMALL, icon) || SendMessageW(window, WM_GETICON, ICON_SMALL, 0) != icon ||
            SendMessageW(window, WM_SETICON, ICON_SMALL, 0) != icon || SendMessageW(window, WM_GETICON, ICON_SMALL, 0))
        return FALSE;
    if (!PostMessageW(window, WM_APP, 7, 42)) return FALSE;

    SendControl send = {window, GetCurrentThreadId(),
        CreateEventExW(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE), 0, FALSE};
    if (!send.completed) return FALSE;
    PTP_WORK work = CreateThreadpoolWork(SendFromWorker, &send, NULL);
    if (!work) { CloseHandle(send.completed); return FALSE; }
    SubmitThreadpoolWork(work);
    MSG event;
    const ULONGLONG deadline = GetTickCount64() + 2000;
    DWORD wait = WAIT_TIMEOUT;
    BOOL sent = FALSE, posted = FALSE;
    for (ULONGLONG now = GetTickCount64(); now < deadline; now = GetTickCount64()) {
        wait = MsgWaitForMultipleObjects(1, &send.completed, FALSE, (DWORD)(deadline - now), QS_SENDMESSAGE);
        if (wait != WAIT_OBJECT_0 + 1) break;
        sent = TRUE;
        if (PeekMessageA(&event, NULL, 0, 0, PM_REMOVE | PM_QS_SENDMESSAGE)) posted = TRUE;
    }
    if (wait != WAIT_OBJECT_0) PeekMessageA(&event, NULL, 0, 0, PM_REMOVE | PM_QS_SENDMESSAGE);
    if (WaitForSingleObject(send.completed, 2000) != WAIT_OBJECT_0) ExitProcess(0x6e1b);
    WaitForThreadpoolWorkCallbacks(work, FALSE);
    CloseThreadpoolWork(work);
    CloseHandle(send.completed);
    if (wait != WAIT_OBJECT_0 || !sent || posted || !send.coordinates || send.result != 0x4e11 ||
            !PeekMessageW(&event, window, WM_APP, WM_APP, PM_REMOVE)) return FALSE;
    DispatchMessageW(&event);
    return TRUE;
}

static unsigned int TryGraphics(void) {
    typedef HGLRC (WINAPI *Create)(HDC);
    typedef BOOL (WINAPI *MakeCurrent)(HDC, HGLRC);
    typedef BOOL (WINAPI *Delete)(HGLRC);
    typedef PROC (WINAPI *Resolve)(LPCSTR);
    typedef HGLRC (WINAPI *CurrentContext)(void);
    typedef HDC (WINAPI *CurrentDC)(void);
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
    if (!VerifySend(window)) return 0x6e1b;
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
    const RECT overlap = {-8, -8, 8, 8};
    const RECT outside = {-32, -32, -16, -16};
    if (MonitorFromRect(&overlap, MONITOR_DEFAULTTONULL) != monitor ||
            MonitorFromRect(&outside, MONITOR_DEFAULTTONULL) ||
            MonitorFromRect(&outside, MONITOR_DEFAULTTONEAREST) != monitor ||
            MonitorFromRect(&outside, MONITOR_DEFAULTTOPRIMARY) != monitor) return 0x6e14;

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
#ifdef PROBE_WGL_LOADER
    if (!SetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_WGL", L"1")) return 0x6e1b;
    PIXELFORMATDESCRIPTOR described;
    if (!dc || GetPixelFormat(dc) || DescribePixelFormat(dc, 0, 0, NULL) != 1 ||
            DescribePixelFormat(dc, 1, sizeof(described), &described) != 1 ||
            described.nSize != sizeof(described) || described.cColorBits != 24 || described.cAlphaBits != 8 ||
            (described.dwFlags & format.dwFlags) != format.dwFlags) return 0x6e1c;
#endif
    int index = dc ? ChoosePixelFormat(dc, &format) : 0;
    if (!index || !SetPixelFormat(dc, index, &format)) return 0x6e050000u | (GetLastError() & 0xffffu);
#ifdef PROBE_WGL_LOADER
    if (GetPixelFormat(dc) != index) return 0x6e1d;
    HMODULE gl = LoadLibraryW(L"OPENGL32.DLL");
#else
    HMODULE gl = LoadLibraryW(L"opengl32.dll");
#endif
    if (!gl) return 0x6e060000u | (GetLastError() & 0xffffu);
    Create create = (Create)(void *)GetProcAddress(gl, "wglCreateContext");
    MakeCurrent makeCurrent = (MakeCurrent)(void *)GetProcAddress(gl, "wglMakeCurrent");
    Delete destroy = (Delete)(void *)GetProcAddress(gl, "wglDeleteContext");
    Resolve resolve = (Resolve)(void *)GetProcAddress(gl, "wglGetProcAddress");
    CurrentContext currentContext = (CurrentContext)(void *)GetProcAddress(gl, "wglGetCurrentContext");
    CurrentDC currentDC = (CurrentDC)(void *)GetProcAddress(gl, "wglGetCurrentDC");
#ifdef PROBE_WGL_LOADER
    if (!create || !makeCurrent || !destroy || !resolve || !currentContext || !currentDC) return 0x6e07;
    HMODULE implementation = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(void *)resolve, &implementation) ||
            implementation != GetModuleHandleW(L"XboxWin32Window.dll")) return 0x6e1e;
    if (GetProcAddress(gl, "wglShareLists") || GetLastError() != ERROR_PROC_NOT_FOUND) return 0x6e20;
#else
    if (create != wglCreateContext || makeCurrent != wglMakeCurrent || destroy != wglDeleteContext ||
            resolve != wglGetProcAddress || currentContext != wglGetCurrentContext || currentDC != wglGetCurrentDC)
        return 0x6e07;
#endif
    HGLRC legacy = create(dc);
    if (!legacy || !makeCurrent(dc, legacy) || currentContext() != legacy || currentDC() != dc)
        return 0x6e080000u | (GetLastError() & 0xffffu);
    CreateAttributes createAttributes = (CreateAttributes)(void *)resolve("wglCreateContextAttribsARB");
    if (!createAttributes) return 0x6e09;
#ifdef PROBE_WGL_LOADER
    const int attributes[] = {0x2091, 3, 0x2092, 3, 0x9126, 1, 0};
#else
    const int attributes[] = {0x2091, 3, 0x2092, 3, 0x9126, 1, 0x2094, 2, 0};
#endif
    HGLRC modern = createAttributes(dc, NULL, attributes);
    if (!modern || !makeCurrent(NULL, NULL) || !destroy(legacy) || !makeCurrent(dc, modern))
        return 0x6e0a0000u | (GetLastError() & 0xffffu);
#ifdef PROBE_WGL_LOADER
    typedef void (WINAPI *GetInteger)(GLenum, GLint *);
    GetInteger query = (GetInteger)(void *)resolve("glGetIntegerv");
    GLint major = 0, minor = 0, profile = 0;
    if (!query) return 0x6e1f;
    query(0x821b, &major); /* GL_MAJOR_VERSION */
    query(0x821c, &minor); /* GL_MINOR_VERSION */
    query(0x9126, &profile); /* GL_CONTEXT_PROFILE_MASK */
    if ((major < 3 || (major == 3 && minor < 3)) || !(profile & 1)) return 0x6e1f;
#endif
    if (!resolve("glBindFramebuffer") || !SetWindowPos(window, NULL, 0, 0, 80, 48, SWP_NOMOVE | SWP_NOZORDER) ||
            lifecycle != 7) return 0x6e0b;
    if (!VerifyCoordinates(window, 4, 8) ||
            !SetWindowPos(window, NULL, -24, 49, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) ||
            !VerifyCoordinates(window, -24, 49)) return 0x6e1a;

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
