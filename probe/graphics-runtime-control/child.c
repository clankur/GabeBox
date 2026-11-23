/* Verify the packaged desktop OpenGL ABI from a separate x64 process. */
#include "../child-process.h"

#define IMPORT __declspec(dllimport)
#define WINAPI __stdcall
#define GL_COLOR_BUFFER_BIT 0x4000u
#define GL_VERSION 0x1f02u
#define GL_RGBA 0x1908u
#define GL_UNSIGNED_BYTE 0x1401u
#define GL_RGBA8 0x8058u
#define GL_FRAMEBUFFER 0x8d40u
#define GL_RENDERBUFFER 0x8d41u
#define GL_COLOR_ATTACHMENT0 0x8ce0u
#define GL_FRAMEBUFFER_COMPLETE 0x8cd5u
#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_BIT 0x0008
#define EGL_OPENGL_API 0x30a2

typedef unsigned short WCHAR;
typedef unsigned long DWORD;
typedef void *Module;
typedef void *EglHandle;

/* MSVC ABI marker for float arguments in this CRT-free child. */
int _fltused;

IMPORT Module WINAPI GetModuleHandleW(const WCHAR *name);
IMPORT DWORD WINAPI GetModuleFileNameW(Module module, WCHAR *path, DWORD capacity);
IMPORT __declspec(noreturn) void WINAPI ExitProcess(unsigned int code);

#ifdef PROBE_LOAD_MESA
IMPORT Module WINAPI LoadPackagedLibrary(const WCHAR *name, DWORD reserved);
IMPORT DWORD WINAPI GetLastError(void);

void WINAPI ChildEntry(void) {
    static const WCHAR *const required[] = { L"z-1.dll", L"libgallium_wgl.dll", L"libEGL.dll", L"opengl32.dll" };
    for (unsigned int i = 0; i < sizeof(required) / sizeof(*required); ++i) {
        if (!LoadPackagedLibrary(required[i], 0)) ExitProcess(((0x6201u + i) << 16) | (GetLastError() & 0xffffu));
    }
    ExitProcess(PROBE_EXPECTED_CHILD_EXIT);
}
#else
IMPORT EglHandle WINAPI eglGetDisplay(void *nativeDisplay);
IMPORT unsigned int WINAPI eglInitialize(EglHandle display, int *major, int *minor);
IMPORT unsigned int WINAPI eglBindAPI(unsigned int api);
IMPORT unsigned int WINAPI eglChooseConfig(EglHandle display, const int *attributes, EglHandle *configs,
                                            int capacity, int *count);
IMPORT EglHandle WINAPI eglCreateContext(EglHandle display, EglHandle config, EglHandle share, const int *attributes);
IMPORT unsigned int WINAPI eglMakeCurrent(EglHandle display, EglHandle draw, EglHandle read, EglHandle context);
IMPORT unsigned int WINAPI eglTerminate(EglHandle display);
typedef void (WINAPI *GlProc)(void);
IMPORT GlProc WINAPI eglGetProcAddress(const char *name);
IMPORT const unsigned char *WINAPI glGetString(unsigned int name);
IMPORT void WINAPI glClearColor(float red, float green, float blue, float alpha);
IMPORT void WINAPI glClear(unsigned int mask);
IMPORT unsigned int WINAPI glGetError(void);
IMPORT void WINAPI glReadPixels(int x, int y, int width, int height, unsigned int format, unsigned int type, void *pixels);

static int IsPackagedOpenGL(void) {
    WCHAR image[512], library[512];
    Module gl = GetModuleHandleW(L"opengl32.dll");
    DWORD imageLength = GetModuleFileNameW(0, image, 512);
    DWORD libraryLength = gl ? GetModuleFileNameW(gl, library, 512) : 0;
    if (!imageLength || imageLength >= 512 || !libraryLength || libraryLength >= 512) return 0;
    while (imageLength && image[imageLength - 1] != L'\\') --imageLength;
    while (libraryLength && library[libraryLength - 1] != L'\\') --libraryLength;
    if (!imageLength || imageLength != libraryLength) return 0;
    for (DWORD offset = 0; offset < imageLength; ++offset) {
        if (image[offset] != library[offset]) return 0;
    }
    return 1;
}

static int CreateFramebuffer(void) {
    typedef void (WINAPI *GenObjects)(int, unsigned int *);
    typedef void (WINAPI *BindObject)(unsigned int, unsigned int);
    typedef void (WINAPI *RenderbufferStorage)(unsigned int, unsigned int, int, int);
    typedef void (WINAPI *AttachRenderbuffer)(unsigned int, unsigned int, unsigned int, unsigned int);
    typedef unsigned int (WINAPI *CheckFramebuffer)(unsigned int);
    GenObjects genFrames = (GenObjects)eglGetProcAddress("glGenFramebuffers");
    GenObjects genBuffers = (GenObjects)eglGetProcAddress("glGenRenderbuffers");
    BindObject bindFrame = (BindObject)eglGetProcAddress("glBindFramebuffer");
    BindObject bindBuffer = (BindObject)eglGetProcAddress("glBindRenderbuffer");
    RenderbufferStorage allocate = (RenderbufferStorage)eglGetProcAddress("glRenderbufferStorage");
    AttachRenderbuffer attach = (AttachRenderbuffer)eglGetProcAddress("glFramebufferRenderbuffer");
    CheckFramebuffer check = (CheckFramebuffer)eglGetProcAddress("glCheckFramebufferStatus");
    if (!genFrames || !genBuffers || !bindFrame || !bindBuffer || !allocate || !attach || !check) return 0;
    unsigned int frame = 0, color = 0;
    genFrames(1, &frame);
    genBuffers(1, &color);
    if (!frame || !color) return 0;
    bindFrame(GL_FRAMEBUFFER, frame);
    bindBuffer(GL_RENDERBUFFER, color);
    allocate(GL_RENDERBUFFER, GL_RGBA8, 4, 4);
    attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    return check(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static unsigned int TestDesktopGl(EglHandle display) {
    const int configFlags[] = { EGL_SURFACE_TYPE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
    EglHandle config;
    int count;
    if (!eglBindAPI(EGL_OPENGL_API)) return 0x6103;
    if (!eglChooseConfig(display, configFlags, &config, 1, &count) || !count) return 0x6104;
    EglHandle context = eglCreateContext(display, config, 0, 0);
    if (!context) return 0x6105;
    if (!eglMakeCurrent(display, 0, 0, context)) return 0x6107;
    if (!glGetString(GL_VERSION)) return 0x6108;
    if (!CreateFramebuffer()) return 0x6106;
    unsigned char pixel[4] = {0};
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (glGetError() || pixel[0] != 255 || pixel[1] != 0 || pixel[2] != 255) return 0x6109;
    return PROBE_EXPECTED_CHILD_EXIT;
}

/* Keep a base relocation in this freestanding Xbox child, as in the process control. */
static unsigned int (*volatile runTest)(EglHandle) = TestDesktopGl;

void WINAPI ChildEntry(void) {
    if (!IsPackagedOpenGL()) ExitProcess(0x6101);
    EglHandle display = eglGetDisplay(0);
    if (!display || !eglInitialize(display, 0, 0)) ExitProcess(0x6102);
    unsigned int result = runTest(display);
    eglMakeCurrent(display, 0, 0, 0);
    eglTerminate(display);
    ExitProcess(result);
}
#endif
