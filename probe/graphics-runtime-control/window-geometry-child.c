#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../runtime/shared/child-process.h"

static UINT messages[8];
static UINT messageCount;
static WINDOWPOS committed;
static RECT client;
static POINT origin;
static LPARAM moved, resized;
static WPARAM resizeState;
static BOOL adjusted, suppressDefault, destroyBeforeChange, vetoRestore, restoreOnMove, restoreOnChanged, maximizeOnShow;
static BOOL sawGeometry, wasIconic, wasZoomed;
static HWND createdParent;
static WPARAM helperMessage;
static BOOL refuseCreation;

static LRESULT CALLBACK OnMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        createdParent = ((const CREATESTRUCTW *)lparam)->hwndParent;
        if (refuseCreation) return FALSE;
    }
    if (message == WM_APP) helperMessage = wparam;
    if (message == WM_APP + 1) return (LRESULT)wparam + lparam;
    switch (message) {
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED:
    case WM_SHOWWINDOW:
    case WM_MOVE:
    case WM_SIZE:
    case WM_QUERYOPEN:
        if (messageCount < ARRAYSIZE(messages)) messages[messageCount] = message;
        ++messageCount;
        break;
    }
    if (message == WM_WINDOWPOSCHANGING) {
        if (destroyBeforeChange) {
            DestroyWindow(window);
            return 0;
        }
        if (adjusted) {
            WINDOWPOS *position = (WINDOWPOS *)lparam;
            position->x = -24;
            position->cx = 1920;
            position->flags &= ~SWP_NOACTIVATE;
        }
    }
    if (message == WM_WINDOWPOSCHANGED) {
        committed = *(const WINDOWPOS *)lparam;
        origin = (POINT){0, 0};
        sawGeometry = GetClientRect(window, &client) && ClientToScreen(window, &origin);
        wasIconic = IsIconic(window);
        wasZoomed = IsZoomed(window);
        if (restoreOnChanged) {
            restoreOnChanged = FALSE;
            ShowWindow(window, SW_RESTORE);
        }
        if (suppressDefault) return 0;
    }
    if (message == WM_MOVE) {
        moved = lparam;
        if (restoreOnMove) {
            restoreOnMove = FALSE;
            ShowWindow(window, SW_RESTORE);
        }
    }
    if (message == WM_SIZE) {
        resized = lparam;
        resizeState = wparam;
    }
    if (message == WM_QUERYOPEN && vetoRestore) return FALSE;
    if (message == WM_SHOWWINDOW && wparam && maximizeOnShow) {
        maximizeOnShow = FALSE;
        ShowWindow(window, SW_MAXIMIZE);
    }
    return CallWindowProcW(DefWindowProcW, window, message, wparam, lparam);
}

static BOOL VerifyMessageWindow(LPCWSTR name, HINSTANCE instance) {
    const HWND foreground = GetForegroundWindow();
    const HWND helper = CreateWindowExW(0, name, L"Message helper", WS_OVERLAPPED,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, HWND_MESSAGE, NULL, instance, NULL);
    RECT client;
    MSG message;
    if (!helper || createdParent != HWND_MESSAGE || !GetClientRect(helper, &client) || client.right || client.bottom ||
            ShowWindow(helper, SW_SHOW) || (GetWindowLongW(helper, GWL_STYLE) & WS_VISIBLE) ||
            SetForegroundWindow(helper) || GetForegroundWindow() != foreground || GetDC(helper)) return FALSE;
    if (!PostMessageW(helper, WM_APP, 42, 0) || !PeekMessageW(&message, helper, WM_APP, WM_APP, PM_REMOVE) ||
            message.hwnd != helper) return FALSE;
    DispatchMessageW(&message);
    const WPARAM token = (WPARAM)0x1a33f005dULL;
    SetLastError(ERROR_RETRY);
    if (helperMessage != 42 || CallWindowProcW(OnMessage, helper, WM_APP + 1, token, -5) != (LRESULT)token - 5 ||
            GetLastError() != ERROR_RETRY || !DestroyWindow(helper) ||
            CallWindowProcW(OnMessage, helper, WM_APP + 1, token, -5) ||
            GetLastError() != ERROR_INVALID_WINDOW_HANDLE) return FALSE;
    refuseCreation = TRUE;
    const HWND refused = CreateWindowExW(0, name, L"Refused message helper", WS_OVERLAPPED,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, HWND_MESSAGE, NULL, instance, NULL);
    refuseCreation = FALSE;
    return !refused;
}

static BOOL Matches(const UINT *expected, UINT count) {
    if (messageCount != count || count > ARRAYSIZE(messages)) return FALSE;
    for (UINT index = 0; index < count; ++index) {
        if (messages[index] != expected[index]) return FALSE;
    }
    return TRUE;
}

static BOOL Geometry(HWND window, int x, int y, int width, int height) {
    return sawGeometry && committed.hwnd == window && committed.x == x && committed.y == y &&
        committed.cx == width && committed.cy == height && origin.x == x && origin.y == y &&
        !client.left && !client.top && client.right == width && client.bottom == height;
}

static unsigned int VerifyGeometry(void) {
    const HINSTANCE module = GetModuleHandleW(NULL);
    const WNDCLASSW windowClass = {0, OnMessage, 0, 0, module, NULL, NULL, NULL, NULL, L"HostedGeometryControl"};
    if (!RegisterClassW(&windowClass)) return 0x75300000u | (GetLastError() & 0xffffu);
    const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"Geometry", WS_OVERLAPPEDWINDOW,
        32, 24, 1280, 720, NULL, NULL, module, NULL);
    if (!window) return 0x75310000u | (GetLastError() & 0xffffu);
    if (SetWindowLongW(window, GWL_STYLE, (LONG)WS_POPUP) != WS_OVERLAPPEDWINDOW ||
            GetWindowLongW(window, GWL_STYLE) != (LONG)WS_POPUP ||
            SetWindowLongW(window, GWLP_WNDPROC, 0) || GetLastError() != ERROR_INVALID_INDEX) return 0x75320000u;

    const UINT visibility[] = {WM_SHOWWINDOW, WM_WINDOWPOSCHANGED};
    messageCount = 0;
    if (ShowWindow(window, SW_SHOWNA) || !Matches(visibility, ARRAYSIZE(visibility)) ||
            !(committed.flags & SWP_SHOWWINDOW) || !Geometry(window, 32, 24, 1280, 720)) return 0x75330000u;

    const UINT moveAndSize[] = {WM_WINDOWPOSCHANGING, WM_WINDOWPOSCHANGED, WM_MOVE, WM_SIZE};
    messageCount = 0;
    adjusted = TRUE;
    if (!SetWindowPos(window, NULL, 0, 0, 1919, 1080, SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED))
        return 0x75340000u | (GetLastError() & 0xffffu);
    adjusted = FALSE;
    if (!Matches(moveAndSize, ARRAYSIZE(moveAndSize)) || !(committed.flags & SWP_NOACTIVATE) ||
            !Geometry(window, -24, 0, 1920, 1080) || moved != MAKELPARAM(-24, 0) ||
            resized != MAKELPARAM(1920, 1080)) return 0x75350000u;

    const UINT changed[] = {WM_WINDOWPOSCHANGED};
    messageCount = 0;
    suppressDefault = TRUE;
    const UINT skipChanging = SWP_NOSENDCHANGING | SWP_NOACTIVATE | SWP_NOZORDER;
    if (!SetWindowPos(window, NULL, 17, 9, 1280, 720, skipChanging) ||
            !Matches(changed, ARRAYSIZE(changed)) || !Geometry(window, 17, 9, 1280, 720)) return 0x75360000u;
    suppressDefault = FALSE;

    messageCount = 0;
    if (!SetWindowPos(window, NULL, 0, 0, 0, 0, skipChanging | SWP_NOMOVE | SWP_NOSIZE) || messageCount)
        return 0x75370000u;
    if (!SetWindowPos(window, NULL, 0, 0, 0, 0, skipChanging | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW) ||
            !Matches(visibility, ARRAYSIZE(visibility)) || !(committed.flags & SWP_HIDEWINDOW) ||
            !Geometry(window, 17, 9, 1280, 720)) return 0x75380000u;

    messageCount = 0;
    if (ShowWindow(window, SW_MAXIMIZE) || !Matches(moveAndSize, ARRAYSIZE(moveAndSize)) ||
            !Geometry(window, 0, 0, 1920, 1080) || !wasZoomed || wasIconic ||
            resizeState != SIZE_MAXIMIZED || resized != MAKELPARAM(1920, 1080)) return 0x753a0000u;
    messageCount = 0;
    if (!ShowWindow(window, SW_MAXIMIZE) || messageCount) return 0x753b0000u;

    const UINT stateOnly[] = {WM_WINDOWPOSCHANGING, WM_WINDOWPOSCHANGED, WM_SIZE};
    if (!ShowWindow(window, SW_MINIMIZE) || !Matches(stateOnly, ARRAYSIZE(stateOnly)) ||
            !Geometry(window, 0, 0, 1920, 1080) || wasZoomed || !wasIconic ||
            resizeState != SIZE_MINIMIZED || resized || GetForegroundWindow() == window) return 0x753c0000u;
    messageCount = 0;
    vetoRestore = TRUE;
    const UINT queryOpen[] = {WM_QUERYOPEN};
    if (!ShowWindow(window, SW_RESTORE) || !Matches(queryOpen, ARRAYSIZE(queryOpen)) ||
            !IsIconic(window) || IsZoomed(window)) return 0x753d0000u;
    messageCount = 0;
    vetoRestore = FALSE;
    const UINT reopen[] = {WM_QUERYOPEN, WM_WINDOWPOSCHANGING, WM_WINDOWPOSCHANGED, WM_SIZE};
    if (!ShowWindow(window, SW_RESTORE) || !Matches(reopen, ARRAYSIZE(reopen)) ||
            !Geometry(window, 0, 0, 1920, 1080) || !wasZoomed || wasIconic ||
            resizeState != SIZE_MAXIMIZED) return 0x753e0000u;
    messageCount = 0;
    if (!ShowWindow(window, SW_RESTORE) || !Matches(moveAndSize, ARRAYSIZE(moveAndSize)) ||
            !Geometry(window, 17, 9, 1280, 720) || wasZoomed || wasIconic ||
            resizeState != SIZE_RESTORED || resized != MAKELPARAM(1280, 720)) return 0x753f0000u;

    messageCount = 0;
    restoreOnMove = TRUE;
    const UINT reentered[] = {WM_WINDOWPOSCHANGING, WM_WINDOWPOSCHANGED, WM_MOVE,
        WM_WINDOWPOSCHANGING, WM_WINDOWPOSCHANGED, WM_MOVE, WM_SIZE};
    if (!ShowWindow(window, SW_MAXIMIZE) || !Matches(reentered, ARRAYSIZE(reentered)) ||
            !Geometry(window, 17, 9, 1280, 720) || wasZoomed || wasIconic ||
            resizeState != SIZE_RESTORED || resized != MAKELPARAM(1280, 720)) return 0x75400000u;

    messageCount = 0;
    restoreOnChanged = TRUE;
    const UINT nestedPosition[] = {WM_WINDOWPOSCHANGING, WM_WINDOWPOSCHANGED,
        WM_WINDOWPOSCHANGING, WM_WINDOWPOSCHANGED, WM_MOVE, WM_SIZE};
    if (!ShowWindow(window, SW_MAXIMIZE) || !Matches(nestedPosition, ARRAYSIZE(nestedPosition)) ||
            !Geometry(window, 17, 9, 1280, 720) || wasZoomed || wasIconic ||
            resizeState != SIZE_RESTORED || resized != MAKELPARAM(1280, 720)) return 0x75410000u;

    ShowWindow(window, SW_HIDE);
    messageCount = 0;
    maximizeOnShow = TRUE;
    const UINT nestedShow[] = {WM_SHOWWINDOW, WM_WINDOWPOSCHANGING, WM_WINDOWPOSCHANGED, WM_MOVE, WM_SIZE,
        WM_WINDOWPOSCHANGED};
    const UINT showFlags = SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE;
    if (ShowWindow(window, SW_SHOWNA) || !Matches(nestedShow, ARRAYSIZE(nestedShow)) ||
            !Geometry(window, 0, 0, 1920, 1080) || (committed.flags & showFlags) != showFlags ||
            !wasZoomed || wasIconic || resizeState != SIZE_MAXIMIZED) return 0x75420000u;

    messageCount = 0;
    if (!VerifyMessageWindow(windowClass.lpszClassName, module)) return 0x75430000u;
    destroyBeforeChange = TRUE;
    const UINT changing[] = {WM_WINDOWPOSCHANGING};
    if (SetWindowPos(window, NULL, 0, 0, 64, 64, SWP_NOZORDER) ||
            GetLastError() != ERROR_INVALID_WINDOW_HANDLE ||
            !Matches(changing, ARRAYSIZE(changing)) || !UnregisterClassW(windowClass.lpszClassName, module))
        return 0x75390000u;
    return PROBE_EXPECTED_CHILD_EXIT;
}

void WINAPI ChildEntry(void) {
    ExitProcess(VerifyGeometry());
}
