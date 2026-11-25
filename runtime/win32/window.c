#include "window.h"
#include "../shared/sdl-host-protocol.h"
#include "window-keyboard.h"
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

typedef struct WindowProperty {
    struct WindowProperty *next;
    HANDLE value;
    WCHAR name[];
} WindowProperty;

typedef struct HostedMessage {
    MSG value;
    UINT16 scan;
    BYTE key;
    BOOL keyboard;
} HostedMessage;

static volatile LONG windowThread;
static WindowClass classes[CLASS_COUNT];
static XboxWindow windows[WINDOW_COUNT];
static SRWLOCK windowGeometryLock = SRWLOCK_INIT;
static ULONG_PTR nextHandle = WINDOW_HANDLE_BASE + 1;
static HostedMessage messages[MESSAGE_COUNT];
static UINT messageCount;
static void* volatile hostClose;
static volatile LONG hostExitChecked;
static void* volatile foregroundWindow;
static HWND focusWindow;
static UINT activationChange;
static BOOL applicationActive;
static SdlHostMailbox *hostPointerMailbox;
static SdlHostKeyboard *hostKeyboard;
static HWND hostKeyboardWindow;
static UINT32 hostKeyboardReset;
static BYTE hostedKeys[256];
static BYTE heldKeys[SDL_HOST_KEY_LIMIT];
static BOOL hostPointerConfigured;
static LONG hostPointerSerial;
static HWND hostPointerWindow;
static POINT hostPointerScreen;
static UINT hostPointerButtons;
static UINT hostPointerPressedSinceQuery;
static BOOL hostPointerStateActive;

#define HOSTED_WINDOW_STATE (WS_MINIMIZE | WS_MAXIMIZE)
#define KEEP_HOSTED_WINDOW_STATE ((DWORD)-1)

typedef struct HostedPositionEvent {
    WINDOWPOS position;
    UINT change;
} HostedPositionEvent;

static const HostedPositionEvent *hostedPositionEvent;
static void ClearHostedKeyboard(void);

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

static void InvalidateWindow(XboxWindow *window) {
    AcquireSRWLockExclusive(&windowGeometryLock);
    window->handle = NULL;
    WindowProperty *property = window->properties;
    window->properties = NULL;
    ReleaseSRWLockExclusive(&windowGeometryLock);
    const DWORD error = GetLastError();
    while (property) {
        WindowProperty *next = property->next;
        HeapFree(GetProcessHeap(), 0, property);
        property = next;
    }
    SetLastError(error);
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
    if (hostKeyboardWindow && (!foreground || focus != hostKeyboardWindow)) ClearHostedKeyboard();
    focusWindow = focus;
    // The window procedure may synchronously change activation or destroy a window.
    if (previousForeground != foreground) {
        AcquireSRWLockExclusive(&windowGeometryLock);
        hostPointerStateActive = FALSE;
        hostPointerPressedSinceQuery = 0;
        InterlockedExchangePointer(&foregroundWindow, foreground);
        ReleaseSRWLockExclusive(&windowGeometryLock);
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

// The lock protects window identity and the entire property list. Properties
// remain available to callbacks through WM_NCDESTROY.
static WindowProperty **WindowPropertyList(HWND handle) {
    for (UINT i = 0; i < WINDOW_COUNT; ++i) {
        if (windows[i].handle == handle) return &windows[i].properties;
    }
    return NULL;
}

static LPCWSTR ResolvePropertyName(LPCWSTR name, WCHAR atomName[256]) {
    if (!name) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!IS_INTRESOURCE(name)) return name;
    // Atom lookups are optional on Xbox. String properties need no atom table.
    typedef UINT (WINAPI *Native)(ATOM, LPWSTR, int);
    HMODULE module = GetModuleHandleW(L"kernel32.dll");
    Native getAtomName = module ? (Native)(void *)GetProcAddress(module, "GlobalGetAtomNameW") : NULL;
    if (!getAtomName) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return NULL;
    }
    return getAtomName((ATOM)(ULONG_PTR)name, atomName, 256) ? atomName : NULL;
}

static WindowProperty **FindWindowProperty(WindowProperty **link, LPCWSTR name) {
    while (*link) {
        LPCWSTR current = (*link)->name;
        if (SameClassNameExact(current, name) ||
                CompareStringOrdinal(current, -1, name, -1, TRUE) == CSTR_EQUAL) break;
        link = &(*link)->next;
    }
    return link;
}

BOOL WINAPI XboxSetPropW(HWND handle, LPCWSTR name, HANDLE value) {
    if (!IsVirtualWindowHandle(handle)) {
        typedef BOOL (WINAPI *Native)(HWND, LPCWSTR, HANDLE);
        Native native = (Native)(void *)NativeWindowProcedure("SetPropW");
        return native ? native(handle, name, value) : FALSE;
    }
    const DWORD previousError = GetLastError();
    WCHAR atomName[256];
    name = ResolvePropertyName(name, atomName);
    if (!name) return FALSE;
    SIZE_T length = 0;
    while (length < 256 && name[length]) ++length;
    if (!length || length == 256) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    DWORD error = ERROR_INVALID_WINDOW_HANDLE;
    AcquireSRWLockExclusive(&windowGeometryLock);
    WindowProperty **list = WindowPropertyList(handle);
    if (list) {
        WindowProperty **link = FindWindowProperty(list, name);
        if (!*link) {
            WindowProperty *property = HeapAlloc(GetProcessHeap(), 0,
                sizeof(*property) + (length + 1) * sizeof(WCHAR));
            if (property) {
                for (SIZE_T i = 0; i <= length; ++i) property->name[i] = name[i];
                property->next = NULL;
                *link = property;
            }
        }
        error = *link ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY;
        if (*link) (*link)->value = value;
    }
    ReleaseSRWLockExclusive(&windowGeometryLock);
    SetLastError(error ? error : previousError);
    return error == ERROR_SUCCESS;
}

static HANDLE ReadWindowProperty(HWND handle, LPCWSTR name, BOOL remove) {
    if (!IsVirtualWindowHandle(handle)) {
        typedef HANDLE (WINAPI *Native)(HWND, LPCWSTR);
        Native native = (Native)(void *)NativeWindowProcedure(remove ? "RemovePropW" : "GetPropW");
        return native ? native(handle, name) : NULL;
    }
    const DWORD previousError = GetLastError();
    WCHAR atomName[256];
    name = ResolvePropertyName(name, atomName);
    if (!name) return NULL;
    HANDLE value = NULL;
    if (remove) AcquireSRWLockExclusive(&windowGeometryLock);
    else AcquireSRWLockShared(&windowGeometryLock);
    WindowProperty **list = WindowPropertyList(handle);
    if (list) {
        WindowProperty **link = FindWindowProperty(list, name);
        WindowProperty *property = *link;
        if (property) {
            value = property->value;
            if (remove) {
                *link = property->next;
                HeapFree(GetProcessHeap(), 0, property);
            }
        }
    }
    if (remove) ReleaseSRWLockExclusive(&windowGeometryLock);
    else ReleaseSRWLockShared(&windowGeometryLock);
    SetLastError(list ? previousError : ERROR_INVALID_WINDOW_HANDLE);
    return value;
}

HANDLE WINAPI XboxGetPropW(HWND handle, LPCWSTR name) {
    return ReadWindowProperty(handle, name, FALSE);
}

HANDLE WINAPI XboxRemovePropW(HWND handle, LPCWSTR name) {
    return ReadWindowProperty(handle, name, TRUE);
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
    if (window->messageOnly) return FALSE;
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
    if (window->messageOnly) return NULL;
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
    const BOOL messageOnly = parent == HWND_MESSAGE;
    if ((parent && !messageOnly) || menu) {
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
    if (messageOnly) {
        style &= ~WS_VISIBLE;
        if (width == CW_USEDEFAULT) width = 0;
        if (height == CW_USEDEFAULT) height = 0;
    }
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
    *window = (XboxWindow){ .handle = handle, .procedure = entry->procedure, .instance = classInstance,
        .bounds = bounds, .normalBounds = bounds, .style = style, .extendedStyle = exStyle,
        .classAtom = (ATOM)(entry - classes + 1), .messageOnly = messageOnly };
    ReleaseSRWLockExclusive(&windowGeometryLock);
    CREATESTRUCTW create = {data, classInstance, menu, parent, height, width, y, x, (LONG)style, title, name, exStyle};
    XboxWindowTrace("win32_window_create=nccreate\n");
    if (!window->procedure(handle, WM_NCCREATE, 0, (LPARAM)&create)) {
        XboxWindowTrace("win32_window_create=nccreate-refused\n");
        if (window->handle == handle) {
            window->destroying = TRUE;
            ClearWindowActivation(handle);
            window->procedure(handle, WM_NCDESTROY, 0, 0);
            InvalidateWindow(window);
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
    XboxWindowTrace(messageOnly ? "win32_window=message-only-created\n" : "win32_window=created\n");
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
    InvalidateWindow(window);
    for (UINT i = 0; i < messageCount;) {
        if (messages[i].value.hwnd != handle) { ++i; continue; }
        for (UINT j = i + 1; j < messageCount; ++j) messages[j - 1] = messages[j];
        --messageCount;
    }
    return TRUE;
}

LRESULT WINAPI XboxDefWindowProcW(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
    (void)wparam;
    if (message == WM_NCCREATE || message == WM_NCACTIVATE || message == WM_QUERYOPEN) return TRUE;
    if (message == WM_CLOSE) XboxDestroyWindow(handle);
    if (message == WM_WINDOWPOSCHANGED && lparam) {
        XboxWindow *window = XboxFindWindow(handle);
        if (!window) return 0;
        if (hostedPositionEvent && lparam == (LPARAM)&hostedPositionEvent->position &&
                window->positionChange != hostedPositionEvent->change) return 0;
        const UINT flags = ((const WINDOWPOS *)lparam)->flags;
        const RECT bounds = window->bounds;
        const UINT change = window->positionChange;
        if (!(flags & SWP_NOMOVE)) window->procedure(handle, WM_MOVE, 0, MAKELPARAM(bounds.left, bounds.top));
        if (window->handle == handle && !window->destroying && window->positionChange == change && !(flags & SWP_NOSIZE)) {
            const WPARAM state = (window->style & WS_MINIMIZE) ? SIZE_MINIMIZED :
                (window->style & WS_MAXIMIZE) ? SIZE_MAXIMIZED : SIZE_RESTORED;
            const LPARAM size = state == SIZE_MINIMIZED ? 0 :
                MAKELPARAM(bounds.right - bounds.left, bounds.bottom - bounds.top);
            window->procedure(handle, WM_SIZE, state, size);
        }
    }
    return 0;
}

LRESULT WINAPI XboxCallWindowProcW(WNDPROC procedure, HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
    if (!IsVirtualWindowHandle(handle)) {
        typedef LRESULT (WINAPI *Native)(WNDPROC, HWND, UINT, WPARAM, LPARAM);
        const Native native = (Native)(void *)NativeWindowProcedure("CallWindowProcW");
        return native ? native(procedure, handle, message, wparam, lparam) : 0;
    }
    if (!procedure || !XboxFindWindow(handle)) return 0;
    // Hosted registration and subclassing return Unicode callback pointers.
    // Native USER32 cannot look up the private HWND to dispatch them.
    if (message == WM_NCCREATE) XboxWindowTrace("win32_window_callproc=hosted-nccreate\n");
    return procedure(handle, message, wparam, lparam);
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

static BOOL UsesHostedFrame(DWORD style, BOOL menu, DWORD extendedStyle) {
    if (menu || (style & WS_CHILD) || ((style & WS_POPUP) && (extendedStyle & WS_EX_TOOLWINDOW))) return FALSE;
    const DWORD previous = GetLastError();
    WCHAR enabled[2];
    const BOOL hosted = GetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_WINDOW_FRAME", enabled, ARRAYSIZE(enabled)) == 1 &&
        enabled[0] == L'1';
    SetLastError(previous);
    return hosted;
}

static BOOL AdjustHostedWindowRect(RECT *area) {
    if (!area || area->right < area->left || area->bottom < area->top ||
            (LONG64)area->right - area->left > MAXLONG || (LONG64)area->bottom - area->top > MAXLONG) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    // The hosted window has no nonclient frame. Its outer and client rectangles coincide.
    return TRUE;
}

BOOL WINAPI XboxAdjustWindowRectEx(RECT *area, DWORD style, BOOL menu, DWORD extendedStyle) {
    if (UsesHostedFrame(style, menu, extendedStyle)) return AdjustHostedWindowRect(area);
    typedef BOOL (WINAPI *Native)(RECT *, DWORD, BOOL, DWORD);
    Native native = (Native)(void *)NativeWindowProcedure("AdjustWindowRectEx");
    return native ? native(area, style, menu, extendedStyle) : FALSE;
}

BOOL WINAPI XboxAdjustWindowRectExForDpi(RECT *area, DWORD style, BOOL menu, DWORD extendedStyle, UINT dpi) {
    if (!UsesHostedFrame(style, menu, extendedStyle)) {
        typedef BOOL (WINAPI *Native)(RECT *, DWORD, BOOL, DWORD, UINT);
        Native native = (Native)(void *)NativeWindowProcedure("AdjustWindowRectExForDpi");
        return native ? native(area, style, menu, extendedStyle, dpi) : FALSE;
    }
    if (!dpi) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return AdjustHostedWindowRect(area);
}

UINT WINAPI XboxGetDpiForWindow(HWND handle) {
    if (IsVirtualWindowHandle(handle)) {
        RECT area;
        return HostedWindowBounds(handle, &area) ? XBOX_WINDOW_DPI : 0;
    }
    typedef UINT (WINAPI *Native)(HWND);
    Native native = (Native)(void *)NativeWindowProcedure("GetDpiForWindow");
    return native ? native(handle) : 0;
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
    XboxWindow *window = XboxFindWindow(handle);
    return window && !window->messageOnly ? (HDC)handle : NULL;
}

int WINAPI XboxReleaseDC(HWND handle, HDC dc) {
    if (!handle) return dc == XBOX_DESKTOP_DC;
    return handle == (HWND)dc && XboxFindWindow(handle) ? 1 : 0;
}

static BOOL ShowHostedWindow(XboxWindow *window, HWND handle, int command, BOOL notify) {
    if (window->messageOnly) return FALSE;
    const BOOL previous = (window->style & WS_VISIBLE) != 0;
    const BOOL showing = command != SW_HIDE;
    const UINT change = activationChange;
    if (previous != showing) {
        if (showing) window->style |= WS_VISIBLE;
        else window->style &= ~WS_VISIBLE;
        if (notify) window->procedure(handle, WM_SHOWWINDOW, showing, 0);
        if (!showing && window->handle == handle && !(window->style & WS_VISIBLE)) ClearWindowActivation(handle);
    }
    if (activationChange == change && window->handle == handle && !window->destroying &&
            (window->style & (WS_VISIBLE | WS_MINIMIZE)) == WS_VISIBLE && (command == SW_SHOW || command == SW_SHOWNORMAL ||
            command == SW_RESTORE || command == SW_SHOWMAXIMIZED || command == SW_SHOWMINIMIZED ||
            command == SW_SHOWDEFAULT)) {
        XboxWindowTrace("win32_window_show=activate\n");
        UpdateWindowActivation(handle, handle);
    }
    return previous;
}

static void NotifyHostedPosition(XboxWindow *window, HWND handle, HWND insertAfter, RECT previous,
        BOOL wasVisible, UINT flags, BOOL stateChanged) {
    if (window->handle != handle || window->destroying) return;
    const RECT bounds = window->bounds;
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    const BOOL visible = (window->style & WS_VISIBLE) != 0;
    flags &= ~(SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_HIDEWINDOW);
    if (bounds.left == previous.left && bounds.top == previous.top) flags |= SWP_NOMOVE;
    if (!stateChanged && width == previous.right - previous.left && height == previous.bottom - previous.top)
        flags |= SWP_NOSIZE;
    if (visible != wasVisible) flags |= visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
    if ((flags & (SWP_NOMOVE | SWP_NOSIZE)) == (SWP_NOMOVE | SWP_NOSIZE) &&
            !(flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW | SWP_FRAMECHANGED))) return;
    HostedPositionEvent event = {{handle, insertAfter, bounds.left, bounds.top, width, height, flags},
        window->positionChange};
    const HostedPositionEvent *previousEvent = hostedPositionEvent;
    hostedPositionEvent = &event;
    window->procedure(handle, WM_WINDOWPOSCHANGED, 0, (LPARAM)&event.position);
    hostedPositionEvent = previousEvent;
}

BOOL WINAPI XboxIsWindowVisible(HWND handle) {
    XboxWindow *window = XboxFindWindow(handle);
    return window && (window->style & WS_VISIBLE);
}

static BOOL SetHostedWindowPosition(XboxWindow *window, WINDOWPOS position, DWORD state) {
    const HWND handle = position.hwnd;
    const DWORD previousState = window->style & HOSTED_WINDOW_STATE;
    const BOOL stateChanged = state != KEEP_HOSTED_WINDOW_STATE && state != previousState;
    UINT flags = position.flags;
    if (!(flags & SWP_NOSENDCHANGING)) {
        window->procedure(handle, WM_WINDOWPOSCHANGING, 0, (LPARAM)&position);
        if (window->handle != handle || window->destroying) {
            SetLastError(ERROR_INVALID_WINDOW_HANDLE);
            return FALSE;
        }
        if (stateChanged && (window->style & HOSTED_WINDOW_STATE) != previousState) return TRUE;
    }
    const RECT previous = window->bounds;
    const BOOL wasVisible = (window->style & WS_VISIBLE) != 0;
    const UINT activationFlags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    flags = (position.flags & ~activationFlags) | (flags & activationFlags);
    const int x = (flags & SWP_NOMOVE) ? previous.left : position.x;
    const int y = (flags & SWP_NOMOVE) ? previous.top : position.y;
    const int width = (flags & SWP_NOSIZE) ? previous.right - previous.left : position.cx;
    const int height = (flags & SWP_NOSIZE) ? previous.bottom - previous.top : position.cy;
    if (width < 0 || height < 0 || (LONG64)x + width > MAXLONG || (LONG64)y + height > MAXLONG) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const BOOL sized = width != previous.right - previous.left || height != previous.bottom - previous.top;
    AcquireSRWLockExclusive(&windowGeometryLock);
    if (stateChanged) {
        window->restoreMaximized = state == WS_MINIMIZE && (previousState & WS_MAXIMIZE);
        window->style = (window->style & ~HOSTED_WINDOW_STATE) | state;
    }
    window->bounds = (RECT){x, y, x + width, y + height};
    if (!(window->style & HOSTED_WINDOW_STATE)) window->normalBounds = window->bounds;
    if (stateChanged || sized || x != previous.left || y != previous.top) ++window->positionChange;
    ReleaseSRWLockExclusive(&windowGeometryLock);
    const UINT change = window->positionChange;
    if (sized) XboxResizeCurrentContext(window);
    if (window->handle == handle && (flags & SWP_SHOWWINDOW))
        ShowHostedWindow(window, handle, (flags & SWP_NOACTIVATE) ? SW_SHOWNA : SW_SHOW, !stateChanged);
    if (window->handle == handle && (flags & SWP_HIDEWINDOW)) ShowHostedWindow(window, handle, SW_HIDE, !stateChanged);
    if (window->handle == handle && window->positionChange == change && stateChanged && state == WS_MINIMIZE)
        ClearWindowActivation(handle);
    if (window->positionChange == change) {
        NotifyHostedPosition(window, handle, position.hwndInsertAfter, previous, wasVisible, flags, stateChanged);
    } else {
        // A callback moved the window. The outer request can still have made it visible.
        NotifyHostedPosition(window, handle, position.hwndInsertAfter, window->bounds, wasVisible,
            flags & ~SWP_FRAMECHANGED, FALSE);
    }
    return window->handle == handle;
}

BOOL WINAPI XboxSetWindowPos(HWND handle, HWND insertAfter, int x, int y, int width, int height, UINT flags) {
    XboxWindow *window = XboxFindWindow(handle);
    if (!window || window->destroying) return FALSE;
    return SetHostedWindowPosition(window, (WINDOWPOS){handle, insertAfter, x, y, width, height, flags},
        KEEP_HOSTED_WINDOW_STATE);
}

static DWORD HostedShowState(const XboxWindow *window, int command) {
    switch (command) {
    case SW_SHOWMAXIMIZED: return WS_MAXIMIZE;
    case SW_SHOWMINIMIZED:
    case SW_SHOWMINNOACTIVE:
    case SW_MINIMIZE:
    case SW_FORCEMINIMIZE: return WS_MINIMIZE;
    case SW_SHOWNOACTIVATE: return 0;
    case SW_SHOWNORMAL:
    case SW_SHOWDEFAULT:
    case SW_RESTORE:
        return (window->style & WS_MINIMIZE) && window->restoreMaximized ? WS_MAXIMIZE : 0;
    default: return window->style & HOSTED_WINDOW_STATE;
    }
}

BOOL WINAPI XboxShowWindow(HWND handle, int command) {
    XboxWindow *window = XboxFindWindow(handle);
    if (!window || window->destroying || window->messageOnly) return FALSE;
    const BOOL previous = (window->style & WS_VISIBLE) != 0;
    const DWORD current = window->style & HOSTED_WINDOW_STATE;
    const DWORD state = HostedShowState(window, command);
    if (state == current) {
        ShowHostedWindow(window, handle, command, TRUE);
        NotifyHostedPosition(window, handle, NULL, window->bounds, previous, SWP_NOZORDER, FALSE);
        return previous;
    }
    if (current & WS_MINIMIZE) {
        if (!window->procedure(handle, WM_QUERYOPEN, 0, 0) || window->handle != handle || window->destroying ||
                (window->style & HOSTED_WINDOW_STATE) != current) return previous;
    }
    UINT flags = SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW;
    if (command == SW_SHOWNOACTIVATE || state == WS_MINIMIZE) flags |= SWP_NOACTIVATE;
    // The host has no minimized icon rectangle. Keep the render surface and report a minimized client in WM_SIZE.
    if (state == WS_MINIMIZE) flags |= SWP_NOMOVE | SWP_NOSIZE;
    const RECT bounds = state == WS_MAXIMIZE ? XboxDesktopBounds() : window->normalBounds;
    SetHostedWindowPosition(window, (WINDOWPOS){handle, NULL, bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top, flags}, state);
    return previous;
}

static BOOL ReadWindowState(HWND handle, DWORD state, LPCSTR name) {
    if (!IsVirtualWindowHandle(handle)) {
        typedef BOOL (WINAPI *Native)(HWND);
        Native native = (Native)(void *)NativeWindowProcedure(name);
        return native ? native(handle) : FALSE;
    }
    XboxWindow *window = XboxFindWindow(handle);
    return window && (window->style & state);
}

BOOL WINAPI XboxIsIconic(HWND handle) { return ReadWindowState(handle, WS_MINIMIZE, "IsIconic"); }
BOOL WINAPI XboxIsZoomed(HWND handle) { return ReadWindowState(handle, WS_MAXIMIZE, "IsZoomed"); }

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

LONG WINAPI XboxGetWindowLongW(HWND handle, int index) {
    if (!IsVirtualWindowHandle(handle)) {
        typedef LONG (WINAPI *Native)(HWND, int);
        Native native = (Native)(void *)NativeWindowProcedure("GetWindowLongW");
        return native ? native(handle, index) : 0;
    }
    if (index == GWLP_WNDPROC || index == GWLP_HINSTANCE || index == GWLP_HWNDPARENT) {
        SetLastError(ERROR_INVALID_INDEX);
        return 0;
    }
    return (LONG)XboxGetWindowLongPtrW(handle, index);
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
    case GWL_STYLE:
        if (window->messageOnly) value &= ~(LONG_PTR)WS_VISIBLE;
        if ((window->style ^ (DWORD)value) & HOSTED_WINDOW_STATE) ++window->positionChange;
        window->style = (DWORD)value;
        break;
    case GWL_EXSTYLE: window->extendedStyle = (DWORD)value; break;
    default: SetLastError(ERROR_INVALID_INDEX); return 0;
    }
    return previous;
}

LONG WINAPI XboxSetWindowLongW(HWND handle, int index, LONG value) {
    if (!IsVirtualWindowHandle(handle)) {
        typedef LONG (WINAPI *Native)(HWND, int, LONG);
        Native native = (Native)(void *)NativeWindowProcedure("SetWindowLongW");
        return native ? native(handle, index, value) : 0;
    }
    if (index == GWLP_WNDPROC || index == GWLP_HINSTANCE || index == GWLP_HWNDPARENT) {
        SetLastError(ERROR_INVALID_INDEX);
        return 0;
    }
    return (LONG)XboxSetWindowLongPtrW(handle, index, value);
}

BOOL WINAPI XboxPostMessageW(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
    if (!XboxFindWindow(handle)) return FALSE;
    if (messageCount == MESSAGE_COUNT) {
        SetLastError(ERROR_NOT_ENOUGH_QUOTA);
        return FALSE;
    }
    messages[messageCount++] = (HostedMessage){{handle, message, wparam, lparam, GetTickCount(), {0, 0}}, 0, 0, FALSE};
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
    const BOOL keyboardEnabled = GetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_KEYBOARD", mode, ARRAYSIZE(mode)) == 1 &&
        mode[0] == L'1';
    if (keyboardEnabled && size.QuadPart >= (LONGLONG)sizeof(SdlHostMailbox)) {
        hostPointerMailbox = MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, sizeof(SdlHostMailbox));
        if (hostPointerMailbox) hostKeyboard = &hostPointerMailbox->keyboard;
    }
    if (!hostPointerMailbox) hostPointerMailbox = MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, header);
    CloseHandle(mapping);
    if (hostPointerMailbox && SdlHostLoad(&hostPointerMailbox->magic) != SDL_HOST_MAGIC) {
        UnmapViewOfFile(hostPointerMailbox);
        hostPointerMailbox = NULL;
        hostKeyboard = NULL;
    }
}

static BYTE HostedKeyState(UINT key);

static void PollHostedPointer(void) {
    const HWND handle = VirtualForegroundWindow();
    if (!handle) return;
    if (!hostPointerConfigured) OpenHostedPointer();
    SdlHostMailbox *mailbox = hostPointerMailbox;
    if (!mailbox || SdlHostLoad(&mailbox->quit)) return;
    const LONG serial = SdlHostLoad(&mailbox->input_serial);
    const UINT pointer = (UINT)SdlHostLoad(&mailbox->input_pointer);
    const UINT keys = (UINT)SdlHostLoad(&mailbox->input_keys);
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
    static const struct { UINT mask, flag, down, up, xbutton; } bindings[] = {
        {SDL_HOST_POINTER_LEFT, MK_LBUTTON, WM_LBUTTONDOWN, WM_LBUTTONUP, 0},
        {SDL_HOST_POINTER_RIGHT, MK_RBUTTON, WM_RBUTTONDOWN, WM_RBUTTONUP, 0},
        {SDL_HOST_POINTER_MIDDLE, MK_MBUTTON, WM_MBUTTONDOWN, WM_MBUTTONUP, 0},
        {SDL_HOST_POINTER_X1, MK_XBUTTON1, WM_XBUTTONDOWN, WM_XBUTTONUP, XBUTTON1},
        {SDL_HOST_POINTER_X2, MK_XBUTTON2, WM_XBUTTONDOWN, WM_XBUTTONUP, XBUTTON2}
    };
    const UINT pressed = pointer & SDL_HOST_POINTER_BUTTONS;
    const UINT changed = pressed ^ (handle == hostPointerWindow ? hostPointerButtons : 0);
    const SHORT wheel = serial != hostPointerSerial ? (SHORT)(keys >> 16) : 0;
    const BOOL moved = handle != hostPointerWindow || screen.x != hostPointerScreen.x || screen.y != hostPointerScreen.y;
    UINT count = moved + (wheel != 0);
    WPARAM buttons = ((HostedKeyState(VK_SHIFT) & 0x80) ? MK_SHIFT : 0) |
        ((HostedKeyState(VK_CONTROL) & 0x80) ? MK_CONTROL : 0);
    for (UINT i = 0; i < ARRAYSIZE(bindings); ++i) {
        if (changed & bindings[i].mask) ++count;
        if (pressed & bindings[i].mask) buttons |= bindings[i].flag;
    }
    if (count > MESSAGE_COUNT - messageCount) return;
    const LPARAM position = MAKELPARAM(x, y);
    const DWORD time = GetTickCount();
    if (moved) messages[messageCount++] = (HostedMessage){{handle, WM_MOUSEMOVE, buttons, position, time, screen}, 0, 0, FALSE};
    for (UINT i = 0; i < ARRAYSIZE(bindings); ++i) {
        if (!(changed & bindings[i].mask)) continue;
        const UINT message = (pressed & bindings[i].mask) ? bindings[i].down : bindings[i].up;
        messages[messageCount++] = (HostedMessage){{handle, message, MAKEWPARAM(buttons, bindings[i].xbutton),
            position, time, screen}, 0, 0, FALSE};
    }
    if (wheel) {
        const UINT message = (keys & SDL_HOST_WHEEL_HORIZONTAL) ? WM_MOUSEHWHEEL : WM_MOUSEWHEEL;
        messages[messageCount++] = (HostedMessage){{handle, message, MAKEWPARAM(buttons, wheel),
            MAKELPARAM(screen.x, screen.y), time, screen}, 0, 0, FALSE};
    }
    AcquireSRWLockExclusive(&windowGeometryLock);
    if (hostPointerWindow != handle) hostPointerPressedSinceQuery = 0;
    hostPointerScreen = screen;
    hostPointerWindow = handle;
    hostPointerButtons = pressed;
    hostPointerStateActive = TRUE;
    hostPointerPressedSinceQuery |= pressed & changed;
    ReleaseSRWLockExclusive(&windowGeometryLock);
    hostPointerSerial = serial;
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

SHORT WINAPI XboxGetAsyncKeyState(int key) {
    UINT button;
    switch (key) {
    case VK_LBUTTON: button = SDL_HOST_POINTER_LEFT; break;
    case VK_RBUTTON: button = SDL_HOST_POINTER_RIGHT; break;
    case VK_MBUTTON: button = SDL_HOST_POINTER_MIDDLE; break;
    case VK_XBUTTON1: button = SDL_HOST_POINTER_X1; break;
    case VK_XBUTTON2: button = SDL_HOST_POINTER_X2; break;
    default: button = 0; break;
    }
    if (button) {
        AcquireSRWLockExclusive(&windowGeometryLock);
        const BOOL hosted = hostPointerStateActive && hostPointerWindow == VirtualForegroundWindow() &&
            !SdlHostLoad(&hostPointerMailbox->quit);
        if (hosted) {
            const SHORT state = (SHORT)(((hostPointerButtons & button) ? 0x8000 : 0) |
                ((hostPointerPressedSinceQuery & button) ? 1 : 0));
            hostPointerPressedSinceQuery &= ~button;
            ReleaseSRWLockExclusive(&windowGeometryLock);
            return state;
        }
        ReleaseSRWLockExclusive(&windowGeometryLock);
    }
    typedef SHORT (WINAPI *Native)(int);
    Native native = (Native)(void *)NativeWindowProcedure("GetAsyncKeyState");
    return native ? native(key) : 0;
}

typedef UINT (WINAPI *MapKey)(UINT, UINT);
static MapKey mapHostedKey;

static BYTE HostedVirtualKey(UINT scan) {
    if (scan == 72) return VK_PAUSE;
    const UINT key = mapHostedKey(hostedKeyScans[scan], MAPVK_VSC_TO_VK_EX);
    return key < ARRAYSIZE(hostedKeys) ? (BYTE)key : 0;
}

static BYTE HostedKeyState(UINT key) {
    switch (key) {
    case VK_SHIFT: return hostedKeys[VK_LSHIFT] | hostedKeys[VK_RSHIFT];
    case VK_CONTROL: return hostedKeys[VK_LCONTROL] | hostedKeys[VK_RCONTROL];
    case VK_MENU: return hostedKeys[VK_LMENU] | hostedKeys[VK_RMENU];
    default: return key < ARRAYSIZE(hostedKeys) ? hostedKeys[key] : 0;
    }
}

static BOOL HasHostedKeyboard(void) {
    return (LONG)GetCurrentThreadId() == windowThread && hostKeyboardWindow && hostKeyboardWindow == focusWindow &&
        !SdlHostLoad(&hostPointerMailbox->quit);
}

SHORT WINAPI XboxGetKeyState(int key) {
    if (HasHostedKeyboard()) {
        const BYTE state = HostedKeyState((UINT)key);
        return (SHORT)(((UINT)state & 0x80) << 8 | (state & 1));
    }
    typedef SHORT (WINAPI *Native)(int);
    Native native = (Native)(void *)NativeWindowProcedure("GetKeyState");
    return native ? native(key) : 0;
}

BOOL WINAPI XboxGetKeyboardState(PBYTE keys) {
    if (HasHostedKeyboard()) {
        if (!keys) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        for (UINT key = 0; key < ARRAYSIZE(hostedKeys); ++key) keys[key] = HostedKeyState(key);
        return TRUE;
    }
    typedef BOOL (WINAPI *Native)(PBYTE);
    Native native = (Native)(void *)NativeWindowProcedure("GetKeyboardState");
    return native ? native(keys) : FALSE;
}

BOOL WINAPI XboxTranslateMessage(const MSG *message) {
    // The host supplies Unicode separately. Native translation would duplicate that text.
    if (message && HasHostedKeyboard() && message->hwnd == hostKeyboardWindow) return FALSE;
    typedef BOOL (WINAPI *Native)(const MSG *);
    Native native = (Native)(void *)NativeWindowProcedure("TranslateMessage");
    return native ? native(message) : FALSE;
}

static void DropHostedKeyboardMessages(void) {
    UINT retained = 0;
    for (UINT i = 0; i < messageCount; ++i) {
        if (!messages[i].keyboard) messages[retained++] = messages[i];
    }
    messageCount = retained;
}

static void ClearHostedKeyboard(void) {
    InterlockedExchange(&hostKeyboard->available, 0);
    DropHostedKeyboardMessages();
    for (UINT scan = 0; scan < ARRAYSIZE(heldKeys); ++scan) heldKeys[scan] = 0;
    for (UINT key = 0; key < ARRAYSIZE(hostedKeys); ++key) hostedKeys[key] &= 1;
    InterlockedExchange(&hostKeyboard->read, SdlHostLoad(&hostKeyboard->write));
    hostKeyboardWindow = NULL;
}

static void QueueHostedKey(HWND window, UINT16 scan, BYTE key, BOOL down, DWORD time) {
    messages[messageCount++] = (HostedMessage){{window, down ? WM_KEYDOWN : WM_KEYUP, HostedKeyModifier(key),
        HostedKeyFlags(hostedKeyScans[scan], down), time, {0, 0}}, scan, key, TRUE};
}

static BOOL QueueHostedText(HWND window, const char *text, DWORD time) {
    UINT bytes = 0;
    while (bytes < SDL_HOST_TEXT_BYTES && text[bytes]) ++bytes;
    if (!bytes || bytes == SDL_HOST_TEXT_BYTES) return TRUE;
    WCHAR utf16[SDL_HOST_TEXT_BYTES];
    const UINT count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, bytes, utf16, ARRAYSIZE(utf16));
    if (count > MESSAGE_COUNT - messageCount) return FALSE;
    for (UINT i = 0; i < count; ++i)
        messages[messageCount++] = (HostedMessage){{window, WM_CHAR, utf16[i], 1, time, {0, 0}}, 0, 0, TRUE};
    return TRUE;
}

static BOOL ReleaseHostedKeys(HWND window, DWORD time) {
    UINT count = 0;
    for (UINT scan = 0; scan < ARRAYSIZE(heldKeys); ++scan) count += heldKeys[scan] != 0;
    if (count > MESSAGE_COUNT - messageCount) return FALSE;
    for (UINT16 scan = 0; scan < ARRAYSIZE(heldKeys); ++scan) {
        if (heldKeys[scan]) QueueHostedKey(window, scan, heldKeys[scan], FALSE, time);
    }
    return TRUE;
}

static void PollHostedKeyboard(void) {
    SdlHostKeyboard *keyboard = hostKeyboard;
    if (!keyboard) return;
    const HWND target = VirtualForegroundWindow() && !SdlHostLoad(&hostPointerMailbox->quit) ? focusWindow : NULL;
    if (target != hostKeyboardWindow) {
        if (hostKeyboardWindow) ClearHostedKeyboard();
        if (!target) return;
        if (!mapHostedKey) mapHostedKey = (MapKey)(void *)NativeWindowProcedure("MapVirtualKeyW");
        if (!mapHostedKey) return;
        hostKeyboardWindow = target;
        hostKeyboardReset = (UINT32)SdlHostLoad(&keyboard->reset);
        InterlockedExchange(&keyboard->read, SdlHostLoad(&keyboard->write));
        InterlockedExchange(&keyboard->available, 1);
        XboxWindowTrace("win32_host_keyboard=ready\n");
    }
    if (!target) return;
    const UINT32 reset = (UINT32)SdlHostLoad(&keyboard->reset);
    const UINT32 write = (UINT32)SdlHostLoad(&keyboard->write);
    UINT32 read = (UINT32)SdlHostLoad(&keyboard->read);
    const DWORD time = GetTickCount();
    const BOOL invalidQueue = write - read > SDL_HOST_KEY_QUEUE_SIZE;
    if (reset != hostKeyboardReset || invalidQueue) {
        DropHostedKeyboardMessages();
        if (!ReleaseHostedKeys(target, time)) return;
        if (invalidQueue) read = write;
        else if (reset - read <= write - read) read = reset;
        hostKeyboardReset = reset;
    }
    while (read != write && messageCount < MESSAGE_COUNT) {
        const SdlHostKey *entry = &keyboard->entries[read % SDL_HOST_KEY_QUEUE_SIZE];
        const UINT16 scan = entry->scan;
        if (scan && scan < ARRAYSIZE(hostedKeyScans) && hostedKeyScans[scan]) {
            const BYTE key = HostedVirtualKey(scan);
            if (key) QueueHostedKey(target, scan, key, entry->down != 0, time);
        } else if (!scan && !QueueHostedText(target, entry->text, time)) {
            break;
        }
        ++read;
    }
    if (reset != (UINT32)SdlHostLoad(&keyboard->reset)) {
        DropHostedKeyboardMessages();
        return;
    }
    InterlockedExchange(&keyboard->read, (LONG)read);
}

static void RecordHostedKey(const HostedMessage *event) {
    const BOOL down = event->value.message == WM_KEYDOWN;
    BYTE toggle = hostedKeys[event->key] & 1;
    if (down && !heldKeys[event->scan] &&
            (event->key == VK_CAPITAL || event->key == VK_NUMLOCK || event->key == VK_SCROLL)) toggle ^= 1;
    heldKeys[event->scan] = down ? event->key : 0;
    if (!down) {
        for (UINT scan = 0; scan < ARRAYSIZE(heldKeys); ++scan) {
            if (heldKeys[scan] == event->key) return;
        }
    }
    hostedKeys[event->key] = toggle | (down ? 0x80 : 0);
}

BOOL WINAPI XboxPeekMessageW(MSG *message, HWND filter, UINT first, UINT last, UINT remove) {
    if (!XboxWindowThread() || !message) return FALSE;
    XboxPollWglHostEvents();
    const DWORD error = GetLastError();
    PollHostedPointer();
    PollHostedKeyboard();
    SetLastError(error);
    HWND close = InterlockedExchangePointer(&hostClose, NULL);
    if (close && XboxFindWindow(close) && !XboxPostMessageW(close, WM_CLOSE, 0, 0))
        InterlockedCompareExchangePointer(&hostClose, close, NULL);
    for (UINT i = 0; i < messageCount; ++i) {
        const HostedMessage *event = &messages[i];
        const MSG *candidate = &event->value;
        if ((filter && filter != candidate->hwnd) ||
                ((first || last) && (candidate->message < first || candidate->message > last))) continue;
        *message = *candidate;
        if (event->scan && heldKeys[event->scan]) message->lParam |= (LPARAM)(1u << 30);
        if (remove & PM_REMOVE) {
            if (event->scan) RecordHostedKey(event);
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
