#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../runtime/shared/child-process.h"

int _fltused;

static const WCHAR property[] = L"SDL_WindowData";
static int first, replacement, other;
static BOOL sawDestroyProperty;

static LRESULT CALLBACK OnMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CREATE) {
        return SetPropW(window, property, ((const CREATESTRUCTW *)lparam)->lpCreateParams) ? 0 : -1;
    }
    if (message == WM_NCDESTROY && GetPropW(window, property) == &replacement) {
        sawDestroyProperty = RemovePropW(window, property) == &replacement;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static HWND NewWindow(void *data) {
    return CreateWindowExW(WS_EX_APPWINDOW, L"GraphicsWindowProperties", L"Window properties",
        WS_POPUP, 0, 0, 64, 64, NULL, NULL, GetModuleHandleW(NULL), data);
}

static unsigned int TryWindowProperties(void) {
    HINSTANCE image = GetModuleHandleW(NULL);
    const WNDCLASSW windowClass = {0, OnMessage, 0, 0, image, NULL, NULL, NULL, NULL, L"GraphicsWindowProperties"};
    if (!RegisterClassW(&windowClass)) return 0x73600000u | (GetLastError() & 0xffffu);
    HWND window = NewWindow(&first);
    if (!window) return 0x73610000u | (GetLastError() & 0xffffu);
    if (GetPropW(window, property) != &first ||
            GetWindowLongW(window, GWL_STYLE) != (LONG)WS_POPUP ||
            GetWindowLongW(window, GWL_EXSTYLE) != WS_EX_APPWINDOW) return 0x73620000u;
    SetLastError(ERROR_SUCCESS);
    if (GetWindowLongW(window, GWLP_HINSTANCE) || GetLastError() != ERROR_INVALID_INDEX) return 0x73620001u;

    WCHAR matchingName[] = L"sdl_windowdata";
    SetLastError(ERROR_RETRY);
    if (!SetPropW(window, matchingName, &replacement) || GetLastError() != ERROR_RETRY) return 0x73630000u;
    matchingName[0] = L'x';
    if (GetPropW(window, property) != &replacement || GetLastError() != ERROR_RETRY) return 0x73640000u;
    if (!SetPropW(window, L"NullValue", NULL) || GetPropW(window, L"NullValue") ||
            !SetPropW(window, L"NullValue", &first) || RemovePropW(window, L"NullValue") != &first ||
            RemovePropW(window, L"NullValue") || GetLastError() != ERROR_RETRY) return 0x73650000u;
    if (!SetPropW(window, L"Unremoved", &first)) return 0x73660000u | (GetLastError() & 0xffffu);

    HWND second = NewWindow(&other);
    if (!second || GetPropW(second, property) != &other || GetPropW(window, property) != &replacement)
        return 0x73670000u;
    if (!DestroyWindow(window) || !sawDestroyProperty || GetPropW(window, property) ||
            GetLastError() != ERROR_INVALID_WINDOW_HANDLE) return 0x73680000u;
    HWND recycled = NewWindow(&first);
    if (!recycled || recycled == window || GetPropW(recycled, property) != &first ||
            GetPropW(recycled, L"Unremoved") || GetPropW(second, property) != &other) return 0x73690000u;
    if (!DestroyWindow(recycled) || !DestroyWindow(second) ||
            !UnregisterClassW(windowClass.lpszClassName, image)) return 0x736a0000u;
    return PROBE_EXPECTED_CHILD_EXIT;
}

void WINAPI ChildEntry(void) {
    ExitProcess(TryWindowProperties());
}
