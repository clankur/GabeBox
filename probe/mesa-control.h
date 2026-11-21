#pragma once

struct MesaControlResult {
    unsigned stage = 0;
    DWORD loadError = ERROR_SUCCESS;
    int eglError = 0;
    unsigned glError = 0;
    int eglMajor = 0;
    int eglMinor = 0;
    unsigned char pixel[4]{};
};

static MesaControlResult RunMesaControl() {
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

    MesaControlResult result;
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
    const auto clearColor = reinterpret_cast<void (WINAPI *)(float, float, float, float)>(GetProcAddress(gl, "glClearColor"));
    const auto clear = reinterpret_cast<void (WINAPI *)(unsigned)>(GetProcAddress(gl, "glClear"));
    const auto read = reinterpret_cast<void (WINAPI *)(int, int, int, int, unsigned, unsigned, void *)>(GetProcAddress(gl, "glReadPixels"));
    const auto glError = reinterpret_cast<unsigned (WINAPI *)()>(GetProcAddress(gl, "glGetError"));

    if (!getDisplay || !initialize || !bind || !choose || !createPbuffer || !createContext || !makeCurrent ||
            !destroySurface || !destroyContext || !terminate || !error || !clearColor || !clear || !read || !glError) {
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
    do {
        result.stage = 1;
        display = getDisplay(nullptr);
        if (!display) break;
        result.stage = 2;
        initialized = initialize(display, &result.eglMajor, &result.eglMinor) != 0;
        if (!initialized) break;
        result.stage = 3;
        int count = 0;
        if (!bind(desktopOpenGl) || !choose(display, configAttributes, &config, 1, &count) || count != 1) break;
        result.stage = 4;
        surface = createPbuffer(display, config, surfaceAttributes);
        if (!surface) break;
        result.stage = 5;
        context = createContext(display, config, nullptr, contextAttributes);
        if (!context) break;
        result.stage = 6;
        current = makeCurrent(display, surface, surface, context) != 0;
        if (!current) break;
        result.stage = 7;
        clearColor(0, 1, 0, 1);
        clear(0x4000); // GL_COLOR_BUFFER_BIT
        read(1, 1, 1, 1, 0x1908, 0x1401, result.pixel); // GL_RGBA, GL_UNSIGNED_BYTE
        result.glError = glError();
        if (result.glError == 0 && result.pixel[0] == 0 && result.pixel[1] == 255 &&
                result.pixel[2] == 0 && result.pixel[3] == 255) result.stage = 8;
    } while (false);

    result.eglError = error();
    if (current) makeCurrent(display, nullptr, nullptr, nullptr);
    if (context) destroyContext(display, context);
    if (surface) destroySurface(display, surface);
    if (initialized) terminate(display);
    FreeLibrary(gl);
    FreeLibrary(egl);
    return result;
}
