#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../runtime/shared/child-process.h"

typedef BOOL (WINAPI *AdjustRect)(RECT *, DWORD, BOOL, DWORD);
typedef BOOL (WINAPI *AdjustRectForDpi)(RECT *, DWORD, BOOL, DWORD, UINT);
typedef UINT (WINAPI *WindowDpi)(HWND);

static const WCHAR policy[] = L"XBOX_WIN32_HOSTED_WINDOW_FRAME";
static const DWORD windowedStyle = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;

static BOOL SameRect(const RECT *left, const RECT *right) {
    return left->left == right->left && left->top == right->top && left->right == right->right &&
        left->bottom == right->bottom;
}

static LRESULT CALLBACK OnMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

static unsigned int VerifyWindowFrame(void) {
    const HMODULE user = LoadLibraryW(L"USER32.DLL");
    const HMODULE adapter = GetModuleHandleW(L"XboxWin32Window.dll");
    if (!user || !adapter || !SetEnvironmentVariableW(policy, L"0"))
        return 0x75400000u | (GetLastError() & 0xffffu);
    const AdjustRect nativeAdjust = (AdjustRect)(PVOID)GetProcAddress(user, "AdjustWindowRectEx");
    const FARPROC nativeDpi = GetProcAddress(user, "GetDpiForWindow");
    const FARPROC nativeAdjustDpi = GetProcAddress(user, "AdjustWindowRectExForDpi");
    const FARPROC unrelated = GetProcAddress(user, "GetDpiForSystem");
    const RECT requested = {-20, -15, 1900, 1065};
    if (nativeAdjust) {
        RECT expected = requested, forwarded = requested;
        const BOOL result = nativeAdjust(&expected, windowedStyle, FALSE, 0);
        if (AdjustWindowRectEx(&forwarded, windowedStyle, FALSE, 0) != result || !SameRect(&expected, &forwarded))
            return 0x75410000u;
    }
    if (!SetEnvironmentVariableW(policy, L"10") || GetProcAddress(user, "GetDpiForWindow") != nativeDpi ||
            GetProcAddress(user, "AdjustWindowRectExForDpi") != nativeAdjustDpi || !SetEnvironmentVariableW(policy, L"1"))
        return 0x75420000u;
    const FARPROC hostedDpi = GetProcAddress(user, "GetDpiForWindow");
    const FARPROC hostedAdjustDpi = GetProcAddress(user, "AdjustWindowRectExForDpi");
    if (!hostedDpi || !hostedAdjustDpi || hostedDpi == nativeDpi || hostedAdjustDpi == nativeAdjustDpi ||
            hostedDpi != GetProcAddress(adapter, "GetDpiForWindow") ||
            hostedAdjustDpi != GetProcAddress(adapter, "AdjustWindowRectExForDpi") ||
            GetProcAddress(user, "GetDpiForSystem") != unrelated || GetProcAddress(user, "AdjustWindowRectEx") !=
                (FARPROC)(PVOID)nativeAdjust) return 0x75430000u;
    const WindowDpi getDpi = (WindowDpi)(PVOID)hostedDpi;
    const AdjustRectForDpi adjustDpi = (AdjustRectForDpi)(PVOID)hostedAdjustDpi;

    RECT legacy = requested, perMonitor = requested;
    SetLastError(ERROR_RETRY);
    if (!AdjustWindowRectEx(&legacy, windowedStyle, FALSE, 0) || GetLastError() != ERROR_RETRY ||
            !SameRect(&legacy, &requested) || !adjustDpi(&perMonitor, windowedStyle, FALSE, 0, 144) ||
            GetLastError() != ERROR_RETRY || !SameRect(&perMonitor, &requested)) return 0x75440000u;
    if (adjustDpi(&perMonitor, windowedStyle, FALSE, 0, 0) || GetLastError() != ERROR_INVALID_PARAMETER ||
            !SameRect(&perMonitor, &requested) || AdjustWindowRectEx(NULL, windowedStyle, FALSE, 0) ||
            GetLastError() != ERROR_INVALID_PARAMETER) return 0x75450000u;
    if (nativeAdjust) {
        RECT expected = requested, forwarded = requested;
        const BOOL result = nativeAdjust(&expected, WS_POPUP, FALSE, WS_EX_TOOLWINDOW);
        if (AdjustWindowRectEx(&forwarded, WS_POPUP, FALSE, WS_EX_TOOLWINDOW) != result ||
                !SameRect(&expected, &forwarded)) return 0x75460000u;
    }

    const HINSTANCE instance = GetModuleHandleW(NULL);
    const WNDCLASSW windowClass = {0, OnMessage, 0, 0, instance, NULL, NULL, NULL, NULL, L"HostedFrameControl"};
    if (!RegisterClassW(&windowClass)) return 0x75470000u | (GetLastError() & 0xffffu);
    const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"Hosted window frame", WS_POPUP,
        0, 0, 1280, 720, NULL, NULL, instance, NULL);
    if (!window || getDpi(window) != 96) return 0x75480000u | (GetLastError() & 0xffffu);
    if (SetWindowLongW(window, GWL_STYLE, windowedStyle) != (LONG)WS_POPUP ||
            !adjustDpi(&perMonitor, windowedStyle, FALSE, 0, getDpi(window)) ||
            !SetWindowPos(window, NULL, perMonitor.left, perMonitor.top, perMonitor.right - perMonitor.left,
                perMonitor.bottom - perMonitor.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE))
        return 0x75490000u | (GetLastError() & 0xffffu);
    RECT client, outer;
    if (!GetClientRect(window, &client) || client.left || client.top || client.right != 1920 || client.bottom != 1080 ||
            !GetWindowRect(window, &outer) || !SameRect(&outer, &requested)) return 0x754a0000u;
    if (!DestroyWindow(window) || getDpi(window) || !UnregisterClassW(windowClass.lpszClassName, instance))
        return 0x754b0000u;
    if (!SetEnvironmentVariableW(policy, L"0") || GetProcAddress(user, "GetDpiForWindow") != nativeDpi ||
            GetProcAddress(user, "AdjustWindowRectExForDpi") != nativeAdjustDpi) return 0x754c0000u;
    return PROBE_EXPECTED_CHILD_EXIT;
}

void WINAPI ChildEntry(void) {
    ExitProcess(VerifyWindowFrame());
}
