#include "window.h"
#include "../steam-handoff-control.h"
#include <appmodel.h>
#include "shell-lookup.h"
#include "xinput-lookup.h"

#define CONTEXT_COUNT 16
#define SDL_INIT_VIDEO 0x20u
#define SDL_WINDOW_OPENGL 2u
#define SDL_WINDOW_HIDDEN 8u
#define SDL_WINDOWEVENT 0x200u
#define SDL_WINDOWEVENT_CLOSE 14u
#define SDL_GETEVENT 2

typedef union SdlWindowEvent {
    struct { UINT type, timestamp, windowId; BYTE event; } window;
    ULONG64 padding[7];  /* SDL 2.x x64 event ABI */
} SdlWindowEvent;

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

void XboxPollWglHostEvents(void) {
    if (!hostEvents) return;
    const BOOL trace = startupHostPoll == HostPollPending;
    if (trace) {
        startupHostPoll = HostPollTraced;
        XboxWindowTrace("win32_wgl_post_swap_events=begin\n");
    }
    sdl.pumpEvents();
    SdlWindowEvent events[32];
    const int count = sdl.peepEvents(events, ARRAYSIZE(events), SDL_GETEVENT, SDL_WINDOWEVENT, SDL_WINDOWEVENT);
    for (int index = 0; index < count; ++index) {
        if (events[index].window.event != SDL_WINDOWEVENT_CLOSE) continue;
        for (UINT i = 0; i < CONTEXT_COUNT; ++i) {
            const Context *context = &contexts[i];
            if (context->handle && sdl.windowId(context->sdlWindow) == events[index].window.windowId) {
                XboxPostMessageW(context->window->handle, WM_CLOSE, 0, 0);
                break;
            }
        }
    }
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
    const FARPROC xinput = HostedXInputExport(module, name);
    if (xinput) return xinput;
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
