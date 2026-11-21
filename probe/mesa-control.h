#pragma once

#include "mesa-indexed-control.h"

struct MesaControlResult {
    unsigned stage = 0;
    DWORD loadError = ERROR_SUCCESS;
    int eglError = 0;
    unsigned glError = 0;
    int pbufferError = 0;
    unsigned framebufferStatus = 0;
    bool surfaceless = false;
    int eglMajor = 0;
    int eglMinor = 0;
    unsigned char pixel[4]{};
};

template <typename ContextCheck>
static MesaControlResult RunMesaControl(void (*checkpoint)(unsigned), ContextCheck checkCurrentContext,
                                       bool indexed = false) {
    using Handle = void *;
    using Boolean = unsigned;
    using GetDisplay = Handle (WINAPI *)(Handle);
    using Initialize = Boolean (WINAPI *)(Handle, int *, int *);
    using BindApi = Boolean (WINAPI *)(unsigned);
    using ChooseConfig = Boolean (WINAPI *)(Handle, const int *, Handle *, int, int *);
    using CreatePbuffer = Handle (WINAPI *)(Handle, Handle, const int *);
    using CreateContext = Handle (WINAPI *)(Handle, Handle, Handle, const int *);
    using MakeCurrent = Boolean (WINAPI *)(Handle, Handle, Handle, Handle);
    using Destroy = Boolean (WINAPI *)(Handle, Handle);
    using Terminate = Boolean (WINAPI *)(Handle);
    using GetError = int (WINAPI *)();
    using QueryString = const char * (WINAPI *)(Handle, int);
    using EglProc = void (WINAPI *)();
    using GetProc = EglProc (WINAPI *)(const char *);
    using DeleteBuffers = void (WINAPI *)(int, const unsigned *);

    MesaControlResult result;
    const auto stage = [&](unsigned next) {
        result.stage = next;
        if (checkpoint) checkpoint(next);
    };
    stage(0);
    auto egl = LoadPackagedLibrary(L"libEGL.dll", 0);
    if (!egl) {
        result.loadError = GetLastError();
        return result;
    }
    auto gl = LoadPackagedLibrary(L"opengl32.dll", 0);
    if (!gl) {
        result.loadError = GetLastError();
        FreeLibrary(egl);
        return result;
    }

    const auto getDisplay = reinterpret_cast<GetDisplay>(GetProcAddress(egl, "eglGetDisplay"));
    const auto initialize = reinterpret_cast<Initialize>(GetProcAddress(egl, "eglInitialize"));
    const auto bind = reinterpret_cast<BindApi>(GetProcAddress(egl, "eglBindAPI"));
    const auto choose = reinterpret_cast<ChooseConfig>(GetProcAddress(egl, "eglChooseConfig"));
    const auto createPbuffer = reinterpret_cast<CreatePbuffer>(GetProcAddress(egl, "eglCreatePbufferSurface"));
    const auto createContext = reinterpret_cast<CreateContext>(GetProcAddress(egl, "eglCreateContext"));
    const auto makeCurrent = reinterpret_cast<MakeCurrent>(GetProcAddress(egl, "eglMakeCurrent"));
    const auto destroySurface = reinterpret_cast<Destroy>(GetProcAddress(egl, "eglDestroySurface"));
    const auto destroyContext = reinterpret_cast<Destroy>(GetProcAddress(egl, "eglDestroyContext"));
    const auto terminate = reinterpret_cast<Terminate>(GetProcAddress(egl, "eglTerminate"));
    const auto error = reinterpret_cast<GetError>(GetProcAddress(egl, "eglGetError"));
    const auto query = reinterpret_cast<QueryString>(GetProcAddress(egl, "eglQueryString"));
    const auto proc = reinterpret_cast<GetProc>(GetProcAddress(egl, "eglGetProcAddress"));
    const auto clearColor = reinterpret_cast<void (WINAPI *)(float, float, float, float)>(GetProcAddress(gl, "glClearColor"));
    const auto clear = reinterpret_cast<void (WINAPI *)(unsigned)>(GetProcAddress(gl, "glClear"));
    const auto read = reinterpret_cast<void (WINAPI *)(int, int, int, int, unsigned, unsigned, void *)>(GetProcAddress(gl, "glReadPixels"));
    const auto glError = reinterpret_cast<unsigned (WINAPI *)()>(GetProcAddress(gl, "glGetError"));

    if (!getDisplay || !initialize || !bind || !choose || !createPbuffer || !createContext || !makeCurrent ||
            !destroySurface || !destroyContext || !terminate || !error || !query || !proc ||
            !clearColor || !clear || !read || !glError) {
        result.loadError = ERROR_PROC_NOT_FOUND;
        FreeLibrary(gl);
        FreeLibrary(egl);
        return result;
    }

    // Khronos EGL pbuffer with RGBA8 and the desktop OpenGL API.
    constexpr int none = 0x3038;
    constexpr unsigned desktopOpenGl = 0x30A2;
    const int configAttributes[] = {0x3033, 1, 0x3040, 8, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, none};
    const int surfaceAttributes[] = {0x3057, 4, 0x3056, 4, none};
    const int contextAttributes[] = {none};
    Handle display = nullptr, surface = nullptr, context = nullptr, config = nullptr;
    bool initialized = false, current = false;
    unsigned framebuffer = 0, colorBuffer = 0;
    DeleteBuffers deleteFramebuffers = nullptr, deleteRenderbuffers = nullptr;
    do {
        stage(1);
        display = getDisplay(nullptr);
        if (!display) break;
        stage(2);
        initialized = initialize(display, &result.eglMajor, &result.eglMinor) != 0;
        if (!initialized) break;
        stage(3);
        int count = 0;
        if (!bind(desktopOpenGl) || !choose(display, configAttributes, &config, 1, &count) || count != 1) break;
        stage(4);
        surface = createPbuffer(display, config, surfaceAttributes);
        if (!surface) {
            result.pbufferError = error();
            constexpr int extensions = 0x3055;
            const auto supported = query(display, extensions);
            result.surfaceless = supported && std::strstr(supported, "EGL_KHR_surfaceless_context");
            if (!result.surfaceless) break;
        }
        stage(5);
        context = createContext(display, config, nullptr, contextAttributes);
        if (!context) break;
        stage(6);
        current = makeCurrent(display, surface, surface, context) != 0;
        if (!current) break;
        stage(7);
        if (result.surfaceless) {
            using Generate = void (WINAPI *)(int, unsigned *);
            using Bind = void (WINAPI *)(unsigned, unsigned);
            using Storage = void (WINAPI *)(unsigned, unsigned, int, int);
            using Attach = void (WINAPI *)(unsigned, unsigned, unsigned, unsigned);
            using Check = unsigned (WINAPI *)(unsigned);
            const auto generateFramebuffers = reinterpret_cast<Generate>(proc("glGenFramebuffers"));
            const auto bindFramebuffer = reinterpret_cast<Bind>(proc("glBindFramebuffer"));
            const auto generateRenderbuffers = reinterpret_cast<Generate>(proc("glGenRenderbuffers"));
            const auto bindRenderbuffer = reinterpret_cast<Bind>(proc("glBindRenderbuffer"));
            const auto storage = reinterpret_cast<Storage>(proc("glRenderbufferStorage"));
            const auto attach = reinterpret_cast<Attach>(proc("glFramebufferRenderbuffer"));
            const auto check = reinterpret_cast<Check>(proc("glCheckFramebufferStatus"));
            deleteFramebuffers = reinterpret_cast<DeleteBuffers>(proc("glDeleteFramebuffers"));
            deleteRenderbuffers = reinterpret_cast<DeleteBuffers>(proc("glDeleteRenderbuffers"));
            if (!generateFramebuffers || !bindFramebuffer || !generateRenderbuffers || !bindRenderbuffer ||
                    !storage || !attach || !check || !deleteFramebuffers || !deleteRenderbuffers) break;
            constexpr unsigned framebufferTarget = 0x8D40, renderbufferTarget = 0x8D41;
            constexpr unsigned rgba8 = 0x8058, colorAttachment = 0x8CE0, complete = 0x8CD5;
            generateFramebuffers(1, &framebuffer);
            generateRenderbuffers(1, &colorBuffer);
            bindFramebuffer(framebufferTarget, framebuffer);
            bindRenderbuffer(renderbufferTarget, colorBuffer);
            storage(renderbufferTarget, rgba8, 4, 4);
            attach(framebufferTarget, colorAttachment, renderbufferTarget, colorBuffer);
            result.framebufferStatus = check(framebufferTarget);
            if (result.framebufferStatus != complete) break;
        }
        clearColor(0, 1, 0, 1);
        clear(0x4000); // GL_COLOR_BUFFER_BIT
        read(1, 1, 1, 1, 0x1908, 0x1401, result.pixel); // GL_RGBA, GL_UNSIGNED_BYTE
        result.glError = glError();
        if (result.glError == 0 && result.pixel[0] == 0 && result.pixel[1] == 255 &&
                result.pixel[2] == 0 && result.pixel[3] == 255) {
            stage(8);
            checkCurrentContext();
            if (indexed) {
                RunMesaIndexedControl([&](const char *name) -> EglProc {
                    const auto entry = GetProcAddress(gl, name);
                    return entry ? reinterpret_cast<EglProc>(entry) : proc(name);
                }, stage, result.pixel);
                result.glError = glError();
            }
        }
    } while (false);

    result.eglError = error();
    if (framebuffer) deleteFramebuffers(1, &framebuffer);
    if (colorBuffer) deleteRenderbuffers(1, &colorBuffer);
    if (current) makeCurrent(display, nullptr, nullptr, nullptr);
    if (context) destroyContext(display, context);
    if (surface) destroySurface(display, surface);
    if (initialized) terminate(display);
    FreeLibrary(gl);
    FreeLibrary(egl);
    return result;
}
