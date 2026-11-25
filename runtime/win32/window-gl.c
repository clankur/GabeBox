#include "window.h"
#include "../shared/steam-handoff-control.h"
#include <appmodel.h>
#include "shell-lookup.h"

#define CONTEXT_COUNT 16
#define SDL_INIT_VIDEO 0x20u
#define SDL_WINDOW_OPENGL 2u
#define SDL_WINDOW_HIDDEN 8u
#define SDL_WINDOWEVENT 0x200u
#define SDL_WINDOWEVENT_CLOSE 14u
#define SDL_KEYDOWN 0x300u
#define SDL_KEYUP 0x301u
#define SDL_TEXTINPUT 0x303u
#define SDL_MOUSEMOTION 0x400u
#define SDL_MOUSEBUTTONDOWN 0x401u
#define SDL_MOUSEBUTTONUP 0x402u
#define SDL_MOUSEWHEEL 0x403u
#define SDL_MOUSEWHEEL_FLIPPED 1u
#define SDL_GETEVENT 2

/* SDL 2.x x64 event ABI. */
typedef union SdlWindowEvent {
    UINT type;
    struct { UINT type, timestamp, windowId; BYTE event; } window;
    struct { UINT type, timestamp, windowId; BYTE state, repeat, padding2, padding3; int scancode, sym; } key;
    struct { UINT type, timestamp, windowId; char text[32]; } text;
    struct { UINT type, timestamp, windowId, which, state; int x, y, xrel, yrel; } motion;
    struct { UINT type, timestamp, windowId, which; BYTE button, state, clicks, padding; int x, y; } button;
    struct { UINT type, timestamp, windowId, which; int x, y; UINT direction; } wheel;
    ULONG64 padding[7];
} SdlWindowEvent;
_Static_assert(sizeof(SdlWindowEvent) == 56, "SDL_Event size");

typedef struct SdlGl {
    void (*ready)(void);
    int (*init)(unsigned int);
    unsigned int (*wasInit)(unsigned int);
    const char *(*driver)(void);
    void *(*createWindow)(const char *, int, int, int, int, unsigned int);
    unsigned int (*windowId)(void *);
    void (*destroyWindow)(void *);
    void (*resizeWindow)(void *, int, int);
    int (*setAttribute)(int, int);
    void (*resetAttributes)(void);
    void *(*createContext)(void *);
    void (*deleteContext)(void *);
    int (*makeCurrent)(void *, void *);
    void *(*currentContext)(void);
    void *(*currentWindow)(void);
    void *(*procedure)(const char *);
    int (*swapInterval)(int);
    void (*swapWindow)(void *);
    void (*pumpEvents)(void);
    int (*peepEvents)(SdlWindowEvent *, int, int, unsigned int, unsigned int);
} SdlGl;

typedef struct Context {
    HGLRC handle;
    XboxWindow *window;
    void *sdlWindow;
    void *sdlContext;
    int width, height;
    BOOL presented;
} Context;

static SdlGl sdl;
static BOOL initialized;
static BOOL hostEvents;
static BOOL steamChecked;
static volatile LONG startupSwapAttempts;
static enum { HostPollBeforeSwap, HostPollPending, HostPollTraced } startupHostPoll;
static Context contexts[CONTEXT_COUNT];
static ULONG_PTR nextContext = 0x584243000001ULL;

static BOOL Equals(const char *left, const char *right) {
    if (!left || !right) return FALSE;
    while (*left && *left == *right) { ++left; ++right; }
    return *left == *right;
}

static BOOL PackagePath(LPCWSTR variable, LPCWSTR library) {
    WCHAR path[512];
    UINT length = ARRAYSIZE(path);
    if (GetCurrentPackagePath(&length, path) != ERROR_SUCCESS || length <= 1 || length >= ARRAYSIZE(path)) return FALSE;
    --length;
    path[length++] = L'\\';
    while (*library && length + 1 < ARRAYSIZE(path)) path[length++] = *library++;
    path[length] = 0;
    return !*library && SetEnvironmentVariableW(variable, path);
}

static void InitializeSteamworks(void) {
    if (steamChecked) return;
    steamChecked = TRUE;
    if (GetEnvironmentVariableW(L"XBOX_STEAM_HANDOFF_SDK", NULL, 0) <= 1) return;
    typedef DWORD (WINAPI *Initialize)(BOOL);
    HMODULE module = LoadPackagedLibrary(L"XboxWin32SteamDiscovery.dll", 0);
    Initialize initialize = module ? (Initialize)(void *)GetProcAddress(module, "XboxSteamInitialize") : NULL;
    if (!initialize) {
        XboxWindowTrace("win32_steam_handoff=unavailable\n");
    } else {
        const DWORD required = STEAM_HANDOFF_SDK_INITIALIZED | STEAM_HANDOFF_SDK_MATCHED;
        const DWORD result = initialize(TRUE);
        XboxWindowTrace((result & required) == required ? "win32_steam_handoff=ready\n" : "win32_steam_handoff=error\n");
    }
    if (module) FreeLibrary(module);
}

static BOOL Initialize(void) {
    if (initialized) return TRUE;
    InitializeSteamworks();
    HMODULE module = LoadLibraryExW(L"SDL2.dll", NULL, 0);
    if (!module) return FALSE;
#define SDL_BIND(member, exportName) \
    sdl.member = (void *)GetProcAddress(module, "SDL_" exportName); \
    if (!sdl.member) goto fail
    SDL_BIND(ready, "SetMainReady");
    SDL_BIND(init, "InitSubSystem");
    SDL_BIND(wasInit, "WasInit");
    SDL_BIND(driver, "GetCurrentVideoDriver");
    SDL_BIND(createWindow, "CreateWindow");
    SDL_BIND(windowId, "GetWindowID");
    SDL_BIND(destroyWindow, "DestroyWindow");
    SDL_BIND(resizeWindow, "SetWindowSize");
    SDL_BIND(setAttribute, "GL_SetAttribute");
    SDL_BIND(resetAttributes, "GL_ResetAttributes");
    SDL_BIND(createContext, "GL_CreateContext");
    SDL_BIND(deleteContext, "GL_DeleteContext");
    SDL_BIND(makeCurrent, "GL_MakeCurrent");
    SDL_BIND(currentContext, "GL_GetCurrentContext");
    SDL_BIND(currentWindow, "GL_GetCurrentWindow");
    SDL_BIND(procedure, "GL_GetProcAddress");
    SDL_BIND(swapInterval, "GL_SetSwapInterval");
    SDL_BIND(swapWindow, "GL_SwapWindow");
    SDL_BIND(pumpEvents, "PumpEvents");
    SDL_BIND(peepEvents, "PeepEvents");
#undef SDL_BIND
    if (!sdl.wasInit(SDL_INIT_VIDEO)) {
        if (!SetEnvironmentVariableW(L"SDL_VIDEODRIVER", L"offscreen") ||
                !SetEnvironmentVariableW(L"XBOX_SDL_SURFACELESS", L"1") ||
                !SetEnvironmentVariableW(L"SDL_VIDEO_WIN_D3DCOMPILER", L"none") ||
                !PackagePath(L"SDL_VIDEO_GL_DRIVER", L"opengl32.dll") ||
                !PackagePath(L"SDL_VIDEO_EGL_DRIVER", L"libEGL.dll")) goto fail;
        sdl.ready();
        if (sdl.init(SDL_INIT_VIDEO) != 0) goto fail;
    }
    if (!Equals(sdl.driver(), "offscreen")) goto fail;
    hostEvents = GetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", NULL, 0) > 1;
    initialized = TRUE;
    return TRUE;
fail:
    FreeLibrary(module);
    SetLastError(ERROR_OPEN_FAILED);
    return FALSE;
}

DECLSPEC_IMPORT HANDLE WINAPI CreateFile2FromAppW(LPCWSTR, DWORD, DWORD, DWORD,
    LPCREATEFILE2_EXTENDED_PARAMETERS);

/* Record the first delivery of each host input kind in the run's loader trace. */
static void TraceFirstInput(volatile LONG *seen, const char *line) {
    if (InterlockedExchange(seen, 1)) return;
    const DWORD error = GetLastError();
    WCHAR path[1024];
    const DWORD length = GetEnvironmentVariableW(L"XBOX_WIN32_LOADER_TRACE", path, ARRAYSIZE(path));
    if (length && length < ARRAYSIZE(path)) {
        const HANDLE file = CreateFile2FromAppW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD count = 0, written;
            while (line[count]) ++count;
            WriteFile(file, line, count, &written, NULL);
            CloseHandle(file);
        }
    }
    SetLastError(error);
}

static HWND EventWindow(UINT windowId, BOOL exact) {
    for (UINT i = 0; i < CONTEXT_COUNT; ++i) {
        const Context *context = &contexts[i];
        if (context->handle && sdl.windowId(context->sdlWindow) == windowId) return context->window->handle;
    }
    /* The host binds one SDL window. Input without that window goes to the first GL window. */
    for (UINT i = 0; !exact && i < CONTEXT_COUNT; ++i) {
        if (contexts[i].handle) return contexts[i].window->handle;
    }
    return NULL;
}

/* The hosted SDL backend turns the host mailbox into SDL events. Deliver them in
   queue order as the window close, keyboard, text and pointer messages of the GL window. */
static void DeliverHostEvent(const SdlWindowEvent *event) {
    static const UINT down[] = {WM_LBUTTONDOWN, WM_MBUTTONDOWN, WM_RBUTTONDOWN};
    static const UINT up[] = {WM_LBUTTONUP, WM_MBUTTONUP, WM_RBUTTONUP};
    static LONG pointerX, pointerY; /* SDL reports a wheel without a position */
    HWND window;
    switch (event->type) {
    case SDL_WINDOWEVENT:
        window = event->window.event == SDL_WINDOWEVENT_CLOSE ? EventWindow(event->window.windowId, TRUE) : NULL;
        if (window) XboxPostMessageW(window, WM_CLOSE, 0, 0);
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        window = EventWindow(event->key.windowId, FALSE);
        if (window && event->key.scancode > 0) {
            static volatile LONG keySeen;
            TraceFirstInput(&keySeen, "input wgl-key=delivered\r\n");
            XboxPostHostedKey(window, (UINT)event->key.scancode, event->type == SDL_KEYDOWN, event->key.repeat != 0);
        }
        break;
    case SDL_TEXTINPUT: {
        char text[sizeof(event->text.text) + 1];
        for (UINT i = 0; i < sizeof(event->text.text); ++i) text[i] = event->text.text[i];
        text[sizeof(event->text.text)] = 0;
        window = EventWindow(event->text.windowId, FALSE);
        if (window) XboxPostHostedText(window, text);
        break;
    }
    case SDL_MOUSEMOTION:
        window = EventWindow(event->motion.windowId, FALSE);
        pointerX = event->motion.x;
        pointerY = event->motion.y;
        if (window) XboxPostHostedPointer(window, WM_MOUSEMOVE, pointerX, pointerY, 0);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        window = EventWindow(event->button.windowId, FALSE);
        pointerX = event->button.x;
        pointerY = event->button.y;
        if (window && event->button.button >= 1 && event->button.button <= 3) {
            static volatile LONG buttonSeen;
            TraceFirstInput(&buttonSeen, "input wgl-pointer-button=delivered\r\n");
        }
        if (window && event->button.button >= 1 && event->button.button <= 3)
            XboxPostHostedPointer(window, (event->type == SDL_MOUSEBUTTONDOWN ? down : up)[event->button.button - 1],
                event->button.x, event->button.y, 0);
        break;
    case SDL_MOUSEWHEEL: {
        const int sign = event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
        window = EventWindow(event->wheel.windowId, FALSE);
        if (window && event->wheel.y) XboxPostHostedPointer(window, WM_MOUSEWHEEL, pointerX, pointerY, sign * event->wheel.y);
        if (window && event->wheel.x) XboxPostHostedPointer(window, WM_MOUSEHWHEEL, pointerX, pointerY, sign * event->wheel.x);
        break;
    }
    default:
        break;
    }
}

void XboxPollWglHostEvents(void) {
    if (!hostEvents) return;
    const BOOL trace = startupHostPoll == HostPollPending;
    if (trace) {
        startupHostPoll = HostPollTraced;
        XboxWindowTrace("win32_wgl_post_swap_events=begin\n");
    }
    sdl.pumpEvents();
    SdlWindowEvent events[32];
    int count;
    do {
        count = sdl.peepEvents(events, ARRAYSIZE(events), SDL_GETEVENT, SDL_WINDOWEVENT, SDL_MOUSEWHEEL);
        for (int index = 0; index < count; ++index) DeliverHostEvent(&events[index]);
    } while (count == ARRAYSIZE(events));
    if (trace) XboxWindowTrace("win32_wgl_post_swap_events=returned\n");
}

BOOL XboxWglHostEventsActive(void) {
    return hostEvents;
}

static Context *FindContext(HGLRC handle) {
    for (UINT i = 0; handle && i < CONTEXT_COUNT; ++i) {
        if (contexts[i].handle == handle) return &contexts[i];
    }
    return NULL;
}

static Context *CurrentContext(void) {
    void *current = initialized ? sdl.currentContext() : NULL;
    for (UINT i = 0; current && i < CONTEXT_COUNT; ++i) {
        if (contexts[i].sdlContext == current) return &contexts[i];
    }
    return NULL;
}

int WINAPI XboxChoosePixelFormat(HDC dc, const PIXELFORMATDESCRIPTOR *format) {
    if (!XboxFindWindow((HWND)dc)) return 0;
    if (!format || format->nSize != sizeof(*format) || format->nVersion != 1 ||
            format->iPixelType != PFD_TYPE_RGBA || (format->dwFlags & PFD_STEREO) ||
            format->iLayerType != PFD_MAIN_PLANE || format->cColorBits > 32 ||
            format->cDepthBits > 24 || format->cStencilBits > 8 || format->cAuxBuffers) {
        SetLastError(ERROR_INVALID_PIXEL_FORMAT);
        return 0;
    }
    return 1;
}

BOOL WINAPI XboxSetPixelFormat(HDC dc, int selection, const PIXELFORMATDESCRIPTOR *format) {
    XboxWindow *window = XboxFindWindow((HWND)dc);
    if (!window) return FALSE;
    if (selection != 1) {
        SetLastError(ERROR_INVALID_PIXEL_FORMAT);
        return FALSE;
    }
    if (!XboxChoosePixelFormat(dc, format)) return FALSE;
    window->pixelFormat = selection;
    return TRUE;
}

static HGLRC CreateContext(HDC dc, int major, int minor, int profile, int flags) {
    XboxWindow *window = XboxFindWindow((HWND)dc);
    if (!window) return NULL;
    const int width = window->bounds.right - window->bounds.left;
    const int height = window->bounds.bottom - window->bounds.top;
    if (!window->pixelFormat || width < 1 || height < 1 || width > 4096 || height > 4096) {
        SetLastError(ERROR_INVALID_PIXEL_FORMAT);
        return NULL;
    }
    Context *slot = NULL;
    for (UINT i = 0; i < CONTEXT_COUNT; ++i) {
        if (!contexts[i].handle) { slot = &contexts[i]; break; }
    }
    if (!slot) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (!Initialize()) return NULL;
    void *oldWindow = sdl.currentWindow();
    void *oldContext = sdl.currentContext();
    sdl.resetAttributes();
    /* SDL_GL_DEPTH_SIZE/STENCIL_SIZE/DOUBLEBUFFER and context version/profile. */
    if (sdl.setAttribute(6, 24) || sdl.setAttribute(7, 8) || sdl.setAttribute(5, 1) ||
            sdl.setAttribute(17, major) || sdl.setAttribute(18, minor) ||
            sdl.setAttribute(20, flags) || sdl.setAttribute(21, profile)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    void *sdlWindow = sdl.createWindow("Win32 OpenGL", 0, 0, width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    void *sdlContext = sdlWindow ? sdl.createContext(sdlWindow) : NULL;
    if (!sdlContext || sdl.makeCurrent(oldWindow, oldContext)) {
        if (sdlContext) sdl.deleteContext(sdlContext);
        if (sdlWindow) sdl.destroyWindow(sdlWindow);
        SetLastError(ERROR_OPEN_FAILED);
        return NULL;
    }
    *slot = (Context){(HGLRC)nextContext++, window, sdlWindow, sdlContext, width, height, FALSE};
    ++window->contexts;
    XboxWindowTrace("win32_wgl_context=created\n");
    return slot->handle;
}

HGLRC WINAPI XboxWglCreateContext(HDC dc) {
    XboxWatchHostExit();
    return CreateContext(dc, 2, 1, 0, 0);
}

static HGLRC WINAPI CreateContextAttribs(HDC dc, HGLRC shared, const int *attributes) {
    int major = 1, minor = 0, profile = 0, flags = 0;
    if (shared) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return NULL;
    }
    if (attributes) {
        for (UINT i = 0; attributes[i]; i += 2) {
            if (i >= 32) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
            const int value = attributes[i + 1];
            switch (attributes[i]) {
            case 0x2091: major = value; break; /* WGL_CONTEXT_MAJOR_VERSION_ARB */
            case 0x2092: minor = value; break;
            case 0x2094: flags = value; break;
            case 0x9126: profile = value; break;
            default: SetLastError(ERROR_INVALID_PARAMETER); return NULL;
            }
        }
    }
    if (major < 1 || major > 4 || minor < 0 || minor > 6 || (flags & ~3) ||
            (profile != 0 && profile != 1 && profile != 2)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    return CreateContext(dc, major, minor, profile, flags);
}

void XboxResizeCurrentContext(XboxWindow *window) {
    Context *context = CurrentContext();
    if (!context || context->window != window) return;
    const int width = window->bounds.right - window->bounds.left;
    const int height = window->bounds.bottom - window->bounds.top;
    if (width > 0 && height > 0 && width <= 4096 && height <= 4096 &&
            (width != context->width || height != context->height)) {
        sdl.resizeWindow(context->sdlWindow, width, height);
        context->width = width;
        context->height = height;
    }
}

BOOL WINAPI XboxWglMakeCurrent(HDC dc, HGLRC handle) {
    if (!XboxWindowThread()) return FALSE;
    if (!handle && (!initialized || !sdl.makeCurrent(NULL, NULL))) return TRUE;
    Context *context = FindContext(handle);
    if (!context || context->window->handle != (HWND)dc) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (sdl.makeCurrent(context->sdlWindow, context->sdlContext)) {
        SetLastError(ERROR_OPEN_FAILED);
        return FALSE;
    }
    XboxResizeCurrentContext(context->window);
    return TRUE;
}

BOOL WINAPI XboxWglDeleteContext(HGLRC handle) {
    if (!XboxWindowThread()) return FALSE;
    Context *context = FindContext(handle);
    if (!context) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (context == CurrentContext() && sdl.makeCurrent(NULL, NULL)) {
        SetLastError(ERROR_OPEN_FAILED);
        return FALSE;
    }
    sdl.deleteContext(context->sdlContext);
    sdl.destroyWindow(context->sdlWindow);
    --context->window->contexts;
    context->sdlContext = NULL;
    context->handle = NULL;
    return TRUE;
}

HGLRC WINAPI XboxWglGetCurrentContext(void) {
    const Context *context = CurrentContext();
    return context ? context->handle : NULL;
}

HDC WINAPI XboxWglGetCurrentDC(void) {
    const Context *context = CurrentContext();
    return context ? (HDC)context->window->handle : NULL;
}

BOOL WINAPI XboxSwapBuffers(HDC dc) {
    LONG attempt = 0;
    if (startupSwapAttempts < 2) attempt = InterlockedIncrement(&startupSwapAttempts);
    if (attempt == 1) XboxWindowTrace("win32_wgl_swap_entry=first\n");
    else if (attempt == 2) XboxWindowTrace("win32_wgl_swap_entry=second\n");
    const BOOL trace = attempt == 1 || attempt == 2;
    if (!XboxWindowThread()) {
        if (trace) XboxWindowTrace("win32_wgl_swap_rejected=thread\n");
        return FALSE;
    }
    Context *context = CurrentContext();
    if (!context || context->window->handle != (HWND)dc) {
        if (trace) XboxWindowTrace("win32_wgl_swap_rejected=context-or-dc\n");
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (trace) XboxWindowTrace("win32_wgl_sdl_swap=begin\n");
    sdl.swapWindow(context->sdlWindow);
    if (trace) XboxWindowTrace("win32_wgl_sdl_swap=returned\n");
    if (hostEvents && startupHostPoll == HostPollBeforeSwap) startupHostPoll = HostPollPending;
    if (!context->presented) {
        context->presented = TRUE;
        XboxWindowTrace("win32_wgl_swap=submitted\n");
    }
    return TRUE;
}

static BOOL WINAPI SwapInterval(int interval) {
    return XboxWindowThread() && CurrentContext() && sdl.swapInterval(interval) == 0;
}

static const char wglExtensions[] = "WGL_ARB_extensions_string WGL_EXT_extensions_string "
    "WGL_ARB_create_context WGL_ARB_create_context_profile WGL_EXT_swap_control";

static LPCSTR WINAPI ExtensionStringARB(HDC dc) {
    if (!XboxFindWindow((HWND)dc)) return NULL;
    return wglExtensions;
}

static LPCSTR WINAPI ExtensionStringEXT(void) {
    if (!XboxWindowThread()) return NULL;
    if (!CurrentContext()) {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return wglExtensions;
}

PROC WINAPI XboxWglGetProcAddress(LPCSTR name) {
    if (!XboxWindowThread() || !CurrentContext() || (ULONG_PTR)name <= 0xffffu) return NULL;
    if (Equals(name, "wglCreateContextAttribsARB")) return (PROC)(void *)CreateContextAttribs;
    if (Equals(name, "wglGetExtensionsStringARB")) return (PROC)(void *)ExtensionStringARB;
    if (Equals(name, "wglGetExtensionsStringEXT")) return (PROC)(void *)ExtensionStringEXT;
    if (Equals(name, "wglSwapIntervalEXT")) return (PROC)(void *)SwapInterval;
    if (name[0] == 'w' && name[1] == 'g' && name[2] == 'l') return NULL;
    return (PROC)sdl.procedure(name);
}

FARPROC WINAPI XboxWindowGetProcAddress(HMODULE module, LPCSTR name) {
    const FARPROC adapted = ScopedShellExport(module, name);
    if (adapted) return adapted;
    if ((ULONG_PTR)name > 0xffffu && module && module == GetModuleHandleW(L"opengl32.dll")) {
        if (Equals(name, "wglCreateContext")) return (FARPROC)(void *)XboxWglCreateContext;
        if (Equals(name, "wglMakeCurrent")) return (FARPROC)(void *)XboxWglMakeCurrent;
        if (Equals(name, "wglDeleteContext")) return (FARPROC)(void *)XboxWglDeleteContext;
        if (Equals(name, "wglGetProcAddress")) return (FARPROC)(void *)XboxWglGetProcAddress;
        if (Equals(name, "wglGetCurrentContext")) return (FARPROC)(void *)XboxWglGetCurrentContext;
        if (Equals(name, "wglGetCurrentDC")) return (FARPROC)(void *)XboxWglGetCurrentDC;
    }
    return GetProcAddress(module, name);
}
