/* Distinguish a missing desktop export from an exported function that cannot run. */
#define IMPORT __declspec(dllimport)
#define WINAPI __stdcall
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x800u

typedef unsigned short WCHAR;
typedef unsigned long DWORD;
typedef void *Handle;
typedef void (WINAPI *Procedure)(void);
typedef long long (WINAPI *WindowProcedure)(Handle, unsigned int, unsigned long long, long long);
typedef Handle (WINAPI *CreateWindow)(DWORD, const WCHAR *, const WCHAR *, DWORD, int, int, int, int,
    Handle, Handle, Handle, void *);
typedef int (WINAPI *DestroyWindow)(Handle);

typedef struct {
    unsigned int size;
    unsigned int style;
    WindowProcedure windowProcedure;
    int extraClassBytes;
    int extraWindowBytes;
    Handle instance;
    Handle icon;
    Handle cursor;
    Handle background;
    const WCHAR *menu;
    const WCHAR *name;
    Handle smallIcon;
} WindowClass;

IMPORT Handle WINAPI LoadLibraryExW(const WCHAR *name, Handle file, DWORD flags);
IMPORT Handle WINAPI GetModuleHandleW(const WCHAR *name);
IMPORT Procedure WINAPI GetProcAddress(Handle module, const char *name);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT __declspec(noreturn) void WINAPI ExitProcess(unsigned int code);

static WindowProcedure defaultWindowProcedure;

static long long WINAPI OnMessage(Handle window, unsigned int message, unsigned long long wparam, long long lparam) {
    return defaultWindowProcedure(window, message, wparam, lparam);
}

static unsigned int CheckWindow(Handle user) {
    typedef unsigned short (WINAPI *RegisterWindow)(const WindowClass *);
    if (!user) return 0;
    RegisterWindow registerWindow = (RegisterWindow)GetProcAddress(user, "RegisterClassExW");
    CreateWindow createWindow = (CreateWindow)GetProcAddress(user, "CreateWindowExW");
    DestroyWindow destroyWindow = (DestroyWindow)GetProcAddress(user, "DestroyWindow");
    defaultWindowProcedure = (WindowProcedure)GetProcAddress(user, "DefWindowProcW");
    if (!registerWindow || !createWindow || !destroyWindow || !defaultWindowProcedure) return 0;

    const Handle instance = GetModuleHandleW(0);
    const WindowClass windowClass = {sizeof(WindowClass), 0, OnMessage, 0, 0, instance, 0, 0, 0, 0,
        L"XboxDesktopWindowControl", 0};
    if (!registerWindow(&windowClass)) return 0x1000u | (GetLastError() & 0x0fffu);
    const Handle window = createWindow(0, windowClass.name, L"", 0, 0, 0, 64, 64, 0, 0, instance, 0);
    if (!window) return 0x2000u | (GetLastError() & 0x0fffu);
    destroyWindow(window);
    return 0x3000u;
}

#ifdef PROBE_DESKTOP_ENDPOINTS
static unsigned int TrySystemWindow(Handle user, const WCHAR *className, Handle parent) {
    if (!user) return 255;
    CreateWindow create = (CreateWindow)GetProcAddress(user, "CreateWindowExW");
    DestroyWindow destroy = (DestroyWindow)GetProcAddress(user, "DestroyWindow");
    if (!create || !destroy) return 255;
    Handle window = create(0, className, L"", 0, 0, 0, 64, 64, parent, 0, GetModuleHandleW(0), 0);
    if (window) {
        destroy(window);
        return 0;
    }
    DWORD error = GetLastError();
    return error && error < 255 ? error : 255;
}
#endif

void WINAPI ChildEntry(void) {
    const Handle user = LoadLibraryExW(L"user32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
#ifdef PROBE_DESKTOP_ENDPOINTS
    const unsigned int registered = CheckWindow(user);
    const unsigned int hidden = TrySystemWindow(user, L"STATIC", 0);
    const unsigned int message = TrySystemWindow(user, L"STATIC", (Handle)(long long)-3);
    const unsigned int registerError = registered ? registered & 255u : 255;
    ExitProcess(0x6B000000u | (registerError << 16) | (hidden << 8) | message);
#else
    unsigned int modules = 0;
    if (LoadLibraryExW(L"msvcp140.dll", 0, 0)) modules |= 1;
    if (LoadLibraryExW(L"msvcp140_app.dll", 0, 0)) modules |= 2;
    Handle adapter = LoadLibraryExW(L"XboxWin32Desktop.dll", 0, 0);
    if (adapter && GetProcAddress(adapter, (const char *)189) && GetProcAddress(adapter, "PlaySoundA")) modules |= 4;
    adapter = LoadLibraryExW(L"XboxWin32Input.dll", 0, 0);
    if (adapter && GetProcAddress(adapter, "LoadCursorFromFileW")) modules |= 8;

    ExitProcess(0x69000000u | (modules << 16) | CheckWindow(user));
#endif
}
