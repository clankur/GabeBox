/* Check that a packaged SDL2 build loads and can initialize its timer. */
#include "../../runtime/shared/child-process.h"
#include "sdl-version.h"

#define IMPORT __declspec(dllimport)
#define WINAPI __stdcall
#define SDL_INIT_TIMER 1u

typedef unsigned long DWORD;
typedef void *Module;
typedef void (WINAPI *Procedure)(void);

IMPORT Module WINAPI LoadPackagedLibrary(const unsigned short *name, DWORD reserved);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT Procedure WINAPI GetProcAddress(Module module, const char *name);
IMPORT __declspec(noreturn) void WINAPI ExitProcess(unsigned int code);

struct SDLVersion {
    unsigned char major, minor, patch;
};

#ifdef PROBE_SDL_VIDEO
#define SDL_INIT_VIDEO 0x20u
#define SDL_WINDOW_OPENGL 0x2u
#define SDL_WINDOW_HIDDEN 0x8u
#define GL_COLOR_BUFFER_BIT 0x4000u
#define GL_RGBA 0x1908u
#define GL_UNSIGNED_BYTE 0x1401u

typedef unsigned short WCHAR;
typedef void *SdlWindow;
typedef void *SdlContext;

IMPORT DWORD WINAPI GetModuleFileNameW(Module module, WCHAR *path, DWORD capacity);
IMPORT int WINAPI SetEnvironmentVariableW(const WCHAR *name, const WCHAR *value);
IMPORT void WINAPI glClearColor(float red, float green, float blue, float alpha);
IMPORT void WINAPI glClear(unsigned int mask);
IMPORT unsigned int WINAPI glGetError(void);
IMPORT void WINAPI glReadPixels(int x, int y, int width, int height, unsigned int format, unsigned int type, void *pixels);
IMPORT Procedure WINAPI wglGetProcAddress(const char *name);

/* Required by the MSVC ABI for float arguments in a child without a C runtime. */
int _fltused;

static int SetPackagedPath(const WCHAR *name, const WCHAR *file) {
    WCHAR path[512];
    DWORD length = GetModuleFileNameW(0, path, sizeof(path) / sizeof(*path));
    if (!length || length >= sizeof(path) / sizeof(*path)) return 0;
    while (length && path[length - 1] != L'\\') --length;
    if (!length) return 0;
    while (*file && length + 1 < sizeof(path) / sizeof(*path)) path[length++] = *file++;
    path[length] = L'\0';
    return !*file && SetEnvironmentVariableW(name, path);
}

static unsigned int TestVideo(Module sdl) {
    SdlWindow (*create)(const char *, int, int, int, int, unsigned int) =
        (SdlWindow (*)(const char *, int, int, int, int, unsigned int))GetProcAddress(sdl, "SDL_CreateWindow");
    void (*destroy)(SdlWindow) = (void (*)(SdlWindow))GetProcAddress(sdl, "SDL_DestroyWindow");
    SdlContext (*makeContext)(SdlWindow) = (SdlContext (*)(SdlWindow))GetProcAddress(sdl, "SDL_GL_CreateContext");
    void (*deleteContext)(SdlContext) = (void (*)(SdlContext))GetProcAddress(sdl, "SDL_GL_DeleteContext");
    void (*swap)(SdlWindow) = (void (*)(SdlWindow))GetProcAddress(sdl, "SDL_GL_SwapWindow");
    Procedure (*getGl)(const char *) = (Procedure (*)(const char *))GetProcAddress(sdl, "SDL_GL_GetProcAddress");
    if (!create || !destroy || !makeContext || !deleteContext || !swap || !getGl) return 0x6503;

    SdlWindow window = create("Xbox SDL video control", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) return 0x6504;
    SdlContext context = makeContext(window);
    if (!context) {
        destroy(window);
        return 0x6505;
    }
    unsigned char pixel[4] = {0};
    glClearColor(0.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    unsigned int result = PROBE_EXPECTED_CHILD_EXIT;
    if (glGetError() || pixel[0] != 0 || pixel[1] != 255 || pixel[2] != 255) {
        result = 0x6506;
    } else if (!getGl("glBindFramebuffer")) {
        result = 0x6507;
    } else {
        swap(window);
        /* A native engine can use WGL even when SDL owns the EGL context. */
        if (!wglGetProcAddress("glBindFramebuffer")) result = 0x6508;
    }
    deleteContext(context);
    destroy(window);
    return result;
}
#endif

static unsigned int TestSDL(void) {
#ifdef PROBE_SDL_VIDEO
    if (!SetEnvironmentVariableW(L"SDL_VIDEODRIVER", L"offscreen") ||
            !SetPackagedPath(L"SDL_VIDEO_GL_DRIVER", L"opengl32.dll") ||
            !SetPackagedPath(L"SDL_VIDEO_EGL_DRIVER", L"libEGL.dll") ||
            !SetEnvironmentVariableW(L"SDL_VIDEO_WIN_D3DCOMPILER", L"none") ||
            !SetEnvironmentVariableW(L"XBOX_SDL_SURFACELESS", L"1")) return 0x6501;
#endif
    Module sdl = LoadPackagedLibrary(L"SDL2.dll", 0);
    if (!sdl) return 0x63010000u | (GetLastError() & 0xffffu);

    void (*getVersion)(struct SDLVersion *) = (void (*)(struct SDLVersion *))GetProcAddress(sdl, "SDL_GetVersion");
    Procedure ready = GetProcAddress(sdl, "SDL_SetMainReady");
    int (*init)(unsigned int) = (int (*)(unsigned int))GetProcAddress(sdl, "SDL_Init");
    Procedure quit = GetProcAddress(sdl, "SDL_Quit");
    if (!getVersion || !ready || !init || !quit) return 0x6302;

    struct SDLVersion version = {0};
    getVersion(&version);
    if (version.major != 2 || version.minor != SDL_EXPECTED_MINOR || version.patch != SDL_EXPECTED_PATCH)
        return 0x6303;
    ready();
#ifdef PROBE_SDL_VIDEO
    if (init(SDL_INIT_VIDEO)) return 0x6502;
    unsigned int result = TestVideo(sdl);
#else
    if (init(SDL_INIT_TIMER)) return 0x6304;
    unsigned int result = PROBE_EXPECTED_CHILD_EXIT;
#endif
    quit();
    return result;
}

/* Retain a base relocation through the same entry route as the graphics control. */
static unsigned int (*volatile runTest)(void) = TestSDL;

void WINAPI ChildEntry(void) {
    ExitProcess(runTest());
}
