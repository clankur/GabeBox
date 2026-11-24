#include "window.h"
#include "../sdl-host-protocol.h"
#include <stddef.h>

#define CLASS_COUNT 32
#define WINDOW_COUNT 32
#define MESSAGE_COUNT 256
#define WINDOW_HANDLE_BASE 0x584257000000ULL

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
static SRWLOCK windowGeometryLock = SRWLOCK_INIT;
static ULONG_PTR nextHandle = WINDOW_HANDLE_BASE + 1;
static MSG messages[MESSAGE_COUNT];
static UINT messageCount;
static void* volatile hostClose;
static volatile LONG hostExitChecked;
static void* volatile foregroundWindow;
static HWND focusWindow;
static UINT activationChange;
static BOOL applicationActive;
static SdlHostMailbox *hostPointerMailbox;
static BOOL hostPointerConfigured;
static LONG hostPointerSerial;
static HWND hostPointerWindow;
static POINT hostPointerScreen;
static BOOL hostPointerDown;

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

static BOOL ParseHostNumber(const WCHAR *text, DWORD length, ULONG64 *result) {
    if (!length || length > 20) return FALSE;
    ULONG64 value = 0;
    for (DWORD index = 0; index < length; ++index) {
        if (text[index] < L'0' || text[index] > L'9') return FALSE;
        const UINT digit = text[index] - L'0';
        if (value > (~(ULONG64)0 - digit) / 10) return FALSE;
        value = value * 10 + digit;
    }
    *result = value;
    return TRUE;
}

static void TraceHostValue(const char *prefix, DWORD value) {
    static const char digits[] = "0123456789ABCDEF";
    char line[48];
    UINT index = 0;
    while (*prefix) line[index++] = *prefix++;
    for (int shift = 28; shift >= 0; shift -= 4) line[index++] = digits[(value >> shift) & 15];
    line[index++] = '\n';
    line[index] = 0;
    XboxWindowTrace(line);
}

static void TraceHostError(const char *reason, DWORD error) {
    XboxWindowTrace(reason);
    TraceHostValue("win32_host_exit_error=0x", error);
}

static VOID CALLBACK RecordHostExit(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WAIT wait,
                                    TP_WAIT_RESULT result) {
    (void)instance;
    const DWORD lastError = GetLastError();
    HANDLE host = context;
    DWORD code;
    if (result != WAIT_OBJECT_0) {
        TraceHostError("win32_host_exit=wait-unexpected\n", result);
    } else if (GetExitCodeProcess(host, &code)) {
        XboxWindowTrace("win32_host_exit=exited\n");
        TraceHostValue("win32_host_exit_code=0x", code);
    } else {
        TraceHostError("win32_host_exit=exit-code-unavailable\n", GetLastError());
    }
    SetThreadpoolWait(wait, NULL, NULL);
    CloseHandle(host);
    CloseThreadpoolWait(wait);
    SetLastError(lastError);
}

static void StartHostExitWatch(void) {
    WCHAR text[21];
    DWORD length = GetEnvironmentVariableW(L"XBOX_WIN32_HOST_START_TIME", text, ARRAYSIZE(text));
    if (!length) return;
    ULONG64 startTime, pid;
    if (!ParseHostNumber(text, length, &startTime) || !startTime) {
        XboxWindowTrace("win32_host_exit=invalid-start-time\n");
        return;
    }
    length = GetEnvironmentVariableW(L"XBOX_STEAM_HANDOFF_PID", text, ARRAYSIZE(text));
    if (!ParseHostNumber(text, length, &pid) || !pid || pid > MAXDWORD || pid == GetCurrentProcessId()) {
        XboxWindowTrace("win32_host_exit=invalid-host-pid\n");
        return;
    }
    HANDLE host = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!host) {
        TraceHostError("win32_host_exit=open-failed\n", GetLastError());
        return;
    }
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(host, &created, &exited, &kernel, &user)) {
        TraceHostError("win32_host_exit=identity-query-failed\n", GetLastError());
    } else if ((((ULONG64)created.dwHighDateTime << 32) | created.dwLowDateTime) != startTime) {
        XboxWindowTrace("win32_host_exit=identity-mismatch\n");
    } else {
        PTP_WAIT wait = CreateThreadpoolWait(RecordHostExit, host, NULL);
        if (wait) {
            XboxWindowTrace("win32_host_exit=watching\n");
            // The one-shot callback owns the wait and process handles from here.
            SetThreadpoolWait(wait, host, NULL);
            return;
        }
        TraceHostError("win32_host_exit=wait-create-failed\n", GetLastError());
    }
    CloseHandle(host);
}

void XboxWatchHostExit(void) {
    if (InterlockedExchange(&hostExitChecked, 1)) return;
    const DWORD error = GetLastError();
    StartHostExitWatch();
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

static BOOL IsVirtualWindowHandle(HWND handle) {
    return ((ULONG_PTR)handle & ~(ULONG_PTR)0xffffff) == WINDOW_HANDLE_BASE;
}

static void InvalidateWindowGeometry(XboxWindow *window) {
    AcquireSRWLockExclusive(&windowGeometryLock);
    window->handle = NULL;
    ReleaseSRWLockExclusive(&windowGeometryLock);
}

static HWND VirtualForegroundWindow(void) {
    return InterlockedCompareExchangePointer(&foregroundWindow, NULL, NULL);
}

static BOOL OwnsVirtualActivation(void) {
    return (LONG)GetCurrentThreadId() == windowThread && VirtualForegroundWindow();
}

static void NotifyWindow(HWND handle, UINT message, WPARAM wparam, HWND related) {
    XboxWindow *window = handle ? XboxFindWindow(handle) : NULL;
    if (window) window->procedure(handle, message, wparam, (LPARAM)related);
}

static void UpdateWindowActivation(HWND foreground, HWND focus) {
    const HWND previousForeground = VirtualForegroundWindow();
    const HWND previousFocus = focusWindow;
    if (previousForeground == foreground && previousFocus == focus) return;
    const DWORD error = GetLastError();
    const UINT change = ++activationChange;
    focusWindow = focus;
    InterlockedExchangePointer(&foregroundWindow, foreground);
    // The window procedure may synchronously change activation or destroy a window.
    if (previousForeground != foreground) {
        NotifyWindow(previousForeground, WM_NCACTIVATE, FALSE, foreground);
        if (activationChange == change) NotifyWindow(previousForeground, WM_ACTIVATE, WA_INACTIVE, foreground);
        if (activationChange == change && applicationActive != (foreground != NULL)) {
            // A callback can switch virtual windows before deactivation is delivered.
            applicationActive = foreground != NULL;
            XboxWindowTrace(applicationActive ? "win32_virtual_activation=active\n" :
                "win32_virtual_activation=inactive\n");
            NotifyWindow(applicationActive ? foreground : previousForeground, WM_ACTIVATEAPP, applicationActive, NULL);
        }
        if (activationChange == change) NotifyWindow(foreground, WM_NCACTIVATE, TRUE, previousForeground);
        if (activationChange == change) NotifyWindow(foreground, WM_ACTIVATE, WA_ACTIVE, previousForeground);
    }
    if (activationChange == change && previousFocus != focus) {
        NotifyWindow(previousFocus, WM_KILLFOCUS, (WPARAM)focus, NULL);
        if (activationChange == change) NotifyWindow(focus, WM_SETFOCUS, (WPARAM)previousFocus, NULL);
    }
    SetLastError(error);
}

static void ClearWindowActivation(HWND handle) {
    const HWND foreground = VirtualForegroundWindow();
    UpdateWindowActivation(foreground == handle ? NULL : foreground, focusWindow == handle ? NULL : focusWindow);
}

static FARPROC NativeWindowProcedure(LPCSTR name) {
    const DWORD error = GetLastError();
    HMODULE module = GetModuleHandleW(L"user32.dll");
    FARPROC procedure = module ? GetProcAddress(module, name) : NULL;
    if (procedure) SetLastError(error);
    return procedure;
}

HWND WINAPI XboxGetForegroundWindow(void) {
    const HWND foreground = VirtualForegroundWindow();
    if (foreground) return foreground;
    typedef HWND (WINAPI *Native)(void);
    Native native = (Native)(void *)NativeWindowProcedure("GetForegroundWindow");
    return native ? native() : NULL;
}

HWND WINAPI XboxGetActiveWindow(void) {
    if (OwnsVirtualActivation()) return VirtualForegroundWindow();
    typedef HWND (WINAPI *Native)(void);
    Native native = (Native)(void *)NativeWindowProcedure("GetActiveWindow");
    return native ? native() : NULL;
}

BOOL WINAPI XboxSetForegroundWindow(HWND handle) {
    if (!IsVirtualWindowHandle(handle)) {
        const BOOL reconcile = OwnsVirtualActivation();
        const UINT change = reconcile ? activationChange : 0;
        typedef BOOL (WINAPI *Native)(HWND);
        Native native = (Native)(void *)NativeWindowProcedure("SetForegroundWindow");
        const BOOL result = native ? native(handle) : FALSE;
        if (result && reconcile && activationChange == change) UpdateWindowActivation(NULL, NULL);
        return result;
    }
    XboxWindow *window = XboxFindWindow(handle);
    if (!window) return FALSE;
    if (window->destroying) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    UpdateWindowActivation(handle, handle);
    return TRUE;
}

HWND WINAPI XboxGetFocus(void) {
    if (OwnsVirtualActivation()) return focusWindow;
    typedef HWND (WINAPI *Native)(void);
    Native native = (Native)(void *)NativeWindowProcedure("GetFocus");
    return native ? native() : NULL;
}

HWND WINAPI XboxSetFocus(HWND handle) {
    const BOOL reconcile = OwnsVirtualActivation();
    if (!handle && reconcile) {
        const HWND previous = focusWindow;
        UpdateWindowActivation(VirtualForegroundWindow(), NULL);
        return previous;
    }
    if (!IsVirtualWindowHandle(handle)) {
        const UINT change = reconcile ? activationChange : 0;
        typedef HWND (WINAPI *Native)(HWND);
        Native native = (Native)(void *)NativeWindowProcedure("SetFocus");
        if (!native) return NULL;
        const HWND previous = native(handle);
        if (reconcile && activationChange == change) {
            const DWORD error = GetLastError();
            BOOL selected = previous != NULL;
            if (!selected) {
                // SetFocus can succeed on a queue that had no previously focused window.
                typedef HWND (WINAPI *Query)(void);
                Query getFocus = (Query)(void *)NativeWindowProcedure("GetFocus");
                selected = getFocus && getFocus() == handle;
            }
            SetLastError(error);
            if (selected && activationChange == change) UpdateWindowActivation(NULL, NULL);
        }
        return previous;
    }
    XboxWindow *window = XboxFindWindow(handle);
    if (!window) return NULL;
    if (window->destroying) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return NULL; }
    const HWND previous = focusWindow;
    UpdateWindowActivation(handle, handle);
    return previous;
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
    if (!window || !IsVirtualWindowHandle((HWND)nextHandle)) {
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
    AcquireSRWLockExclusive(&windowGeometryLock);
    *window = (XboxWindow){handle, entry->procedure, classInstance, bounds, style, exStyle, 0,
        (ATOM)(entry - classes + 1), 0, 0, FALSE};
    ReleaseSRWLockExclusive(&windowGeometryLock);
    CREATESTRUCTW create = {data, classInstance, menu, parent, height, width, y, x, (LONG)style, title, name, exStyle};
    XboxWindowTrace("win32_window_create=nccreate\n");
    if (!window->procedure(handle, WM_NCCREATE, 0, (LPARAM)&create)) {
        XboxWindowTrace("win32_window_create=nccreate-refused\n");
        if (window->handle == handle) {
            window->destroying = TRUE;
            ClearWindowActivation(handle);
            window->procedure(handle, WM_NCDESTROY, 0, 0);
            InvalidateWindowGeometry(window);
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
    ClearWindowActivation(handle);
    window->procedure(handle, WM_DESTROY, 0, 0);
    window->procedure(handle, WM_NCDESTROY, 0, 0);
    InvalidateWindowGeometry(window);
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
    if (message == WM_NCCREATE || message == WM_NCACTIVATE) return TRUE;
    if (message == WM_CLOSE) XboxDestroyWindow(handle);
    return 0;
}

static BOOL HostedWindowBounds(HWND handle, RECT *area) {
    if (!area) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    // The rendering thread can query geometry while the window thread owns input and messages.
    if (IsVirtualWindowHandle(handle)) {
        AcquireSRWLockShared(&windowGeometryLock);
        for (UINT i = 0; i < WINDOW_COUNT; ++i) {
            if (windows[i].handle != handle) continue;
            *area = windows[i].bounds;
            ReleaseSRWLockShared(&windowGeometryLock);
            return TRUE;
        }
        ReleaseSRWLockShared(&windowGeometryLock);
    }
    SetLastError(ERROR_INVALID_WINDOW_HANDLE);
    return FALSE;
}

BOOL WINAPI XboxGetClientRect(HWND handle, RECT *area) {
    if (!HostedWindowBounds(handle, area)) return FALSE;
    *area = (RECT){0, 0, area->right - area->left, area->bottom - area->top};
    return TRUE;
}

BOOL WINAPI XboxGetWindowRect(HWND handle, RECT *area) {
    return HostedWindowBounds(handle, area);
}

static BOOL TranslateWindowPoint(HWND handle, POINT *point, BOOL toScreen) {
    if (!IsVirtualWindowHandle(handle)) {
        typedef BOOL (WINAPI *Native)(HWND, LPPOINT);
        Native native = (Native)(void *)NativeWindowProcedure(toScreen ? "ClientToScreen" : "ScreenToClient");
        return native ? native(handle, point) : FALSE;
    }
    if (!point) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    RECT bounds;
    if (!HostedWindowBounds(handle, &bounds)) return FALSE;
    // The virtual client occupies the whole window, including for a negative screen origin.
    const LONG64 direction = toScreen ? 1 : -1;
    const LONG64 x = point->x + direction * bounds.left;
    const LONG64 y = point->y + direction * bounds.top;
    const LONG64 minimum = -(LONG64)MAXLONG - 1;
    if (x < minimum || x > MAXLONG || y < minimum || y > MAXLONG) {
        SetLastError(ERROR_ARITHMETIC_OVERFLOW);
        return FALSE;
    }
    point->x = (LONG)x;
    point->y = (LONG)y;
    return TRUE;
}

BOOL WINAPI XboxClientToScreen(HWND handle, POINT *point) {
    return TranslateWindowPoint(handle, point, TRUE);
}

BOOL WINAPI XboxScreenToClient(HWND handle, POINT *point) {
    return TranslateWindowPoint(handle, point, FALSE);
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
    const UINT change = activationChange;
    if (previous != showing) {
        if (showing) window->style |= WS_VISIBLE;
        else window->style &= ~WS_VISIBLE;
        window->procedure(handle, WM_SHOWWINDOW, showing, 0);
        if (!showing && window->handle == handle && !(window->style & WS_VISIBLE)) ClearWindowActivation(handle);
    }
    if (activationChange == change && window->handle == handle && !window->destroying &&
            (window->style & WS_VISIBLE) && (command == SW_SHOW || command == SW_SHOWNORMAL ||
            command == SW_RESTORE || command == SW_SHOWMAXIMIZED || command == SW_SHOWMINIMIZED ||
            command == SW_SHOWDEFAULT)) {
        XboxWindowTrace("win32_window_show=activate\n");
        UpdateWindowActivation(handle, handle);
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
    AcquireSRWLockExclusive(&windowGeometryLock);
    window->bounds = (RECT){x, y, x + width, y + height};
    ReleaseSRWLockExclusive(&windowGeometryLock);
    if (width != previous.right - previous.left || height != previous.bottom - previous.top) {
        XboxResizeCurrentContext(window);
        window->procedure(handle, WM_SIZE, SIZE_RESTORED, MAKELPARAM(width, height));
    }
    if (window->handle != handle) return FALSE;
    if (x != previous.left || y != previous.top) window->procedure(handle, WM_MOVE, 0, MAKELPARAM(x, y));
    if (window->handle != handle) return FALSE;
    if (flags & SWP_SHOWWINDOW) XboxShowWindow(handle, (flags & SWP_NOACTIVATE) ? SW_SHOWNA : SW_SHOW);
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

static void OpenHostedPointer(void) {
    hostPointerConfigured = TRUE;
    WCHAR mode[2], path[1024];
    if (GetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_DXGI", mode, ARRAYSIZE(mode)) != 1 || mode[0] != L'1') return;
    const DWORD length = GetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) return;
    const HANDLE file = CreateFile2FromAppW(path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    const SIZE_T header = offsetof(SdlHostMailbox, frame);
    LARGE_INTEGER size;
    const HANDLE mapping = GetFileSizeEx(file, &size) && size.QuadPart >= (LONGLONG)header
        ? CreateFileMappingFromApp(file, NULL, PAGE_READWRITE, 0, NULL) : NULL;
    CloseHandle(file);
    if (!mapping) return;
    hostPointerMailbox = MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, header);
    CloseHandle(mapping);
    if (hostPointerMailbox && SdlHostLoad(&hostPointerMailbox->magic) != SDL_HOST_MAGIC) {
        UnmapViewOfFile(hostPointerMailbox);
        hostPointerMailbox = NULL;
    }
}

static void PollHostedPointer(void) {
    const HWND handle = VirtualForegroundWindow();
    if (!handle) return;
    if (!hostPointerConfigured) OpenHostedPointer();
    SdlHostMailbox *mailbox = hostPointerMailbox;
    if (!mailbox || SdlHostLoad(&mailbox->quit)) return;
    const LONG serial = SdlHostLoad(&mailbox->input_serial);
    const UINT pointer = (UINT)SdlHostLoad(&mailbox->input_pointer);
    if (!serial || (serial & 1) || serial != SdlHostLoad(&mailbox->input_serial) ||
            (handle == hostPointerWindow && serial == hostPointerSerial)) return;
    RECT bounds;
    if (!HostedWindowBounds(handle, &bounds)) return;
    const LONG width = bounds.right - bounds.left;
    const LONG height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768) return;
    const LONG x = (LONG)(((pointer & SDL_HOST_POINTER_MAX) * (width - 1) + 2047) / SDL_HOST_POINTER_MAX);
    const LONG y = (LONG)((((pointer >> 12) & SDL_HOST_POINTER_MAX) * (height - 1) + 2047) / SDL_HOST_POINTER_MAX);
    const POINT screen = {bounds.left + x, bounds.top + y};
    const BOOL down = (pointer & (1u << 24)) != 0;
    const BOOL moved = handle != hostPointerWindow || screen.x != hostPointerScreen.x || screen.y != hostPointerScreen.y;
    const BOOL clicked = down != (handle == hostPointerWindow && hostPointerDown);
    const UINT count = moved + clicked;
    if (count > MESSAGE_COUNT - messageCount) return;
    const LPARAM position = MAKELPARAM(x, y);
    const WPARAM buttons = down ? MK_LBUTTON : 0;
    const DWORD time = GetTickCount();
    if (moved) messages[messageCount++] = (MSG){handle, WM_MOUSEMOVE, buttons, position, time, screen};
    if (clicked) messages[messageCount++] = (MSG){handle, down ? WM_LBUTTONDOWN : WM_LBUTTONUP,
        buttons, position, time, screen};
    AcquireSRWLockExclusive(&windowGeometryLock);
    hostPointerScreen = screen;
    hostPointerWindow = handle;
    ReleaseSRWLockExclusive(&windowGeometryLock);
    hostPointerSerial = serial;
    hostPointerDown = down;
    InterlockedExchangeAdd(&mailbox->input_events, count);
    InterlockedExchange(&mailbox->input_ack, serial);
}

static BOOL ReadCursorPosition(LPPOINT position, const char *nativeName) {
    if (!position) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    AcquireSRWLockShared(&windowGeometryLock);
    const BOOL hosted = hostPointerWindow && hostPointerWindow == VirtualForegroundWindow();
    if (hosted) *position = hostPointerScreen;
    ReleaseSRWLockShared(&windowGeometryLock);
    if (hosted) return TRUE;
    typedef BOOL (WINAPI *ReadCursor)(LPPOINT);
    const DWORD error = GetLastError();
    HMODULE user = GetModuleHandleW(L"user32.dll");
    const ReadCursor native = user ? (ReadCursor)(void *)GetProcAddress(user, nativeName) : NULL;
    if (native) {
        SetLastError(error);
        return native(position);
    }
    SetLastError(ERROR_PROC_NOT_FOUND);
    return FALSE;
}

BOOL WINAPI XboxGetCursorPos(LPPOINT position) {
    return ReadCursorPosition(position, "GetCursorPos");
}

BOOL WINAPI XboxGetPhysicalCursorPos(LPPOINT position) {
    return ReadCursorPosition(position, "GetPhysicalCursorPos");
}

BOOL WINAPI XboxPeekMessageW(MSG *message, HWND filter, UINT first, UINT last, UINT remove) {
    if (!XboxWindowThread() || !message) return FALSE;
    XboxPollWglHostEvents();
    const DWORD error = GetLastError();
    PollHostedPointer();
    SetLastError(error);
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
