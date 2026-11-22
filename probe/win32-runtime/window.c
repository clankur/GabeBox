#include "window.h"

#define CLASS_COUNT 32
#define WINDOW_COUNT 32
#define MESSAGE_COUNT 256

DECLSPEC_IMPORT HANDLE WINAPI CreateFile2FromAppW(LPCWSTR, DWORD, DWORD, DWORD,
    LPCREATEFILE2_EXTENDED_PARAMETERS);

typedef struct WindowClass {
    WNDPROC procedure;
    HINSTANCE instance;
    WCHAR name[256];
} WindowClass;

static volatile LONG windowThread;
static WindowClass classes[CLASS_COUNT];
static XboxWindow windows[WINDOW_COUNT];
static ULONG_PTR nextHandle = 0x584257000001ULL;
static MSG messages[MESSAGE_COUNT];
static UINT messageCount;
static void* volatile hostClose;

void XboxWindowTrace(const char *line) {
    const DWORD error = GetLastError();
    WCHAR path[1024];
    const DWORD length = GetEnvironmentVariableW(L"XBOX_SDL_TRACE_FILE", path, ARRAYSIZE(path));
    if (length && length < ARRAYSIZE(path)) {
        const HANDLE file = CreateFile2FromAppW(path, GENERIC_READ | FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size;
            DWORD count = 0, written;
            while (line[count]) ++count;
            if (GetFileSizeEx(file, &size) && size.QuadPart + count < 4096)
                WriteFile(file, line, count, &written, NULL);
            CloseHandle(file);
        }
    }
    SetLastError(error);
}

BOOL XboxWindowThread(void) {
    const LONG thread = (LONG)GetCurrentThreadId();
    const LONG owner = InterlockedCompareExchange(&windowThread, thread, 0);
    if (!owner || owner == thread) return TRUE;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

static BOOL SameClassNameExact(LPCWSTR stored, LPCWSTR requested) {
    while (*stored == *requested) {
        if (!*stored) return TRUE;
        ++stored;
        ++requested;
    }
    return FALSE;
}

static WindowClass *FindClass(HINSTANCE instance, LPCWSTR name, BOOL traceMiss) {
    if ((ULONG_PTR)name <= 0xffffu) {
        const UINT index = (UINT)(ULONG_PTR)name;
        if (index && index <= CLASS_COUNT) {
            WindowClass *entry = &classes[index - 1];
            if (entry->procedure && entry->instance == instance) return entry;
        }
        if (traceMiss) XboxWindowTrace("win32_window_class_lookup=atom-miss\n");
        return NULL;
    }
    BOOL otherInstance = FALSE;
    BOOL ordinalError = FALSE;
    for (UINT i = 0; i < CLASS_COUNT; ++i) {
        WindowClass *entry = &classes[i];
        if (!entry->procedure) continue;
        const BOOL exact = SameClassNameExact(entry->name, name);
        if (entry->instance != instance) {
            if (exact) otherInstance = TRUE;
            continue;
        }
        // An identical name has no case mapping to resolve, even if the
        // operating system cannot complete an ordinal comparison.
        if (exact) return entry;
        const int comparison = CompareStringOrdinal(entry->name, -1, name, -1, TRUE);
        if (comparison == CSTR_EQUAL) return entry;
        if (!comparison) ordinalError = TRUE;
    }
    if (traceMiss) {
        if (otherInstance) XboxWindowTrace("win32_window_class_lookup=other-instance\n");
        else if (ordinalError) XboxWindowTrace("win32_window_class_lookup=ordinal-error\n");
        else XboxWindowTrace("win32_window_class_lookup=name-miss\n");
    }
    return NULL;
}

XboxWindow *XboxFindWindow(HWND handle) {
    if (!XboxWindowThread()) return NULL;
    for (UINT i = 0; handle && i < WINDOW_COUNT; ++i) {
        if (windows[i].handle == handle) return &windows[i];
    }
    SetLastError(ERROR_INVALID_WINDOW_HANDLE);
    return NULL;
}

ATOM WINAPI XboxRegisterClassExW(const WNDCLASSEXW *parameters) {
    if (!XboxWindowThread()) return 0;
    if (!parameters || parameters->cbSize != sizeof(*parameters) || !parameters->lpfnWndProc ||
            (ULONG_PTR)parameters->lpszClassName <= 0xffffu) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    LPCWSTR name = parameters->lpszClassName;
    UINT length = 0;
    while (length < ARRAYSIZE(classes[0].name) && name[length]) ++length;
    if (!length || length == ARRAYSIZE(classes[0].name)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const HINSTANCE instance = parameters->hInstance ? parameters->hInstance : GetModuleHandleW(NULL);
    if (FindClass(instance, name, FALSE)) {
        SetLastError(ERROR_CLASS_ALREADY_EXISTS);
        return 0;
    }
    for (UINT i = 0; i < CLASS_COUNT; ++i) {
        WindowClass *entry = &classes[i];
        if (entry->procedure) continue;
        for (UINT j = 0; j <= length; ++j) entry->name[j] = name[j];
        entry->instance = instance;
        entry->procedure = parameters->lpfnWndProc;
        XboxWindowTrace("win32_window_class=registered\n");
        return (ATOM)(i + 1);
    }
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

ATOM WINAPI XboxRegisterClassW(const WNDCLASSW *parameters) {
    if (!parameters) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const WNDCLASSEXW extended = {sizeof(extended), parameters->style, parameters->lpfnWndProc,
        parameters->cbClsExtra, parameters->cbWndExtra, parameters->hInstance, parameters->hIcon,
        parameters->hCursor, parameters->hbrBackground, parameters->lpszMenuName, parameters->lpszClassName, NULL};
    return XboxRegisterClassExW(&extended);
}

BOOL WINAPI XboxUnregisterClassW(LPCWSTR name, HINSTANCE instance) {
    if (!XboxWindowThread()) return FALSE;
    if (!instance) instance = GetModuleHandleW(NULL);
    WindowClass *entry = FindClass(instance, name, FALSE);
    if (!entry) {
        SetLastError(ERROR_CLASS_DOES_NOT_EXIST);
        return FALSE;
    }
    const ATOM atom = (ATOM)(entry - classes + 1);
    for (UINT i = 0; i < WINDOW_COUNT; ++i) {
        if (windows[i].handle && windows[i].classAtom == atom) {
            SetLastError(ERROR_CLASS_HAS_WINDOWS);
            return FALSE;
        }
    }
    entry->procedure = NULL;
    return TRUE;
}

HWND WINAPI XboxCreateWindowExW(DWORD exStyle, LPCWSTR name, LPCWSTR title, DWORD style, int x, int y,
        int width, int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID data) {
    XboxWindowTrace("win32_window_create=begin\n");
    if (!XboxWindowThread()) {
        XboxWindowTrace("win32_window_create=thread-unsupported\n");
        return NULL;
    }
    const HINSTANCE classInstance = instance ? instance : GetModuleHandleW(NULL);
    const WindowClass *entry = FindClass(classInstance, name, TRUE);
    if (!entry) {
        XboxWindowTrace("win32_window_create=class-missing\n");
        SetLastError(ERROR_CANNOT_FIND_WND_CLASS);
        return NULL;
    }
    if (parent || menu) {
        XboxWindowTrace("win32_window_create=parent-or-menu-unsupported\n");
        SetLastError(ERROR_NOT_SUPPORTED);
        return NULL;
    }
    XboxWindow *window = NULL;
    for (UINT i = 0; i < WINDOW_COUNT; ++i) {
        if (!windows[i].handle) { window = &windows[i]; break; }
    }
    if (!window) {
        XboxWindowTrace("win32_window_create=limit-reached\n");
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (x == CW_USEDEFAULT) x = 0;
    if (y == CW_USEDEFAULT) y = 0;
    if (width == CW_USEDEFAULT || height == CW_USEDEFAULT) {
        const RECT desktop = XboxDesktopBounds();
        if (width == CW_USEDEFAULT) width = desktop.right;
        if (height == CW_USEDEFAULT) height = desktop.bottom;
    }
    if (width < 0 || height < 0 || (LONG64)x + width > MAXLONG || (LONG64)y + height > MAXLONG) {
        XboxWindowTrace("win32_window_create=invalid-size\n");
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    const HWND handle = (HWND)nextHandle++;
    const RECT bounds = {x, y, x + width, y + height};
    *window = (XboxWindow){handle, entry->procedure, classInstance, bounds, style, exStyle, 0,
        (ATOM)(entry - classes + 1), 0, 0, FALSE};
    CREATESTRUCTW create = {data, classInstance, menu, parent, height, width, y, x, (LONG)style, title, name, exStyle};
    XboxWindowTrace("win32_window_create=nccreate\n");
    if (!window->procedure(handle, WM_NCCREATE, 0, (LPARAM)&create)) {
        XboxWindowTrace("win32_window_create=nccreate-refused\n");
        if (window->handle == handle) {
            window->destroying = TRUE;
            window->procedure(handle, WM_NCDESTROY, 0, 0);
            window->handle = NULL;
        }
        return NULL;
    }
    if (window->handle != handle) return NULL;
    XboxWindowTrace("win32_window_create=create\n");
    if (window->procedure(handle, WM_CREATE, 0, (LPARAM)&create) == -1) {
        XboxWindowTrace("win32_window_create=create-refused\n");
        if (window->handle == handle) XboxDestroyWindow(handle);
        return NULL;
    }
    if (window->handle != handle) return NULL;
    XboxWindowTrace("win32_window=created\n");
    return handle;
}

BOOL WINAPI XboxDestroyWindow(HWND handle) {
    XboxWindow *window = XboxFindWindow(handle);
    if (!window || window->destroying) return FALSE;
    if (window->contexts) {
        SetLastError(ERROR_BUSY);
        return FALSE;
    }
    window->destroying = TRUE;
    window->procedure(handle, WM_DESTROY, 0, 0);
    window->procedure(handle, WM_NCDESTROY, 0, 0);
    window->handle = NULL;
    for (UINT i = 0; i < messageCount;) {
        if (messages[i].hwnd != handle) { ++i; continue; }
        for (UINT j = i + 1; j < messageCount; ++j) messages[j - 1] = messages[j];
        --messageCount;
    }
    return TRUE;
}

LRESULT WINAPI XboxDefWindowProcW(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
    (void)wparam;
    (void)lparam;
    if (message == WM_NCCREATE) return TRUE;
    if (message == WM_CLOSE) XboxDestroyWindow(handle);
    return 0;
}

BOOL WINAPI XboxGetClientRect(HWND handle, RECT *area) {
    XboxWindow *window = XboxFindWindow(handle);
    if (!window || !area) return FALSE;
    *area = (RECT){0, 0, window->bounds.right - window->bounds.left, window->bounds.bottom - window->bounds.top};
    return TRUE;
}

BOOL WINAPI XboxGetWindowRect(HWND handle, RECT *area) {
    XboxWindow *window = XboxFindWindow(handle);
    if (!window || !area) return FALSE;
    *area = window->bounds;
    return TRUE;
}

HDC WINAPI XboxGetDC(HWND handle) {
    if (!handle) return XBOX_DESKTOP_DC;
    return XboxFindWindow(handle) ? (HDC)handle : NULL;
}

int WINAPI XboxReleaseDC(HWND handle, HDC dc) {
    if (!handle) return dc == XBOX_DESKTOP_DC;
    return handle == (HWND)dc && XboxFindWindow(handle) ? 1 : 0;
}

BOOL WINAPI XboxShowWindow(HWND handle, int command) {
    XboxWindow *window = XboxFindWindow(handle);
    if (!window) return FALSE;
    const BOOL previous = (window->style & WS_VISIBLE) != 0;
    const BOOL showing = command != SW_HIDE;
    if (previous != showing) {
        if (showing) window->style |= WS_VISIBLE;
        else window->style &= ~WS_VISIBLE;
        window->procedure(handle, WM_SHOWWINDOW, showing, 0);
    }
    return previous;
}

BOOL WINAPI XboxIsWindowVisible(HWND handle) {
    XboxWindow *window = XboxFindWindow(handle);
    return window && (window->style & WS_VISIBLE);
}

BOOL WINAPI XboxSetWindowPos(HWND handle, HWND insertAfter, int x, int y, int width, int height, UINT flags) {
    (void)insertAfter;
    XboxWindow *window = XboxFindWindow(handle);
    if (!window) return FALSE;
    const RECT previous = window->bounds;
    if (flags & SWP_NOMOVE) { x = previous.left; y = previous.top; }
    if (flags & SWP_NOSIZE) { width = previous.right - previous.left; height = previous.bottom - previous.top; }
    if (width < 0 || height < 0 || (LONG64)x + width > MAXLONG || (LONG64)y + height > MAXLONG) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    window->bounds = (RECT){x, y, x + width, y + height};
    if (width != previous.right - previous.left || height != previous.bottom - previous.top) {
        XboxResizeCurrentContext(window);
        window->procedure(handle, WM_SIZE, SIZE_RESTORED, MAKELPARAM(width, height));
    }
    if (window->handle != handle) return FALSE;
    if (x != previous.left || y != previous.top) window->procedure(handle, WM_MOVE, 0, MAKELPARAM(x, y));
    if (window->handle != handle) return FALSE;
    if (flags & SWP_SHOWWINDOW) XboxShowWindow(handle, SW_SHOW);
    if (flags & SWP_HIDEWINDOW) XboxShowWindow(handle, SW_HIDE);
    return window->handle == handle;
}

LONG_PTR WINAPI XboxGetWindowLongPtrW(HWND handle, int index) {
    XboxWindow *window = XboxFindWindow(handle);
    if (!window) return 0;
    switch (index) {
    case GWLP_USERDATA: return window->userData;
    case GWLP_WNDPROC: return (LONG_PTR)window->procedure;
    case GWLP_HINSTANCE: return (LONG_PTR)window->instance;
    case GWL_STYLE: return (LONG_PTR)(LONG)window->style;
    case GWL_EXSTYLE: return (LONG_PTR)(LONG)window->extendedStyle;
    default: SetLastError(ERROR_INVALID_INDEX); return 0;
    }
}

LONG_PTR WINAPI XboxSetWindowLongPtrW(HWND handle, int index, LONG_PTR value) {
    XboxWindow *window = XboxFindWindow(handle);
    if (!window) return 0;
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = XboxGetWindowLongPtrW(handle, index);
    switch (index) {
    case GWLP_USERDATA: window->userData = value; break;
    case GWLP_WNDPROC:
        if (!value) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
        window->procedure = (WNDPROC)value; break;
    case GWLP_HINSTANCE: window->instance = (HINSTANCE)value; break;
    case GWL_STYLE: window->style = (DWORD)value; break;
    case GWL_EXSTYLE: window->extendedStyle = (DWORD)value; break;
    default: SetLastError(ERROR_INVALID_INDEX); return 0;
    }
    return previous;
}

BOOL WINAPI XboxPostMessageW(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
    if (!XboxFindWindow(handle)) return FALSE;
    if (messageCount == MESSAGE_COUNT) {
        SetLastError(ERROR_NOT_ENOUGH_QUOTA);
        return FALSE;
    }
    messages[messageCount++] = (MSG){handle, message, wparam, lparam, GetTickCount(), {0, 0}};
    return TRUE;
}

BOOL WINAPI XboxPeekMessageW(MSG *message, HWND filter, UINT first, UINT last, UINT remove) {
    if (!XboxWindowThread() || !message) return FALSE;
    XboxPollWglHostEvents();
    HWND close = InterlockedExchangePointer(&hostClose, NULL);
    if (close && XboxFindWindow(close) && !XboxPostMessageW(close, WM_CLOSE, 0, 0))
        InterlockedCompareExchangePointer(&hostClose, close, NULL);
    for (UINT i = 0; i < messageCount; ++i) {
        const MSG *candidate = &messages[i];
        if ((filter && filter != candidate->hwnd) ||
                ((first || last) && (candidate->message < first || candidate->message > last))) continue;
        *message = *candidate;
        if (remove & PM_REMOVE) {
            for (UINT j = i + 1; j < messageCount; ++j) messages[j - 1] = messages[j];
            --messageCount;
        }
        return TRUE;
    }
    return FALSE;
}

void WINAPI XboxRequestWindowClose(HWND window) {
    InterlockedExchangePointer(&hostClose, window);
}

LRESULT WINAPI XboxDispatchMessageW(const MSG *message) {
    XboxWindow *window = message ? XboxFindWindow(message->hwnd) : NULL;
    return window ? window->procedure(message->hwnd, message->message, message->wParam, message->lParam) : 0;
}
